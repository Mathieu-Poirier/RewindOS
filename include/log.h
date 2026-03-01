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
int      log_put_s32(int32_t v);
int      log_put_hex8(uint8_t v);
int      log_put_hex32(uint32_t v);
uint16_t log_available(void);
uint16_t log_read(char *buf, uint16_t max);
void     log_clear(void);

/* Emit: "<tag>: err=<rc> @<file>:<line>\r\n" into the log ring. */
void     log_err(const char *tag, int32_t rc, const char *file, uint32_t line);

/* Convenience macro — captures __FILE__ / __LINE__ automatically. */
#define LOG_ERR(tag, rc) log_err((tag), (int32_t)(rc), __FILE__, (uint32_t)__LINE__)
