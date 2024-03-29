/*
 * Copyright (c) 2013-2019, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */


/*******************************************************************************
 * This is the Secure Payload Dispatcher (SPD). The dispatcher is meant to be a
 * plug-in component to the Secure Monitor, registered as a runtime service. The
 * SPD is expected to be a functional extension of the Secure Payload (SP) that
 * executes in Secure EL1. The Secure Monitor will delegate all SMCs targeting
 * the Trusted OS/Applications range to the dispatcher. The SPD will either
 * handle the request locally or delegate it to the Secure Payload. It is also
 * responsible for initialising and maintaining communication with the SP.
 ******************************************************************************/
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <arch_helpers.h>
#include <bl31/bl31.h>
#include <bl31/ehf.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <common/runtime_svc.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <plat/common/platform.h>
#include <tools_share/uuid.h>

#include "linux.h"
#include "linuxd_private.h"

/*******************************************************************************
 * Address of the entrypoint vector table in the Secure Payload. It is
 * initialised once on the primary core after a cold boot.
 ******************************************************************************/
linux_vectors_t *linux_vectors;

/*******************************************************************************
 * Array to keep track of per-cpu Secure Payload state
 ******************************************************************************/
linux_context_t linuxd_sp_context[LINUXD_CORE_COUNT];


DEFINE_SVC_UUID2(linux_uuid,
	0xa056305b, 0x9132, 0x7b42, 0x98, 0x11,
	0x71, 0x68, 0xca, 0x50, 0xf3, 0xfb);

int32_t linuxd_init(void);

/*
 * This helper function handles Secure EL1 preemption. The preemption could be
 * due Non Secure interrupts or EL3 interrupts. In both the cases we context
 * switch to the normal world and in case of EL3 interrupts, it will again be
 * routed to EL3 which will get handled at the exception vectors.
 */
uint64_t linuxd_handle_sp_preemption(void *handle)
{
	cpu_context_t *ns_cpu_context;

	assert(handle == cm_get_context(SECURE));
	cm_el1_sysregs_context_save(SECURE);
	/* Get a reference to the non-secure context */
	ns_cpu_context = cm_get_context(NON_SECURE);
	assert(ns_cpu_context);

	/*
	 * To allow Secure EL1 interrupt handler to re-enter Linux while Linux
	 * is preempted, the secure system register context which will get
	 * overwritten must be additionally saved. This is currently done
	 * by the LinuxD S-EL1 interrupt handler.
	 */

	/*
	 * Restore non-secure state.
	 */
	cm_el1_sysregs_context_restore(NON_SECURE);
	cm_set_next_eret_context(NON_SECURE);

	/*
	 * The Linux was preempted during execution of a Yielding SMC Call.
	 * Return back to the normal world with SMC_PREEMPTED as error
	 * code in x0.
	 */
	SMC_RET1(ns_cpu_context, SMC_PREEMPTED);
}

/*******************************************************************************
 * This function is the handler registered for S-EL1 interrupts by the LinuxD. It
 * validates the interrupt and upon success arranges entry into the Linux at
 * 'linux_sel1_intr_entry()' for handling the interrupt.
 ******************************************************************************/
