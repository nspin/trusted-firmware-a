/*
 * Copyright (c) 2013-2016, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <plat/common/platform.h>

#include "linux.h"
#include "linuxd_private.h"

/*******************************************************************************
 * The target cpu is being turned on. Allow the LINUXD/LINUX to perform any actions
 * needed. Nothing at the moment.
 ******************************************************************************/
static void linuxd_cpu_on_handler(u_register_t target_cpu)
{
}

/*******************************************************************************
 * This cpu is being turned off. Allow the LINUXD/LINUX to perform any actions
 * needed
 ******************************************************************************/
static int32_t linuxd_cpu_off_handler(u_register_t unused)
{
	int32_t rc = 0;
	uint32_t linear_id = plat_my_core_pos();
	linux_context_t *linux_ctx = &linuxd_sp_context[linear_id];

	assert(linux_vectors);
	assert(get_linux_pstate(linux_ctx->state) == LINUX_PSTATE_ON);

	/*
	 * Abort any preempted SMC request before overwriting the SECURE
	 * context.
	 */
	linuxd_abort_preempted_smc(linux_ctx);

	/* Program the entry point and enter the LINUX */
	cm_set_elr_el3(SECURE, (uint64_t) &linux_vectors->cpu_off_entry);
	rc = linuxd_synchronous_sp_entry(linux_ctx);

	/*
	 * Read the response from the LINUX. A non-zero return means that
	 * something went wrong while communicating with the LINUX.
	 */
	if (rc != 0)
		panic();

	/*
	 * Reset LINUX's context for a fresh start when this cpu is turned on
	 * subsequently.
	 */
	set_linux_pstate(linux_ctx->state, LINUX_PSTATE_OFF);

	return 0;
}

/*******************************************************************************
 * This cpu is being suspended. S-EL1 state must have been saved in the
 * resident cpu (mpidr format) if it is a UP/UP migratable LINUX.
 ******************************************************************************/
static void linuxd_cpu_suspend_handler(u_register_t max_off_pwrlvl)
{
	int32_t rc = 0;
	uint32_t linear_id = plat_my_core_pos();
	linux_context_t *linux_ctx = &linuxd_sp_context[linear_id];

	assert(linux_vectors);
	assert(get_linux_pstate(linux_ctx->state) == LINUX_PSTATE_ON);

	/*
	 * Abort any preempted SMC request before overwriting the SECURE
	 * context.
	 */
	linuxd_abort_preempted_smc(linux_ctx);

	/* Program the entry point and enter the LINUX */
	cm_set_elr_el3(SECURE, (uint64_t) &linux_vectors->cpu_suspend_entry);
	rc = linuxd_synchronous_sp_entry(linux_ctx);

	/*
	 * Read the response from the LINUX. A non-zero return means that
	 * something went wrong while communicating with the LINUX.
	 */
	if (rc)
		panic();

	/* Update its context to reflect the state the LINUX is in */
	set_linux_pstate(linux_ctx->state, LINUX_PSTATE_SUSPEND);
}

/*******************************************************************************
 * This cpu has been turned on. Enter the LINUX to initialise S-EL1 and other bits
 * before passing control back to the Secure Monitor. Entry in S-EL1 is done
 * after initialising minimal architectural state that guarantees safe
 * execution.
 ******************************************************************************/
static void linuxd_cpu_on_finish_handler(u_register_t unused)
{
	int32_t rc = 0;
	uint32_t linear_id = plat_my_core_pos();
	linux_context_t *linux_ctx = &linuxd_sp_context[linear_id];
	entry_point_info_t linux_on_entrypoint;

	assert(linux_vectors);
	assert(get_linux_pstate(linux_ctx->state) == LINUX_PSTATE_OFF);

	linuxd_init_linux_ep_state(&linux_on_entrypoint,
				LINUX_AARCH64,
				(uint64_t) &linux_vectors->cpu_on_entry,
				linux_ctx);

	/* Initialise this cpu's secure context */
	cm_init_my_context(&linux_on_entrypoint);

#if LINUX_NS_INTR_ASYNC_PREEMPT
	/*
	 * Disable the NS interrupt locally since it will be enabled globally
	 * within cm_init_my_context.
	 */
	disable_intr_rm_local(INTR_TYPE_NS, SECURE);
#endif

	/* Enter the LINUX */
	rc = linuxd_synchronous_sp_entry(linux_ctx);

	/*
	 * Read the response from the LINUX. A non-zero return means that
	 * something went wrong while communicating with the SP.
	 */
	if (rc != 0)
		panic();

	/* Update its context to reflect the state the SP is in */
	set_linux_pstate(linux_ctx->state, LINUX_PSTATE_ON);
}

