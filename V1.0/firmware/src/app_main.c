#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "tkl_system.h"

#include <string.h>

#include "lvgl.h"
#include "lv_vendor.h"
#include "board_com_api.h"
#include "tdl_display_manage.h"

#include "brain_ui.h"
#include "brain_ble.h"

#define BRAIN_BRIGHTNESS 80

/* Runs in the NimBLE host task. Every LVGL call must hold the vendor display
 * lock, otherwise the render task can walk a half-built object tree. */
static void on_ble_score(uint8_t state_index, uint8_t score)
{
    lv_vendor_disp_lock();
    /* Patches the realtime page in place when it is already showing, so a state
     * sync does not rebuild (and flash) the whole screen. */
    brain_ui_set_brain_score(score);
    lv_vendor_disp_unlock();

    brain_ble_note_score(score);
    PR_NOTICE("[brain] BLE score applied: index=%u score=%u", state_index, score);
}

/* Link up: the phone owns the UI, so the offline tap-to-demo shortcuts must go,
 * otherwise the user can create a session the App does not know about.
 * Link down: hand local touch back and keep the last score, as the plan says
 * ("BLE lost -> keep last state, offline six-state demo allowed"). */
static void on_ble_link(bool connected)
{
    lv_vendor_disp_lock();
    brain_ui_set_offline_demo(!connected);
    /* Only refresh the idle page: rebuilding here would kick the user out of
     * the demo page or reset a running countdown for a hint-text change. */
    if (brain_ui_is_realtime()) {
        brain_ui_show_realtime();
    }
    lv_vendor_disp_unlock();

    PR_NOTICE("[brain] phone link %s: offline demo %s",
              connected ? "up" : "down", connected ? "off" : "on");
}

static brain_ui_break_t break_from(const char *name)
{
    if (name == NULL) {
        return BRAIN_UI_BREAK_RETURN;
    }
    if (strcmp(name, "water") == 0) {
        return BRAIN_UI_BREAK_WATER;
    }
    if (strcmp(name, "move") == 0) {
        return BRAIN_UI_BREAK_MOVE;
    }
    if (strcmp(name, "sleep") == 0) {
        return BRAIN_UI_BREAK_SLEEP;
    }
    return BRAIN_UI_BREAK_RETURN;
}

/* Phone -> device protocol frames. Runs on the NimBLE host task, so the same
 * display lock as on_ble_score applies. */
static void on_ble_frame(const brain_ble_frame_t *frame)
{
    lv_vendor_disp_lock();

    if (strcmp(frame->type, "APP_LAUNCH") == 0 || strcmp(frame->type, "STATE_SYNC") == 0) {
        if (frame->brain_score != 0xFF) {
            brain_ui_set_brain_score(frame->brain_score);
            brain_ble_note_score(frame->brain_score);
        }
        /* Phone says "idle": drop any session page, but never yank the demo. */
        if (brain_ui_is_session_page()) {
            brain_ui_show_realtime();
        }
    } else if (strcmp(frame->type, "REFLECT_DONE") == 0) {
        if (frame->brain_score != 0xFF) {
            brain_ui_set_brain_score(frame->brain_score);
            brain_ble_note_score(frame->brain_score);
        }
        /* End the countdown, but leave the 15s break page to finish its own
         * timer: the break the user just picked must stay visible. */
        if (brain_ui_is_focusing()) {
            brain_ui_show_realtime();
        }
    } else if (strcmp(frame->type, "FOCUS_START") == 0) {
        if (frame->seconds > 0) {
            brain_ui_start_focus(frame->seconds);
        }
    } else if (strcmp(frame->type, "FOCUS_EXTEND") == 0) {
        if (frame->seconds > 0) {
            brain_ui_extend_focus(frame->seconds);
        }
    } else if (strcmp(frame->type, "FOCUS_END") == 0) {
        brain_ui_end_focus(frame->brain_score == 0xFF ? 80 : frame->brain_score);
    } else if (strcmp(frame->type, "BREAK_MENU") == 0) {
        brain_ui_show_break_menu();
    } else if (strcmp(frame->type, "BREAK_ACTION") == 0) {
        brain_ui_show_break(break_from(frame->break_type));
    }

    lv_vendor_disp_unlock();
}

static void on_ui_event(brain_ui_event_t event, const char *detail, void *user_data)
{
    (void)user_data;

    switch (event) {
    case BRAIN_UI_EVENT_END_REQUEST:
        PR_NOTICE("[brain] ui END_REQUEST");
        brain_ble_emit("END_REQUEST", NULL);
        break;
    case BRAIN_UI_EVENT_TIME_UP:
        PR_NOTICE("[brain] ui TIME_UP");
        brain_ble_emit("TIME_UP", NULL);
        break;
    case BRAIN_UI_EVENT_BREAK_ACTION:
        PR_NOTICE("[brain] ui BREAK_ACTION=%s", detail == NULL ? "-" : detail);
        brain_ble_emit("BREAK_ACTION", detail);
        break;
    case BRAIN_UI_EVENT_EXTEND_REQUEST:
        PR_NOTICE("[brain] ui EXTEND_REQUEST");
        brain_ble_emit("EXTEND_REQUEST", NULL);
        break;
    case BRAIN_UI_EVENT_KEEP_REQUEST:
        PR_NOTICE("[brain] ui KEEP_REQUEST");
        brain_ble_emit("KEEP_REQUEST", NULL);
        break;
    default:
        break;
    }
}

void user_main(void)
{
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    tal_sw_timer_init();
    OPERATE_RET ret = tal_workq_init();
    if (ret != OPRT_OK) {
        PR_ERR("[brain] work queue init failed: %d", ret);
        return;
    }

    PR_NOTICE("[brain] project=%s version=%s", PROJECT_NAME, PROJECT_VERSION);
    board_register_hardware();
    lv_vendor_init(DISPLAY_NAME);

    TDL_DISP_HANDLE_T display = tdl_disp_find_dev(DISPLAY_NAME);
    if (display != NULL) {
        tdl_disp_set_brightness(display, BRAIN_BRIGHTNESS);
    }

    /* Objects are created before the render task starts: no lock needed here. */
    brain_ui_init();
    brain_ui_set_action_callback(on_ui_event, NULL);
    lv_vendor_start(THREAD_PRIO_0, 1024 * 8);

    brain_ble_set_link_callback(on_ble_link);
    brain_ble_set_frame_callback(on_ble_frame);
    ret = brain_ble_init(on_ble_score);
    if (ret != OPRT_OK) {
        PR_ERR("[brain] BLE init failed: %d", ret);
    }
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    user_main();
    while (1) {
        tal_system_sleep(500);
    }
    return 0;
}
#else
static THREAD_HANDLE app_thread;

static void app_main_task(void *arg)
{
    (void)arg;
    user_main();
    tal_thread_delete(app_thread);
    app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T config = {
        .stackDepth = 1024 * 4,
        .priority = 4,
        .thrdname = "brain_companion",
    };
    tal_thread_create_and_start(&app_thread, NULL, NULL, app_main_task, NULL, &config);
}
#endif
