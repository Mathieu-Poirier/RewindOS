= Architecture Overview

== Memory Map (ARM Architecture)

#table(
  columns: 6,
  table.header(
    [*Address Range*],
    [*Name*],
    [*Device Type*],
    [*XN?*],
    [*Cache*],
    [*Description*],
  ),

  [0x0000_0000–0x1FFF_FFFF],
  [Code],
  [Normal],
  [-],
  [WT],
  [Typically ROM or Flash memory.],

  [0x2000_0000–0x3FFF_FFFF],
  [SRAM],
  [Normal],
  [-],
  [WBWA],
  [On-chip SRAM region.],

  [0x4000_0000–0x5FFF_FFFF],
  [Peripheral],
  [Device],
  [XN],
  [-],
  [On-chip peripheral address space.],

  [0x6000_0000–0x7FFF_FFFF],
  [RAM],
  [Normal],
  [-],
  [WBWA],
  [External RAM with write-back/write-allocate.],

  [0x8000_0000–0x9FFF_FFFF],
  [RAM],
  [Normal],
  [-],
  [WT],
  [External RAM with write-through attribute.],

  [0xA000_0000–0xBFFF_FFFF],
  [Device],
  [Device (shareable)],
  [XN],
  [-],
  [Shared device space.],

  [0xC000_0000–0xDFFF_FFFF],
  [Device],
  [Device (non-shareable)],
  [XN],
  [-],
  [Non-shared external device space.],

  [0xE000_0000–0xE00F_FFFF],
  [PPB],
  [Strongly-ordered],
  [XN],
  [-],
  [Private Peripheral Bus (NVIC, SysTick, SCB).],

  [0xE010_0000–0xFFFF_FFFF],
  [Vendor_SYS],
  [Device],
  [XN],
  [-],
  [Vendor system region.],
)

== Glossary

*Address Range:* An address range is a contiguous segment of the processor's address space assigned to a specific memory or hardware function.\

*Name:* A descriptive label indicating the functional category of hardware mapped into that address range.\

*Device Type:* Device Type tells the CPU whether reads/writes act like real memory or like hardware control registers\

*XN (Execute Never):* XN denotes whether the CPU is allowed to treat that memory as executable code.\

*Cache:* Cache describes how the CPU’s memory system treats the region with respect to caching behavior.\

- *WT (Write-Back, Write-Allocate):* When the CPU writes to memory, the data is written to both the cache and main memory immediately, keeping them fully synchronized at all times
-  *WBWA:* When the CPU writes to memory, the data is updated only in the cache first and written back to main memory later, allowing faster performance but requiring cache flushes when coherence is needed.
- *Uncached (-):* Memory accesses bypass the cache entirely, meaning every read and write goes directly to main memory or the device, ensuring correct behavior for peripherals but at the cost of speed.

== STM32F746 Memory Map