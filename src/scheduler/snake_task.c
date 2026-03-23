#include "../../include/snake_task.h"
#include "../../include/cmd_context.h"
#include "../../include/console.h"
#include "../../include/restore_registry.h"
#include "../../include/systick.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/task_spec.h"
#include "../../include/terminal.h"

#define SNAKE_DEFAULT_TICK_MS 160u
#define SNAKE_RENDER_BUF_SIZE 768u

static snake_game_state_t g_snake_ctx;
static event_t g_snake_queue_storage[8];
static scheduler_t *g_snake_sched;
static uint8_t g_snake_state_restored;
static uint8_t g_snake_restore_needs_stdin_rebind;
static uint8_t g_snake_tick_pending;
static uint8_t g_snake_full_redraw;
static char g_snake_render_buf[SNAKE_RENDER_BUF_SIZE];

static void snake_render(void);
static char snake_board_char(uint8_t x, uint8_t y);

static void snake_state_copy(snake_game_state_t *dst, const snake_game_state_t *src)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0u; i < sizeof(snake_game_state_t); i++) {
        d[i] = s[i];
    }
}

static uint16_t snake_render_append_char(uint16_t off, char c)
{
    if (off < SNAKE_RENDER_BUF_SIZE) {
        g_snake_render_buf[off++] = c;
    }
    return off;
}

static uint16_t snake_render_append_str(uint16_t off, const char *s)
{
    if (s == 0) {
        return off;
    }
    while (*s != '\0' && off < SNAKE_RENDER_BUF_SIZE) {
        g_snake_render_buf[off++] = *s++;
    }
    return off;
}

static uint16_t snake_render_append_u32(uint16_t off, uint32_t v)
{
    char tmp[10];
    uint8_t n = 0u;

    if (v == 0u) {
        return snake_render_append_char(off, '0');
    }

    while (v > 0u && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0u) {
        off = snake_render_append_char(off, tmp[--n]);
    }
    return off;
}

static uint16_t snake_render_append_border(uint16_t off)
{
    off = snake_render_append_str(off, "\x1b[2K");
    off = snake_render_append_char(off, '+');
    for (uint8_t x = 0u; x < SNAKE_BOARD_W; x++) {
        off = snake_render_append_char(off, '-');
    }
    off = snake_render_append_str(off, "+\r\n");
    return off;
}

static uint16_t snake_render_append_line_start(uint16_t off)
{
    return snake_render_append_str(off, "\x1b[2K");
}

static uint16_t snake_render_append_board(uint16_t off)
{
    off = snake_render_append_border(off);
    for (uint8_t y = 0u; y < SNAKE_BOARD_H; y++) {
        off = snake_render_append_char(off, '|');
        for (uint8_t x = 0u; x < SNAKE_BOARD_W; x++) {
            off = snake_render_append_char(off, snake_board_char(x, y));
        }
        off = snake_render_append_str(off, "|\r\n");
    }
    off = snake_render_append_border(off);
    return off;
}

static void snake_zero_body(void)
{
    for (uint32_t i = 0u; i < SNAKE_MAX_CELLS; i++) {
        g_snake_ctx.body[i].x = 0u;
        g_snake_ctx.body[i].y = 0u;
    }
}

void snake_task_reset_state(void)
{
    g_snake_ctx.active = 0u;
    g_snake_ctx.ui_mode = SNAKE_UI_TITLE;
    g_snake_ctx.dir = SNAKE_DIR_RIGHT;
    g_snake_ctx.pending_dir = SNAKE_DIR_RIGHT;
    g_snake_ctx.length = 0u;
    g_snake_ctx.head_idx = 0u;
    g_snake_ctx.food_x = 0u;
    g_snake_ctx.food_y = 0u;
    g_snake_ctx.game_over_reason = SNAKE_GAME_OVER_NONE;
    g_snake_ctx.reserved0[0] = 0u;
    g_snake_ctx.reserved0[1] = 0u;
    g_snake_ctx.reserved0[2] = 0u;
    g_snake_ctx.score = 0u;
    g_snake_ctx.tick_interval_ms = SNAKE_DEFAULT_TICK_MS;
    g_snake_ctx.next_tick = 0u;
    snake_zero_body();
    g_snake_restore_needs_stdin_rebind = 0u;
    g_snake_tick_pending = 0u;
    g_snake_full_redraw = 1u;
}

