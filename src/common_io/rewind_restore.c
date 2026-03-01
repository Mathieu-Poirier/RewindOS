#include "../../include/rewind_restore.h"
#include "../../include/journal.h"
#include "../../include/panic.h"

#define REWIND_RESTORE_STACK_BYTES 2048u

__attribute__((section(".snap_exclude"), aligned(8)))
static uint8_t g_rewind_restore_stack[REWIND_RESTORE_STACK_BYTES];

static volatile rewind_handoff_t *rewind_handoff_ptr(void)
{
    return (volatile rewind_handoff_t *)(uintptr_t)REWIND_HANDOFF_ADDR;
}

void rewind_handoff_clear(void)
{
    PANIC_IF((sizeof(rewind_handoff_t) % sizeof(uint32_t)) != 0u, "handoff size not word aligned");
    volatile uint32_t *w = (volatile uint32_t *)rewind_handoff_ptr();
    for (uint32_t i = 0u; i < (uint32_t)(sizeof(rewind_handoff_t) / sizeof(uint32_t)); i++) {
        w[i] = 0u;
    }
}

void rewind_handoff_mark_resume(uint32_t slot, uint32_t seq, uint32_t journal_start_lba, uint32_t journal_blocks)
{
    volatile rewind_handoff_t *h = rewind_handoff_ptr();
    rewind_handoff_clear();
    h->magic = REWIND_HANDOFF_MAGIC;
    h->version = REWIND_HANDOFF_VERSION;
    h->flags = REWIND_HANDOFF_FLAG_RESUME;
    h->slot = slot;
    h->seq = seq;
    h->journal_start_lba = journal_start_lba;
    h->journal_blocks = journal_blocks;
}

int rewind_handoff_resume_requested(void)
{
    volatile rewind_handoff_t *h = rewind_handoff_ptr();
    if (h->magic != REWIND_HANDOFF_MAGIC) {
        return 0;
    }
    if (h->version != REWIND_HANDOFF_VERSION) {
        return 0;
    }
    return (h->flags & REWIND_HANDOFF_FLAG_RESUME) != 0u;
}

void rewind_handoff_read(rewind_handoff_t *out)
{
    if (out == 0) {
        return;
    }
    volatile rewind_handoff_t *h = rewind_handoff_ptr();
    const volatile uint32_t *src = (const volatile uint32_t *)h;
    uint32_t *dst = (uint32_t *)out;
    for (uint32_t i = 0u; i < (uint32_t)(sizeof(rewind_handoff_t) / sizeof(uint32_t)); i++) {
        dst[i] = src[i];
    }
}

uint32_t rewind_restore_stack_top(void)
{
    uintptr_t top = (uintptr_t)&g_rewind_restore_stack[REWIND_RESTORE_STACK_BYTES];
    top &= ~(uintptr_t)7u; /* ARM EABI stack alignment */
    return (uint32_t)top;
}

void rewind_journal_replay(uint32_t snapshot_seq, uint32_t journal_start_lba, uint32_t journal_blocks)
{
    journal_replay_from_snapshot(snapshot_seq, journal_start_lba, journal_blocks);
}

__attribute__((weak)) void rewind_resume_entry(void)
{
    extern int main(void);
    (void)main();
    for (;;) {
    }
}
