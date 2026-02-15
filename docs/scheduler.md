# Cooperative Scheduler (Kernel-Owned)

## Goal

Implement a cooperative, event-driven scheduler in the kernel where:

- The **kernel scheduler** owns dispatch policy, ready tracking, and idle handling.
- The **terminal** is only one active object (AO), not the scheduler host.
- Other background services (SD, storage, snapshot, maintenance) are scheduled the same way.

This follows a run-to-completion (RTC) model: each AO handles one event and returns to the kernel scheduler.

## Why This Model

- No CPU register context switching is required.
- Works well with IRQ-driven event discovery.
- Better response than a superloop when long work is split into short RTC steps.
- Clear separation of concerns:
  - IRQs discover work and post events.
  - Kernel schedules.
  - AOs process domain logic.

## Ownership and Layering

- `kernel`: scheduler core, event queues, ready bitmap, idle hook.
- `drivers`: produce events from IRQ handlers.
- `services/apps`: implement AO dispatch/state logic (terminal, SD workflow, snapshot manager, etc.).

Terminal must not run a private `for (;;)` scheduler loop. It should register with kernel scheduler and receive events.

## Terminology

- **Event**: message with signal + optional payload.
- **AO (Active Object)**: state machine + private queue + priority.
- **Task Descriptor**: function pointer plus metadata describing one schedulable work item.
- **RTC step**: single dispatch call for one event, must return.
- **Reminder pattern**: AO posts event to itself to continue long work in slices.

## Data Structures

### Event

```c
typedef struct {
    uint16_t sig;        // signal ID
    uint16_t src;        // source ID (optional)
    uintptr_t arg0;      // optional payload or pointer
    uintptr_t arg1;      // optional payload
    uint32_t tick;       // timestamp (optional)
} event_t;
```

### Task Descriptor (Function + Metadata)

A task-oriented API, model each queued work item as a descriptor that carries:

- which function to run
- what context/data it runs with
- metadata for priority/flags/debugging

```c
struct ao;
typedef void (*ao_dispatch_fn)(struct ao *self, const event_t *e);

typedef struct {
    uint16_t task_id;
    uint8_t  prio;
    uint8_t  flags;
    ao_dispatch_fn run;
    void *ctx;
    const char *name;   // optional, for debug/trace
} task_desc_t;
```

In AO mode, this descriptor is usually stored in the AO control block. In a generic task queue, descriptors can be queued directly.

### Queue (per AO)

Ring buffer with single consumer (scheduler) and producer from thread/IRQ context.

```c
typedef struct {
    event_t *buf;
    uint16_t cap;
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile uint16_t count;
    volatile uint16_t high_watermark;
    volatile uint32_t dropped;
} event_queue_t;
```

### AO Control Block (TCB Equivalent)

In this architecture, the "TCB" is an AO control block, not a CPU context block.

```c
struct ao;
typedef void (*ao_dispatch_fn)(struct ao *self, const event_t *e);

typedef struct ao {
    uint8_t id;
    uint8_t prio;              // 0 = lowest, higher = more urgent
    event_queue_t q;
    ao_dispatch_fn dispatch;   // equivalent to task_desc.run in AO form
    void *state;               // state-machine state pointer/context
    volatile uint32_t flags;
    volatile uint32_t rtc_max_ticks;
    volatile uint32_t events_handled;
} ao_t;
```

### Scheduler Core

```c
typedef struct {
    ao_t *table[32];
    volatile uint32_t ready_bitmap; // bit(prio)=queue non-empty
    void (*idle_hook)(void);        // default: __WFI
} scheduler_t;
```

## Scheduling Policy

1. Find highest-priority non-empty AO queue.
2. Pop one event.
3. Dispatch event by calling the registered task/dispatch function.
4. AO executes one RTC step and returns.
5. Repeat.
6. If no AO ready, execute idle hook (`WFI`).

Priority is strict and fixed per AO.
Tie-break inside one priority level is round-robin across ready AOs.

Equivalent task view: enqueue task + metadata, scheduler invokes `task_desc.run(task_desc.ctx, &event)`.

### Same-Priority Rule (Round-Robin)

When multiple AOs at the same priority are ready:

1. Scheduler starts from the AO after the last one served at that priority.
2. First ready AO found is dispatched.
3. Cursor advances so the next dispatch at that priority serves the next AO.

This avoids starvation between peers while keeping strict preemption order between different priorities.

## Event Sources