static void snake_seed_new_session(void)
{
    snake_task_reset_state();
    g_snake_ctx.active = 1u;
}

static uint8_t snake_dir_valid(uint8_t dir)
{
    return (uint8_t)(dir >= SNAKE_DIR_UP && dir <= SNAKE_DIR_LEFT);
}

static uint8_t snake_state_tail_index(const snake_game_state_t *state)
{
    return (uint8_t)((state->head_idx + SNAKE_MAX_CELLS + 1u - state->length) % SNAKE_MAX_CELLS);
}

static int snake_state_valid(const snake_game_state_t *state)
{
    if (state == 0) {
        return 0;
    }
    if (state->active > 1u) {
        return 0;
    }
    if (state->ui_mode < SNAKE_UI_TITLE || state->ui_mode > SNAKE_UI_GAME_OVER) {
        return 0;
    }
    if (!snake_dir_valid(state->dir) || !snake_dir_valid(state->pending_dir)) {
        return 0;
    }
    if (state->length > SNAKE_MAX_CELLS) {
        return 0;
    }
    if (state->head_idx >= SNAKE_MAX_CELLS) {
        return 0;
    }
    if (state->food_x >= SNAKE_BOARD_W || state->food_y >= SNAKE_BOARD_H) {
        if (!(state->ui_mode == SNAKE_UI_GAME_OVER && state->game_over_reason == SNAKE_GAME_OVER_WIN)) {
            return 0;
        }
    }
    if (state->game_over_reason > SNAKE_GAME_OVER_WIN) {
        return 0;
    }
    if (state->active && state->ui_mode != SNAKE_UI_TITLE && state->length == 0u) {
        return 0;
    }
    for (uint32_t i = 0u; i < state->length; i++) {
        uint32_t idx = (snake_state_tail_index(state) + i) % SNAKE_MAX_CELLS;
        const snake_cell_t *cell = &state->body[idx];
        if (cell->x >= SNAKE_BOARD_W || cell->y >= SNAKE_BOARD_H) {
            return 0;
        }
        for (uint32_t j = i + 1u; j < state->length; j++) {
            uint32_t other_idx = (snake_state_tail_index(state) + j) % SNAKE_MAX_CELLS;
            const snake_cell_t *other = &state->body[other_idx];
            if (cell->x == other->x && cell->y == other->y) {
                return 0;
            }
        }
    }
    return 1;
}

int snake_task_get_state(snake_game_state_t *out)
{
    if (out == 0) {
        return SCHED_ERR_PARAM;
    }
    snake_state_copy(out, &g_snake_ctx);
    return SCHED_OK;
}

int snake_task_encode_state_blob(const snake_game_state_t *state,
                                 void *out,
                                 uint32_t *io_len)
{
    if (state == 0 || out == 0 || io_len == 0) {
        return SCHED_ERR_PARAM;
    }
    if (!snake_state_valid(state)) {
        return SCHED_ERR_PARAM;
    }
    if (*io_len < sizeof(snake_game_state_t)) {
        return SCHED_ERR_PARAM;
    }
    snake_state_copy((snake_game_state_t *)out, state);
    *io_len = (uint32_t)sizeof(snake_game_state_t);
    return SCHED_OK;
}

int snake_task_decode_state_blob(const void *blob,
                                 uint32_t len,
                                 snake_game_state_t *out)
{
    if (blob == 0 || out == 0) {
        return SCHED_ERR_PARAM;
    }
    if (len != sizeof(snake_game_state_t)) {
        return SCHED_ERR_PARAM;
    }
    snake_state_copy(out, (const snake_game_state_t *)blob);
    if (!snake_state_valid(out)) {
        return SCHED_ERR_PARAM;
    }
    return SCHED_OK;
}

