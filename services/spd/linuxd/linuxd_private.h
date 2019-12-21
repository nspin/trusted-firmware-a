/*
 * Copyright (c) 2013-2018, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LINUXD_PRIVATE_H
#define LINUXD_PRIVATE_H

#include <platform_def.h>

#include <arch.h>
#include <bl31/interrupt_mgmt.h>
#include <context.h>
#include <lib/psci/psci.h>

/*******************************************************************************
 * Secure Payload PM state information e.g. SP is suspended, uninitialised etc
 * and macros to access the state information in the per-cpu 'state' flags
 ******************************************************************************/
#define LINUX_PSTATE_OFF		0
#define LINUX_PSTATE_ON		1
#define LINUX_PSTATE_SUSPEND	2
#define LINUX_PSTATE_SHIFT	0
#define LINUX_PSTATE_MASK	0x3
#define get_linux_pstate(state)	((state >> LINUX_PSTATE_SHIFT) & LINUX_PSTATE_MASK)
#define clr_linux_pstate(state)	(state &= ~(LINUX_PSTATE_MASK \
					    << LINUX_PSTATE_SHIFT))
#define set_linux_pstate(st, pst)	do {					       \
					clr_linux_pstate(st);		       \
					st |= (pst & LINUX_PSTATE_MASK) <<       \
						LINUX_PSTATE_SHIFT;	       \
				} while (0);


/*
 * This flag is used by the LinuxD to determine if the Linux is servicing a yielding
 * SMC request prior to programming the next entry into the Linux e.g. if Linux
 * execution is preempted by a non-secure interrupt and handed control to the
 * normal world. If another request which is distinct from what the Linux was
 * previously doing arrives, then this flag will be help the LinuxD to either
 * reject the new request or service it while ensuring that the previous context
 * is not corrupted.
 */
#define YIELD_SMC_ACTIVE_FLAG_SHIFT	2
#define YIELD_SMC_ACTIVE_FLAG_MASK	1
#define get_yield_smc_active_flag(state)				\
				((state >> YIELD_SMC_ACTIVE_FLAG_SHIFT) \
				& YIELD_SMC_ACTIVE_FLAG_MASK)
#define set_yield_smc_active_flag(state)	(state |=		\
					1 << YIELD_SMC_ACTIVE_FLAG_SHIFT)
#define clr_yield_smc_active_flag(state)	(state &=		\
					~(YIELD_SMC_ACTIVE_FLAG_MASK	\
					<< YIELD_SMC_ACTIVE_FLAG_SHIFT))

/*******************************************************************************
 * Secure Payload execution state information i.e. aarch32 or aarch64
 ******************************************************************************/
#define LINUX_AARCH32		MODE_RW_32
#define LINUX_AARCH64		MODE_RW_64

/*******************************************************************************
 * Number of cpus that the present on this platform. TODO: Rely on a topology
 * tree to determine this in the future to avoid assumptions about mpidr
 * allocation
 ******************************************************************************/
#define LINUXD_CORE_COUNT		PLATFORM_CORE_COUNT

/*******************************************************************************
 * Constants that allow assembler code to preserve callee-saved registers of the
 * C runtime context while performing a security state switch.
 ******************************************************************************/
#define LINUXD_C_RT_CTX_X19		0x0
#define LINUXD_C_RT_CTX_X20		0x8
#define LINUXD_C_RT_CTX_X21		0x10
#define LINUXD_C_RT_CTX_X22		0x18
#define LINUXD_C_RT_CTX_X23		0x20
#define LINUXD_C_RT_CTX_X24		0x28
#define LINUXD_C_RT_CTX_X25		0x30
#define LINUXD_C_RT_CTX_X26		0x38
#define LINUXD_C_RT_CTX_X27		0x40
#define LINUXD_C_RT_CTX_X28		0x48
#define LINUXD_C_RT_CTX_X29		0x50
#define LINUXD_C_RT_CTX_X30		0x58
#define LINUXD_C_RT_CTX_SIZE		0x60
#define LINUXD_C_RT_CTX_ENTRIES		(LINUXD_C_RT_CTX_SIZE >> DWORD_SHIFT)

/*******************************************************************************
 * Constants that allow assembler code to preserve caller-saved registers of the
 * SP context while performing a Linux preemption.
 * Note: These offsets have to match with the offsets for the corresponding
 * registers in cpu_context as we are using memcpy to copy the values from
 * cpu_context to sp_ctx.
 ******************************************************************************/
#define LINUXD_SP_CTX_X0		0x0
#define LINUXD_SP_CTX_X1		0x8
#define LINUXD_SP_CTX_X2		0x10
#define LINUXD_SP_CTX_X3		0x18
#define LINUXD_SP_CTX_X4		0x20
#define LINUXD_SP_CTX_X5		0x28
#define LINUXD_SP_CTX_X6		0x30
#define LINUXD_SP_CTX_X7		0x38
#define LINUXD_SP_CTX_X8		0x40
#define LINUXD_SP_CTX_X9		0x48
#define LINUXD_SP_CTX_X10		0x50
#define LINUXD_SP_CTX_X11		0x58
#define LINUXD_SP_CTX_X12		0x60
#define LINUXD_SP_CTX_X13		0x68
#define LINUXD_SP_CTX_X14		0x70
#define LINUXD_SP_CTX_X15		0x78
#define LINUXD_SP_CTX_X16		0x80
#define LINUXD_SP_CTX_X17		0x88
#define LINUXD_SP_CTX_SIZE	0x90
#define LINUXD_SP_CTX_ENTRIES		(LINUXD_SP_CTX_SIZE >> DWORD_SHIFT)

