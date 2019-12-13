BL32_SOURCES += \
	drivers/delay_timer/generic_delay_timer.c \
	drivers/delay_timer/delay_timer.c \
	plat/common/aarch64/platform_mp_stack.S \
	${QEMU_GIC_SOURCES} \
	${PLAT_QEMU_COMMON_PATH}/${ARCH}/plat_helpers.S \
	${PLAT_QEMU_PATH}/tsp/tsp_plat_setup.c
