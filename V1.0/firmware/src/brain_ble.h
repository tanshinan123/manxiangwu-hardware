#ifndef BRAIN_BLE_H
#define BRAIN_BLE_H

#include <stdbool.h>

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*brain_ble_score_cb_t)(uint8_t state_index, uint8_t score);

/* Fired on the NimBLE host task whenever the phone link comes up or goes down.
 * The UI uses it to hand control back to local touch after the phone leaves,
 * matching the plan's "BLE lost -> keep last score, allow offline demo". */
typedef void (*brain_ble_link_cb_t)(bool connected);

/* One decoded protocol frame (plan section 5). All pointers stay valid only for
 * the duration of the callback. */
typedef struct {
    const char *type;         /* eventType, e.g. "STATE_SYNC" */
    const char *session_id;   /* "" when the frame carries none */
    const char *event_id;     /* "" when the frame carries none */
    uint8_t brain_score;      /* 0xFF when absent */
    uint32_t seconds;         /* remaining span for FOCUS_*, 0 when absent */
    const char *break_type;   /* BREAK_ACTION detail, NULL otherwise */
    uint8_t skin_mask;        /* STATE_SYNC/APP_LAUNCH unlocked Skin bitmask */
    const char *wearing;      /* Current Skin raw value, NULL for none */
    const char *content_kind; /* CONTENT_SYNC: postcard / insight */
    int content_index;
    bool content_present;
    const char *title;
    const char *text;
    int image_index;       /* CONTENT_SYNC postcard: brain image 0..13, -1 absent */
} brain_ble_frame_t;

typedef void (*brain_ble_frame_cb_t)(const brain_ble_frame_t *frame);

OPERATE_RET brain_ble_init(brain_ble_score_cb_t score_cb);
void brain_ble_set_link_callback(brain_ble_link_cb_t link_cb);
void brain_ble_set_frame_callback(brain_ble_frame_cb_t frame_cb);
/* Device -> phone event. Safe to call from any task. */
void brain_ble_emit(const char *event_type, const char *detail);
/* Keep ACK frames truthful after the UI applied a score. */
void brain_ble_note_score(uint8_t score);

#ifdef __cplusplus
}
#endif

#endif