static uint8_t snake_tail_index(void)
{
    return (uint8_t)((g_snake_ctx.head_idx + SNAKE_MAX_CELLS + 1u - g_snake_ctx.length) % SNAKE_MAX_CELLS);
}

static snake_cell_t snake_body_cell_from_tail(uint8_t offset)
{
    uint8_t idx = (uint8_t)((snake_tail_index() + offset) % SNAKE_MAX_CELLS);
    return g_snake_ctx.body[idx];
}

static uint32_t snake_mix_u32(uint32_t seed, uint32_t value)
{
    seed ^= value + 0x9E3779B9u + (seed << 6) + (seed >> 2);
    return seed;
}

static uint8_t snake_dir_is_opposite(uint8_t a, uint8_t b)
{
    return (uint8_t)((a == SNAKE_DIR_UP && b == SNAKE_DIR_DOWN) ||
                     (a == SNAKE_DIR_DOWN && b == SNAKE_DIR_UP) ||
                     (a == SNAKE_DIR_LEFT && b == SNAKE_DIR_RIGHT) ||
                     (a == SNAKE_DIR_RIGHT && b == SNAKE_DIR_LEFT));
}

static uint8_t snake_cell_occupied(uint8_t x, uint8_t y, uint8_t ignore_tail)
{
    uint8_t start = ignore_tail ? 1u : 0u;

    for (uint8_t i = start; i < g_snake_ctx.length; i++) {
        snake_cell_t cell = snake_body_cell_from_tail(i);
        if (cell.x == x && cell.y == y) {
            return 1u;
        }
    }
    return 0u;
}

static void snake_spawn_food(void)
{
    uint32_t seed = 0x51A7E5D3u;
    uint32_t start;

    seed = snake_mix_u32(seed, g_snake_ctx.score);
    seed = snake_mix_u32(seed, (uint32_t)g_snake_ctx.length);
    seed = snake_mix_u32(seed, (uint32_t)g_snake_ctx.dir);
    seed = snake_mix_u32(seed, (uint32_t)g_snake_ctx.pending_dir);
    for (uint8_t i = 0u; i < g_snake_ctx.length; i++) {
        snake_cell_t cell = snake_body_cell_from_tail(i);
        seed = snake_mix_u32(seed, ((uint32_t)cell.x << 8) | (uint32_t)cell.y);
    }

    start = seed % SNAKE_MAX_CELLS;
    for (uint32_t step = 0u; step < SNAKE_MAX_CELLS; step++) {
        uint32_t idx = (start + (step * 97u)) % SNAKE_MAX_CELLS;
        uint8_t x = (uint8_t)(idx % SNAKE_BOARD_W);
        uint8_t y = (uint8_t)(idx / SNAKE_BOARD_W);
        if (!snake_cell_occupied(x, y, 0u)) {
            g_snake_ctx.food_x = x;
            g_snake_ctx.food_y = y;
            return;
        }
    }

    g_snake_ctx.ui_mode = SNAKE_UI_GAME_OVER;
    g_snake_ctx.game_over_reason = SNAKE_GAME_OVER_WIN;
}

static void snake_start_game(void)
{
    uint8_t start_x = (uint8_t)(SNAKE_BOARD_W / 2u);
    uint8_t start_y = (uint8_t)(SNAKE_BOARD_H / 2u);

    snake_zero_body();
    g_snake_ctx.ui_mode = SNAKE_UI_PLAYING;
    g_snake_ctx.dir = SNAKE_DIR_RIGHT;
    g_snake_ctx.pending_dir = SNAKE_DIR_RIGHT;
    g_snake_ctx.length = 3u;
    g_snake_ctx.head_idx = 2u;
    g_snake_ctx.body[0].x = (uint8_t)(start_x - 2u);
    g_snake_ctx.body[0].y = start_y;
    g_snake_ctx.body[1].x = (uint8_t)(start_x - 1u);
    g_snake_ctx.body[1].y = start_y;
    g_snake_ctx.body[2].x = start_x;
    g_snake_ctx.body[2].y = start_y;
    g_snake_ctx.score = 0u;
    g_snake_ctx.game_over_reason = SNAKE_GAME_OVER_NONE;
    g_snake_ctx.next_tick = systick_now() + g_snake_ctx.tick_interval_ms;
    g_snake_tick_pending = 0u;
    g_snake_full_redraw = 1u;
    snake_spawn_food();
}

