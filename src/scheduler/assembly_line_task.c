#include "../../include/assembly_line_task.h"
#include "../../include/cmd_context.h"
#include "../../include/console.h"
#include "../../include/restore_registry.h"
#include "../../include/systick.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/task_spec.h"
#include "../../include/terminal.h"

#define ASSEMBLY_LINE_OP_TICKS 300u

static assembly_line_state_t g_assembly_line_ctx;
static event_t g_assembly_line_queue_storage[8];
static scheduler_t *g_assembly_line_sched;
static uint8_t g_assembly_line_state_restored;
static uint8_t g_assembly_line_restore_needs_stdin_rebind;
static uint8_t g_assembly_line_tick_pending;

static void assembly_line_render(void);

static void assembly_line_zero_parts(void)
{
    for (uint32_t i = 0u; i < ASSEMBLY_LINE_PARTS; i++) {
        g_assembly_line_ctx.parts[i].part_id = 0u;
        g_assembly_line_ctx.parts[i].scan_result = ASSEMBLY_LINE_SCAN_UNKNOWN;
        g_assembly_line_ctx.parts[i].location = (uint8_t)i;
        g_assembly_line_ctx.parts[i].process_state = ASSEMBLY_LINE_PART_RAW;
        g_assembly_line_ctx.parts[i].reserved[0] = 0u;
        g_assembly_line_ctx.parts[i].reserved[1] = 0u;
        g_assembly_line_ctx.parts[i].reserved[2] = 0u;
    }
}

void assembly_line_task_reset_state(void)
{
    g_assembly_line_ctx.active = 0u;
    g_assembly_line_ctx.ui_mode = ASSEMBLY_LINE_UI_RUNNING;
    g_assembly_line_ctx.selected_slot = 0u;
    g_assembly_line_ctx.robot_state = ASSEMBLY_LINE_ROBOT_HOME;
    g_assembly_line_ctx.robot_needs_home = 0u;
    g_assembly_line_ctx.pending_op = ASSEMBLY_LINE_OP_NONE;
    g_assembly_line_ctx.fixture_slot = ASSEMBLY_LINE_NO_SLOT;
    g_assembly_line_ctx.reserved0 = 0u;
    g_assembly_line_ctx.current_part_seq = 1u;
    g_assembly_line_ctx.completed_count = 0u;
    g_assembly_line_ctx.rejected_count = 0u;
    g_assembly_line_ctx.next_tick = 0u;
    assembly_line_zero_parts();
    g_assembly_line_restore_needs_stdin_rebind = 0u;
    g_assembly_line_tick_pending = 0u;
}

static void assembly_line_seed_new_run(void)
{
    assembly_line_task_reset_state();
    g_assembly_line_ctx.active = 1u;
    g_assembly_line_ctx.current_part_seq = ASSEMBLY_LINE_PARTS + 1u;
    for (uint32_t i = 0u; i < ASSEMBLY_LINE_PARTS; i++) {
        g_assembly_line_ctx.parts[i].part_id = (uint16_t)(i + 1u);
        g_assembly_line_ctx.parts[i].location = (uint8_t)i;
    }
}

static int assembly_line_state_valid(const assembly_line_state_t *state)
{
    if (state == 0) {
        return 0;
    }
    if (state->active > 1u) {
        return 0;
    }
    if (state->ui_mode != ASSEMBLY_LINE_UI_RUNNING &&
        state->ui_mode != ASSEMBLY_LINE_UI_COMPLETE) {
        return 0;
    }
    if (state->selected_slot >= ASSEMBLY_LINE_PARTS) {
        return 0;
    }
    if (state->robot_state < ASSEMBLY_LINE_ROBOT_HOME ||
        state->robot_state > ASSEMBLY_LINE_ROBOT_HOME_REQUIRED) {
        return 0;
    }
    if (state->robot_needs_home > 1u) {
        return 0;
    }
    if (state->pending_op > ASSEMBLY_LINE_OP_REJECT) {
        return 0;
    }
    if (state->fixture_slot != ASSEMBLY_LINE_NO_SLOT &&
        state->fixture_slot >= ASSEMBLY_LINE_PARTS) {
        return 0;
    }
    for (uint32_t i = 0u; i < ASSEMBLY_LINE_PARTS; i++) {
        const assembly_part_record_t *part = &state->parts[i];
        if (part->scan_result > ASSEMBLY_LINE_SCAN_FAIL) {
            return 0;
        }
        if (part->location > ASSEMBLY_LINE_LOC_REJECT_BIN) {
            return 0;
        }
        if (part->process_state > ASSEMBLY_LINE_PART_DONE_REJECT) {
            return 0;
        }
    }
    return 1;
}

