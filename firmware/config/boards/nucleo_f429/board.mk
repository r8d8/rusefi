# List of all the board related files.

# this board has 2Mb chip
include $(PROJECT_DIR)/hw_layer/ports/stm32/2mb_flash.mk

BOARDCPPSRC = $(BOARD_DIR)/board_configuration.cpp
DDEFS += -DLED_CRITICAL_ERROR_BRAIN_PIN=Gpio::B14

# Enable ethernet
EFI_ETHERNET = yes

# This is an F429!
IS_STM32F429 = yes

BUNDLE_OPENOCD = yes

# because of RAM
MODULE_DTC_MANAGER = no

ifeq ($(SHORT_BOARD_NAME),stm32f429_nucleo_hitl)
# Hybrid controller hardware-in-the-loop bench: the Nucleo (F429ZI or F439ZI) stands in for
# uaEFI running the DLE-60 preset, with the bench pins set in board_configuration.cpp
DDEFS += -DHW_NUCLEO_F429_HITL=1
DDEFS += -DEFI_SERVO_THROTTLE=TRUE
DDEFS += -DFIRMWARE_ID=\"nucleo_f429_hitl\"
DDEFS += -DDEFAULT_ENGINE_TYPE=engine_type_e::DLE_60_TWIN
else
DDEFS += -DFIRMWARE_ID=\"nucleo_f429\"
DDEFS += -DDEFAULT_ENGINE_TYPE=engine_type_e::MINIMAL_PINS
endif
DDEFS += -DSTATIC_BOARD_ID=STATIC_BOARD_ID_NUCLEO_F429

# reducing RAM consumption for EFI_ETHERNET to fit
DDEFS += -DEFI_ALTERNATOR_CONTROL=FALSE -DEFI_LOGIC_ANALYZER=FALSE -DEFI_ENABLE_ASSERTS=FALSE

# Save some RAM: EFI_LUA=FALSE is declared in
# prepend.txt and lifted into DDEFS by the Makefile - see [tag:ts_page_table]