static const char *snake_mode_name(uint8_t mode)
{
    if (mode == SNAKE_UI_TITLE) return "TITLE";
    if (mode == SNAKE_UI_PLAYING) return "PLAYING";
    if (mode == SNAKE_UI_PAUSED) return "PAUSED";
    if (mode == SNAKE_UI_GAME_OVER) return "GAME_OVER";
    return "?";
}

static const char *snake_game_over_name(uint8_t reason)
{
    if (reason == SNAKE_GAME_OVER_WALL) return "wall";
    if (reason == SNAKE_GAME_OVER_SELF) return "self";
    if (reason == SNAKE_GAME_OVER_WIN) return "win";
    return "none";
}

static char snake_board_char(uint8_t x, uint8_t y)
{
    if (g_snake_ctx.ui_mode != SNAKE_UI_TITLE &&
        g_snake_ctx.game_over_reason != SNAKE_GAME_OVER_WIN &&
        g_snake_ctx.food_x == x && g_snake_ctx.food_y == y) {
        return '*';
    }

    for (uint8_t i = 0u; i < g_snake_ctx.length; i++) {
        snake_cell_t cell = snake_body_cell_from_tail(i);
        if (cell.x == x && cell.y == y) {
            return (i + 1u == g_snake_ctx.length) ? '@' : 'o';
        }
    }
    return '.';
}

static void snake_render(void)
{
    uint16_t len = 0u;

    if (g_snake_full_redraw) {
        len = snake_render_append_str(len, "\x1b[2J\x1b[H");
    } else {
        len = snake_render_append_str(len, "\x1b[H");
    }

    len = snake_render_append_line_start(len);
    len = snake_render_append_str(len, "SNAKE\r\n");
    len = snake_render_append_line_start(len);
    len = snake_render_append_str(len, "mode=");
    len = snake_render_append_str(len, snake_mode_name(g_snake_ctx.ui_mode));
    len = snake_render_append_str(len, " score=");
    len = snake_render_append_u32(len, g_snake_ctx.score);
    len = snake_render_append_str(len, " len=");
    len = snake_render_append_u32(len, g_snake_ctx.length);
    len = snake_render_append_str(len, " area=");
    len = snake_render_append_u32(len, SNAKE_BOARD_W);
    len = snake_render_append_char(len, 'x');
    len = snake_render_append_u32(len, SNAKE_BOARD_H);
    len = snake_render_append_str(len, "\r\n");

    len = snake_render_append_line_start(len);
    if (g_snake_ctx.ui_mode == SNAKE_UI_TITLE) {
        len = snake_render_append_str(len, "press r to start, q to quit\r\n");
    } else if (g_snake_ctx.ui_mode == SNAKE_UI_PAUSED) {
        len = snake_render_append_str(len, "paused: p resume, r restart, q quit\r\n");
    } else if (g_snake_ctx.ui_mode == SNAKE_UI_GAME_OVER) {
        len = snake_render_append_str(len, "game over: ");
        len = snake_render_append_str(len, snake_game_over_name(g_snake_ctx.game_over_reason));
        len = snake_render_append_str(len, "  press r to restart or q to quit\r\n");
    } else {
        len = snake_render_append_str(len, "w/a/s/d move  p pause  r restart  q quit\r\n");
    }

    len = snake_render_append_board(len);
    g_snake_full_redraw = 0u;
    (void)console_write(g_snake_render_buf, len);
}

static void snake_stop(void)
{
    g_snake_ctx.active = 0u;
    g_snake_ctx.next_tick = 0u;
    g_snake_tick_pending = 0u;
    g_snake_restore_needs_stdin_rebind = 0u;
    g_snake_full_redraw = 1u;
    (void)console_puts("\x1b[2J\x1b[H");
    (void)terminal_stdin_release(AO_SNAKE);
    if (g_snake_sched != 0) {
        (void)sched_unregister(g_snake_sched, AO_SNAKE);
    }
}