- `USART6_IRQHandler`: post RX/TX related events to terminal or UART service AO.
- `SDMMC1_IRQHandler`: post completion/error/progress events to SD service AO.
- `SysTick_Handler`: optional periodic tick events (timer AO or direct timer wheel).

ISR rule: keep ISR short, do minimal data capture, post event, exit.

## AO Candidates in RewindOS

- `AO_TERMINAL`: command parser and shell interaction.
- `AO_SD`: async SD transaction workflow/state.
- `AO_CMD`: command execution worker (runs parsed shell commands as queued task work).
- `AO_SNAPSHOT`: snapshot scheduling/persistence work.
- `AO_SHUTDOWN`: staged shutdown/reboot handling.
- `AO_HEALTH` (optional): watchdog/telemetry/counters.

## Concrete Header Draft

Canonical API now lives in:

- `include/event.h`
- `include/scheduler.h`
- `include/task_spec.h`

### Scheduler API

```c
void sched_init(scheduler_t *s, void (*idle_hook)(void));
int  sched_register(scheduler_t *s, ao_t *ao);
int  sched_register_task(scheduler_t *s, const task_spec_t *spec);
int  sched_unregister(scheduler_t *s, uint8_t ao_id);
int  sched_post(scheduler_t *s, uint8_t ao_id, const event_t *e);      // thread context
int  sched_post_isr(scheduler_t *s, uint8_t ao_id, const event_t *e);  // ISR-safe
int  sched_pause_accept(scheduler_t *s, uint8_t ao_id);
int  sched_resume_accept(scheduler_t *s, uint8_t ao_id);
void sched_run(scheduler_t *s);                                         // never returns
```

### Event Queue API

```c
void eq_init(event_queue_t *q, event_t *storage, uint16_t capacity);
int eq_push(event_queue_t *q, const event_t *e);
int eq_push_isr(event_queue_t *q, const event_t *e);
int eq_pop(event_queue_t *q, event_t *out);
int eq_is_empty(const event_queue_t *q);
void eq_drain(event_queue_t *q);
```

### Return Codes

```c
/* scheduler.h */
SCHED_OK, SCHED_ERR_PARAM, SCHED_ERR_FULL, SCHED_ERR_EXISTS,
SCHED_ERR_NOT_FOUND, SCHED_ERR_QUEUE_FULL, SCHED_ERR_DISABLED

/* event.h */
EQ_OK, EQ_ERR_PARAM, EQ_ERR_FULL, EQ_ERR_EMPTY
```

## Concurrency Rules

- Scheduler is the only consumer of AO queues.
- Producers can be IRQ or cooperative code.
- Critical sections around queue index updates must be bounded and minimal.
- Never block inside AO dispatch.

## Runtime Panic Policy

Use `PANIC(...)` for unrecoverable kernel invariants, not for normal runtime errors.

- `PANIC` means the kernel can no longer guarantee correct scheduling behavior.
- `PANIC_IF(cond, msg)` is the conditional form.
- Panic path should:
  - disable interrupts
  - emit diagnostic text
  - halt in a debug-friendly loop (`bkpt`/`wfi`)

Guideline:

- Driver/IO failures (CRC timeout, device not present, queue full) should become events/errors, not panic.
- Structural corruption (invalid scheduler state, null dispatch for ready task, impossible invariants) can panic.

## Task Dispatch Contract

Each AO/task handler must follow this RTC contract:

1. Receive exactly one event (`dispatch(self, &event)`).
2. Handle a bounded chunk of work for that event.
3. Return control to scheduler (plain `return`, no private infinite loop).

Additional rules:

- Use event `sig` to select behavior (`switch`/state-machine transition).
- If work is not finished, post continuation event to self and return.
- Do not busy-wait for hardware completion inside dispatch.
- Keep worst-case RTC step short and measurable.

## Everything-As-Task Policy

For observability and uniform control flow, execute work through registered tasks whenever practical:

- Input task (`AO_TERMINAL`) captures lines/events.
- Command task (`AO_CMD`) executes parsed commands from a queued mailbox event.
- Device/service tasks (`AO_SD`, snapshot, shutdown, etc.) handle driver/service workflows.

This keeps scheduler accounting (`events_handled`, queue depth, drops, max RTC) meaningful across both previously async and previously sync paths.

## Task Template + SD Conversion Playbook

### A) Minimum Task Interface (to prevent drift)

Every new kernel task should provide this minimum spec before registration:

