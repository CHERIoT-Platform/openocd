// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "target/target.h"
#include "target/register.h"
#include "riscv.h"
#include "program.h"
#include "helper/log.h"

#include "debug_defines.h"
#include "encoding.h"

/* Program interface. */
int riscv_program_init(struct riscv_program *p, struct target *target)
{
	memset(p, 0, sizeof(*p));
	p->target = target;
	p->instruction_count = 0;

	for (unsigned int i = 0; i < RISCV013_MAX_PROGBUF_SIZE; ++i)
		p->progbuf[i] = (uint32_t)(-1);

	p->execution_result = RISCV_PROGBUF_EXEC_RESULT_NOT_EXECUTED;
	return ERROR_OK;
}

int riscv_program_write(struct riscv_program *program)
{
	for (unsigned int i = 0; i < program->instruction_count; ++i) {
		LOG_TARGET_DEBUG(program->target, "progbuf[%02x] = DASM(0x%08x)",
				i, program->progbuf[i]);
		if (riscv_write_progbuf(program->target, i, program->progbuf[i]) != ERROR_OK)
			return ERROR_FAIL;
	}
	return ERROR_OK;
}

static unsigned int cheriot_get_saverestore_csr(enum gdb_regno regid) {
	unsigned offset = regid - (GDB_REGNO_ZERO + 1);
	return offset + 9;
}

int cheriot_save_register(struct target *target, enum gdb_regno regid) {
	struct riscv_info *info = target->arch_info;
	if (!cheriot_variant_can_spill_registers(info)) {
		return ERROR_FAIL;
	}

	if (regid < GDB_REGNO_ZERO + 1 || regid > GDB_REGNO_XPR15) {
		return ERROR_FAIL;
	}

	unsigned scr = cheriot_get_saverestore_csr(regid);

	struct riscv_program program;
	riscv_program_init(&program, target);
	riscv_program_insert(&program, ct_cspecialw(scr, regid - GDB_REGNO_ZERO));

	if (riscv_program_exec(&program, target) != ERROR_OK) {
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

int cheriot_restore_register(struct target *target, enum gdb_regno regid) {
	struct riscv_info *info = target->arch_info;
	if (!cheriot_variant_can_spill_registers(info)) {
		return ERROR_FAIL;
	}

	if (regid < GDB_REGNO_ZERO + 1 || regid > GDB_REGNO_XPR15) {
		return ERROR_FAIL;
	}

	unsigned scr = cheriot_get_saverestore_csr(regid);

	struct riscv_program program;
	riscv_program_init(&program, target);
	riscv_program_insert(&program, ct_cspecialr(regid - GDB_REGNO_ZERO, scr));
	// NOTE: Intentionally do not set writes_xreg[i], even though we do,
	// because we don't want to recursively trigger another save/restore.

	if (riscv_program_exec(&program, target) != ERROR_OK) {
		return ERROR_FAIL;
	}

	// Invalidate the register cache
	// FIXME: This could be done fine-grained.
	register_cache_invalidate(target->reg_cache);

	return ERROR_OK;
}

/** Add ebreak and execute the program. */
int riscv_program_exec(struct riscv_program *p, struct target *t)
{
	keep_alive();

	// Register saving / restoring was removed in
	// https://github.com/riscv-collab/riscv-openocd
	// commit d0615e4a12f5be543ea29e89dcfab5bdb9d9c011
	p->execution_result = RISCV_PROGBUF_EXEC_RESULT_UNKNOWN;

	if (riscv_program_ebreak(p) != ERROR_OK) {
		LOG_TARGET_ERROR(t, "Unable to insert ebreak into program buffer");
		for (unsigned int i = 0; i < riscv_progbuf_size(p->target); ++i)
			LOG_TARGET_ERROR(t, "progbuf[%02x]: DASM(0x%08" PRIx32 ") [0x%08" PRIx32 "]",
					i, p->progbuf[i], p->progbuf[i]);
		return ERROR_FAIL;
	}

	if (riscv_program_write(p) != ERROR_OK)
		return ERROR_FAIL;

	uint32_t cmderr;
	if (riscv_execute_progbuf(t, &cmderr) != ERROR_OK) {
		p->execution_result = (cmderr == DM_ABSTRACTCS_CMDERR_EXCEPTION)
			? RISCV_PROGBUF_EXEC_RESULT_EXCEPTION
			: RISCV_PROGBUF_EXEC_RESULT_UNKNOWN_ERROR;
		/* TODO: what happens if we fail here, but need to restore registers? */
		LOG_TARGET_DEBUG(t, "Unable to execute program %p", p);
		return ERROR_FAIL;
	}
	p->execution_result = RISCV_PROGBUF_EXEC_RESULT_SUCCESS;

	return ERROR_OK;
}

int riscv_program_sdr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int16_t offset)
{
	return riscv_program_insert(p, sd(d, b, offset));
}

int riscv_program_swr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int16_t offset)
{
	return riscv_program_insert(p, sw(d, b, offset));
}