static void snake_game_over(uint8_t reason)
{
    g_snake_ctx.ui_mode = SNAKE_UI_GAME_OVER;
    g_snake_ctx.game_over_reason = reason;
    g_snake_ctx.next_tick = 0u;
    g_snake_tick_pending = 0u;
    snake_render();
}

static void snake_step(void)
{
    snake_cell_t head;
    uint8_t new_x;
    uint8_t new_y;
    uint8_t grow;
    uint8_t new_head_idx;

    g_snake_tick_pending = 0u;
    if (!g_snake_ctx.active || g_snake_ctx.ui_mode != SNAKE_UI_PLAYING || g_snake_ctx.length == 0u) {
        return;
    }

    if (!snake_dir_is_opposite(g_snake_ctx.dir, g_snake_ctx.pending_dir) || g_snake_ctx.length <= 1u) {
        g_snake_ctx.dir = g_snake_ctx.pending_dir;
    }

    head = g_snake_ctx.body[g_snake_ctx.head_idx];
    new_x = head.x;
    new_y = head.y;
    if (g_snake_ctx.dir == SNAKE_DIR_UP) {
        if (new_y == 0u) {
            snake_game_over(SNAKE_GAME_OVER_WALL);
            return;
        }
        new_y--;
    } else if (g_snake_ctx.dir == SNAKE_DIR_DOWN) {
        new_y++;
        if (new_y >= SNAKE_BOARD_H) {
            snake_game_over(SNAKE_GAME_OVER_WALL);
            return;
        }
    } else if (g_snake_ctx.dir == SNAKE_DIR_LEFT) {
        if (new_x == 0u) {
            snake_game_over(SNAKE_GAME_OVER_WALL);
            return;
        }
        new_x--;
    } else {
        new_x++;
        if (new_x >= SNAKE_BOARD_W) {
            snake_game_over(SNAKE_GAME_OVER_WALL);
            return;
        }
    }

    grow = (uint8_t)((g_snake_ctx.food_x == new_x && g_snake_ctx.food_y == new_y) ? 1u : 0u);
    if (snake_cell_occupied(new_x, new_y, (uint8_t)(!grow))) {
        snake_game_over(SNAKE_GAME_OVER_SELF);
        return;
    }

    new_head_idx = (uint8_t)((g_snake_ctx.head_idx + 1u) % SNAKE_MAX_CELLS);
    g_snake_ctx.body[new_head_idx].x = new_x;
    g_snake_ctx.body[new_head_idx].y = new_y;
    g_snake_ctx.head_idx = new_head_idx;

    if (grow) {
        if (g_snake_ctx.length < SNAKE_MAX_CELLS) {
            g_snake_ctx.length++;
        }
        g_snake_ctx.score++;
        if (g_snake_ctx.length >= SNAKE_MAX_CELLS) {
            snake_game_over(SNAKE_GAME_OVER_WIN);
            return;
        }
        snake_spawn_food();
        if (g_snake_ctx.ui_mode == SNAKE_UI_GAME_OVER) {
            snake_render();
            return;
        }
    }

    g_snake_ctx.next_tick = systick_now() + g_snake_ctx.tick_interval_ms;
    snake_render();
}

