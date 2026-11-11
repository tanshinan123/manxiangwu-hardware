#ifndef BRAIN_UI_H
#define BRAIN_UI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BRAIN_UI_BREAK_WATER = 0,
    BRAIN_UI_BREAK_MOVE,
    BRAIN_UI_BREAK_SLEEP,
    BRAIN_UI_BREAK_RETURN,
} brain_ui_break_t;

typedef enum {
    /* Hold the early-end control for exactly two seconds. Continue counting. */
    BRAIN_UI_EVENT_END_REQUEST = 0,
    /* The local countdown reaches zero. Play "刷好了吗？" once here. */
    BRAIN_UI_EVENT_TIME_UP,
    /* The user picked a break on the hardware. detail: water/move/sleep/return. */
    BRAIN_UI_EVENT_BREAK_ACTION,
    /* Wardrobe selection. detail: sleep/love/dizzy/hurt/cool/study/sport. */
    BRAIN_UI_EVENT_SKIN_SET_REQUEST,
} brain_ui_event_t;

typedef void (*brain_ui_action_cb_t)(brain_ui_event_t event, const char *detail, void *user_data);

/* Call only from the LVGL task (or before lv_vendor_start). */
void brain_ui_init(void);
void brain_ui_set_action_callback(brain_ui_action_cb_t callback, void *user_data);
/* Disable the offline tap-to-demo shortcut as soon as the phone owns the UI. */
void brain_ui_set_offline_demo(bool enabled);

/* score is clamped to one of the App's six levels: 0/20/40/60/80/100. */
void brain_ui_set_brain_score(uint8_t score);
/* True while the companion countdown owns the display. A score sync from the
 * phone must not tear down a running session, so the BLE glue checks this. */
bool brain_ui_is_focusing(void);
/* True while the idle realtime page is showing. Used to refresh the hint text
 * on link changes without interrupting the demo or countdown pages. */
bool brain_ui_is_realtime(void);
/* True on any page that only exists because there is a session: countdown, due,
 * break menu, break result. An idle sync may safely tear these down. */
bool brain_ui_is_session_page(void);
void brain_ui_show_realtime(void);
void brain_ui_start_focus(uint32_t duration_seconds);
void brain_ui_end_focus(uint8_t latest_brain_score);
void brain_ui_show_break_menu(void);
void brain_ui_show_break(brain_ui_break_t type);

/* App-owned collection snapshots. Skins and monologues page every entry and
 * use a lock badge for the rest; insights page only what the App sent. */
void brain_ui_set_skins(uint8_t unlocked_mask, const char *wearing);
void brain_ui_set_postcard(uint8_t index, bool present, const char *title, const char *line, uint8_t image_index);
void brain_ui_set_insight(uint8_t index, bool present, const char *title, const char *body);

#ifdef __cplusplus
}
#endif

#endif
