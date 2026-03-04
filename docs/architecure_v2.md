# Snapshot Arhictecture V2

## Different Program Types
- Some tasks like counter run and then die
- Other tasks have an infinite loop with UI

## Shared resources
- Right now counter as an IO lock for printing to stdin so we can't type while counter is running (aquires a lock)

## Needed invariants
- State should look exactly like it did when we snapshot (this is hard on an external board so some UI state has to be coupled with the program state)
- We need to think about what things programs running on our OS need to implement let's say they include one system header and write hooks with what about the program they want to save 
- Snapshotting should be a kernel run service that runs periodically. Other than the app hooks, apps shouldn't have to worry about how to implement their snapshot mechanics (obviously other than hooks and maybe some datatypes provided by the kernel)
- Scheduler shouldn't have to pause for a very long time to make a snapshot so that our apps don't get interrupted or seem laggy/unresponsive 
- Priority level needs to be accounted for in snapshotting task
- We cannot show invalid state to a user

## Valid Snapshots
- If a snapshot started but didn't complete we should be able to boot the last valid one (non-corrupted), this makes sense because on power loss we need to be able to rewind to a valid state# Different Program Types
- Some tasks like counter run and then die
- Other tasks have an infinite loop with UI

## Shared resources
- Right now counter as an IO lock for printing to stdin so we can't type while counter is running (aquires a lock)

## Needed invariants
- State should look exactly like it did when we snapshot (this is hard on an external board so some UI state has to be coupled with the program state)

## Concrete outline of what powers the snapshot and restore (rewind) mechanism
### v2 draft slice (append-only)

1. Snapshot trigger path
- `AO_CHECKPOINT` is posted periodically (or manually from shell).
- It captures a consistent kernel-owned header first: `seq`, `tick`, `stdin_owner`, `ready_bitmap`, `eventlog_tail`.
- Then it asks each registered task for saveable state via explicit hooks/API (`get_state`).

2. Snapshot write model
- Write to inactive slot only.
- Order: header (pending) -> task regions -> commit marker.
- A snapshot is valid only if commit marker + checksum/CRC pass.
- Boot picks latest valid committed slot, never partial writes.

3. Restore boot model
- Reinitialize hardware/drivers normally first.
- Load latest valid snapshot header + regions.
- Re-register tasks, then apply `restore_state` per task.
- Rebind shared resources from header (example: `stdin_owner`).

4. Replay tail model (for going "forward" after restore)
- Replay starts at `snapshot.eventlog_tail` and ends at current log tail.
- Only deterministic events are replayed.
- Side effects are gated while replay flag is active.
- After replay completes, clear replay flag and resume normal scheduling.

5. Back-and-forth rewind workflow
- Rewind = load older snapshot + replay to selected point.
- Forward again = choose newer snapshot or replay further tail events.
- This means snapshots are stable anchors and event log is the timeline between anchors.

6. Guardrails for v2
- No task can persist raw pointers to transient kernel internals.
- Queue internals are never checkpointed.
- Any task state without explicit save/restore API is considered non-restorable.
- If restore of one task fails, boot falls back to previous valid snapshot (or cold boot policy).

### Open questions for next iteration
- Do we want one global replay queue, or per-task replay queues with resource gating?
- Should snapshot cadence be fixed interval, dirty-triggered, or hybrid?
- Do we keep one or two snapshot slots for fast rollback when a restore attempt fails?

### Checkpoint Event Contract (mandatory)

- `AO_CHECKPOINT` is the coordinator task; apps do not own this AO.
- Every app must implement checkpoint handling, but through a standard save hook/API (`get_state` / `on_checkpoint`), not by owning scheduler policy.
- During checkpoint cycle, `AO_CHECKPOINT` iterates all registered app AOs and requests saveable state.
- If an app does not implement checkpoint handling, it is marked `non-restorable` and excluded from valid rewind targets.
- Checkpoint request must be RTC-safe: app handling must be bounded and non-blocking.

Practical rule for implementation:
- If app receives a checkpoint-related signal/event, it must parse and return a valid status (`OK`, `BUSY`, `RETRY`, `FATAL`).
- `BUSY/RETRY` means checkpoint task can retry that app in a later slice; `FATAL` aborts snapshot commit.

