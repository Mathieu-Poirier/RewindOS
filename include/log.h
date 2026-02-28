#pragma once

#include "stdint.h"

/*
 * log - RAM ring buffer for background command output.
 *
 * 512-byte circular buffer.  Oldest bytes are overwritten on overflow.
 * Only called from task dispatchers (main thread) — no locking needed.
 */

int      log_putc(char c);
int      log_puts(const char *s);
int      log_write(const char *s, uint16_t len);
int      log_put_u32(uint32_t v);
int      log_put_hex8(uint8_t v);
int      log_put_hex32(uint32_t v);
uint16_t log_available(void);
uint16_t log_read(char *buf, uint16_t max);
void     log_clear(void);
