#include <drivers/console.h>
#include <drivers/arm/pl011.h>
#include <drivers/generic_delay_timer.h>

#include <platform_def.h>
#include <platform_tsp.h>

#include "../qemu_private.h"

unsigned int plat_get_syscnt_freq2(void)
{
	return SYS_COUNTER_FREQ_IN_TICKS;
}

void tsp_early_platform_setup(void)
{
	static console_pl011_t console;
	(void)console_pl011_register(PLAT_QEMU_CRASH_UART_BASE,
		PLAT_QEMU_CRASH_UART_CLK_IN_HZ,
		PLAT_QEMU_CONSOLE_BAUDRATE, &console);

	console_set_scope(&console.console,
		CONSOLE_FLAG_BOOT | CONSOLE_FLAG_RUNTIME);

	generic_delay_timer_init();
}

void tsp_platform_setup(void)
{
	plat_qemu_gic_init();
}

void tsp_plat_arch_setup(void)
{
	qemu_configure_mmu_el1(BL32_BASE, (BL32_END - BL32_BASE),
		BL_CODE_BASE, BL_CODE_END,
		BL_RO_DATA_BASE, BL_RO_DATA_END,
		BL_COHERENT_RAM_BASE, BL_COHERENT_RAM_END);
}