static uint64_t linuxd_sel1_interrupt_handler(uint32_t id,
					    uint32_t flags,
					    void *handle,
					    void *cookie)
{
	uint32_t linear_id;
	linux_context_t *linux_ctx;

	/* Check the security state when the exception was generated */
	assert(get_interrupt_src_ss(flags) == NON_SECURE);

	/* Sanity check the pointer to this cpu's context */
	assert(handle == cm_get_context(NON_SECURE));

	/* Save the non-secure context before entering the Linux */
	cm_el1_sysregs_context_save(NON_SECURE);

	/* Get a reference to this cpu's Linux context */
	linear_id = plat_my_core_pos();
	linux_ctx = &linuxd_sp_context[linear_id];
	assert(&linux_ctx->cpu_ctx == cm_get_context(SECURE));

	/*
	 * Determine if the Linux was previously preempted. Its last known
	 * context has to be preserved in this case.
	 * The Linux should return control to the LinuxD after handling this
	 * S-EL1 interrupt. Preserve essential EL3 context to allow entry into
	 * the Linux at the S-EL1 interrupt entry point using the 'cpu_context'
	 * structure. There is no need to save the secure system register
	 * context since the Linux is supposed to preserve it during S-EL1
	 * interrupt handling.
	 */
	if (get_yield_smc_active_flag(linux_ctx->state)) {
		linux_ctx->saved_spsr_el3 = SMC_GET_EL3(&linux_ctx->cpu_ctx,
						      CTX_SPSR_EL3);
		linux_ctx->saved_elr_el3 = SMC_GET_EL3(&linux_ctx->cpu_ctx,
						     CTX_ELR_EL3);
#if LINUX_NS_INTR_ASYNC_PREEMPT
		/*Need to save the previously interrupted secure context */
		memcpy(&linux_ctx->sp_ctx, &linux_ctx->cpu_ctx, LINUXD_SP_CTX_SIZE);
#endif
	}

	cm_el1_sysregs_context_restore(SECURE);
	cm_set_elr_spsr_el3(SECURE, (uint64_t) &linux_vectors->sel1_intr_entry,
		    SPSR_64(MODE_EL1, MODE_SP_ELX, DISABLE_ALL_EXCEPTIONS));

	cm_set_next_eret_context(SECURE);

	/*
	 * Tell the Linux that it has to handle a S-EL1 interrupt synchronously.
	 * Also the instruction in normal world where the interrupt was
	 * generated is passed for debugging purposes. It is safe to retrieve
	 * this address from ELR_EL3 as the secure context will not take effect
	 * until el3_exit().
	 */
	SMC_RET2(&linux_ctx->cpu_ctx, LINUX_HANDLE_SEL1_INTR_AND_RETURN, read_elr_el3());
}

#if LINUX_NS_INTR_ASYNC_PREEMPT
/*******************************************************************************
 * This function is the handler registered for Non secure interrupts by the
 * LinuxD. It validates the interrupt and upon success arranges entry into the
 * normal world for handling the interrupt.
 ******************************************************************************/
static uint64_t linuxd_ns_interrupt_handler(uint32_t id,
					    uint32_t flags,
					    void *handle,
					    void *cookie)
{
	/* Check the security state when the exception was generated */
	assert(get_interrupt_src_ss(flags) == SECURE);

	/*
	 * Disable the routing of NS interrupts from secure world to EL3 while
	 * interrupted on this core.
	 */
	disable_intr_rm_local(INTR_TYPE_NS, SECURE);

	return linuxd_handle_sp_preemption(handle);
}
#endif

/*******************************************************************************
 * Secure Payload Dispatcher setup. The SPD finds out the SP entrypoint and type
 * (aarch32/aarch64) if not already known and initialises the context for entry
 * into the SP for its initialisation.
 ******************************************************************************/
static int32_t linuxd_setup(void)
{
	entry_point_info_t *linux_ep_info;
	uint32_t linear_id;

	linear_id = plat_my_core_pos();

	/*
	 * Get information about the Secure Payload (BL32) image. Its
	 * absence is a critical failure.  TODO: Add support to
	 * conditionally include the SPD service
	 */
	linux_ep_info = bl31_plat_get_next_image_ep_info(SECURE);
	if (!linux_ep_info) {
		WARN("No Linux provided by BL2 boot loader, Booting device"
			" without Linux initialization. SMC`s destined for Linux"
			" will return SMC_UNK\n");
		return 1;
	}

	/*
	 * If there's no valid entry point for SP, we return a non-zero value
	 * signalling failure initializing the service. We bail out without
	 * registering any handlers
	 */
	if (!linux_ep_info->pc)
		return 1;

	/*
	 * We could inspect the SP image and determine its execution
	 * state i.e whether AArch32 or AArch64. Assuming it's AArch64
	 * for the time being.
	 */
	linuxd_init_linux_ep_state(linux_ep_info,
				LINUX_AARCH64,
				linux_ep_info->pc,
				&linuxd_sp_context[linear_id]);

#if LINUX_INIT_ASYNC
	bl31_set_next_image_type(SECURE);
#else
	/*
	 * All LinuxD initialization done. Now register our init function with
	 * BL31 for deferred invocation
	 */
	bl31_register_bl32_init(&linuxd_init);
#endif
	return 0;
}