/*******************************************************************************
 * This cpu has resumed from suspend. The SPD saved the LINUX context when it
 * completed the preceding suspend call. Use that context to program an entry
 * into the LINUX to allow it to do any remaining book keeping
 ******************************************************************************/
static void linuxd_cpu_suspend_finish_handler(u_register_t max_off_pwrlvl)
{
	int32_t rc = 0;
	uint32_t linear_id = plat_my_core_pos();
	linux_context_t *linux_ctx = &linuxd_sp_context[linear_id];

	assert(linux_vectors);
	assert(get_linux_pstate(linux_ctx->state) == LINUX_PSTATE_SUSPEND);

	/* Program the entry point, max_off_pwrlvl and enter the SP */
	write_ctx_reg(get_gpregs_ctx(&linux_ctx->cpu_ctx),
		      CTX_GPREG_X0,
		      max_off_pwrlvl);
	cm_set_elr_el3(SECURE, (uint64_t) &linux_vectors->cpu_resume_entry);
	rc = linuxd_synchronous_sp_entry(linux_ctx);

	/*
	 * Read the response from the LINUX. A non-zero return means that
	 * something went wrong while communicating with the LINUX.
	 */
	if (rc != 0)
		panic();

	/* Update its context to reflect the state the SP is in */
	set_linux_pstate(linux_ctx->state, LINUX_PSTATE_ON);
}

/*******************************************************************************
 * Return the type of LINUX the LINUXD is dealing with. Report the current resident
 * cpu (mpidr format) if it is a UP/UP migratable LINUX.
 ******************************************************************************/
static int32_t linuxd_cpu_migrate_info(u_register_t *resident_cpu)
{
	return LINUX_MIGRATE_INFO;
}

/*******************************************************************************
 * System is about to be switched off. Allow the LINUXD/LINUX to perform
 * any actions needed.
 ******************************************************************************/
static void linuxd_system_off(void)
{
	uint32_t linear_id = plat_my_core_pos();
	linux_context_t *linux_ctx = &linuxd_sp_context[linear_id];

	assert(linux_vectors);
	assert(get_linux_pstate(linux_ctx->state) == LINUX_PSTATE_ON);

	/*
	 * Abort any preempted SMC request before overwriting the SECURE
	 * context.
	 */
	linuxd_abort_preempted_smc(linux_ctx);

	/* Program the entry point */
	cm_set_elr_el3(SECURE, (uint64_t) &linux_vectors->system_off_entry);

	/* Enter the LINUX. We do not care about the return value because we
	 * must continue the shutdown anyway */
	linuxd_synchronous_sp_entry(linux_ctx);
}

/*******************************************************************************
 * System is about to be reset. Allow the LINUXD/LINUX to perform
 * any actions needed.
 ******************************************************************************/
static void linuxd_system_reset(void)
{
	uint32_t linear_id = plat_my_core_pos();
	linux_context_t *linux_ctx = &linuxd_sp_context[linear_id];

	assert(linux_vectors);
	assert(get_linux_pstate(linux_ctx->state) == LINUX_PSTATE_ON);

	/*
	 * Abort any preempted SMC request before overwriting the SECURE
	 * context.
	 */
	linuxd_abort_preempted_smc(linux_ctx);

	/* Program the entry point */
	cm_set_elr_el3(SECURE, (uint64_t) &linux_vectors->system_reset_entry);

	/*
	 * Enter the LINUX. We do not care about the return value because we
	 * must continue the reset anyway
	 */
	linuxd_synchronous_sp_entry(linux_ctx);
}

/*******************************************************************************
 * Structure populated by the LINUX Dispatcher to be given a chance to perform any
 * LINUX bookkeeping before PSCI executes a power mgmt.  operation.
 ******************************************************************************/
const spd_pm_ops_t linuxd_pm = {
	.svc_on = linuxd_cpu_on_handler,
	.svc_off = linuxd_cpu_off_handler,
	.svc_suspend = linuxd_cpu_suspend_handler,
	.svc_on_finish = linuxd_cpu_on_finish_handler,
	.svc_suspend_finish = linuxd_cpu_suspend_finish_handler,
	.svc_migrate = NULL,
	.svc_migrate_info = linuxd_cpu_migrate_info,
	.svc_system_off = linuxd_system_off,
	.svc_system_reset = linuxd_system_reset
};