int riscv_program_shr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int16_t offset)
{
	return riscv_program_insert(p, sh(d, b, offset));
}

int riscv_program_sbr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int16_t offset)
{
	return riscv_program_insert(p, sb(d, b, offset));
}

int riscv_program_store(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int16_t offset,
		unsigned int size)
{
	switch (size) {
	case 1:
		return riscv_program_sbr(p, d, b, offset);
	case 2:
		return riscv_program_shr(p, d, b, offset);
	case 4:
		return riscv_program_swr(p, d, b, offset);
	case 8:
		return riscv_program_sdr(p, d, b, offset);
	}
	assert(false && "Unsupported size");
	return ERROR_FAIL;
}

int riscv_program_ldr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int16_t offset)
{
	return riscv_program_insert(p, ld(d, b, offset));
}

int riscv_program_lwr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int16_t offset)
{
	return riscv_program_insert(p, lw(d, b, offset));
}

int riscv_program_lhr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int16_t offset)
{
	return riscv_program_insert(p, lh(d, b, offset));
}

int riscv_program_lbr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int16_t offset)
{
	return riscv_program_insert(p, lb(d, b, offset));
}

int riscv_program_load(struct riscv_program *p, enum gdb_regno d, enum gdb_regno b, int16_t offset,
		unsigned int size)
{
	switch (size) {
	case 1:
		return riscv_program_lbr(p, d, b, offset);
	case 2:
		return riscv_program_lhr(p, d, b, offset);
	case 4:
		return riscv_program_lwr(p, d, b, offset);
	case 8:
		return riscv_program_ldr(p, d, b, offset);
	}
	assert(false && "Unsupported size");
	return ERROR_FAIL;
}

int riscv_program_csrrsi(struct riscv_program *p, enum gdb_regno d, uint8_t z, enum gdb_regno csr)
{
	assert(csr >= GDB_REGNO_CSR0 && csr <= GDB_REGNO_CSR4095);
	return riscv_program_insert(p, csrrsi(d, z, csr - GDB_REGNO_CSR0));
}

int riscv_program_csrrci(struct riscv_program *p, enum gdb_regno d, uint8_t z, enum gdb_regno csr)
{
	assert(csr >= GDB_REGNO_CSR0 && csr <= GDB_REGNO_CSR4095);
	return riscv_program_insert(p, csrrci(d, z, csr - GDB_REGNO_CSR0));
}

int riscv_program_csrr(struct riscv_program *p, enum gdb_regno d, enum gdb_regno csr)
{
	assert(csr >= GDB_REGNO_CSR0 && csr <= GDB_REGNO_CSR4095);
	return riscv_program_insert(p, csrrs(d, GDB_REGNO_ZERO, csr - GDB_REGNO_CSR0));
}

int riscv_program_csrw(struct riscv_program *p, enum gdb_regno s, enum gdb_regno csr)
{
	assert(csr >= GDB_REGNO_CSR0 && csr <= GDB_REGNO_CSR4095);
	return riscv_program_insert(p, csrrw(GDB_REGNO_ZERO, s, csr - GDB_REGNO_CSR0));
}

int riscv_program_fence_i(struct riscv_program *p)
{
	return riscv_program_insert(p, fence_i());
}

int riscv_program_fence_rw_rw(struct riscv_program *p)
{
	return riscv_program_insert(p, fence_rw_rw());
}

int riscv_program_ebreak(struct riscv_program *p)
{
	struct target *target = p->target;
	RISCV_INFO(r);
	if (p->instruction_count == riscv_progbuf_size(target) &&
			r->get_impebreak(target)) {
		return ERROR_OK;
	}
	return riscv_program_insert(p, ebreak());
}

int riscv_program_addi(struct riscv_program *p, enum gdb_regno d, enum gdb_regno s, int16_t u)
{
	return riscv_program_insert(p, addi(d, s, u));
}

int riscv_program_insert(struct riscv_program *p, riscv_insn_t i)
{
	if (p->instruction_count >= riscv_progbuf_size(p->target)) {
		LOG_TARGET_ERROR(p->target, "Unable to insert program into progbuf, "
			"capacity would be exceeded (progbufsize=%u).",
			riscv_progbuf_size(p->target));
		return ERROR_FAIL;
	}

	p->progbuf[p->instruction_count] = i;
	p->instruction_count++;
	return ERROR_OK;
}
