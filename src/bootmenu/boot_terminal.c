#include "../include/lineio.h"
#include "../include/parse.h"
#include "../include/uart.h"
#include "../include/jump.h"
#include "../include/sd.h"

extern void enable_sdmmc1_kerclk_sysclk(void);
extern void enable_sdmmc1_kerclk_pll48(void);
extern void pc8pc12_to_sdmmc1(void);
extern void pd2_to_sdmmc1(void);
extern void enable_gpioc(void);
extern void enable_gpiod(void);

#define APP_BASE 0x08020000u
#define MAX_ARGUMENTS 8
#define SD_DUMP_BYTES 64u
#define SD_READ_MAX_BLOCKS 4u
#define RCC_BASE 0x40023800u
#define SDMMC1_BASE 0x40012C00u
#define GPIOC_BASE 0x40020800u
#define GPIOD_BASE 0x40020C00u

static uint32_t sd_buf_words[SD_BLOCK_SIZE / 4u];

static void uart_put_s32(int v)
{
        if (v < 0)
        {
                uart_putc('-');
                uart_put_u32((uint32_t)(-v));
                return;
        }
        uart_put_u32((uint32_t)v);
}

static void uart_put_hex8(uint8_t v)
{
        static const char *hx = "0123456789ABCDEF";
        uart_putc(hx[(v >> 4) & 0xF]);
        uart_putc(hx[v & 0xF]);
}

static void sd_dump_bytes(const uint8_t *buf, uint32_t count)
{
        for (uint32_t i = 0; i < count; i++)
        {
                uart_put_hex8(buf[i]);
                if ((i & 0x0Fu) == 0x0Fu)
                        uart_puts("\r\n");
                else
                        uart_putc(' ');
        }

        if ((count & 0x0Fu) != 0u)
                uart_puts("\r\n");
}

static void sd_print_info(void)
{
        const sd_info_t *info = sd_get_info();
        if (!info->initialized)
        {
                uart_puts("sd not initialized\r\n");
                return;
        }

        uart_puts("rca=");
        uart_put_hex32(info->rca);
        uart_puts(" ocr=");
        uart_put_hex32(info->ocr);
        uart_puts("\r\n");

        uart_puts("capacity_blocks=");
        uart_put_u32(info->capacity_blocks);
        uart_puts(" capacity_mb=");
        uart_put_u32(info->capacity_blocks / 2048u);
        uart_puts("\r\n");

        uart_puts("high_capacity=");
        uart_put_u32(info->high_capacity);
        uart_puts(" bus_width=");
        uart_put_u32(info->bus_width);
        uart_puts("\r\n");
}

static void dump_reg(const char *name, uint32_t addr)
{
        uart_puts(name);
        uart_puts("=");
        uart_put_hex32(*(volatile uint32_t *)addr);
        uart_puts("\r\n");
}

static void rcc_dump(void)
{
        dump_reg("RCC_CR", RCC_BASE + 0x00u);
        dump_reg("RCC_PLLCFGR", RCC_BASE + 0x04u);
        dump_reg("RCC_CFGR", RCC_BASE + 0x08u);
        dump_reg("RCC_DCKCFGR1", RCC_BASE + 0x8Cu);
        dump_reg("RCC_DCKCFGR2", RCC_BASE + 0x90u);
        dump_reg("RCC_APB2ENR", RCC_BASE + 0x44u);
}

static void sdmmc_dump(void)
{
        dump_reg("SDMMC_POWER", SDMMC1_BASE + 0x00u);
        dump_reg("SDMMC_CLKCR", SDMMC1_BASE + 0x04u);
        dump_reg("SDMMC_ARG", SDMMC1_BASE + 0x08u);
        dump_reg("SDMMC_CMD", SDMMC1_BASE + 0x0Cu);
        dump_reg("SDMMC_RESP1", SDMMC1_BASE + 0x14u);
        dump_reg("SDMMC_RESP2", SDMMC1_BASE + 0x18u);
        dump_reg("SDMMC_RESP3", SDMMC1_BASE + 0x1Cu);
        dump_reg("SDMMC_RESP4", SDMMC1_BASE + 0x20u);
        dump_reg("SDMMC_DTIMER", SDMMC1_BASE + 0x24u);
        dump_reg("SDMMC_DLEN", SDMMC1_BASE + 0x28u);
        dump_reg("SDMMC_DCTRL", SDMMC1_BASE + 0x2Cu);
        dump_reg("SDMMC_STA", SDMMC1_BASE + 0x34u);
        dump_reg("SDMMC_ICR", SDMMC1_BASE + 0x38u);
        dump_reg("SDMMC_MASK", SDMMC1_BASE + 0x3Cu);
        dump_reg("SDMMC_FIFOCNT", SDMMC1_BASE + 0x48u);
        uart_puts("SD_LAST_CMD=");
        uart_put_hex32(sd_last_cmd());
        uart_puts("\r\n");
        uart_puts("SD_LAST_STA=");
        uart_put_hex32(sd_last_sta());
        uart_puts("\r\n");
}

