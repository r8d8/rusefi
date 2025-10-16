# List of all the board related files.
BOARDCPPSRC = $(BOARD_DIR)/board_configuration.cpp
DDEFS += -DLED_CRITICAL_ERROR_BRAIN_PIN=Gpio::B14

# Enable ethernet
LWIP = yes
ALLOW_SHADOW = yes
DDEFS += -DCH_CFG_USE_DYNAMIC=TRUE
DDEFS += -DEFI_ETHERNET=TRUE

# This is an F439 with 2MB flash!
IS_STM32F429 = yes

# Configure 2MB flash support (same as F767 Nucleo and other 2MB boards)
include $(PROJECT_DIR)/hw_layer/ports/stm32/2mb_flash.mk

# Disable SD card storage - Nucleo boards don't have SD card slot
DDEFS += -DEFI_STORAGE_SD=FALSE
DDEFS += -DEFI_FILE_LOGGING=FALSE
# Disable FATFS which depends on SD/MMC
USE_FATFS = no

BUNDLE_OPENOCD = yes

DDEFS += -DFIRMWARE_ID=\"nucleo_f439\"
DDEFS += -DDEFAULT_ENGINE_TYPE=engine_type_e::MINIMAL_PINS
DDEFS += -DSTATIC_BOARD_ID=STATIC_BOARD_ID_NUCLEO_F439
DDEFS += -DSTM32F439xx