static void snake_task_dispatch(ao_t *self, const event_t *e)
{
    uint8_t key;
    uint8_t new_dir;

    (void)self;
    if (e == 0) {
        return;
    }

    if (e->sig == SNAKE_SIG_START) {
        if (g_snake_ctx.active) {
            console_puts("snake: busy\r\n");
            return;
        }
        snake_seed_new_session();
        snake_render();
        return;
    }

    if (!g_snake_ctx.active) {
        return;
    }

    if (e->sig == SNAKE_SIG_TICK) {
        snake_step();
        return;
    }
    if (e->sig != TERM_SIG_STDIN_RAW) {
        return;
    }

    key = (uint8_t)e->arg0;
    if (key == 'q' || key == 'Q') {
        snake_stop();
        return;
    }
    if (key == 'r' || key == 'R') {
        snake_start_game();
        snake_render();
        return;
    }
    if (key == 'p' || key == 'P') {
        if (g_snake_ctx.ui_mode == SNAKE_UI_PLAYING) {
            g_snake_ctx.ui_mode = SNAKE_UI_PAUSED;
            g_snake_ctx.next_tick = 0u;
            g_snake_tick_pending = 0u;
            snake_render();
        } else if (g_snake_ctx.ui_mode == SNAKE_UI_PAUSED) {
            g_snake_ctx.ui_mode = SNAKE_UI_PLAYING;
            g_snake_ctx.next_tick = systick_now() + g_snake_ctx.tick_interval_ms;
            g_snake_tick_pending = 0u;
            snake_render();
        }
        return;
    }
    if (g_snake_ctx.ui_mode != SNAKE_UI_PLAYING && g_snake_ctx.ui_mode != SNAKE_UI_PAUSED) {
        return;
    }

    if (key == 'w' || key == 'W') {
        new_dir = SNAKE_DIR_UP;
    } else if (key == 'd' || key == 'D') {
        new_dir = SNAKE_DIR_RIGHT;
    } else if (key == 's' || key == 'S') {
        new_dir = SNAKE_DIR_DOWN;
    } else if (key == 'a' || key == 'A') {
        new_dir = SNAKE_DIR_LEFT;
    } else {
        return;
    }

    if (!snake_dir_is_opposite(g_snake_ctx.dir, new_dir) || g_snake_ctx.length <= 1u) {
        g_snake_ctx.pending_dir = new_dir;
    }
}

int snake_task_restore_state(const snake_game_state_t *in)
{
    if (in == 0 || !snake_state_valid(in)) {
        return SCHED_ERR_PARAM;
    }
    if (!in->active) {
        snake_task_reset_state();
        if (g_snake_sched != 0 && g_snake_sched->table[AO_SNAKE] != 0) {
            (void)sched_unregister(g_snake_sched, AO_SNAKE);
        }
        g_snake_state_restored = 0u;
        return SCHED_OK;
    }

    snake_state_copy(&g_snake_ctx, in);
    g_snake_tick_pending = 0u;
    if (g_snake_ctx.ui_mode == SNAKE_UI_PLAYING) {
        g_snake_ctx.next_tick = systick_now() + g_snake_ctx.tick_interval_ms;
    } else {
        g_snake_ctx.next_tick = 0u;
    }
    g_snake_full_redraw = 1u;
    g_snake_state_restored = 1u;
    return SCHED_OK;
}

void snake_task_restore_rebind_stdin_if_needed(void)
{
    int rc;

    if (!g_snake_restore_needs_stdin_rebind) {
        return;
    }
    if (!g_snake_ctx.active) {
        g_snake_restore_needs_stdin_rebind = 0u;
        return;
    }
    if (g_snake_sched == 0 || g_snake_sched->table[AO_SNAKE] == 0) {
        return;
    }

    rc = terminal_stdin_acquire(AO_SNAKE, TERM_STDIN_MODE_RAW);
    if (rc == SCHED_OK || rc == SCHED_ERR_EXISTS) {
        g_snake_restore_needs_stdin_rebind = 0u;
        snake_render();
    }
}

int snake_task_register(scheduler_t *sched)
{
    task_spec_t spec;
    int rc;

    if (sched == 0) {
        return SCHED_ERR_PARAM;
    }

    g_snake_sched = sched;
    if (!g_snake_state_restored) {
        snake_task_reset_state();
    }
    g_snake_state_restored = 0u;
    if (g_snake_ctx.active) {
        g_snake_restore_needs_stdin_rebind = 1u;
    }

    spec.id = AO_SNAKE;
    spec.prio = 2;
    spec.dispatch = snake_task_dispatch;
    spec.ctx = &g_snake_ctx;
    spec.queue_storage = g_snake_queue_storage;
    spec.queue_capacity = (uint16_t)(sizeof(g_snake_queue_storage) / sizeof(g_snake_queue_storage[0]));
    spec.rtc_budget_ticks = 1;
    spec.name = "snake";

    rc = sched_register_task(sched, &spec);
    if (rc == SCHED_ERR_EXISTS) {
        return SCHED_OK;
    }
    return rc;
}