### Restore model revision: user-started tasks

The basic restore flow is not sufficient for tasks that were launched dynamically by a user command.

Additional requirement:
- Snapshot must store launch intent metadata for dynamic/user-started tasks (task id, launch mode fg/bg, owner/session, startup args).
- On restore, kernel must recreate the launched task instance first, then apply `restore_state`.
- If launch intent is missing, that task cannot be restored even if state bytes exist.

Implication:
- Restore is two-phase for dynamic tasks:
1. Recreate task existence from launch intent.
2. Apply task state + rebind shared resources.

### Durable State vs Replay Tail (design rule)

Replay is not the source of truth for app progress. Durable app state is.

Rule:
- Snapshot captures authoritative per-app progress/state.
- Replay only applies events after snapshot cut (`eventlog_tail_at_snapshot`).
- Restore must never re-run pre-snapshot work.

Consequence for `counter 50`:
- If snapshot happened at value `25`, restore must continue from `25` to `50`.
- It must not restart from `1`.

### What should be durable per app type

Counter-like finite job app:
- `active`
- `value` (progress cursor)
- `limit`
- `next_tick`
- fg/bg ownership metadata

Terminal app:
- stdin owner/lock mode
- prompt/shell state at dispatch boundary
- foreground/background job metadata
- command execution mailbox metadata (only durable envelope, not transient ring internals)

Kernel service apps (sd/checkpoint/etc):
- operation phase/state-machine step
- request descriptors and durable cursor/index fields
- owner/route metadata for completion events

### Queue policy

Runtime queues are transient and not checkpointed directly.
Instead, each app provides a durable representation of pending work:
- either a small durable queue model
- or a single active job + progress cursor

On restore, app rebuilds runtime queue from durable representation, then resumes processing.

### Snapshot cut policy

- Snapshot cut is taken at deterministic scheduler boundaries (after RTC dispatch step).
- Not mid-ISR byte stream, not mid-partial command parse.
- Terminal state is captured at these boundaries so resumed UX is coherent.

### Minimal restore sequence (per app)

1. Recreate app instance/launch intent if dynamically started.
2. Apply durable state.
3. Rebind shared resources/owners.
4. Reconstruct transient runtime queue from durable model.
5. Resume scheduler; replay only post-snapshot tail events.

### v2 direction update: checkpoint-first, no mandatory replay

Primary recovery goal is resume from latest valid checkpoint, not event replay.

Policy:
- Latest committed checkpoint is the only source of truth for restore.
- Replay path is optional/disabled for normal boot recovery.
- App progress must be fully represented in durable checkpoint state.

### Checkpoint log role (catalog only)

Event/log stream is used to catalog checkpoint lifecycle, not to rebuild runtime state:

- `CKPT_INTENT`: written before checkpoint write starts.
  Contains: target slot, next seq, included task ids, metadata/version.
- `CKPT_COMMIT`: written only after checkpoint data is fully durable and validated.

Boot rule:
- Only checkpoints with a matching successful commit marker are eligible.
- If power loss happens after intent but before commit, ignore that checkpoint attempt and boot previous valid committed checkpoint.

### Restore rule under this model

1. Reinitialize hardware/drivers.
2. Select latest valid committed checkpoint.
3. Recreate dynamic task instances from launch intent metadata.
4. Apply durable task state.
5. Rebind shared kernel resources/owners.
6. Resume scheduler (no replay required).

### Two-tier checkpoint path (performance)

Use a two-tier model to reduce checkpoint latency:

Tier 1: RAM checkpoint catalog (hot path)
- While tasks run, kernel records small in-RAM catalog entries (dirty task id, state generation, launch/owner changes).
- No SD write is required for each runtime event.

Tier 2: Checkpoint flush (cold path)
- `AO_CHECKPOINT` consumes RAM catalog entries to identify exactly which task regions changed.
- It writes only required checkpoint regions + header/commit markers.
- After successful commit, catalog window is advanced/cleared.