static void sdmmc_set_clkdiv(uint32_t div)
{
        volatile uint32_t *clkcr = (uint32_t *)(SDMMC1_BASE + 0x04u);
        uint32_t val = *clkcr;
        val &= ~0xFFu;
        val |= (div & 0xFFu);
        val |= (1u << 8);
        *clkcr = val;
}

static void sdmmc_set_kernel_clk_sys(void)
{
        enable_sdmmc1_kerclk_sysclk();
}

static void sdmmc_set_kernel_clk_pll(void)
{
        enable_sdmmc1_kerclk_pll48();
}

static void gpio_dump_port(const char *name, uint32_t base)
{
        uart_puts(name);
        uart_puts("\r\n");
        dump_reg("MODER", base + 0x00u);
        dump_reg("OTYPER", base + 0x04u);
        dump_reg("OSPEEDR", base + 0x08u);
        dump_reg("PUPDR", base + 0x0Cu);
        dump_reg("IDR", base + 0x10u);
        dump_reg("AFRL", base + 0x20u);
        dump_reg("AFRH", base + 0x24u);
}

static void sd_lines_dump(void)
{
        uint32_t idrc = *(volatile uint32_t *)(GPIOC_BASE + 0x10u);
        uint32_t idrd = *(volatile uint32_t *)(GPIOD_BASE + 0x10u);

        uart_puts("SD_LINES D0=");
        uart_put_u32((idrc >> 8) & 1u);
        uart_puts(" D1=");
        uart_put_u32((idrc >> 9) & 1u);
        uart_puts(" D2=");
        uart_put_u32((idrc >> 10) & 1u);
        uart_puts(" D3=");
        uart_put_u32((idrc >> 11) & 1u);
        uart_puts(" CK=");
        uart_put_u32((idrc >> 12) & 1u);
        uart_puts(" CMD=");
        uart_put_u32((idrd >> 2) & 1u);
        uart_puts("\r\n");
}

static void gpio_dump(void)
{
        gpio_dump_port("GPIOC", GPIOC_BASE);
        gpio_dump_port("GPIOD", GPIOD_BASE);
        sd_lines_dump();
}

static void sd_toggle_lines(uint32_t cycles)
{
        volatile uint32_t *gpioc_moder = (uint32_t *)(GPIOC_BASE + 0x00u);
        volatile uint32_t *gpioc_otyper = (uint32_t *)(GPIOC_BASE + 0x04u);
        volatile uint32_t *gpioc_ospeedr = (uint32_t *)(GPIOC_BASE + 0x08u);
        volatile uint32_t *gpioc_pupdr = (uint32_t *)(GPIOC_BASE + 0x0Cu);
        volatile uint32_t *gpioc_odr = (uint32_t *)(GPIOC_BASE + 0x14u);
        volatile uint32_t *gpiod_moder = (uint32_t *)(GPIOD_BASE + 0x00u);
        volatile uint32_t *gpiod_otyper = (uint32_t *)(GPIOD_BASE + 0x04u);
        volatile uint32_t *gpiod_ospeedr = (uint32_t *)(GPIOD_BASE + 0x08u);
        volatile uint32_t *gpiod_pupdr = (uint32_t *)(GPIOD_BASE + 0x0Cu);
        volatile uint32_t *gpiod_odr = (uint32_t *)(GPIOD_BASE + 0x14u);

        enable_gpioc();
        enable_gpiod();

        uint32_t moder = *gpioc_moder;
        moder &= ~0x03FF0000u;
        moder |= 0x01550000u;
        *gpioc_moder = moder;
        *gpioc_otyper &= ~0x00001F00u;
        uint32_t ospeed = *gpioc_ospeedr;
        ospeed &= ~0x03FF0000u;
        ospeed |= 0x03FF0000u;
        *gpioc_ospeedr = ospeed;
        *gpioc_pupdr &= ~0x03FF0000u;

        uint32_t d_moder = *gpiod_moder;
        d_moder &= ~0x00000030u;
        d_moder |= 0x00000010u;
        *gpiod_moder = d_moder;
        *gpiod_otyper &= ~0x00000004u;
        uint32_t d_speed = *gpiod_ospeedr;
        d_speed &= ~0x00000030u;
        d_speed |= 0x00000030u;
        *gpiod_ospeedr = d_speed;
        *gpiod_pupdr &= ~0x00000030u;

        uint32_t mask_c = (0x1Fu << 8);
        uint32_t mask_d = (1u << 2);
        for (uint32_t i = 0; i < cycles; i++)
        {
                *gpioc_odr ^= mask_c;
                *gpiod_odr ^= mask_d;
                for (volatile uint32_t d = 0; d < 20000u; d++)
                {
                }
        }

        pc8pc12_to_sdmmc1();
        pd2_to_sdmmc1();
}

