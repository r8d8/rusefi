/*
    STM32F439ZI memory setup - 2MB Flash variant
    Based on STM32F4.ld with increased flash size
*/

/* On devices with 256K RAM, use additional memory SRAM3. */
/* STM32F42x and F46x have SRAM3 */
RAM3_SIZE = DEFINED(STM32F4_HAS_SRAM3) ? 64k : 0;

/* change address & length if bootloader */
bootloader_size = 32k;
flash_start = 0x08000000 + (DEFINED(HAS_BOOTLOADER) ? bootloader_size : 0);
/* STM32F439 has 2MB flash */
flash_size = DEFINED(IS_BOOTLOADER) ? bootloader_size : 2048k;
image_size = DEFINED(HAS_BOOTLOADER) ? (flash_size - bootloader_size) : flash_size;

/* OpenBLT <-> main FW shared area */
_OpenBLT_Shared_Params_Size = DEFINED(HAS_BOOTLOADER) || DEFINED(IS_BOOTLOADER) ? 16 : 0;

MEMORY
{
    bl      : org = 0x08000000, len = bootloader_size     /* bootloader section */
    flash0  : org = flash_start, len = image_size
    flash1  : org = 0x00000000, len = 0
    flash2  : org = 0x00000000, len = 0
    flash3  : org = 0x00000000, len = 0
    flash4  : org = 0x00000000, len = 0
    flash5  : org = 0x00000000, len = 0
    flash6  : org = 0x00000000, len = 0
    flash7  : org = 0x00000000, len = 0
    /* ram0 should be used when SRAM3 is not available or not desired to be used */
    /* CCM is best for DMA buffers that must NOT be in CCM */
    ram0    : org = 0x20000000, len = 128k      /* SRAM1 + SRAM2 */
    ram1    : org = 0x20000000, len = 112k      /* SRAM1 */
    ram2    : org = 0x2001C000, len = 16k       /* SRAM2 */
    ram3    : org = 0x20020000, len = RAM3_SIZE /* SRAM3 on F427/F437/F429/F439/F469/F479 */
    ram4    : org = 0x10000000, len = 64k       /* CCM SRAM */
    ram5    : org = 0x40024000, len = 4k        /* BCKP SRAM */
    ram6    : org = 0x00000000, len = 0
    ram7    : org = 0x00000000, len = 0
}

/* For each data/text section two region are defined, a virtual region
   and a load region (_LMA suffix).*/

/* Flash region to be used for exception vectors.*/
REGION_ALIAS("VECTORS_FLASH", flash0);
REGION_ALIAS("VECTORS_FLASH_LMA", flash0);

/* Flash region to be used for constructors and destructors.*/
REGION_ALIAS("XTORS_FLASH", flash0);
REGION_ALIAS("XTORS_FLASH_LMA", flash0);

/* Flash region to be used for code text.*/
REGION_ALIAS("TEXT_FLASH", flash0);
REGION_ALIAS("TEXT_FLASH_LMA", flash0);

/* Flash region to be used for read only data.*/
REGION_ALIAS("RODATA_FLASH", flash0);
REGION_ALIAS("RODATA_FLASH_LMA", flash0);

/* Flash region to be used for various.*/
REGION_ALIAS("VARIOUS_FLASH", flash0);
REGION_ALIAS("VARIOUS_FLASH_LMA", flash0);

/* Flash region to be used for RAM(n) initialization data.*/
REGION_ALIAS("RAM_INIT_FLASH_LMA", flash0);

/* RAM region to be used for Main stack. This stack accommodates the processing
   of all exceptions and interrupts.*/
REGION_ALIAS("MAIN_STACK_RAM", ram4);

/* RAM region to be used for the process stack. This is the stack used by
   the main() function.*/
REGION_ALIAS("PROCESS_STACK_RAM", ram4);

/* RAM region to be used for Main stack. This stack accommodates the processing
   of all exceptions and interrupts.*/
REGION_ALIAS("MAIN_STACK_RAM", ram4);

/* RAM region to be used for the process stack. This is the stack used by
   the main() function.*/
REGION_ALIAS("PROCESS_STACK_RAM", ram4);

/* RAM region to be used for data segment.*/
REGION_ALIAS("DATA_RAM", ram0);
REGION_ALIAS("DATA_RAM_LMA", flash0);

/* RAM region to be used for BSS segment.*/
REGION_ALIAS("BSS_RAM", ram0);

/* RAM region to be used for the default heap.*/
REGION_ALIAS("HEAP_RAM", ram0);

/* Generic rules inclusion.*/
INCLUDE rules.ld