Why this helps:
- Less scanning work at checkpoint time.
- Smaller SD write set (delta-oriented).
- Better RTC responsiveness when checkpoint work is sliced.

Durability rule:
- RAM catalog is optimization only, not recovery source of truth.
- Recovery always uses latest valid committed checkpoint.
- If crash occurs before commit, RAM catalog is discarded and previous committed checkpoint is restored.

### Flow model (boot -> resume -> interact -> checkpoint)

State flow:
- `BOOT_INIT` -> hardware/drivers init
- `RESTORE_SELECT` -> choose latest valid committed checkpoint
- `RESTORE_RECREATE` -> recreate dynamic/user-started task instances from launch intent
- `RESTORE_APPLY` -> apply durable per-task state + kernel ownership state
- `RUNNING` -> normal RTC scheduling and user interaction
- `CKPT_PREPARE` -> checkpoint cut at deterministic dispatch boundary
- `CKPT_WRITE` -> write checkpoint payload to inactive slot
- `CKPT_COMMIT` -> commit marker + final CRC
- back to `RUNNING`

Detailed runtime sequence:
1. Boot initializes board/clock/uart/sd/scheduler.
2. System selects latest valid committed checkpoint.
3. Kernel restores global header fields (`seq`, `tick`, ownership info).
4. Kernel recreates dynamic tasks from launch intent metadata.
5. Kernel applies each task's durable state (`restore_state`).
6. Kernel rebinds shared resources/owners (`stdin_owner`, resource table).
7. Scheduler starts and system becomes interactive.
8. On checkpoint trigger, checkpoint AO collects current durable state.
9. SD write order: intent/pending header -> task regions -> commit marker/final CRC.
10. If commit succeeds, checkpoint becomes latest valid restore target.

Power-loss rule in this flow:
- Loss before commit: discard in-progress checkpoint and restore previous committed checkpoint.
- Loss after commit: restore new checkpoint.

### Required saved data (minimum set)

1. Global checkpoint metadata
- magic + format version
- checkpoint `seq`
- `tick_at_checkpoint`
- commit/pending marker
- checksum/CRC (header + regions)
- schema version for task state layouts

2. Kernel ownership/runtime coordination state
- stdin owner + lock mode
- shared resource ownership table
- active task/app registry bitmap

3. Dynamic launch intent metadata (for user-started apps)
- task/app id
- launch mode (fg/bg)
- launch args/config
- owner/session metadata

4. Per-task durable state (authoritative progress)
- counter-like apps: `active`, `value`, `limit`, `next_tick`, mode/owner fields
- terminal: prompt/shell durable state at RTC boundary, fg/bg job metadata, command envelope metadata
- service tasks (sd/checkpoint/etc): state-machine phase + durable request descriptors/cursors

5. Optional optimization metadata
- dirty-task generation/catalog metadata (if using RAM catalog optimization)

Explicitly not saved:
- runtime queue internals (`head/tail/count`)
- ISR transient buffers
- raw pointers to volatile kernel internals

### Migration + SD Layout v2

### Migration plan from current kernel/apps

1. Add universal task durability contract for all schedulable tasks
- `get_state(void *out, uint32_t *len)`
- `restore_state(const void *in, uint32_t len)`
- `on_checkpoint_prepare()`
- `on_checkpoint_commit()`
- `on_checkpoint_abort()`

2. Classify current tasks during migration
- `RESTORABLE_NOW`: tasks with complete durable state contract
- `RESTART_ONLY`: tasks safe to reinit without continuity
- `NON_RESTORABLE`: excluded from valid rewind targets until upgraded

3. Add launch intent metadata for dynamic/user-started apps
- app/task id, fg/bg mode, startup args/config, owner/session
- restore sequence: recreate instance from launch intent, then apply `restore_state`

4. Make checkpoint coordinator AO own persistence protocol
- apps never write SD directly for checkpoint data
- apps only expose durable state and status

### Durable data structures (v2 concepts)

- `checkpoint_header_t`: global checkpoint metadata (versioned + CRC)
- `region_dir_entry_t`: per-region metadata (`region_id`, `offset`, `len`, `crc`)
- `task_state_blob`: opaque per-task durable bytes returned by `get_state`
- `launch_intent_table`: dynamic app recreation metadata
- `resource_owner_state`: shared lock/resource ownership snapshot

