# Contributing to RewindOS

## Architecture overview

RewindOS uses the **Active Object (AO)** pattern. Every concurrent unit of work
is an AO — a task with its own event queue and a single dispatch function. There
are no threads and no mutexes. The scheduler runs in `main()` and calls each
task's dispatch function one event at a time.

```
IRQ handlers         sched_post_isr()
Application code     sched_post()
                          │
                     event_queue_t   ← per-task FIFO
                          │
                    ao_dispatch_fn   ← your task function
                          │
                     console_put*()  ← serialised UART output
```

All output goes through `AO_CONSOLE` (the console task) so it is serialised and
non-blocking. Never call `uart_puts` / `uart_putc` directly from a task; use the
`console_put*` API instead. The two exceptions are `reboot` and `shutdown`, which
intentionally tear down the scheduler before sending their final message.

---

## Adding a new task: step by step

### 1. Assign a task ID — `include/task_ids.h`

Append a new entry to the enum. IDs index into `scheduler_t.table[]`; keep them
contiguous starting from 0.

```c
enum {
    AO_TERMINAL = 0,
    AO_SD       = 1,
    AO_CMD      = 2,
    AO_CONSOLE  = 3,
    AO_FOO      = 4,   /* ← new task */
};
```

The scheduler supports up to `SCHED_MAX_AO` (currently 32) tasks.

---

### 2. Define signals — `include/task_signals.h`

Every event has a `sig` field. Reserve a unique non-zero integer for each
signal your task sends or receives. Never reuse or overlap signal values across
tasks — a misrouted event causes silent misbehaviour.

```c
enum {
    /* ... existing signals ... */
    FOO_SIG_START  = 9,    /* ← posted to AO_FOO to start work  */
    FOO_SIG_DONE   = 10,   /* ← posted from AO_FOO when finished */
};
```

Signal 0 is reserved (uninitialised). Start the next task's signals immediately
after the last used value.

---

### 3. Write the header — `include/foo_task.h`

Expose only what other modules need: the register function and any public
request helpers.

```c
#pragma once

#include "scheduler.h"
#include "stdint.h"

int foo_task_register(scheduler_t *sched);

/* Post FOO_SIG_START to AO_FOO */
int foo_task_request_start(uint32_t param);
```

---

### 4. Implement the task — `src/scheduler/foo_task.c`

The Makefile picks up `src/scheduler/*.c` via wildcard for both images.
If your task should only exist in the main image (e.g. it depends on the
terminal or shutdown), put it in `src/terminal/` instead.

```c
#include "../../include/foo_task.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/task_spec.h"
#include "../../include/console.h"
#include "../../include/panic.h"

/* ---- context ------------------------------------------------------------- */

typedef struct {
    uint32_t completed;
    uint32_t param;
} foo_ctx_t;

static foo_ctx_t   g_foo_ctx;
static event_t     g_foo_queue[8];
static scheduler_t *g_foo_sched;

/* ---- dispatch ------------------------------------------------------------ */

static void foo_task_dispatch(ao_t *self, const event_t *e)
{
    (void)self;
    PANIC_IF(e == 0, "foo dispatch: null event");

    if (e->sig == FOO_SIG_START) {
        /* do work, output goes through console */
        console_puts("foo: started param=");
        console_put_u32(g_foo_ctx.param);
        console_puts("\r\n");
        g_foo_ctx.completed++;
        return;
    }
}

/* ---- registration -------------------------------------------------------- */

int foo_task_register(scheduler_t *sched)
{
    if (sched == 0) {
        return SCHED_ERR_PARAM;
    }

    g_foo_sched = sched;
    g_foo_ctx.completed = 0u;

    task_spec_t spec;
    spec.id             = AO_FOO;
    spec.prio           = 1;           /* see priority guide below */
    spec.dispatch       = foo_task_dispatch;
    spec.ctx            = &g_foo_ctx;
    spec.queue_storage  = g_foo_queue;
    spec.queue_capacity = (uint16_t)(sizeof(g_foo_queue) / sizeof(g_foo_queue[0]));
    spec.rtc_budget_ticks = 1;
    spec.name           = "foo";

    return sched_register_task(sched, &spec);
}

/* ---- public API ---------------------------------------------------------- */

int foo_task_request_start(uint32_t param)
{
    if (g_foo_sched == 0) {
        return SCHED_ERR_PARAM;
    }
    g_foo_ctx.param = param;
    return sched_post(g_foo_sched, AO_FOO,
                      &(event_t){ .sig = FOO_SIG_START, .arg0 = param });
}
```