#ifndef __ASSEMBLER__

#include <stdint.h>

#include <lib/cassert.h>

/*
 * The number of arguments to save during a SMC call for Linux.
 * Currently only x1 and x2 are used by Linux.
 */
#define LINUX_NUM_ARGS	0x2

/* AArch64 callee saved general purpose register context structure. */
DEFINE_REG_STRUCT(c_rt_regs, LINUXD_C_RT_CTX_ENTRIES);

/*
 * Compile time assertion to ensure that both the compiler and linker
 * have the same double word aligned view of the size of the C runtime
 * register context.
 */
CASSERT(LINUXD_C_RT_CTX_SIZE == sizeof(c_rt_regs_t),	\
	assert_spd_c_rt_regs_size_mismatch);

/* SEL1 Secure payload (SP) caller saved register context structure. */
DEFINE_REG_STRUCT(sp_ctx_regs, LINUXD_SP_CTX_ENTRIES);

/*
 * Compile time assertion to ensure that both the compiler and linker
 * have the same double word aligned view of the size of the C runtime
 * register context.
 */
CASSERT(LINUXD_SP_CTX_SIZE == sizeof(sp_ctx_regs_t),	\
	assert_spd_sp_regs_size_mismatch);

/*******************************************************************************
 * Structure which helps the SPD to maintain the per-cpu state of the SP.
 * 'saved_spsr_el3' - temporary copy to allow S-EL1 interrupt handling when
 *                    the Linux has been preempted.
 * 'saved_elr_el3'  - temporary copy to allow S-EL1 interrupt handling when
 *                    the Linux has been preempted.
 * 'state'          - collection of flags to track SP state e.g. on/off
 * 'mpidr'          - mpidr to associate a context with a cpu
 * 'c_rt_ctx'       - stack address to restore C runtime context from after
 *                    returning from a synchronous entry into the SP.
 * 'cpu_ctx'        - space to maintain SP architectural state
 * 'saved_linux_args' - space to store arguments for Linux arithmetic operations
 *                    which will queried using the LINUX_GET_ARGS SMC by Linux.
 * 'sp_ctx'         - space to save the SEL1 Secure Payload(SP) caller saved
 *                    register context after it has been preempted by an EL3
 *                    routed NS interrupt and when a Secure Interrupt is taken
 *                    to SP.
 ******************************************************************************/
typedef struct linux_context {
	uint64_t saved_elr_el3;
	uint32_t saved_spsr_el3;
	uint32_t state;
	uint64_t mpidr;
	uint64_t c_rt_ctx;
	cpu_context_t cpu_ctx;
	uint64_t saved_linux_args[LINUX_NUM_ARGS];
#if LINUX_NS_INTR_ASYNC_PREEMPT
	sp_ctx_regs_t sp_ctx;
#endif
} linux_context_t;

/* Helper macros to store and retrieve linux args from linux_context */
#define store_linux_args(_linux_ctx, _x1, _x2)		do {\
				_linux_ctx->saved_linux_args[0] = _x1;\
				_linux_ctx->saved_linux_args[1] = _x2;\
			} while (0)

#define get_linux_args(_linux_ctx, _x1, _x2)	do {\
				_x1 = _linux_ctx->saved_linux_args[0];\
				_x2 = _linux_ctx->saved_linux_args[1];\
			} while (0)

/* LinuxD power management handlers */
extern const spd_pm_ops_t linuxd_pm;

/*******************************************************************************
 * Forward declarations
 ******************************************************************************/
typedef struct linux_vectors linux_vectors_t;

/*******************************************************************************
 * Function & Data prototypes
 ******************************************************************************/
uint64_t linuxd_enter_sp(uint64_t *c_rt_ctx);
void __dead2 linuxd_exit_sp(uint64_t c_rt_ctx, uint64_t ret);
uint64_t linuxd_synchronous_sp_entry(linux_context_t *linux_ctx);
void __dead2 linuxd_synchronous_sp_exit(linux_context_t *linux_ctx, uint64_t ret);
void linuxd_init_linux_ep_state(struct entry_point_info *linux_entry_point,
				uint32_t rw,
				uint64_t pc,
				linux_context_t *linux_ctx);
int linuxd_abort_preempted_smc(linux_context_t *linux_ctx);

uint64_t linuxd_handle_sp_preemption(void *handle);

extern linux_context_t linuxd_sp_context[LINUXD_CORE_COUNT];
extern linux_vectors_t *linux_vectors;
#endif /*__ASSEMBLER__*/

#endif /* LINUXD_PRIVATE_H */