int assembly_line_task_get_state(assembly_line_state_t *out)
{
    if (out == 0) {
        return SCHED_ERR_PARAM;
    }
    *out = g_assembly_line_ctx;
    return SCHED_OK;
}

int assembly_line_task_encode_state_blob(const assembly_line_state_t *state,
                                         void *out,
                                         uint32_t *io_len)
{
    if (state == 0 || out == 0 || io_len == 0) {
        return SCHED_ERR_PARAM;
    }
    if (!assembly_line_state_valid(state)) {
        return SCHED_ERR_PARAM;
    }
    if (*io_len < sizeof(assembly_line_state_t)) {
        return SCHED_ERR_PARAM;
    }
    *(assembly_line_state_t *)out = *state;
    *io_len = (uint32_t)sizeof(assembly_line_state_t);
    return SCHED_OK;
}

int assembly_line_task_decode_state_blob(const void *blob,
                                         uint32_t len,
                                         assembly_line_state_t *out)
{
    if (blob == 0 || out == 0) {
        return SCHED_ERR_PARAM;
    }
    if (len != sizeof(assembly_line_state_t)) {
        return SCHED_ERR_PARAM;
    }
    *out = *(const assembly_line_state_t *)blob;
    if (!assembly_line_state_valid(out)) {
        return SCHED_ERR_PARAM;
    }
    return SCHED_OK;
}

static assembly_part_record_t *assembly_line_find_part_in_location(uint8_t location)
{
    for (uint32_t i = 0u; i < ASSEMBLY_LINE_PARTS; i++) {
        if (g_assembly_line_ctx.parts[i].location == location &&
            g_assembly_line_ctx.parts[i].process_state != ASSEMBLY_LINE_PART_DONE_GOOD &&
            g_assembly_line_ctx.parts[i].process_state != ASSEMBLY_LINE_PART_DONE_REJECT) {
            return &g_assembly_line_ctx.parts[i];
        }
    }
    return 0;
}

static assembly_part_record_t *assembly_line_find_selected_part(void)
{
    return assembly_line_find_part_in_location(g_assembly_line_ctx.selected_slot);
}

static assembly_part_record_t *assembly_line_find_fixture_part(void)
{
    return assembly_line_find_part_in_location(ASSEMBLY_LINE_LOC_FIXTURE);
}

static const char *assembly_line_scan_name(uint8_t result)
{
    if (result == ASSEMBLY_LINE_SCAN_PASS) return "PASS";
    if (result == ASSEMBLY_LINE_SCAN_FAIL) return "FAIL";
    return "--";
}

static const char *assembly_line_part_state_name(uint8_t state)
{
    if (state == ASSEMBLY_LINE_PART_RAW) return "RAW";
    if (state == ASSEMBLY_LINE_PART_SCANNED_OK) return "SCANNED_OK";
    if (state == ASSEMBLY_LINE_PART_SCANNED_BAD) return "SCANNED_BAD";
    if (state == ASSEMBLY_LINE_PART_AT_FIXTURE) return "FIXTURE";
    if (state == ASSEMBLY_LINE_PART_DONE_GOOD) return "GOOD";
    if (state == ASSEMBLY_LINE_PART_DONE_REJECT) return "REJECT";
    return "?";
}

static const char *assembly_line_robot_state_name(uint8_t state)
{
    if (state == ASSEMBLY_LINE_ROBOT_HOME) return "HOME";
    if (state == ASSEMBLY_LINE_ROBOT_BUSY) return "BUSY";
    if (state == ASSEMBLY_LINE_ROBOT_FIXTURE) return "FIXTURE";
    if (state == ASSEMBLY_LINE_ROBOT_HOME_REQUIRED) return "HOME_REQUIRED";
    return "?";
}