/*******************************************************************************
 * This function passes control to the Secure Payload image (BL32) for the first
 * time on the primary cpu after a cold boot. It assumes that a valid secure
 * context has already been created by linuxd_setup() which can be directly used.
 * It also assumes that a valid non-secure context has been initialised by PSCI
 * so it does not need to save and restore any non-secure state. This function
 * performs a synchronous entry into the Secure payload. The SP passes control
 * back to this routine through a SMC.
 ******************************************************************************/
int32_t linuxd_init(void)
{
	uint32_t linear_id = plat_my_core_pos();
	linux_context_t *linux_ctx = &linuxd_sp_context[linear_id];
	entry_point_info_t *linux_entry_point;
	uint64_t rc;

	/*
	 * Get information about the Secure Payload (BL32) image. Its
	 * absence is a critical failure.
	 */
	linux_entry_point = bl31_plat_get_next_image_ep_info(SECURE);
	assert(linux_entry_point);

	cm_init_my_context(linux_entry_point);

	/*
	 * Arrange for an entry into the test secure payload. It will be
	 * returned via LINUX_ENTRY_DONE case
	 */
	rc = linuxd_synchronous_sp_entry(linux_ctx);
	assert(rc != 0);

	return rc;
}


/*******************************************************************************
 * This function is responsible for handling all SMCs in the Trusted OS/App
 * range from the non-secure state as defined in the SMC Calling Convention
 * Document. It is also responsible for communicating with the Secure payload
 * to delegate work and return results back to the non-secure state. Lastly it
 * will also return any information that the secure payload needs to do the
 * work assigned to it.
 ******************************************************************************/
