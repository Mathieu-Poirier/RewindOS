#include "../../include/stddef.h"
#include "../../include/bump.h"

extern unsigned char _end;      /* heap start (after .bss etc.) */
extern unsigned char _sram_end; /* one-past-last byte of SRAM */

static unsigned char* brk = &_end;

static inline size_t align_up(size_t n, size_t a)
{
        return (n + (a - 1)) & ~(a - 1);
}

void bump_init(void)
{
        brk = &_end;
}

void* bump_alloc(size_t n)
{
        const size_t ALIGN = 8; /* 8-byte alignment is a good default */
        n = align_up(n, ALIGN);

        unsigned char* p = brk;

        if (brk + n > &_sram_end)
        {
                return NULL; 
        }

        brk += n;
        return (void *)p;
}