static const char *assembly_line_op_name(uint8_t op)
{
    if (op == ASSEMBLY_LINE_OP_HOME) return "HOME";
    if (op == ASSEMBLY_LINE_OP_SCAN) return "SCAN";
    if (op == ASSEMBLY_LINE_OP_PICK) return "PICK";
    if (op == ASSEMBLY_LINE_OP_FINISH) return "FINISH";
    if (op == ASSEMBLY_LINE_OP_REJECT) return "REJECT";
    return "NONE";
}

static uint8_t assembly_line_all_parts_done(void)
{
    for (uint32_t i = 0u; i < ASSEMBLY_LINE_PARTS; i++) {
        if (g_assembly_line_ctx.parts[i].process_state != ASSEMBLY_LINE_PART_DONE_GOOD &&
            g_assembly_line_ctx.parts[i].process_state != ASSEMBLY_LINE_PART_DONE_REJECT) {
            return 0u;
        }
    }
    return 1u;
}

static void assembly_line_check_complete(void)
{
    if (assembly_line_all_parts_done() &&
        g_assembly_line_ctx.fixture_slot == ASSEMBLY_LINE_NO_SLOT &&
        g_assembly_line_ctx.pending_op == ASSEMBLY_LINE_OP_NONE) {
        g_assembly_line_ctx.ui_mode = ASSEMBLY_LINE_UI_COMPLETE;
        g_assembly_line_ctx.robot_state = ASSEMBLY_LINE_ROBOT_HOME;
        g_assembly_line_ctx.robot_needs_home = 0u;
    }
}

static void assembly_line_render_slot(uint8_t slot)
{
    assembly_part_record_t *part = assembly_line_find_part_in_location(slot);

    console_puts((g_assembly_line_ctx.selected_slot == slot) ? ">" : " ");
    console_puts(" SLOT");
    console_put_u32((uint32_t)slot);
    console_puts(": ");
    if (part == 0) {
        console_puts("(empty)\r\n");
        return;
    }

    console_puts("part=");
    console_put_u32(part->part_id);
    console_puts(" scan=");
    console_puts(assembly_line_scan_name(part->scan_result));
    console_puts(" state=");
    console_puts(assembly_line_part_state_name(part->process_state));
    console_puts("\r\n");
}

static void assembly_line_render_fixture(void)
{
    assembly_part_record_t *part = assembly_line_find_fixture_part();

    console_puts("  FIXTURE: ");
    if (part == 0) {
        console_puts("(empty)\r\n");
        return;
    }
    console_puts("part=");
    console_put_u32(part->part_id);
    console_puts(" scan=");
    console_puts(assembly_line_scan_name(part->scan_result));
    console_puts(" state=");
    console_puts(assembly_line_part_state_name(part->process_state));
    console_puts("\r\n");
}

static void assembly_line_render(void)
{
    console_puts("\x1b[2J\x1b[H");
    console_puts("ASSEMBLY LINE\r\n");
    console_puts("robot=");
    console_puts(assembly_line_robot_state_name(g_assembly_line_ctx.robot_state));
    console_puts(" pending=");
    console_puts(assembly_line_op_name(g_assembly_line_ctx.pending_op));
    console_puts(" completed=");
    console_put_u32(g_assembly_line_ctx.completed_count);
    console_puts(" rejected=");
    console_put_u32(g_assembly_line_ctx.rejected_count);
    console_puts("\r\n");

    if (g_assembly_line_ctx.robot_needs_home) {
        console_puts("status: HOME REQUIRED before work can continue\r\n");
    } else if (g_assembly_line_ctx.ui_mode == ASSEMBLY_LINE_UI_COMPLETE) {
        console_puts("status: COMPLETE, press q to exit\r\n");
    } else {
        console_puts("status: ready\r\n");
    }

    for (uint8_t slot = 0u; slot < ASSEMBLY_LINE_PARTS; slot++) {
        assembly_line_render_slot(slot);
    }
    assembly_line_render_fixture();

    console_puts("\r\n");
    console_puts("keys: a/d select  s scan  p pick  f finish-good  x reject  h home  q quit\r\n");
}