static void boot_dispatch(char *line)
{
        char *argv[MAX_ARGUMENTS];
        int argc = tokenize(line, argv, MAX_ARGUMENTS);

        if (argc == 0)
                return;

        if (streq(argv[0], "help"))
        {
                uart_puts("\r\n");
                uart_puts("  Boot\r\n");
                uart_puts("    bootfast          Jump to main firmware\r\n");
                uart_puts("\r\n");
                uart_puts("  SD Card\r\n");
                uart_puts("    sdinit            Initialize SD card\r\n");
                uart_puts("    sdinfo            Show card info\r\n");
                uart_puts("    sdtest            Init + read test\r\n");
                uart_puts("    sdread <lba> [n]  Read blocks (n<=4)\r\n");
                uart_puts("    sddetect          Check card presence\r\n");
                uart_puts("\r\n");
                uart_puts("  SD Debug\r\n");
                uart_puts("    sdclk <sys|pll>   Set kernel clock source\r\n");
                uart_puts("    sdclkdiv <0-255>  Set clock divider\r\n");
                uart_puts("    sdstat            Dump SDMMC registers\r\n");
                uart_puts("    sdlast            Show last cmd/error\r\n");
                uart_puts("    sdtoggle <n>      Toggle SD lines\r\n");
                uart_puts("\r\n");
                uart_puts("  Debug\r\n");
                uart_puts("    gpiodump          Dump GPIO registers\r\n");
                uart_puts("    rccdump           Dump RCC registers\r\n");
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "bootfast"))
        {
                uart_puts("booting...\r\n");
                uart_flush_tx();
                jump_to_image(APP_BASE);
                for (;;)
                {
                }
        }

        if (streq(argv[0], "sdinit"))
        {
                sd_use_pll48(0);
                sd_set_data_clkdiv(SD_CLKDIV_BOOT);
                int rc = sd_init();
                if (rc == SD_OK)
                {
                        uart_puts("sdinit: ok\r\n");
                        return;
                }
                uart_puts("sdinit: err=");
                uart_put_s32(rc);
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "sdinfo"))
        {
                sd_print_info();
                return;
        }

        if (streq(argv[0], "sddetect"))
        {
                sd_detect_init();
                if (sd_is_detected())
                {
                        uart_puts("sd detect: present\r\n");
                }
                else
                {
                        uart_puts("sd detect: not present\r\n");
                }
                return;
        }

        if (streq(argv[0], "gpiodump"))
        {
                gpio_dump();
                return;
        }

        if (streq(argv[0], "sdclk"))
        {
                if (argc < 2)
                {
                        uart_puts("usage: sdclk <sys|pll>\r\n");
                        return;
                }
                if (streq(argv[1], "sys"))
                {
                        sdmmc_set_kernel_clk_sys();
                        uart_puts("sdclk: sys\r\n");
                        return;
                }
                if (streq(argv[1], "pll"))
                {
                        sdmmc_set_kernel_clk_pll();
                        uart_puts("sdclk: pll\r\n");
                        return;
                }
                uart_puts("sdclk: unknown\r\n");
                return;
        }

        if (streq(argv[0], "sdclkdiv"))
        {
                if (argc < 2)
                {
                        uart_puts("usage: sdclkdiv <0-255>\r\n");
                        return;
                }
                uint32_t div = 0;
                if (!parse_u32(argv[1], &div))
                {
                        uart_puts("sdclkdiv: bad value\r\n");
                        return;
                }
                if (div > 255u)
                        div = 255u;
                sdmmc_set_clkdiv(div);
                sd_set_init_clkdiv(div);
                uart_puts("sdclkdiv: ");
                uart_put_u32(div);
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "sdtoggle"))
        {
                uint32_t cycles = 8;
                if (argc >= 2)
                {
                        if (!parse_u32(argv[1], &cycles))
                        {
                                uart_puts("sdtoggle: bad value\r\n");
                                return;
                        }
                        if (cycles == 0)
                                cycles = 1;
                        if (cycles > 100)
                                cycles = 100;
                }
                sd_toggle_lines(cycles);
                uart_puts("sdtoggle: done\r\n");
                return;
        }

        if (streq(argv[0], "rccdump"))
        {
                rcc_dump();
                return;
        }

        if (streq(argv[0], "sdstat"))
        {
                sdmmc_dump();
                return;
        }

        if (streq(argv[0], "sdlast"))
        {
                uart_puts("sd_last_error=");
                uart_put_s32(sd_last_error());
                uart_puts(" sd_last_cmd=");
                uart_put_hex32(sd_last_cmd());
                uart_puts(" sd_last_sta=");
                uart_put_hex32(sd_last_sta());
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "sdtest"))
        {
                uart_puts("sdtest: initializing...\r\n");
                sd_use_pll48(0);
                sd_set_data_clkdiv(SD_CLKDIV_BOOT);
                int rc = sd_init();
                if (rc != SD_OK)
                {
                        uart_puts("sdtest: init failed err=");
                        uart_put_s32(rc);
                        uart_puts(" cmd=");
                        uart_put_hex32(sd_last_cmd());
                        uart_puts(" sta=");
                        uart_put_hex32(sd_last_sta());
                        uart_puts("\r\n");
                        return;
                }

                const sd_info_t *info = sd_get_info();
                uart_puts("sdtest: init ok, capacity=");
                uart_put_u32(info->capacity_blocks / 2048u);
                uart_puts("MB, hc=");
                uart_put_u32(info->high_capacity);
                uart_puts(", bus=");
                uart_put_u32(info->bus_width);
                uart_puts("bit\r\n");

                uart_puts("sdtest: reading block 0...\r\n");
                rc = sd_read_blocks(0, 1, sd_buf_words);
                if (rc != SD_OK)
                {
                        uart_puts("sdtest: read failed err=");
                        uart_put_s32(rc);
                        uart_puts("\r\n");
                        return;
                }

                const uint8_t *buf = (const uint8_t *)sd_buf_words;
                uint8_t sig0 = buf[510];
                uint8_t sig1 = buf[511];
                uart_puts("sdtest: block 0 signature=");
                uart_put_hex8(sig0);
                uart_put_hex8(sig1);
                if (sig0 == 0x55 && sig1 == 0xAA)
                        uart_puts(" (valid MBR)\r\n");
                else
                        uart_puts(" (no MBR)\r\n");

                uart_puts("sdtest: first 64 bytes:\r\n");
                sd_dump_bytes(buf, 64);
                uart_puts("sdtest: PASS\r\n");
                return;
        }

        if (streq(argv[0], "sdread"))
        {
                if (argc < 2)
                {
                        uart_puts("usage: sdread <lba> [count]\r\n");
                        return;
                }

                uint32_t lba = 0;
                uint32_t count = 1;
                if (!parse_u32(argv[1], &lba))
                {
                        uart_puts("sdread: bad lba\r\n");
                        return;
                }
                if (argc >= 3 && !parse_u32(argv[2], &count))
                {
                        uart_puts("sdread: bad count\r\n");
                        return;
                }
                if (count == 0)
                        count = 1;
                if (count > SD_READ_MAX_BLOCKS)
                        count = SD_READ_MAX_BLOCKS;

                for (uint32_t i = 0; i < count; i++)
                {
                        int rc = sd_read_blocks(lba + i, 1, sd_buf_words);
                        if (rc != SD_OK)
                        {
                                uart_puts("sdread: err=");
                                uart_put_s32(rc);
                                uart_puts("\r\n");
                                return;
                        }

                        uart_puts("lba ");
                        uart_put_u32(lba + i);
                        uart_puts(":\r\n");
                        sd_dump_bytes((const uint8_t *)sd_buf_words, SD_DUMP_BYTES);
                }
                return;
        }

        uart_puts("unknown cmd: ");
        uart_puts(argv[0]);
        uart_puts("\r\n");
}

void boot_main(void)
{
        uart_puts("\r\n");
        uart_puts("RewindOS Bootloader\r\n");
        uart_puts("\r\n");

        sd_detect_init();
        if (sd_is_detected())
        {
                uart_puts("SD card detected, initializing...\r\n");
                sd_use_pll48(0);
                sd_set_data_clkdiv(SD_CLKDIV_BOOT);
                int rc = sd_init();
                if (rc == SD_OK)
                {
                        const sd_info_t *info = sd_get_info();
                        uart_puts("SD card: ");
                        uart_put_u32(info->capacity_blocks / 2048u);
                        uart_puts("MB\r\n");
                }
                else
                {
                        uart_puts("SD init failed: err=");
                        uart_put_s32(rc);
                        uart_puts("\r\n");
                }
        }
        else
        {
                uart_puts("WARNING: No SD card detected\r\n");
        }

        uart_puts("\r\n");
        shell_loop("boot> ", boot_dispatch);
}
