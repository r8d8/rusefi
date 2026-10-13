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
# uaEFI running the DLE-120 preset (the stage 1.1 baseline engine; DLE_60_TWIN stays selectable),
# with the bench pins set in board_configuration.cpp
DDEFS += -DHW_NUCLEO_F429_HITL=1
DDEFS += -DEFI_SERVO_THROTTLE=TRUE
# No Ethernet on the bench: ChibiOS's MAC init in halInit loops on the PHY's soft reset (MDIO) and
# on the DMA reset (needs the PHY's 50 MHz REF_CLK), so a board whose PHY does not answer hangs
# before USB and the LEDs (seen on the HITL F439, 2026-10-13)
EFI_ETHERNET = no
DDEFS += -DFIRMWARE_ID=\"nucleo_f429_hitl\"
DDEFS += -DDEFAULT_ENGINE_TYPE=engine_type_e::DLE_120_TWIN
else
DDEFS += -DFIRMWARE_ID=\"nucleo_f429\"
DDEFS += -DDEFAULT_ENGINE_TYPE=engine_type_e::MINIMAL_PINS
endif
DDEFS += -DSTATIC_BOARD_ID=STATIC_BOARD_ID_NUCLEO_F429

# reducing RAM consumption for EFI_ETHERNET to fit
DDEFS += -DEFI_ALTERNATOR_CONTROL=FALSE -DEFI_LOGIC_ANALYZER=FALSE -DEFI_ENABLE_ASSERTS=FALSE

# Save some RAM: EFI_LUA=FALSE is declared in
# prepend.txt and lifted into DDEFS by the Makefile - see [tag:ts_page_table]