static void assembly_line_stop(void)
{
    g_assembly_line_ctx.active = 0u;
    g_assembly_line_ctx.pending_op = ASSEMBLY_LINE_OP_NONE;
    g_assembly_line_ctx.next_tick = 0u;
    g_assembly_line_tick_pending = 0u;
    g_assembly_line_restore_needs_stdin_rebind = 0u;
    (void)terminal_stdin_release(AO_ASSEMBLY_LINE);
    if (g_assembly_line_sched != 0) {
        (void)sched_unregister(g_assembly_line_sched, AO_ASSEMBLY_LINE);
    }
}

static void assembly_line_begin_op(uint8_t op)
{
    g_assembly_line_ctx.pending_op = op;
    g_assembly_line_ctx.robot_state = ASSEMBLY_LINE_ROBOT_BUSY;
    g_assembly_line_ctx.next_tick = systick_now() + ASSEMBLY_LINE_OP_TICKS;
    g_assembly_line_tick_pending = 0u;
    assembly_line_render();
}

static void assembly_line_finish_scan(void)
{
    assembly_part_record_t *part = assembly_line_find_selected_part();

    if (part == 0 || part->process_state != ASSEMBLY_LINE_PART_RAW) {
        console_puts("line: nothing raw in selected slot\r\n");
        return;
    }

    part->scan_result = (uint8_t)((part->part_id & 1u) ? ASSEMBLY_LINE_SCAN_PASS
                                                       : ASSEMBLY_LINE_SCAN_FAIL);
    part->process_state = (part->scan_result == ASSEMBLY_LINE_SCAN_PASS)
                              ? ASSEMBLY_LINE_PART_SCANNED_OK
                              : ASSEMBLY_LINE_PART_SCANNED_BAD;
    g_assembly_line_ctx.robot_state = ASSEMBLY_LINE_ROBOT_HOME;
}

static void assembly_line_finish_pick(void)
{
    assembly_part_record_t *part = assembly_line_find_selected_part();

    if (g_assembly_line_ctx.fixture_slot != ASSEMBLY_LINE_NO_SLOT) {
        console_puts("line: fixture occupied\r\n");
        return;
    }
    if (part == 0 ||
        (part->process_state != ASSEMBLY_LINE_PART_SCANNED_OK &&
         part->process_state != ASSEMBLY_LINE_PART_SCANNED_BAD)) {
        console_puts("line: selected slot needs scanned part\r\n");
        return;
    }

    part->location = ASSEMBLY_LINE_LOC_FIXTURE;
    part->process_state = ASSEMBLY_LINE_PART_AT_FIXTURE;
    g_assembly_line_ctx.fixture_slot = g_assembly_line_ctx.selected_slot;
    g_assembly_line_ctx.robot_state = ASSEMBLY_LINE_ROBOT_FIXTURE;
}

static void assembly_line_finish_good(void)
{
    assembly_part_record_t *part = assembly_line_find_fixture_part();

    if (part == 0) {
        console_puts("line: fixture empty\r\n");
        return;
    }
    if (part->scan_result != ASSEMBLY_LINE_SCAN_PASS) {
        console_puts("line: only PASS parts can finish good\r\n");
        return;
    }

    part->location = ASSEMBLY_LINE_LOC_GOOD_BIN;
    part->process_state = ASSEMBLY_LINE_PART_DONE_GOOD;
    g_assembly_line_ctx.fixture_slot = ASSEMBLY_LINE_NO_SLOT;
    g_assembly_line_ctx.completed_count++;
    g_assembly_line_ctx.robot_state = ASSEMBLY_LINE_ROBOT_HOME;
}