### Task upgrade requirements (current codebase)

Counter task:
- durable progress cursor + target + active/mode fields
- timer resume basis (`next_tick` or equivalent deterministic timer basis)

Terminal/cmd path:
- stdin owner/lock mode
- shell/prompt durable state at RTC boundary
- fg/bg execution metadata + command envelope metadata

Service tasks (sd/checkpoint/etc):
- state-machine phase
- durable request descriptors/cursors
- no transient ISR-only internals in durable state

Console/log path:
- usually restart-only unless exact pending output continuity is required

### SD layout (checkpoint-first)

- Superblock A/B (redundant metadata)
- Checkpoint Slot 0
- Checkpoint Slot 1
- Optional checkpoint catalog/log region (intent/commit records only)

Per checkpoint slot format:
1. Slot header written as `PENDING`
2. Region directory
3. Region payload blobs
4. Commit trailer/header update to `COMMITTED` with final CRC

### Checkpoint write flow

1. `AO_CHECKPOINT` triggers at deterministic RTC boundary
2. Collect durable state from restorable/dirty tasks
3. Write inactive slot header (`PENDING`)
4. Write region directory + payloads
5. Write commit marker/final CRC
6. Atomically update superblock active seq/slot

### Boot/restore flow

1. Initialize hardware/drivers/scheduler
2. Select latest valid committed slot (CRC verified)
3. Restore global kernel ownership/resource state
4. Recreate dynamic tasks from launch intent
5. Apply per-task durable state (`restore_state`)
6. Resume scheduler and interaction

### Failure policy

- Power loss before commit: ignore in-progress slot and restore previous committed slot
- Power loss after commit: restore newly committed slot
- Any non-restorable task encountered: follow policy (`restart-only` or fail restore target)

### Debug Logging and Observability

Use structured debug events during migration and v2 bring-up. Avoid ad-hoc print-only debugging.

### Structured debug event IDs

Checkpoint path:
- `DBG_CKPT_START`
- `DBG_CKPT_COLLECT_BEGIN` / `DBG_CKPT_COLLECT_END`
- `DBG_CKPT_REGION_WRITE`
- `DBG_CKPT_COMMIT_OK`
- `DBG_CKPT_COMMIT_FAIL`

Restore path:
- `DBG_RESTORE_SELECT_SLOT`
- `DBG_RESTORE_RECREATE_TASK`
- `DBG_RESTORE_APPLY_TASK_OK`
- `DBG_RESTORE_APPLY_TASK_FAIL`
- `DBG_RESTORE_DONE`

Recovery/policy path:
- `DBG_RECOVER_PREV_SLOT`
- `DBG_RECOVER_ABORT_TARGET`
- `DBG_RECOVER_RESTART_ONLY_TASK`

### Logging sinks

1. RAM trace ring (primary debug sink)
- always available in debug builds
- low overhead, suitable for frequent events

2. Console/log stream (secondary)
- human-readable summaries
- rate-limited to avoid flooding and timing distortion

### Correlation fields (required in debug events)

Each debug record should include:
- checkpoint `seq`
- checkpoint `slot`
- `task_id` (if task-scoped)
- `phase`
- return/status code (`rc`)

This allows one checkpoint/restore attempt to be reconstructed end-to-end.

### Boot/restore phase markers

Emit explicit phase boundaries:
- `BOOT_INIT_DONE`
- `RESTORE_SLOT_CHOSEN`
- `RESTORE_APPLY_BEGIN`
- `RESTORE_APPLY_END`
- `SCHED_RUN_ENTER`

### Task-level restore diagnostics

For each task restore attempt log:
- state blob length
- crc/validation result
- `restore_state` return code
- policy branch taken on failure (`restart-only` or abort target)

### Panic/fault integration

On panic/fault path:
- emit current checkpoint `seq` and `slot`
- dump recent trace window (last N records)
- include active restore/checkpoint phase marker

### Build-time controls

