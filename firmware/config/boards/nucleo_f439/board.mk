# List of all the board related files.
BOARDCPPSRC = $(BOARD_DIR)/board_configuration.cpp
DDEFS += -DLED_CRITICAL_ERROR_BRAIN_PIN=Gpio::B14

# Enable ethernet
LWIP = yes
ALLOW_SHADOW = yes
DDEFS += -DCH_CFG_USE_DYNAMIC=TRUE
DDEFS += -DEFI_ETHERNET=TRUE

# This is an F439! (F4 family with crypto)
IS_STM32F429 = yes
DDEFS += -DSTM32F439xx

# Enable servo throttle for testing
DDEFS += -DEFI_SERVO_THROTTLE=TRUE

BUNDLE_OPENOCD = yes

DDEFS += -DFIRMWARE_ID=\"nucleo_f439\"
DDEFS += -DDEFAULT_ENGINE_TYPE=engine_type_e::MINIMAL_PINS
DDEFS += -DSTATIC_BOARD_ID=STATIC_BOARD_ID_NUCLEO_F439

# Override CPU specific includes for F439
USE_CHIBIOS_RT = yes