```c
typedef struct {
    uint8_t id;
    uint8_t prio;
    ao_dispatch_fn dispatch;   // required
    void *ctx;                 // required
    event_t *queue_storage;    // required
    uint16_t queue_capacity;   // required (>0)
    uint32_t rtc_budget_ticks; // required: expected max RTC budget
    const char *name;          // optional but recommended
} task_spec_t;
```

Registration gate idea:

```c
int sched_register_task(scheduler_t *s, const task_spec_t *spec);
```

Validation in `sched_register_task`:

- reject null `dispatch` or null `ctx`
- reject zero queue capacity or null queue storage
- reject invalid/duplicate task ID
- reject out-of-range priority

This keeps task implementation consistent and prevents partial task definitions.

### B) Task Development Checklist

1. Define event signals (`EV_*`) for the task.
2. Define task context struct (all persistent state in one place).
3. Implement `dispatch` as `switch (e->sig)` RTC handlers.
4. Ensure each case does bounded work and returns.
5. Use self-post for continuation (`EV_CONT`) when CPU work is chunked.
6. Use ISR-post for hardware completion/failure events.
7. Add counters: handled events, dropped events, max RTC ticks.
8. Register task using the minimum task spec.

### C) SD Driver Conversion Pattern (Chunked RTC)

#### Signals

```c
enum {
    EV_SD_READ_REQ = 1,
    EV_SD_CMD_SENT,
    EV_SD_FIFO_CHUNK,
    EV_SD_BLOCK_DONE,
    EV_SD_DONE,
    EV_SD_ERR,
    EV_SD_CONT
};
```

#### Context

```c
typedef struct {
    uint32_t lba;
    uint32_t total_blocks;
    uint32_t blocks_done;
    uint8_t *buf;
    uint8_t *ptr;
    uint32_t fifo_words_left;
    int busy;
    int last_error;
} sd_task_ctx_t;
```

#### Dispatch skeleton

```c
static void sd_dispatch(ao_t *self, const event_t *e)
{
    sd_task_ctx_t *ctx = (sd_task_ctx_t *)self->state;

    switch (e->sig) {
    case EV_SD_READ_REQ:
        /* validate request, init ctx, issue first HW command */
        ctx->busy = 1;
        sd_hw_start_block(ctx->lba);
        return;

    case EV_SD_FIFO_CHUNK:
        /* move bounded FIFO chunk only, then return */
        sd_copy_fifo_chunk(ctx);
        if (ctx->fifo_words_left > 0) {
            sched_post(&g_sched, self->id, &(event_t){ .sig = EV_SD_CONT });
        }
        return;

    case EV_SD_CONT:
        /* continuation of CPU-side chunking */
        sd_continue_chunk(ctx);
        return;

    case EV_SD_BLOCK_DONE:
        ctx->blocks_done++;
        if (ctx->blocks_done < ctx->total_blocks) {
            sd_hw_start_block(ctx->lba + ctx->blocks_done);
        } else {
            sched_post(&g_sched, self->id, &(event_t){ .sig = EV_SD_DONE });
        }
        return;

    case EV_SD_DONE:
        ctx->busy = 0;
        notify_client_ok();
        return;

    case EV_SD_ERR:
        ctx->busy = 0;
        ctx->last_error = (int)e->arg0;
        notify_client_err(ctx->last_error);
        return;
    }
}
```

#### Self-post vs ISR-post rule

- Post to self (`EV_SD_CONT`) when next step is internal CPU continuation.
- Post from ISR (`EV_SD_FIFO_CHUNK`, `EV_SD_BLOCK_DONE`, `EV_SD_ERR`) when hardware indicates progress/completion/error.

### D) Practical Splitting Heuristic

When converting blocking code, break at:

- hardware wait points
- large loops over buffers/blocks
- command boundaries (send cmd -> await response -> process data)

Each breakpoint becomes either:

- event posted by ISR (for hardware edge), or
- self continuation event (for long CPU loop chunking).

## Response-Time Guidance

Worst-case task-level response equals longest RTC step of all AOs.

To keep latency low:

- Keep handlers short.
- Split heavy work into chunks.
- Use Reminder events (`post(self, EV_CONTINUE)`).
- Track `rtc_max_ticks` per AO and regress when it grows unexpectedly.

## Integration Plan

1. Add kernel scheduler module.
   - New files: `include/scheduler.h`, `include/event.h`, `src/kernel/scheduler.c`.
2. Move app loop ownership to kernel.
   - Replace terminal-owned infinite loop with `sched_run(...)` in `src/terminal/main.c`.
3. Convert terminal to AO.
   - Terminal dispatch handles input and command events only.