- `DEBUG_TRACE=1` to enable verbose trace path
- category mask support:
  - `DBG_CAT_CKPT`
  - `DBG_CAT_RESTORE`
  - `DBG_CAT_TASK`
  - `DBG_CAT_POLICY`

### Concrete restore scenario (counter task)

Scenario: user runs `counter 50`, checkpoint occurs at progress `25`, board reboots, task resumes at `25`.

1. User starts `counter 50` (foreground).
2. Counter durable state reaches:
- `active=1`
- `value=25`
- `limit=50`
- `bg=0`
- `next_tick=<saved>`
3. `AO_CHECKPOINT` triggers and requests counter durable state.
4. Checkpoint writer writes inactive slot in order:
- header `PENDING` (new `seq`, target slot)
- region directory entry for counter state
- counter state blob
- commit marker + final CRC (`COMMITTED`)
5. Power loss happens after commit.
6. On boot, kernel initializes hardware/drivers/scheduler.
7. Restore loader scans slots, validates CRC/commit, selects latest committed slot.
8. Kernel recreates/registers counter task instance.
9. Kernel applies saved counter state to task.
10. Kernel rebinds stdin ownership (foreground owner = counter).
11. Scheduler runs; SysTick triggers next step event.
12. Counter continues from `25` to `50` and exits normally.

### Pushback: how does kernel know which restore function to call?

Hardcoding calls like `counter_task_restore_state()` in boot code does not scale and will drift as tasks grow.
A raw global list of function pointers is also fragile unless it is typed, versioned, and tied to task identity.

Required mechanism:

- Add a task restore registry with explicit descriptors keyed by task/app id.
- Each descriptor includes:
  - `task_id`
  - `state_version`
  - `state_size_min/max`
  - `register_fn` (create/register task instance)
  - `restore_fn` (apply durable state blob)
  - `get_state_fn` (checkpoint path)
- Checkpoint region directory stores `task_id` + `state_version` per blob.
- Restore loader resolves descriptor by `task_id`, validates version/size, then calls `register_fn` and `restore_fn`.

Why this is safer than ad-hoc function pointer lists:
- typed contract per task (not anonymous callbacks)
- version compatibility checks are enforced centrally
- missing/unknown task ids become explicit policy decisions (`restart-only`, skip, or restore fail)
- boot code stays generic as new tasks/apps are added

Policy recommendation:
- Unknown `task_id` or incompatible `state_version` => do not call restore blindly.
- Apply configured policy (`restart-only` or reject checkpoint target).

### Restore support policy (default required)

Policy:
- Every program/task is restorable by default.
- Each program must implement restore path hooks (`get_state` + `restore_state`/`on_restore`).
- Opt-out is explicit only: mark task as `restart-only` (or `non-restorable`).

Checkpoint metadata requirement:
- Record restore capability/class for included programs at checkpoint time.

Restore-time rule:
- If a required program is non-restorable, do not silently fake restore.
- Apply explicit policy:
  - `restart-only` path for that program, or
  - reject that checkpoint target as incompatible.

Design goal:
- deterministic behavior, no hidden partial restores.

### UI restore requirements (first-class)

Restoring program logic state is not sufficient. UI state must also be restored and redrawn automatically.

Problem to avoid:
- Restored progress exists internally, but display only updates on next input/tick.
- Example: checkpoint at value `50`, restore shows nothing until next tick (appears as `51` first).

Required UI model:

1. Durable UI view state
- active screen/menu id
- selection/cursor position
- prompt/shell visible state
- status/job lines needed to reflect running tasks and current progress

2. Restore-time UI rehydrate phase
- apply task durable state first
- then trigger explicit UI redraw event (example: `UI_SIG_RESTORE_REDRAW`)
- redraw must not depend on new user input

3. Deterministic display rule
- on restore, UI must reflect exact checkpoint view/progress immediately
- next value/change appears only after normal post-restore dispatch/tick

4. Terminal/menu behavior
- restore active mode/context (prompt/menu/bg status)
- repaint current screen and task statuses on boot resume

Design policy:
- checkpoint completeness includes both task state and visible UI state coherence.

### Multi-UI lock + lifecycle coherence

When multiple UIs/tasks share interaction resources, checkpoint must capture both ownership and lifecycle state.

