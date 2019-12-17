TSPD_DIR		:=	services/spd/linuxd

ifeq (${ERROR_DEPRECATED},0)
SPD_INCLUDES		:=	-Iinclude/bl32/tsp
endif

SPD_SOURCES		:=	services/spd/linuxd/linuxd_common.c		\
				services/spd/linuxd/linuxd_helpers.S	\
				services/spd/linuxd/linuxd_main.c		\
				services/spd/linuxd/linuxd_pm.c

NEED_BL32 := yes

# Flag used to enable routing of non-secure interrupts to EL3 when they are
# generated while the code is executing in S-EL1/0.
TSP_NS_INTR_ASYNC_PREEMPT	:=	0

ifeq ($(EL3_EXCEPTION_HANDLING),1)
ifeq ($(TSP_NS_INTR_ASYNC_PREEMPT),0)
$(error When EL3_EXCEPTION_HANDLING=1, TSP_NS_INTR_ASYNC_PREEMPT must also be 1)
endif
endif

$(eval $(call assert_boolean,TSP_NS_INTR_ASYNC_PREEMPT))
$(eval $(call add_define,TSP_NS_INTR_ASYNC_PREEMPT))
