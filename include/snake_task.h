#pragma once

#include "scheduler.h"
#include "stdint.h"

#define SNAKE_BOARD_W 20u
#define SNAKE_BOARD_H 12u
#define SNAKE_MAX_CELLS (SNAKE_BOARD_W * SNAKE_BOARD_H)

enum {
    SNAKE_UI_TITLE = 1,
    SNAKE_UI_PLAYING = 2,
    SNAKE_UI_PAUSED = 3,
    SNAKE_UI_GAME_OVER = 4
};

enum {
    SNAKE_DIR_UP = 1,
    SNAKE_DIR_RIGHT = 2,
    SNAKE_DIR_DOWN = 3,
    SNAKE_DIR_LEFT = 4
};

enum {
    SNAKE_GAME_OVER_NONE = 0,
    SNAKE_GAME_OVER_WALL = 1,
    SNAKE_GAME_OVER_SELF = 2,
    SNAKE_GAME_OVER_WIN = 3
};

typedef struct {
    uint8_t x;
    uint8_t y;
} snake_cell_t;

typedef struct {
    uint8_t active;
    uint8_t ui_mode;
    uint8_t dir;
    uint8_t pending_dir;
    uint8_t length;
    uint8_t head_idx;
    uint8_t food_x;
    uint8_t food_y;
    uint8_t game_over_reason;
    uint8_t reserved0[3];
    uint32_t score;
    uint32_t tick_interval_ms;
    uint32_t next_tick;
    snake_cell_t body[SNAKE_MAX_CELLS];
} snake_game_state_t;

int snake_task_register(scheduler_t *sched);
int snake_task_request_start(void);
void snake_task_systick_hook(void);
int snake_task_register_restore_descriptor(void);
void snake_task_restore_rebind_stdin_if_needed(void);

int snake_task_get_state(snake_game_state_t *out);
int snake_task_restore_state(const snake_game_state_t *in);
void snake_task_reset_state(void);
int snake_task_encode_state_blob(const snake_game_state_t *state,
                                 void *out,
                                 uint32_t *io_len);
int snake_task_decode_state_blob(const void *blob,
                                 uint32_t len,
                                 snake_game_state_t *out);