Required durable state:

1. Shared UI/IO lock table
- lock id
- current owner task/app id (or none)
- lock mode (raw/menu/line/etc)

2. Task lifecycle state
- `RUNNING`
- `EXITED`
- `FAILED`
- `STOPPED`

3. Exit metadata
- exit reason/code
- completion timestamp/sequence
- optional completion/notification state

Restore rules:

- Do not resurrect tasks marked `EXITED` at checkpoint time.
- Restore lock ownership only for tasks in restorable running states.
- If a lock owner was exited, clear lock on restore and emit a safe UI refresh.
- UI must show exited/completed status immediately after restore (without requiring user input).

Coordinator policy:
- Checkpoint coordinator validates lock/lifecycle consistency before commit.
- Inconsistent state (lock owned by non-running task) must be normalized before commit or snapshot is rejected.

Design goal:
- restore reproduces not just "what was running" but also "what already finished", with coherent lock ownership across all UIs.

### Lifecycle truth table for restore

Store more than "is running". Persist lifecycle terminal states and last transition metadata.

Required persisted fields:
- lifecycle state (`RUNNING`, `EXITED`, `FAILED`, `STOPPED`)
- last transition seq/tick
- exit/failure reason + code (when applicable)

Restore truth table:

- Saved state = `RUNNING`
  - Restore action: recreate instance + apply durable state + resume
  - UI action: show as running

- Saved state = `EXITED`
  - Restore action: do not relaunch instance
  - UI action: show completed/exited status

- Saved state = `FAILED`
  - Restore action: do not relaunch automatically (unless explicit policy says retry)
  - UI action: show failed status + reason/code

- Saved state = `STOPPED`
  - Restore action: do not relaunch
  - UI action: show stopped status

Design rule:
- "running-only" persistence is insufficient because it cannot distinguish
  "never started" from "started and already exited before checkpoint".

### v2 Implementation Contract (appendix)

This appendix defines the minimum concrete contract required to implement v2 without ambiguity.

### 1) On-disk structures (normative)

All numeric fields are little-endian. All structs are packed and versioned.

`checkpoint_header_t` (fixed-size):
- `magic` (u32)
- `format_version` (u16)
- `header_size` (u16)
- `seq` (u32)
- `tick_at_checkpoint` (u32)
- `slot_id` (u8)
- `state` (u8: `PENDING`/`COMMITTED`)
- `region_count` (u16)
- `active_task_bitmap` (u32)
- `stdin_owner` (u8)
- `reserved[3]`
- `regions_crc32` (u32)
- `header_crc32` (u32)

`region_dir_entry_t` (repeated `region_count` times):
- `region_id` (u16)  // includes task id namespace
- `state_version` (u16)
- `offset` (u32)     // payload offset from slot start
- `length` (u32)
- `crc32` (u32)

CRC scope:
- `header_crc32`: header bytes excluding `header_crc32` field itself
- `regions_crc32`: CRC over directory + payload area

### 2) Version compatibility policy

- `format_version` mismatch => slot incompatible
- per-region `state_version` mismatch => apply task policy:
  - `RESTORABLE_NOW`: fail restore target
  - `RESTART_ONLY`: skip region, restart task
  - `NON_RESTORABLE`: skip region, do not recreate

No best-effort binary reinterpretation of unknown versions.

### 3) Task restore registry API (kernel side)

Define one descriptor table keyed by `task_id`:

- `task_id`
- `class` (`RESTORABLE_NOW`, `RESTART_ONLY`, `NON_RESTORABLE`)
- `state_version`
- `min_state_len`, `max_state_len`
- `register_fn(scheduler_t *s, const launch_intent_t *li)`
- `get_state_fn(void *out, uint32_t *len)`
- `restore_fn(const void *blob, uint32_t len)`
- `ui_rehydrate_fn(void)` (optional)

Restore loader behavior:
1. resolve descriptor by `task_id`
2. validate class + version + length
3. call `register_fn`
4. call `restore_fn` when applicable

### 4) Checkpoint state machine (normative)

