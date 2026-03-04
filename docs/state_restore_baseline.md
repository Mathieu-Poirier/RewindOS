# State Save/Restore Baseline

This branch is the cleanup baseline before re-introducing full rewind/replay.

## Current Rules

- Scheduler and task execution remain run-to-completion (RTC).
- Task queues are runtime-only and are **not** part of checkpoint state.
- Checkpointed task state must be plain data (no transient queue internals).
- After restore, timer-driven work is re-armed by normal runtime hooks.

## Counter Task Contract

The counter task now exposes explicit state APIs in `include/counter_task.h`:

- `counter_task_get_state()`
- `counter_task_restore_state()`
- `counter_task_reset_state()`

Restore behavior:

- Restored state is applied before task registration.
- `counter_task_register()` preserves restored state once.
- `step_pending` is cleared on restore because queued events are not checkpointed.

## Why This Baseline

This gives a deterministic handoff point for checkpoint/restore without relying on
private static variables inside task implementations.
