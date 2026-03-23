#pragma once

#include "scheduler.h"
#include "stdint.h"

#define ASSEMBLY_LINE_PARTS 4u
#define ASSEMBLY_LINE_NO_SLOT 0xFFu

enum {
    ASSEMBLY_LINE_UI_RUNNING = 1,
    ASSEMBLY_LINE_UI_COMPLETE = 2
};

enum {
    ASSEMBLY_LINE_ROBOT_HOME = 1,
    ASSEMBLY_LINE_ROBOT_BUSY = 2,
    ASSEMBLY_LINE_ROBOT_FIXTURE = 3,
    ASSEMBLY_LINE_ROBOT_HOME_REQUIRED = 4
};

enum {
    ASSEMBLY_LINE_OP_NONE = 0,
    ASSEMBLY_LINE_OP_HOME = 1,
    ASSEMBLY_LINE_OP_SCAN = 2,
    ASSEMBLY_LINE_OP_PICK = 3,
    ASSEMBLY_LINE_OP_FINISH = 4,
    ASSEMBLY_LINE_OP_REJECT = 5
};

enum {
    ASSEMBLY_LINE_SCAN_UNKNOWN = 0,
    ASSEMBLY_LINE_SCAN_PASS = 1,
    ASSEMBLY_LINE_SCAN_FAIL = 2
};

enum {
    ASSEMBLY_LINE_LOC_SLOT0 = 0,
    ASSEMBLY_LINE_LOC_SLOT1 = 1,
    ASSEMBLY_LINE_LOC_SLOT2 = 2,
    ASSEMBLY_LINE_LOC_SLOT3 = 3,
    ASSEMBLY_LINE_LOC_FIXTURE = 4,
    ASSEMBLY_LINE_LOC_GOOD_BIN = 5,
    ASSEMBLY_LINE_LOC_REJECT_BIN = 6
};

enum {
    ASSEMBLY_LINE_PART_RAW = 0,
    ASSEMBLY_LINE_PART_SCANNED_OK = 1,
    ASSEMBLY_LINE_PART_SCANNED_BAD = 2,
    ASSEMBLY_LINE_PART_AT_FIXTURE = 3,
    ASSEMBLY_LINE_PART_DONE_GOOD = 4,
    ASSEMBLY_LINE_PART_DONE_REJECT = 5
};

typedef struct {
    uint16_t part_id;
    uint8_t scan_result;
    uint8_t location;
    uint8_t process_state;
    uint8_t reserved[3];
} assembly_part_record_t;

typedef struct {
    uint8_t active;
    uint8_t ui_mode;
    uint8_t selected_slot;
    uint8_t robot_state;
    uint8_t robot_needs_home;
    uint8_t pending_op;
    uint8_t fixture_slot;
    uint8_t reserved0;
    uint32_t current_part_seq;
    uint32_t completed_count;
    uint32_t rejected_count;
    uint32_t next_tick;
    assembly_part_record_t parts[ASSEMBLY_LINE_PARTS];
} assembly_line_state_t;

int assembly_line_task_register(scheduler_t *sched);
int assembly_line_task_request_start(void);
void assembly_line_task_systick_hook(void);
int assembly_line_task_register_restore_descriptor(void);
void assembly_line_task_restore_rebind_stdin_if_needed(void);

int assembly_line_task_get_state(assembly_line_state_t *out);
int assembly_line_task_restore_state(const assembly_line_state_t *in);
void assembly_line_task_reset_state(void);
int assembly_line_task_encode_state_blob(const assembly_line_state_t *state,
                                         void *out,
                                         uint32_t *io_len);
int assembly_line_task_decode_state_blob(const void *blob,
                                         uint32_t len,
                                         assembly_line_state_t *out);