States:
- `RUNNING`
- `CKPT_PREPARE`
- `CKPT_COLLECT`
- `CKPT_WRITE_PENDING`
- `CKPT_WRITE_REGIONS`
- `CKPT_COMMIT`
- `CKPT_DONE`
- `CKPT_ABORT`

Rules:
- Each state executes in bounded RTC slices.
- Any write/CRC failure transitions to `CKPT_ABORT`.
- `CKPT_ABORT` never updates active superblock pointer.
- Retry limit is bounded (`N` attempts), then defer to next trigger.

### 5) Cut-time concurrency and ISR rules

- Snapshot cut occurs only after an RTC dispatch boundary.
- ISR remains enabled; ISR transient queues are not persisted.
- Only durable task state captured through `get_state_fn`.
- Lock/owner table must be captured atomically with header fields.
- No task may block during `on_checkpoint_prepare` or `get_state_fn`.

### 6) Recovery policy matrix (boot)

Per task class:

- `RESTORABLE_NOW`
  - recreate + restore required
  - failure => checkpoint target rejected

- `RESTART_ONLY`
  - recreate with normal start
  - ignore missing/incompatible region

- `NON_RESTORABLE`
  - do not recreate from checkpoint unless explicit launch policy says start fresh

Global decision:
- If any required `RESTORABLE_NOW` task fails restore, boot previous committed slot.
- If no older valid slot exists, cold-boot policy applies.

### 7) UI rehydrate phase (mandatory)

After task restore and before normal interaction:
- emit UI restore redraw pass
- apply terminal/menu/status rehydrate callbacks
- ensure checkpoint value is visible immediately (no input required)

### 8) Launch/lifecycle metadata (required)

Persist per instance:
- `program_id`, `instance_id`
- `launch_mode` (fg/bg), `owner_session`
- `lifecycle_state` (`RUNNING`/`EXITED`/`FAILED`/`STOPPED`)
- `exit_code`, `exit_reason`, `last_transition_seq`

Restore truth:
- only `RUNNING` instances are recreated/restored
- terminal states are shown in UI but not relaunched

### Durable queue schema and parse/resume contract

For apps that need resumable pending work, define a durable queue model (persisted) separate from runtime scheduler queues (transient).

### Durable queue entry fields (minimum)

Each persisted work entry should include:
- `seq`                // monotonic entry ordering/idempotency key
- `action`             // operation code describing what to do
- `data`               // action payload/args blob or fixed fields
- `state`              // `PENDING` / `RUNNING` / `DONE` / `FAILED`
- `value`              // current progress value
- `next_value`         // next expected progress value/step
- `updated_seq/tick`   // last mutation marker
- `retry_count`        // optional, recommended for retry control

### Durable queue header fields (minimum)

- `magic` / `version`
- `entry_size`
- `capacity`
- `count`
- `read_idx`           // next entry to process
- `write_idx`
- `generation`
- `crc32`

### Kernel parse responsibility

Kernel does not interpret task-private action payload semantics.
Kernel responsibilities are limited to:
1. Validate checkpoint header/directory CRC and lengths.
2. Resolve task descriptor by `task_id`.
3. Validate region `state_version` + blob size bounds.
4. Pass blob to task `restore_fn`.

### Program parse responsibility

Program `restore_fn` must:
1. Validate durable queue header (`magic/version/count/read_idx/crc`).
2. Identify next work using `read_idx` and entry `state`.
3. Resume from the first `PENDING` or resumable `RUNNING` entry.
4. Skip terminal entries (`DONE`/`FAILED`) by advancing cursor rules.
5. Rebuild transient runtime queue from remaining durable work entries.

### Deterministic resume rule

"What happens next" is defined by this tuple per active entry:
- `(action, state, value, next_value)`

Without this tuple persisted durably, restore cannot deterministically continue work.

### Best-effort implementation decisions (to unblock coding)

These defaults are implementation-ready and can be revised later with version bumps.

### 1) Atomicity and superblock update order

- Maintain Superblock A/B with monotonic `super_seq`.
- On checkpoint commit:
  1. fully write checkpoint slot + commit marker
  2. write next superblock copy with updated active slot + checkpoint seq
  3. verify write return status
