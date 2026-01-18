#include "../../../include/sd.h"

extern int sd_init(void);
extern int sd_read_blocks(uint32_t lba, uint32_t count, void *buf);
extern const sd_info_t *sd_get_info(void);
extern int sd_last_error(void);
extern void sd_detect_init(void);
extern int sd_is_detected(void);
extern uint32_t sd_last_cmd(void);
extern uint32_t sd_last_sta(void);
extern void sd_set_init_clkdiv(uint32_t div);
extern uint32_t sd_get_init_clkdiv(void);
extern void sd_set_data_clkdiv(uint32_t div);
extern uint32_t sd_get_data_clkdiv(void);
extern void sd_use_pll48(int use_pll);
extern int sd_get_use_pll48(void);

/* Assembly helpers used by the SDMMC1 driver. */
extern void pc8pc12_to_sdmmc1(void);
extern void pd2_to_sdmmc1(void);
extern void enable_sdmmc1_kerclk_pll48(void);
extern void enable_sdmmc1_kerclk_sysclk(void);
extern void enable_sdmmc1_clock(void);
extern int sdmmc_send_cmd(uint32_t cmd, uint32_t arg, uint32_t waitresp, uint32_t ignore_crc);