static uintptr_t linuxd_smc_handler(uint32_t smc_fid,
			 u_register_t x1,
			 u_register_t x2,
			 u_register_t x3,
			 u_register_t x4,
			 void *cookie,
			 void *handle,
			 u_register_t flags)
{
	cpu_context_t *ns_cpu_context;
	uint32_t linear_id = plat_my_core_pos(), ns;
	linux_context_t *linux_ctx = &linuxd_sp_context[linear_id];
	uint64_t rc;
#if LINUX_INIT_ASYNC
	entry_point_info_t *next_image_info;
#endif

	/* Determine which security state this SMC originated from */
	ns = is_caller_non_secure(flags);

	switch (smc_fid) {

	case LINUX_PUTCHAR:
		putchar(x1);
		SMC_RET0(handle);

	/*
	 * This function ID is used by Linux to indicate that it was
	 * preempted by a normal world IRQ.
	 *
	 */
	case LINUX_PREEMPTED:
		if (ns)
			SMC_RET1(handle, SMC_UNK);

		return linuxd_handle_sp_preemption(handle);

	/*
	 * This function ID is used only by the Linux to indicate that it has
	 * finished handling a S-EL1 interrupt or was preempted by a higher
	 * priority pending EL3 interrupt. Execution should resume
	 * in the normal world.
	 */
	case LINUX_HANDLED_S_EL1_INTR:
		if (ns)
			SMC_RET1(handle, SMC_UNK);

		assert(handle == cm_get_context(SECURE));

		/*
		 * Restore the relevant EL3 state which saved to service
		 * this SMC.
		 */
		if (get_yield_smc_active_flag(linux_ctx->state)) {
			SMC_SET_EL3(&linux_ctx->cpu_ctx,
				    CTX_SPSR_EL3,
				    linux_ctx->saved_spsr_el3);
			SMC_SET_EL3(&linux_ctx->cpu_ctx,
				    CTX_ELR_EL3,
				    linux_ctx->saved_elr_el3);
#if LINUX_NS_INTR_ASYNC_PREEMPT
			/*
			 * Need to restore the previously interrupted
			 * secure context.
			 */
			memcpy(&linux_ctx->cpu_ctx, &linux_ctx->sp_ctx,
				LINUXD_SP_CTX_SIZE);
#endif
		}

		/* Get a reference to the non-secure context */
		ns_cpu_context = cm_get_context(NON_SECURE);
		assert(ns_cpu_context);

		/*
		 * Restore non-secure state. There is no need to save the
		 * secure system register context since the Linux was supposed
		 * to preserve it during S-EL1 interrupt handling.
		 */
		cm_el1_sysregs_context_restore(NON_SECURE);
		cm_set_next_eret_context(NON_SECURE);

		SMC_RET0((uint64_t) ns_cpu_context);

	/*
	 * This function ID is used only by the SP to indicate it has
	 * finished initialising itself after a cold boot
	 */
	case LINUX_ENTRY_DONE:
		if (ns)
			SMC_RET1(handle, SMC_UNK);

		/*
		 * Stash the SP entry points information. This is done
		 * only once on the primary cpu
		 */
		assert(linux_vectors == NULL);
		linux_vectors = (linux_vectors_t *) x1;

		if (linux_vectors) {
			set_linux_pstate(linux_ctx->state, LINUX_PSTATE_ON);

			/*
			 * Linux has been successfully initialized. Register power
			 * management hooks with PSCI
			 */
			psci_register_spd_pm_hook(&linuxd_pm);

			/*
			 * Register an interrupt handler for S-EL1 interrupts
			 * when generated during code executing in the
			 * non-secure state.
			 */
			flags = 0;
			set_interrupt_rm_flag(flags, NON_SECURE);
			rc = register_interrupt_type_handler(INTR_TYPE_S_EL1,
						linuxd_sel1_interrupt_handler,
						flags);
			if (rc)
				panic();

#if LINUX_NS_INTR_ASYNC_PREEMPT
			/*
			 * Register an interrupt handler for NS interrupts when
			 * generated during code executing in secure state are
			 * routed to EL3.
			 */
			flags = 0;
			set_interrupt_rm_flag(flags, SECURE);

			rc = register_interrupt_type_handler(INTR_TYPE_NS,
						linuxd_ns_interrupt_handler,
						flags);
			if (rc)
				panic();

			/*
			 * Disable the NS interrupt locally.
			 */
			disable_intr_rm_local(INTR_TYPE_NS, SECURE);
#endif
		}


#if LINUX_INIT_ASYNC
		/* Save the Secure EL1 system register context */
		assert(cm_get_context(SECURE) == &linux_ctx->cpu_ctx);
		cm_el1_sysregs_context_save(SECURE);

		/* Program EL3 registers to enable entry into the next EL */
		next_image_info = bl31_plat_get_next_image_ep_info(NON_SECURE);
		assert(next_image_info);
		assert(NON_SECURE ==
				GET_SECURITY_STATE(next_image_info->h.attr));

		cm_init_my_context(next_image_info);
		cm_prepare_el3_exit(NON_SECURE);
		SMC_RET0(cm_get_context(NON_SECURE));
#else
		/*
		 * SP reports completion. The SPD must have initiated
		 * the original request through a synchronous entry
		 * into the SP. Jump back to the original C runtime
		 * context.
		 */
		linuxd_synchronous_sp_exit(linux_ctx, x1);
		break;
#endif
	/*
	 * This function ID is used only by the SP to indicate it has finished
	 * aborting a preempted Yielding SMC Call.
	 */
	case LINUX_ABORT_DONE:

	/*
	 * These function IDs are used only by the SP to indicate it has
	 * finished:
	 * 1. turning itself on in response to an earlier psci
	 *    cpu_on request
	 * 2. resuming itself after an earlier psci cpu_suspend
	 *    request.
	 */
	case LINUX_ON_DONE:
	case LINUX_RESUME_DONE:

	/*
	 * These function IDs are used only by the SP to indicate it has
	 * finished:
	 * 1. suspending itself after an earlier psci cpu_suspend
	 *    request.
	 * 2. turning itself off in response to an earlier psci
	 *    cpu_off request.
	 */
	case LINUX_OFF_DONE:
	case LINUX_SUSPEND_DONE:
	case LINUX_SYSTEM_OFF_DONE:
	case LINUX_SYSTEM_RESET_DONE:
		if (ns)
			SMC_RET1(handle, SMC_UNK);

		/*
		 * SP reports completion. The SPD must have initiated the
		 * original request through a synchronous entry into the SP.
		 * Jump back to the original C runtime context, and pass x1 as
		 * return value to the caller
		 */
		linuxd_synchronous_sp_exit(linux_ctx, x1);
		break;

	/*
	 * Request from the non-secure world to abort a preempted Yielding SMC
	 * Call.
	 */
	case LINUX_FID_ABORT:
		/* ABORT should only be invoked by normal world */
		if (!ns) {
			assert(0);
			break;
		}

		assert(handle == cm_get_context(NON_SECURE));
		cm_el1_sysregs_context_save(NON_SECURE);

		/* Abort the preempted SMC request */
		if (!linuxd_abort_preempted_smc(linux_ctx)) {
			/*
			 * If there was no preempted SMC to abort, return
			 * SMC_UNK.
			 *
			 * Restoring the NON_SECURE context is not necessary as
			 * the synchronous entry did not take place if the
			 * return code of linuxd_abort_preempted_smc is zero.
			 */
			cm_set_next_eret_context(NON_SECURE);
			break;
		}

		cm_el1_sysregs_context_restore(NON_SECURE);
		cm_set_next_eret_context(NON_SECURE);
		SMC_RET1(handle, SMC_OK);

		/*
		 * Request from non secure world to resume the preempted
		 * Yielding SMC Call.
		 */
	case LINUX_FID_RESUME:
		/* RESUME should be invoked only by normal world */
		if (!ns) {
			assert(0);
			break;
		}

		/*
		 * This is a resume request from the non-secure client.
		 * save the non-secure state and send the request to
		 * the secure payload.
		 */
		assert(handle == cm_get_context(NON_SECURE));

		/* Check if we are already preempted before resume */
		if (!get_yield_smc_active_flag(linux_ctx->state))
			SMC_RET1(handle, SMC_UNK);

		cm_el1_sysregs_context_save(NON_SECURE);

		/*
		 * We are done stashing the non-secure context. Ask the
		 * secure payload to do the work now.
		 */
#if LINUX_NS_INTR_ASYNC_PREEMPT
		/*
		 * Enable the routing of NS interrupts to EL3 during resumption
		 * of a Yielding SMC Call on this core.
		 */
		enable_intr_rm_local(INTR_TYPE_NS, SECURE);
#endif

#if EL3_EXCEPTION_HANDLING
		/*
		 * Allow the resumed yielding SMC processing to be preempted by
		 * Non-secure interrupts. Also, supply the preemption return
		 * code for Linux.
		 */
		ehf_allow_ns_preemption(LINUX_PREEMPTED);
#endif

		/* We just need to return to the preempted point in
		 * Linux and the execution will resume as normal.
		 */
		cm_el1_sysregs_context_restore(SECURE);
		cm_set_next_eret_context(SECURE);
		SMC_RET0(&linux_ctx->cpu_ctx);

		/*
		 * This is a request from the secure payload for more arguments
		 * for an ongoing arithmetic operation requested by the
		 * non-secure world. Simply return the arguments from the non-
		 * secure client in the original call.
		 */
	case LINUX_GET_ARGS:
		if (ns)
			SMC_RET1(handle, SMC_UNK);

		get_linux_args(linux_ctx, x1, x2);
		SMC_RET2(handle, x1, x2);

	case TOS_CALL_COUNT:
		/*
		 * Return the number of service function IDs implemented to
		 * provide service to non-secure
		 */
		SMC_RET1(handle, LINUX_NUM_FID);

	case TOS_UID:
		/* Return Linux UID to the caller */
		SMC_UUID_RET(handle, linux_uuid);

	case TOS_CALL_VERSION:
		/* Return the version of current implementation */
		SMC_RET2(handle, LINUX_VERSION_MAJOR, LINUX_VERSION_MINOR);

	default:
		break;
	}

	SMC_RET1(handle, SMC_UNK);
}

/* Define a SPD runtime service descriptor for fast SMC calls */
DECLARE_RT_SVC(
	linuxd_fast,

	OEN_TOS_START,
	OEN_TOS_END,
	SMC_TYPE_FAST,
	linuxd_setup,
	linuxd_smc_handler
);

/* Define a SPD runtime service descriptor for Yielding SMC Calls */
DECLARE_RT_SVC(
	linuxd_std,

	OEN_TOS_START,
	OEN_TOS_END,
	SMC_TYPE_YIELD,
	NULL,
	linuxd_smc_handler
);
