#pragma once

#include "stdint.h"

void kernel_panic(const char *msg, const char *file, uint32_t line);

#define PANIC(msg) kernel_panic((msg), __FILE__, (uint32_t)__LINE__)
#define PANIC_IF(cond, msg)         \
    do {                            \
        if (cond) {                 \
            PANIC(msg);             \
        }                           \
    } while (0)
