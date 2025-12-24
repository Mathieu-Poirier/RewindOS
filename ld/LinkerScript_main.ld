/*
*****************************************************************************
**  File        : LinkerScript.ld (RewindOS-ready, no-libc)
**  Target      : STM32F746NGHx  (1MB FLASH, 320KB internal SRAM)
**  Notes       :
**   - Execute code from FLASH (XIP). Kernel data in internal SRAM.
**   - SDRAM is for user programs, big buffers, and snapshots.
**   - No libc/newlib required. No C++ constructors/destructors expected.
**   - Exports symbols used by startup for copies/zeroing and by snapshot logic.
*****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* Minimal bookkeeping for heap/stack (tune/remove as needed) */
_Min_Heap_Size  = 0x200;   /* 512 B */
_Min_Stack_Size = 0x400;   /* 1 KB  */

/*============================== MEMORY MAP ==============================*/
MEMORY
{
  /* Internal memories */
  FLASH (rx) : ORIGIN = 0x08020000, LENGTH = 896K
  RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 320K     /* SRAM (DTCM+SRAM1/2 aggregate if applicable) */

  /* External SDRAM via FMC (adjust LENGTH to your board) */
  SDRAM  (xrw) : ORIGIN = 0xC0000000, LENGTH = 8M
}

/* Top of main stack derived from RAM region */
_estack = ORIGIN(RAM) + LENGTH(RAM);

/*============================ SNAPSHOT SYMBOLS ==========================*/
/* Whole-region boundaries (used by snapshot/loader code) */
PROVIDE(_sram_start  = ORIGIN(RAM));
PROVIDE(_sram_end    = ORIGIN(RAM)   + LENGTH(RAM));
PROVIDE(_sdram_start = ORIGIN(SDRAM));
PROVIDE(_sdram_end   = ORIGIN(SDRAM) + LENGTH(SDRAM));

/*============================== SECTIONS ================================*/
SECTIONS
{
  /* ----------------------- VECTORS & CODE IN FLASH -------------------- */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text) *(.text*)                     /* all code */
    *(.glue_7) *(.glue_7t)                 /* ARM/Thumb veneers if any */
    . = ALIGN(4);
    _etext = .;                             /* end of code in FLASH */
  } > FLASH

  /* Place const data in FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata) *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  /* Exception index tables (harmless even if you don't use exceptions) */
  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > FLASH

  /* ----------------------- RAM INITIALIZED DATA ----------------------- */

  /* Source address in FLASH for .data (used by startup) */
  _sidata = LOADADDR(.data);

  /* .data lives in RAM, loaded from FLASH */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;                             /* start of .data in RAM */
    *(.data) *(.data*)
    . = ALIGN(4);
    _edata = .;                             /* end of .data in RAM */
  } > RAM AT > FLASH

  /* ----------------------- ZEROED DATA IN RAM ------------------------- */
  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .; __bss_start__ = _sbss;
    *(.bss) *(.bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .; __bss_end__ = _ebss;
  } > RAM

  /* ----------------------- OPTIONAL RAM SECTIONS ---------------------- */

  /* Not initialized on reset (retain across soft reset). */
  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    _snoinit = .;
    *(.noinit) *(.noinit*)
    . = ALIGN(4);
    _enoinit = .;
  } > RAM

  /* RAM to exclude from snapshots (DMA rings, caches, scratch). */
  .snap_exclude (NOLOAD) :
  {
    . = ALIGN(4);
    _ssnap_exclude = .;
    *(.snap_exclude) *(.snap_exclude*)
    . = ALIGN(4);
    _esnap_exclude = .;
  } > RAM

  /* ----------------------- SDRAM (OPTIONAL, INIT FROM FLASH) ---------- */

  /* Source address in FLASH for .sdram_data (used by startup) */
  _sisdram_data = LOADADDR(.sdram_data);

  /* Large initialized objects explicitly in SDRAM.
     Use: __attribute__((section(".sdram_data"))) */
  .sdram_data :
  {
    . = ALIGN(8);
    _ssdram_data = .;
    *(.sdram_data) *(.sdram_data*)
    . = ALIGN(8);
    _esdram_data = .;
  } > SDRAM AT > FLASH

  /* ----------------------- HEAP / STACK ACCOUNTING -------------------- */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end  = .);
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } > RAM

  /* ----------------------- MISC / ATTRIBUTES -------------------------- */

  /* Do NOT forcibly discard libgcc/libm/libc here; simply avoid linking them
     with -nostdlib/-ffreestanding. Keeping this empty prevents accidental drops. */
  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}

/*============================== ASSERTIONS ==============================*/

/* Fail link if internal RAM is overflowed by kernel sections + heap/stack */
ASSERT(_ebss <= (ORIGIN(RAM) + LENGTH(RAM)),
       "ERROR: Internal RAM overflow (reduce .data/.bss or move buffers to SDRAM)");

/* Guard that FLASH usage fits device */
ASSERT(_etext <= (ORIGIN(FLASH) + LENGTH(FLASH)),
       "ERROR: FLASH overflow");