static void assembly_line_finish_reject(void)
{
    assembly_part_record_t *part = assembly_line_find_fixture_part();

    if (part != 0) {
        part->location = ASSEMBLY_LINE_LOC_REJECT_BIN;
        part->process_state = ASSEMBLY_LINE_PART_DONE_REJECT;
        g_assembly_line_ctx.fixture_slot = ASSEMBLY_LINE_NO_SLOT;
        g_assembly_line_ctx.rejected_count++;
        g_assembly_line_ctx.robot_state = ASSEMBLY_LINE_ROBOT_HOME;
        return;
    }

    part = assembly_line_find_selected_part();
    if (part == 0 || part->process_state != ASSEMBLY_LINE_PART_SCANNED_BAD) {
        console_puts("line: selected slot has nothing to reject\r\n");
        return;
    }

    part->location = ASSEMBLY_LINE_LOC_REJECT_BIN;
    part->process_state = ASSEMBLY_LINE_PART_DONE_REJECT;
    g_assembly_line_ctx.rejected_count++;
    g_assembly_line_ctx.robot_state = ASSEMBLY_LINE_ROBOT_HOME;
}

static void assembly_line_complete_pending_op(void)
{
    uint8_t op = g_assembly_line_ctx.pending_op;

    g_assembly_line_ctx.pending_op = ASSEMBLY_LINE_OP_NONE;
    g_assembly_line_ctx.next_tick = 0u;
    g_assembly_line_tick_pending = 0u;

    if (op == ASSEMBLY_LINE_OP_HOME) {
        g_assembly_line_ctx.robot_state = ASSEMBLY_LINE_ROBOT_HOME;
        g_assembly_line_ctx.robot_needs_home = 0u;
    } else if (op == ASSEMBLY_LINE_OP_SCAN) {
        assembly_line_finish_scan();
    } else if (op == ASSEMBLY_LINE_OP_PICK) {
        assembly_line_finish_pick();
    } else if (op == ASSEMBLY_LINE_OP_FINISH) {
        assembly_line_finish_good();
    } else if (op == ASSEMBLY_LINE_OP_REJECT) {
        assembly_line_finish_reject();
    }

    assembly_line_check_complete();
    assembly_line_render();
}

static void assembly_line_task_dispatch(ao_t *self, const event_t *e)
{
    uint8_t key;

    (void)self;
    if (e == 0) {
        return;
    }

    if (e->sig == ASSEMBLY_LINE_SIG_START) {
        if (g_assembly_line_ctx.active) {
            console_puts("line: busy\r\n");
            return;
        }
        assembly_line_seed_new_run();
        assembly_line_render();
        return;
    }

    if (!g_assembly_line_ctx.active) {
        return;
    }

    if (e->sig == ASSEMBLY_LINE_SIG_TICK) {
        assembly_line_complete_pending_op();
        return;
    }

    if (e->sig != TERM_SIG_STDIN_RAW) {
        return;
    }

    key = (uint8_t)e->arg0;
    if (key == 'q' || key == 'Q') {
        assembly_line_stop();
        return;
    }
    if (g_assembly_line_ctx.pending_op != ASSEMBLY_LINE_OP_NONE) {
        return;
    }
    if (key == 'a' || key == 'A') {
        g_assembly_line_ctx.selected_slot =
            (uint8_t)((g_assembly_line_ctx.selected_slot + ASSEMBLY_LINE_PARTS - 1u) % ASSEMBLY_LINE_PARTS);
        assembly_line_render();
        return;
    }
    if (key == 'd' || key == 'D') {
        g_assembly_line_ctx.selected_slot =
            (uint8_t)((g_assembly_line_ctx.selected_slot + 1u) % ASSEMBLY_LINE_PARTS);
        assembly_line_render();
        return;
    }
    if (key == 'h' || key == 'H') {
        assembly_line_begin_op(ASSEMBLY_LINE_OP_HOME);
        return;
    }
    if (g_assembly_line_ctx.robot_needs_home) {
        console_puts("line: press h to home robot first\r\n");
        return;
    }
    if (g_assembly_line_ctx.ui_mode == ASSEMBLY_LINE_UI_COMPLETE) {
        console_puts("line: complete, press q to exit\r\n");
        return;
    }
    if (key == 's' || key == 'S') {
        assembly_line_begin_op(ASSEMBLY_LINE_OP_SCAN);
        return;
    }
    if (key == 'p' || key == 'P') {
        assembly_line_begin_op(ASSEMBLY_LINE_OP_PICK);
        return;
    }
    if (key == 'f' || key == 'F') {
        assembly_line_begin_op(ASSEMBLY_LINE_OP_FINISH);
        return;
    }
    if (key == 'x' || key == 'X') {
        assembly_line_begin_op(ASSEMBLY_LINE_OP_REJECT);
        return;
    }
}