4. Convert SD async notifications to events.
   - Replace ad-hoc polling with SD AO events.
5. Add idle processing.
   - Default idle hook uses `__WFI`.
6. Instrument and validate.
   - Queue high-water marks, dropped-event counters, max RTC ticks.

## Migration Notes for Current Code

- `terminal_main_async()` currently hosts a private scheduler-like loop. This should be removed.
- Keep existing UART ring buffers initially; use events as wake/notification mechanism.
- Preserve existing CLI commands while changing execution model under the hood.

## Acceptance Criteria

- Scheduler resides in kernel module and runs system-wide.
- Terminal, SD, and at least one background AO are registered and dispatched by kernel scheduler.
- Idle path enters low-power wait when no queues are ready.
- No blocking busy-wait loops remain in AO logic.
- Measured max RTC step is bounded and documented.

## Example Flow: Launch, Run, Remove

This example uses `AO_SNAPSHOT` as a background process-like task.

### 1) Launch (register + enqueue first work)

At startup, kernel registers the AO/task descriptor, then posts a start event.

```c
static ao_t ao_snapshot;
static scheduler_t g_sched;

void system_boot(void)
{
    sched_init(&g_sched, idle_wfi_hook);

    ao_snapshot.id = AO_SNAPSHOT;
    ao_snapshot.prio = 2;
    ao_snapshot.dispatch = snapshot_dispatch;
    ao_snapshot.state = &snapshot_ctx;
    eq_init(&ao_snapshot.q, snapshot_q_buf, SNAPSHOT_Q_CAP);

    sched_register(&g_sched, &ao_snapshot);

    event_t start = { .sig = EV_SNAPSHOT_START, .src = SRC_BOOT };
    sched_post(&g_sched, AO_SNAPSHOT, &start);
}
```

What happened:

- A schedulable unit (function + metadata) was registered.
- Initial work was created by posting an event into that unit's queue.

### 2) Scheduler Run (dispatch work)

Kernel loop picks highest-priority ready queue and runs one RTC step.

```c
void sched_run(scheduler_t *s)
{
    for (;;) {
        int prio = sched_highest_ready(s);
        if (prio < 0) {
            s->idle_hook(); // __WFI
            continue;
        }

        ao_t *ao = s->table[prio];
        event_t e;
        if (eq_pop(&ao->q, &e) == 0) {
            ao->dispatch(ao, &e); // run-to-completion
            ao->events_handled++;
        }

        if (eq_is_empty(&ao->q)) {
            sched_clear_ready(s, ao->prio);
        }
    }
}
```

What happened:

- Scheduler ran exactly one event handler call.
- Control returned to scheduler after the RTC step.

### 3) Continue Work (Reminder pattern)

If work is long, AO splits it and posts continuation event to itself.

```c
static void snapshot_dispatch(ao_t *self, const event_t *e)
{
    snapshot_ctx_t *ctx = (snapshot_ctx_t *)self->state;

    switch (e->sig) {
    case EV_SNAPSHOT_START:
        snapshot_begin(ctx);
        sched_post(&g_sched, self->id, &(event_t){ .sig = EV_SNAPSHOT_CONT });
        break;

    case EV_SNAPSHOT_CONT:
        if (snapshot_step(ctx)) {
            sched_post(&g_sched, self->id, &(event_t){ .sig = EV_SNAPSHOT_CONT });
        } else {
            sched_post(&g_sched, AO_TERMINAL, &(event_t){ .sig = EV_SNAPSHOT_DONE });
        }
        break;
    }
}
```

What happened:

- No blocking loop.
- Scheduler regains control between chunks.

### 4) Remove Process (drain + unregister)

When service is no longer needed, mark stopping, stop accepting new events, drain queue, unregister.

```c
void snapshot_stop(void)
{
    sched_pause_accept(&g_sched, AO_SNAPSHOT);   // optional gate
    eq_drain(&ao_snapshot.q);                    // clear pending work safely
    sched_unregister(&g_sched, AO_SNAPSHOT);     // remove from dispatch table
}
```

What happened:

- Scheduler will no longer pick that process/AO.
- Any future post to that AO ID should fail with `SCHED_ERR_NOT_FOUND`.

### Minimal State Transition View

```text
UNREGISTERED
  -> (sched_register)
READY/IDLE
  -> (event posted)
READY
  -> (scheduler dispatch)
RUNNING (RTC)
  -> (returns)
READY or IDLE
  -> (sched_unregister)
UNREGISTERED
```