- Boot chooses valid superblock with highest `super_seq`.
- If A/B disagree and both valid, highest `super_seq` wins.
- If one is invalid, use the other.

### 2) Struct/layout freeze (v2 baseline)

- All on-disk structs are packed, little-endian.
- `checkpoint_header_t` fixed-size target: 64 bytes.
- `region_dir_entry_t` fixed-size target: 16 bytes.
- Hard limits:
  - max regions per checkpoint: 64
  - max task state blob: 4096 bytes
  - max checkpoint slot payload: bounded by slot size and directory

### 3) Task/app identity namespace

- `0..127` reserved for kernel tasks.
- `128..255` reserved for user apps.
- IDs are stable across firmware releases once assigned.
- Removing an ID requires explicit compatibility policy entry.

### 4) Version migration policy

- `format_version` mismatch => incompatible checkpoint.
- Per-task `state_version` mismatch:
  - if task class `RESTORABLE_NOW`: reject checkpoint target
  - if `RESTART_ONLY`: skip restore blob and relaunch normal path
  - if `NON_RESTORABLE`: ignore blob and do not recreate unless explicit launch policy says start fresh

### 5) Checkpoint cadence and budgeting

- Default cadence: every 10 seconds or manual trigger.
- Budget: max 1 RTC slice per scheduler turn for checkpoint AO.
- If checkpoint backlog persists, defer next checkpoint trigger until current one finishes.
- Retry failed checkpoint write up to 2 times, then wait for next cadence tick.

### 6) Recovery policy matrix (boot)

- Any failed `RESTORABLE_NOW` required task restore => fallback to previous committed slot.
- If no previous committed slot exists => cold boot.
- `RESTART_ONLY` failures do not block restore target.
- `NON_RESTORABLE` tasks are not considered restore blockers.

### 7) UI consistency contract (mandatory fields)

Terminal/UI durable minimum:
- active view/menu id
- cursor/selection
- stdin owner + mode
- foreground/background status lines
- completion notifications pending flag

Restore requires explicit redraw pass before interaction acceptance.

### 8) Durable queue capacity/overflow

Per-task defaults:
- durable queue capacity: 32 entries
- max entry payload: 128 bytes (or fixed args fields)

Overflow policy:
- reject enqueue with error status
- emit debug event (`DBG_QUEUE_DURABLE_OVERFLOW`)
- never silently overwrite unread durable entries

### 9) Integrity/security policy

- CRC32 required on header, directory, and each region.
- CRC failure marks slot invalid for restore.
- Restore never panics solely on corrupt slot; attempts fallback slot first.
- Panic only on internal invariant violations (e.g., impossible registry state).

### 10) Acceptance test baseline

Minimum required tests:
1. power loss before commit -> previous slot restored
2. power loss after commit -> new slot restored
3. task `state_version` mismatch handling per class
4. running task restored at exact progress cursor (counter checkpoint at 25 resumes at 25)
5. exited task is not relaunched
6. lock owner coherence after restore
7. UI redraw shows checkpoint state without user input
8. durable queue resume consumes correct next entry (`read_idx` semantics)

### 11) Implementation ordering (recommended)

1. finalize on-disk structs + constants in headers
2. build restore registry + task class matrix
3. implement checkpoint AO write state machine
4. implement boot restore selector + per-task restore dispatch
5. implement terminal/UI rehydrate pass
6. add durability queue support for first app (counter) as reference pattern
7. execute acceptance tests and promote schema/version if needed

### Interpretation order (based on existing v2 content)

Because this document was built append-only, implementation must follow this precedence order when sections conflict:

1. `v2 direction update: checkpoint-first, no mandatory replay`
2. `v2 Implementation Contract (appendix)`
3. `Best-effort implementation decisions (to unblock coding)`
4. Earlier draft/replay sections

Explicit override decisions:
- Replay is optional and not required for normal boot restore.
- Latest committed checkpoint is authoritative restore source.
- Task/UI lifecycle + lock coherence rules are mandatory.
- Durable queue parsing/resume contract is task-owned via registry restore callbacks.

Any new implementation decision should be appended in this same style and treated as higher precedence than earlier draft text.