int assembly_line_task_restore_state(const assembly_line_state_t *in)
{
    if (in == 0 || !assembly_line_state_valid(in)) {
        return SCHED_ERR_PARAM;
    }
    if (!in->active) {
        assembly_line_task_reset_state();
        if (g_assembly_line_sched != 0 && g_assembly_line_sched->table[AO_ASSEMBLY_LINE] != 0) {
            (void)sched_unregister(g_assembly_line_sched, AO_ASSEMBLY_LINE);
        }
        g_assembly_line_state_restored = 0u;
        return SCHED_OK;
    }

    g_assembly_line_ctx = *in;
    g_assembly_line_ctx.pending_op = ASSEMBLY_LINE_OP_NONE;
    g_assembly_line_ctx.next_tick = 0u;
    g_assembly_line_ctx.robot_state = (g_assembly_line_ctx.ui_mode == ASSEMBLY_LINE_UI_COMPLETE)
                                          ? ASSEMBLY_LINE_ROBOT_HOME
                                          : ASSEMBLY_LINE_ROBOT_HOME_REQUIRED;
    g_assembly_line_ctx.robot_needs_home =
        (uint8_t)((g_assembly_line_ctx.ui_mode == ASSEMBLY_LINE_UI_COMPLETE) ? 0u : 1u);
    g_assembly_line_tick_pending = 0u;
    g_assembly_line_state_restored = 1u;
    return SCHED_OK;
}

void assembly_line_task_restore_rebind_stdin_if_needed(void)
{
    int rc;

    if (!g_assembly_line_restore_needs_stdin_rebind) {
        return;
    }
    if (!g_assembly_line_ctx.active) {
        g_assembly_line_restore_needs_stdin_rebind = 0u;
        return;
    }
    if (g_assembly_line_sched == 0 || g_assembly_line_sched->table[AO_ASSEMBLY_LINE] == 0) {
        return;
    }

    rc = terminal_stdin_acquire(AO_ASSEMBLY_LINE, TERM_STDIN_MODE_RAW);
    if (rc == SCHED_OK || rc == SCHED_ERR_EXISTS) {
        g_assembly_line_restore_needs_stdin_rebind = 0u;
        assembly_line_render();
    }
}

int assembly_line_task_register(scheduler_t *sched)
{
    task_spec_t spec;
    int rc;

    if (sched == 0) {
        return SCHED_ERR_PARAM;
    }

    g_assembly_line_sched = sched;
    if (!g_assembly_line_state_restored) {
        assembly_line_task_reset_state();
    }
    g_assembly_line_state_restored = 0u;
    if (g_assembly_line_ctx.active) {
        g_assembly_line_restore_needs_stdin_rebind = 1u;
    }

    spec.id = AO_ASSEMBLY_LINE;
    spec.prio = 2;
    spec.dispatch = assembly_line_task_dispatch;
    spec.ctx = &g_assembly_line_ctx;
    spec.queue_storage = g_assembly_line_queue_storage;
    spec.queue_capacity =
        (uint16_t)(sizeof(g_assembly_line_queue_storage) / sizeof(g_assembly_line_queue_storage[0]));
    spec.rtc_budget_ticks = 1;
    spec.name = "line";

    rc = sched_register_task(sched, &spec);
    if (rc == SCHED_ERR_EXISTS) {
        return SCHED_OK;
    }
    return rc;
}

