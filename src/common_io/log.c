#include "../../include/log.h"

#define LOG_BUF_SIZE 512u

static char     g_log_buf[LOG_BUF_SIZE];
static uint16_t g_log_head;   /* next write position */
static uint16_t g_log_tail;   /* next read position  */
static uint16_t g_log_count;  /* bytes available     */

int log_putc(char c)
{
    if (g_log_count == LOG_BUF_SIZE) {
        /* Overwrite oldest byte */
        g_log_tail = (uint16_t)((g_log_tail + 1u) % LOG_BUF_SIZE);
        g_log_count--;
    }
    g_log_buf[g_log_head] = c;
    g_log_head = (uint16_t)((g_log_head + 1u) % LOG_BUF_SIZE);
    g_log_count++;
    return 0;
}

int log_write(const char *s, uint16_t len)
{
    if (s == 0 || len == 0u) {
        return -1;
    }
    for (uint16_t i = 0u; i < len; i++) {
        log_putc(s[i]);
    }
    return 0;
}

int log_puts(const char *s)
{
    if (s == 0) {
        return -1;
    }
    uint16_t len = 0u;
    while (s[len] != '\0') {
        len++;
    }
    return log_write(s, len);
}

int log_put_u32(uint32_t v)
{
    char buf[10];
    uint8_t n = 0u;
    if (v == 0u) {
        buf[n++] = '0';
    } else {
        while (v > 0u && n < (uint8_t)sizeof(buf)) {
            buf[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        for (uint8_t i = 0u; i < n / 2u; i++) {
            char t = buf[i];
            buf[i] = buf[n - 1u - i];
            buf[n - 1u - i] = t;
        }
    }
    return log_write(buf, (uint16_t)n);
}

int log_put_hex8(uint8_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    char buf[2];
    buf[0] = hx[(v >> 4) & 0xFu];
    buf[1] = hx[v & 0xFu];
    return log_write(buf, 2u);
}

int log_put_hex32(uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    char buf[8];
    for (uint32_t i = 0u; i < 8u; i++) {
        uint32_t sh = (7u - i) * 4u;
        buf[i] = hx[(v >> sh) & 0xFu];
    }
    return log_write(buf, 8u);
}

uint16_t log_available(void)
{
    return g_log_count;
}

uint16_t log_read(char *buf, uint16_t max)
{
    if (buf == 0 || max == 0u) {
        return 0u;
    }
    uint16_t n = (g_log_count < max) ? g_log_count : max;
    for (uint16_t i = 0u; i < n; i++) {
        buf[i] = g_log_buf[g_log_tail];
        g_log_tail = (uint16_t)((g_log_tail + 1u) % LOG_BUF_SIZE);
    }
    g_log_count = (uint16_t)(g_log_count - n);
    return n;
}

void log_clear(void)
{
    g_log_head  = 0u;
    g_log_tail  = 0u;
    g_log_count = 0u;
}