**Rules for the dispatch function:**
- Always `PANIC_IF(e == 0, ...)` as the first line.
- Never block (no busy-wait loops, no `uart_flush_tx`).
- Never call `sched_run` or modify the scheduler table.
- All output via `console_put*()` — it is non-blocking and serialised.
- Silently ignore unknown signals (just `return`).

---

### 5. Register in `main()` — `src/terminal/main.c`

`AO_CONSOLE` must always be registered first because `terminal_task_register`
prints the prompt via the console during init. Register your task before
`sched_run`.

```c
#include "../../include/foo_task.h"

/* inside main(), after the other registrations: */
if (foo_task_register(&sched) != SCHED_OK)
{
    PANIC("foo task init failed");
}
```

---

### 6. Add a terminal command — `src/terminal/terminal.c`

Add an entry in `term_execute` following the existing pattern, and update the
`help` command to list it.

In `term_execute`:
```c
if (streq(argv[0], "foo"))
{
    uint32_t param = 0;
    if (argc >= 2 && !parse_u32(argv[1], &param))
    {
        console_puts("foo: bad param\r\n");
        return;
    }
    int rc = foo_task_request_start(param);
    if (rc != SCHED_OK)
    {
        console_puts("foo: queue err=");
        /* use the local uart_put_s32 helper */
        uart_put_s32(rc);
        console_puts("\r\n");
    }
    return;
}
```

In the `help` block:
```c
console_puts("    foo [n]           Run foo with optional param\r\n");
```

---

### 7. Build and verify

```sh
make
```

Both the boot and main ELFs are rebuilt. Check for warnings — the flags include
`-Wall -Wextra`. Then flash and test via serial:

```sh
make flash
make connect    # opens picocom at 115200
```

---

## Priority guide

Higher numeric priority runs first. Pick from this table:

| Priority | Current users | Use for |
|----------|--------------|---------|
| 3 | `AO_CONSOLE` | Output tasks that must drain before anything else runs |
| 2 | `AO_SD` | Hardware I/O tasks (DMA completion, peripheral drivers) |
| 1 | `AO_TERMINAL` | User-facing tasks that process input |
| 0 | `AO_CMD` | Command execution — runs after all higher-priority work is done |

Most new application tasks should start at priority **1** and be adjusted if
they interact with hardware at interrupt level.

---

## Event fields

```c
typedef struct {
    uint16_t  sig;   /* signal ID — identifies the event type         */
    uint16_t  src;   /* optional: sender's AO ID (not enforced)       */
    uintptr_t arg0;  /* first payload word — cast to whatever you need */
    uintptr_t arg1;  /* second payload word                            */
    uint32_t  tick;  /* optional: systick timestamp at post time       */
} event_t;
```

Use compound-literal syntax when posting:

```c
sched_post(sched, AO_FOO, &(event_t){ .sig = FOO_SIG_START, .arg0 = lba });
```

`sched_post` is for use from task dispatch (main thread).
`sched_post_isr` is for use from IRQ handlers — no locking, no `event_t` copy.

---

## Queue sizing

Start with 8 events. Increase if `ps` shows non-zero DROPPED for your task
under load. Queue storage is static so it comes out of `.bss`; 8 × 20 bytes =
160 bytes per task.

---

## File placement

| Location | Compiled into | Use for |
|----------|--------------|---------|
| `src/scheduler/` | boot + main | Tasks that may be useful in both images |
| `src/terminal/` | main only | Tasks that depend on the terminal, console, or shutdown |
| `src/drivers/*/` | boot + main | Hardware peripheral drivers |
| `src/kernel/` | boot + main | Core OS services (panic, systick) |

The Makefile uses `$(wildcard ...)` in each directory — dropping a `.c` file in
the right folder is all that is needed for it to be compiled.

---

## Checklist

- [ ] New AO ID added to `include/task_ids.h`
- [ ] New signals added to `include/task_signals.h` (no value reuse)
- [ ] Header in `include/` with `_task_register` and public request API
- [ ] Implementation in `src/scheduler/` or `src/terminal/`
- [ ] `PANIC_IF(e == 0, ...)` is first line of every dispatch function
- [ ] All output via `console_put*()`, never direct `uart_*`
- [ ] Registered in `src/terminal/main.c` with `PANIC` on failure
- [ ] Terminal command added to `term_execute` in `src/terminal/terminal.c`
- [ ] `help` text updated
- [ ] `make` produces no new warnings
- [ ] `ps` output shows the new task name