int assembly_line_task_request_start(void)
{
    int rc;

    if (g_assembly_line_sched == 0) {
        return SCHED_ERR_PARAM;
    }
    if (g_cmd_bg_ctx) {
        return SCHED_ERR_DISABLED;
    }

    rc = terminal_stdin_acquire(AO_ASSEMBLY_LINE, TERM_STDIN_MODE_RAW);
    if (rc != SCHED_OK) {
        return rc;
    }

    rc = sched_post(g_assembly_line_sched,
                    AO_ASSEMBLY_LINE,
                    &(event_t){ .sig = ASSEMBLY_LINE_SIG_START });
    if (rc != SCHED_OK) {
        (void)terminal_stdin_release(AO_ASSEMBLY_LINE);
        return rc;
    }

    g_assembly_line_restore_needs_stdin_rebind = 0u;
    g_cmd_fg_async = 1u;
    return SCHED_OK;
}

void assembly_line_task_systick_hook(void)
{
    if (g_assembly_line_sched == 0) {
        return;
    }
    if (g_assembly_line_sched->table[AO_ASSEMBLY_LINE] == 0) {
        g_assembly_line_ctx.active = 0u;
        g_assembly_line_ctx.pending_op = ASSEMBLY_LINE_OP_NONE;
        g_assembly_line_ctx.next_tick = 0u;
        g_assembly_line_tick_pending = 0u;
        return;
    }
    if (!g_assembly_line_ctx.active ||
        g_assembly_line_ctx.pending_op == ASSEMBLY_LINE_OP_NONE ||
        g_assembly_line_tick_pending ||
        g_assembly_line_ctx.next_tick == 0u) {
        return;
    }
    if ((int32_t)(systick_now() - g_assembly_line_ctx.next_tick) < 0) {
        return;
    }

    if (sched_post_isr(g_assembly_line_sched,
                       AO_ASSEMBLY_LINE,
                       &(event_t){ .sig = ASSEMBLY_LINE_SIG_TICK }) == SCHED_OK) {
        g_assembly_line_tick_pending = 1u;
    }
}

static int assembly_line_restore_register_fn(scheduler_t *sched, const launch_intent_t *intent)
{
    (void)intent;
    return assembly_line_task_register(sched);
}

static int assembly_line_restore_get_state_fn(void *out, uint32_t *io_len)
{
    assembly_line_state_t state;

    if (out == 0 || io_len == 0) {
        return SCHED_ERR_PARAM;
    }
    if (assembly_line_task_get_state(&state) != SCHED_OK) {
        return SCHED_ERR_PARAM;
    }
    if (!state.active) {
        return SCHED_ERR_NOT_FOUND;
    }
    return assembly_line_task_encode_state_blob(&state, out, io_len);
}

static int assembly_line_restore_apply_state_fn(const void *blob, uint32_t len)
{
    assembly_line_state_t state;
    int lock_rc;
    int rc = assembly_line_task_decode_state_blob(blob, len, &state);

    if (rc != SCHED_OK) {
        return rc;
    }
    rc = assembly_line_task_restore_state(&state);
    if (rc != SCHED_OK) {
        return rc;
    }
    if (!g_assembly_line_ctx.active) {
        return SCHED_OK;
    }

    g_assembly_line_restore_needs_stdin_rebind = 0u;
    lock_rc = terminal_stdin_acquire(AO_ASSEMBLY_LINE, TERM_STDIN_MODE_RAW);
    if (lock_rc == SCHED_OK || lock_rc == SCHED_ERR_EXISTS) {
        assembly_line_render();
        return SCHED_OK;
    }

    g_assembly_line_restore_needs_stdin_rebind = 1u;
    return SCHED_OK;
}

int assembly_line_task_register_restore_descriptor(void)
{
    static const restore_task_descriptor_t desc = {
        .task_id = AO_ASSEMBLY_LINE,
        .task_class = TASK_CLASS_RESTORABLE_NOW,
        .state_version = 1u,
        .min_state_len = sizeof(assembly_line_state_t),
        .max_state_len = sizeof(assembly_line_state_t),
        .register_fn = assembly_line_restore_register_fn,
        .get_state_fn = assembly_line_restore_get_state_fn,
        .restore_fn = assembly_line_restore_apply_state_fn,
        .ui_rehydrate_fn = 0
    };
    return restore_registry_register_descriptor(&desc);
}