int snake_task_request_start(void)
{
    int rc;

    if (g_snake_sched == 0) {
        return SCHED_ERR_PARAM;
    }
    if (g_cmd_bg_ctx) {
        return SCHED_ERR_DISABLED;
    }

    rc = terminal_stdin_acquire(AO_SNAKE, TERM_STDIN_MODE_RAW);
    if (rc != SCHED_OK) {
        return rc;
    }

    rc = sched_post(g_snake_sched, AO_SNAKE, &(event_t){ .sig = SNAKE_SIG_START });
    if (rc != SCHED_OK) {
        (void)terminal_stdin_release(AO_SNAKE);
        return rc;
    }

    g_snake_restore_needs_stdin_rebind = 0u;
    g_cmd_fg_async = 1u;
    return SCHED_OK;
}

void snake_task_systick_hook(void)
{
    if (g_snake_sched == 0) {
        return;
    }
    if (g_snake_sched->table[AO_SNAKE] == 0) {
        g_snake_ctx.active = 0u;
        g_snake_ctx.next_tick = 0u;
        g_snake_tick_pending = 0u;
        return;
    }
    if (!g_snake_ctx.active ||
        g_snake_ctx.ui_mode != SNAKE_UI_PLAYING ||
        g_snake_tick_pending ||
        g_snake_ctx.next_tick == 0u) {
        return;
    }
    if ((int32_t)(systick_now() - g_snake_ctx.next_tick) < 0) {
        return;
    }

    if (sched_post_isr(g_snake_sched,
                       AO_SNAKE,
                       &(event_t){ .sig = SNAKE_SIG_TICK }) == SCHED_OK) {
        g_snake_tick_pending = 1u;
    }
}

static int snake_restore_register_fn(scheduler_t *sched, const launch_intent_t *intent)
{
    (void)intent;
    return snake_task_register(sched);
}

static int snake_restore_get_state_fn(void *out, uint32_t *io_len)
{
    snake_game_state_t state;

    if (out == 0 || io_len == 0) {
        return SCHED_ERR_PARAM;
    }
    if (snake_task_get_state(&state) != SCHED_OK) {
        return SCHED_ERR_PARAM;
    }
    if (!state.active) {
        return SCHED_ERR_NOT_FOUND;
    }
    return snake_task_encode_state_blob(&state, out, io_len);
}

static int snake_restore_apply_state_fn(const void *blob, uint32_t len)
{
    snake_game_state_t state;
    int lock_rc;
    int rc = snake_task_decode_state_blob(blob, len, &state);

    if (rc != SCHED_OK) {
        return rc;
    }
    rc = snake_task_restore_state(&state);
    if (rc != SCHED_OK) {
        return rc;
    }
    if (!g_snake_ctx.active) {
        return SCHED_OK;
    }

    g_snake_restore_needs_stdin_rebind = 0u;
    lock_rc = terminal_stdin_acquire(AO_SNAKE, TERM_STDIN_MODE_RAW);
    if (lock_rc == SCHED_OK || lock_rc == SCHED_ERR_EXISTS) {
        snake_render();
        return SCHED_OK;
    }

    g_snake_restore_needs_stdin_rebind = 1u;
    return SCHED_OK;
}

int snake_task_register_restore_descriptor(void)
{
    static const restore_task_descriptor_t desc = {
        .task_id = AO_SNAKE,
        .task_class = TASK_CLASS_RESTORABLE_NOW,
        .state_version = 2u,
        .min_state_len = sizeof(snake_game_state_t),
        .max_state_len = sizeof(snake_game_state_t),
        .register_fn = snake_restore_register_fn,
        .get_state_fn = snake_restore_get_state_fn,
        .restore_fn = snake_restore_apply_state_fn,
        .ui_rehydrate_fn = 0
    };
    return restore_registry_register_descriptor(&desc);
}
