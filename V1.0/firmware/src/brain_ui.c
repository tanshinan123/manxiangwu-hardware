#include "brain_ui.h"

#include <stdio.h>
#include <string.h>

#include "brain_images.h"
#include "lvgl.h"
#include "tal_api.h"

LV_FONT_DECLARE(brain_font_14);
LV_FONT_DECLARE(brain_font_16);
LV_FONT_DECLARE(brain_font_18);
LV_FONT_DECLARE(brain_font_22);
LV_FONT_DECLARE(brain_font_30);
LV_FONT_DECLARE(brain_font_36);
LV_FONT_DECLARE(brain_font_44);

#define PAPER       0xFFF9F2
#define INK         0x29453F
#define MINT        0x5CB89E
#define DEEP        0x388575
#define PALE        0xE2F2EC
#define MUTED       0x7E918B
#define TRACK       0xD6E8E0
/* Mint gradient taken from the App's screen background (mist -> surface) with
 * a touch more mint, so a round panel does not read as plain white. */
#define BG_TOP      0xE3F3EC
#define BG_BOTTOM   0xFCFEFD
#define BG_PULSE    0xC6EADC
#define BG_PULSE_LO 0xE9F8F1
#define HOLD_MS     2000
#define BREAK_MS    15000
/* Offline demo only: the phone picks the real extension length. */
#define OFFLINE_EXTEND_SECONDS 300
#define MAX_MINUTES (99 * 60 + 59)
/* One breath: 2.8s calm, 2.0s during the last five seconds. */
#define BREATH_MS      2800
#define BREATH_FAST_MS 2000

typedef enum {
    SCREEN_REALTIME,
    SCREEN_BRAIN_DEMO,
    SCREEN_FOCUS,
    SCREEN_DUE,
    SCREEN_BREAK_MENU,
    SCREEN_BREAK,
} screen_state_t;

/* What the decision page is asking. Primary action always sits on the right. */
typedef enum {
    /* Held 提前结束: cancel (secondary, left) / end (primary, right). */
    DECISION_EARLY,
    /* Countdown reached zero on the first pass: extend (left) / keep (right). */
    DECISION_DUE,
    /* Countdown reached zero after an extension: only 确定 is left to do. */
    DECISION_EXTENDED,
} decision_kind_t;

static const uint8_t SCORES[] = {0, 20, 40, 60, 80, 100};
static const lv_image_dsc_t *const IMAGES[] = {
    &brain_0, &brain_20, &brain_40, &brain_60, &brain_80, &brain_100,
};
static const char *const TAGS[] = {
    "难受到掉色", "嘶~好难受", "我好累我想哭",
    "你看我的黑眼圈", "有点疲倦诶", "柔柔软软",
};
static const char *const BREAK_NAMES[] = {"water", "move", "sleep", "return"};
static const char *const BREAK_COPY[] = {
    "起身喝口水吧～", "站起来走一走呢～", "美美睡一觉吧～", "回到刚才的事吧～",
};

static struct {
    lv_obj_t *root;
    lv_obj_t *image;
    lv_obj_t *ring;
    lv_obj_t *countdown;
    lv_obj_t *hold_label;
    lv_obj_t *hold_progress;
    lv_obj_t *score_label;
    lv_obj_t *tag_label;
    lv_obj_t *demo_score;
    lv_obj_t *demo_tag;
    lv_obj_t *break_pager;
    lv_obj_t *break_tiles[4];
    lv_timer_t *clock;
    screen_state_t screen;
    decision_kind_t decision;
    uint8_t score;
    uint8_t demo_index;
    uint8_t break_type;
    uint8_t bg_level;
    uint32_t image_scale;
    uint32_t focus_started;
    uint32_t break_started;
    uint32_t break_menu_at;
    uint32_t paused_remaining;
    uint32_t focus_seconds;
    uint32_t last_remaining;
    uint32_t hold_ms;
    bool holding;
    bool end_sent;
    bool offline_demo;
    bool focus_extended;
    brain_ui_action_cb_t callback;
    void *user_data;
} ui = {.score = 80, .demo_index = 5, .offline_demo = true};

static void show_realtime(void);

static const lv_image_dsc_t *current_image(void)
{
    uint8_t index = (ui.score + 10) / 20;
    if (index > 5) {
        index = 5;
    }
    return IMAGES[index];
}

static const char *current_tag(void)
{
    uint8_t index = (ui.score + 10) / 20;
    return TAGS[index > 5 ? 5 : index];
}

static void emit(brain_ui_event_t event, const char *detail)
{
    PR_NOTICE("[brain-ui] event=%d detail=%s", event, detail == NULL ? "-" : detail);
    if (ui.callback != NULL) {
        ui.callback(event, detail, ui.user_data);
    }
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       uint32_t colour, lv_coord_t y)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(colour), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
    return obj;
}

/* Integer blend between two 0xRRGGBB values. level 0 -> a, 255 -> b. */
static lv_color_t mix_hex(uint32_t a, uint32_t b, uint8_t level)
{
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    int r = ar + (br - ar) * (int)level / 255;
    int g = ag + (bg - ag) * (int)level / 255;
    int bl = ab + (bb - ab) * (int)level / 255;
    return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)bl);
}

/* Mint gradient background. `pulse` 0 is calm, 255 is the deepest breath of the
 * final five seconds, so the whole panel reads like a slow breathing lamp. */
static void apply_background(uint8_t pulse)
{
    if (ui.bg_level == pulse) {
        return;
    }
    ui.bg_level = pulse;
    lv_obj_set_style_bg_color(ui.root, mix_hex(BG_TOP, BG_PULSE, pulse), 0);
    lv_obj_set_style_bg_grad_color(ui.root, mix_hex(BG_BOTTOM, BG_PULSE_LO, pulse), 0);
    lv_obj_set_style_bg_grad_dir(ui.root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(ui.root, LV_OPA_COVER, 0);
}

/* Triangle wave 0..half..0 so the motion eases in and out instead of jumping. */
static uint32_t breath_level(uint32_t period, uint32_t span)
{
    uint32_t half = period / 2;
    uint32_t t = lv_tick_get() % period;
    uint32_t up = (t < half) ? t : period - t;
    return half == 0 ? 0 : up * span / half;
}

static void brain_breathe(uint32_t period, uint32_t span)
{
    if (ui.image == NULL || ui.image_scale == 0) {
        return;
    }
    lv_image_set_scale(ui.image, ui.image_scale + breath_level(period, span));
}

static void prepare_screen(screen_state_t state)
{
    ui.screen = state;
    ui.holding = false;
    ui.hold_ms = 0;
    ui.end_sent = false;
    ui.image = NULL;
    ui.ring = NULL;
    ui.countdown = NULL;
    ui.hold_label = NULL;
    ui.hold_progress = NULL;
    ui.score_label = NULL;
    ui.tag_label = NULL;
    ui.demo_score = NULL;
    ui.demo_tag = NULL;
    ui.break_pager = NULL;
    ui.image_scale = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        ui.break_tiles[i] = NULL;
    }
    lv_obj_clean(ui.root);
    ui.bg_level = 0xFF; /* force the gradient to be written after the clean */
    apply_background(0);
    lv_obj_clear_flag(ui.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui.root, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *brain(lv_obj_t *parent, const lv_image_dsc_t *source, lv_coord_t size, lv_coord_t y)
{
    lv_obj_t *obj = lv_image_create(parent);
    lv_image_set_src(obj, source);
    lv_obj_set_size(obj, size, size);
    lv_image_set_scale(obj, (uint32_t)size * 256 / 256);
    /* Base for the breathing animation in tick(). */
    ui.image_scale = (uint32_t)size;
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *soft_button(lv_obj_t *parent, const char *text, lv_coord_t width,
                             lv_coord_t height, const lv_font_t *font)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, height / 2, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(PALE), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(MINT), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(TRACK), LV_STATE_PRESSED);
    lv_obj_t *caption = lv_label_create(button);
    lv_label_set_text(caption, text);
    lv_obj_set_style_text_font(caption, font, 0);
    lv_obj_set_style_text_color(caption, lv_color_hex(DEEP), 0);
    lv_obj_center(caption);
    return button;
}

static void make_ring(lv_coord_t width, uint32_t colour)
{
    ui.ring = lv_arc_create(ui.root);
    lv_obj_set_size(ui.ring, 442, 442);
    lv_obj_center(ui.ring);
    lv_arc_set_bg_angles(ui.ring, 0, 360);
    lv_arc_set_range(ui.ring, 0, 1000);
    lv_arc_set_value(ui.ring, 1000);
    lv_arc_set_rotation(ui.ring, 270);
    lv_obj_set_style_arc_width(ui.ring, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.ring, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui.ring, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(ui.ring, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.ring, lv_color_hex(TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.ring, lv_color_hex(colour), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ui.ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.ring, 0, 0);
    lv_obj_set_style_pad_all(ui.ring, 0, 0);
    lv_obj_set_style_arc_width(ui.ring, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ui.ring, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(ui.ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(ui.ring);
}

static void offline_brain_clicked(lv_event_t *event)
{
    (void)event;
    if (ui.offline_demo && ui.screen == SCREEN_REALTIME) {
        brain_ui_start_focus(30);
    }
}

static void demo_exit_clicked(lv_event_t *event);
static void demo_cycle_clicked(lv_event_t *event);

static void show_brain_demo(void)
{
    prepare_screen(SCREEN_BRAIN_DEMO);
    label(ui.root, "脑力演示", &brain_font_22, DEEP, 30);
    ui.demo_score = label(ui.root, "脑力 100", &brain_font_44, INK, 60);
    ui.image = brain(ui.root, IMAGES[ui.demo_index], 236, 126);
    lv_obj_add_flag(ui.image, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *pill = lv_obj_create(ui.root);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 300, 52);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 368);
    lv_obj_set_style_bg_color(pill, lv_color_hex(PALE), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 26, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    ui.demo_tag = label(pill, TAGS[ui.demo_index], &brain_font_30, DEEP, 6);
    label(ui.root, "轻点脑力切状态", &brain_font_16, MUTED, 426);

    lv_obj_t *exit_button = soft_button(ui.root, "退出", 84, 42, &brain_font_16);
    lv_obj_align(exit_button, LV_ALIGN_TOP_RIGHT, -46, 104);
    lv_obj_add_event_cb(exit_button, demo_exit_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui.image, demo_cycle_clicked, LV_EVENT_CLICKED, NULL);
}

static void demo_open_clicked(lv_event_t *event)
{
    (void)event;
    /* Always available: the demo page sends no BLE events and does not change
     * the real score, so the phone being in control is no reason to hide it. */
    if (ui.screen == SCREEN_REALTIME) {
        ui.demo_index = 5;
        show_brain_demo();
    }
}

static void demo_exit_clicked(lv_event_t *event)
{
    (void)event;
    if (ui.screen == SCREEN_BRAIN_DEMO) {
        show_realtime();
    }
}

static void demo_cycle_clicked(lv_event_t *event)
{
    (void)event;
    if (ui.screen != SCREEN_BRAIN_DEMO) {
        return;
    }
    ui.demo_index = (ui.demo_index + 1) % 6;
    lv_image_set_src(ui.image, IMAGES[ui.demo_index]);
    lv_label_set_text_fmt(ui.demo_score, "脑力 %u", SCORES[ui.demo_index]);
    lv_label_set_text(ui.demo_tag, TAGS[ui.demo_index]);
}

static void show_realtime(void)
{
    prepare_screen(SCREEN_REALTIME);
    char text[24];
    snprintf(text, sizeof(text), "脑力 %u", ui.score);
    label(ui.root, "慢想屋", &brain_font_22, DEEP, 30);
    ui.score_label = label(ui.root, text, &brain_font_44, INK, 60);
    ui.image = brain(ui.root, current_image(), 236, 126);
    if (ui.offline_demo) {
        lv_obj_add_flag(ui.image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ui.image, offline_brain_clicked, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *pill = lv_obj_create(ui.root);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 300, 52);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 368);
    lv_obj_set_style_bg_color(pill, lv_color_hex(PALE), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 26, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    ui.tag_label = label(pill, current_tag(), &brain_font_30, DEEP, 6);
    label(ui.root, ui.offline_demo ? "轻点脑子开始" : "我在这儿，慢慢陪你",
          &brain_font_16, MUTED, 426);
    /* Kept unconditionally so the panel never becomes a dead end after the
     * phone writes a score. See demo_open_clicked(). */
    lv_obj_t *demo_button = soft_button(ui.root, "演示", 84, 42, &brain_font_16);
    lv_obj_align(demo_button, LV_ALIGN_TOP_RIGHT, -46, 104);
    lv_obj_add_event_cb(demo_button, demo_open_clicked, LV_EVENT_CLICKED, NULL);
}

static void cancel_hold(void)
{
    ui.holding = false;
    ui.hold_ms = 0;
    ui.end_sent = false;
    if (ui.hold_label != NULL) {
        lv_label_set_text(ui.hold_label, "提前结束");
    }
    if (ui.hold_progress != NULL) {
        lv_obj_set_width(ui.hold_progress, 0);
    }
}

static void hold_pressed(lv_event_t *event)
{
    (void)event;
    if (ui.screen == SCREEN_FOCUS && !ui.end_sent) {
        ui.holding = true;
        ui.hold_ms = 0;
    }
}

static void hold_released(lv_event_t *event)
{
    (void)event;
    cancel_hold();
}

/* The pager is the selector now: whichever tile is centred gets chosen. */
static void break_confirm_clicked(lv_event_t *event)
{
    (void)event;
    if (ui.screen != SCREEN_BREAK_MENU || ui.break_pager == NULL) {
        return;
    }
    /* A finger already resting on the glass when the page appears must not pick
     * something the user never aimed at. */
    if (lv_tick_elaps(ui.break_menu_at) < 300) {
        return;
    }
    lv_obj_t *active = lv_tileview_get_tile_active(ui.break_pager);
    uint8_t type = BRAIN_UI_BREAK_WATER;
    for (uint8_t i = 0; i < 4; ++i) {
        if (ui.break_tiles[i] == active) {
            type = i;
            break;
        }
    }
    emit(BRAIN_UI_EVENT_BREAK_ACTION, BREAK_NAMES[type]);
    brain_ui_show_break((brain_ui_break_t)type);
}

/* Reflection page, hardware edition. The App still owns the data; this page only
 * asks the one question that lets the user stay on the panel. Primary action is
 * always the right-hand button. */
static void decision_cancel_clicked(lv_event_t *event)
{
    (void)event;
    if (ui.screen != SCREEN_DUE || ui.decision != DECISION_EARLY) {
        return;
    }
    /* Cancelled the early end: the countdown resumes exactly where it stopped. */
    if (ui.paused_remaining > 0) {
        brain_ui_start_focus(ui.paused_remaining);
    } else {
        show_realtime();
    }
}

static void decision_end_clicked(lv_event_t *event)
{
    (void)event;
    if (ui.screen != SCREEN_DUE || ui.decision != DECISION_EARLY) {
        return;
    }
    emit(BRAIN_UI_EVENT_END_REQUEST, NULL);
    if (ui.offline_demo) {
        brain_ui_show_break_menu();
    }
}

static void decision_extend_clicked(lv_event_t *event)
{
    (void)event;
    if (ui.screen != SCREEN_DUE || ui.decision != DECISION_DUE) {
        return;
    }
    emit(BRAIN_UI_EVENT_EXTEND_REQUEST, NULL);
    /* No phone attached: keep the demo self-contained. */
    if (ui.offline_demo) {
        brain_ui_start_focus(OFFLINE_EXTEND_SECONDS);
        ui.focus_extended = true;
    }
}

static void decision_keep_clicked(lv_event_t *event)
{
    (void)event;
    if (ui.screen != SCREEN_DUE || ui.decision == DECISION_EARLY) {
        return;
    }
    emit(BRAIN_UI_EVENT_KEEP_REQUEST, NULL);
    if (ui.offline_demo) {
        brain_ui_show_break_menu();
    }
}

static void show_decision(decision_kind_t kind)
{
    prepare_screen(SCREEN_DUE);
    ui.decision = kind;
    make_ring(8, MINT);
    lv_arc_set_value(ui.ring, 0);

    bool early = (kind == DECISION_EARLY);
    label(ui.root, early ? "先停一下" : "时间到啦", &brain_font_22, DEEP, 34);
    ui.image = brain(ui.root, current_image(), 196, 68);
    label(ui.root, early ? "还要再看会儿吗？" : "刷好了吗？", &brain_font_30, DEEP, 274);

    if (kind == DECISION_EXTENDED) {
        /* Nothing left to choose: one confirmation, centred. */
        lv_obj_t *done = soft_button(ui.root, "确定", 174, 56, &brain_font_22);
        lv_obj_align(done, LV_ALIGN_TOP_MID, 0, 330);
        lv_obj_add_event_cb(done, decision_keep_clicked, LV_EVENT_CLICKED, NULL);
        return;
    }

    lv_obj_t *secondary = soft_button(ui.root, early ? "取消" : "续时", 150, 56, &brain_font_22);
    lv_obj_align(secondary, LV_ALIGN_TOP_MID, -85, 330);
    lv_obj_add_event_cb(secondary, early ? decision_cancel_clicked : decision_extend_clicked,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *primary = soft_button(ui.root, early ? "结束" : "按时", 150, 56, &brain_font_22);
    lv_obj_align(primary, LV_ALIGN_TOP_MID, 85, 330);
    lv_obj_add_event_cb(primary, early ? decision_end_clicked : decision_keep_clicked,
                        LV_EVENT_CLICKED, NULL);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;

    if (ui.screen == SCREEN_FOCUS) {
        uint32_t elapsed = lv_tick_elaps(ui.focus_started) / 1000;
        uint32_t remaining = elapsed >= ui.focus_seconds ? 0 : ui.focus_seconds - elapsed;
        if (remaining != ui.last_remaining) {
            ui.last_remaining = remaining;
            char text[16];
            snprintf(text, sizeof(text), "%02lu:%02lu", (unsigned long)(remaining / 60),
                     (unsigned long)(remaining % 60));
            lv_label_set_text(ui.countdown, text);
            lv_arc_set_value(ui.ring, (int32_t)(remaining * 1000 / ui.focus_seconds));
        }

        if (remaining <= 5 && remaining > 0) {
            /* Last five seconds: the breath gets deeper and the whole panel
             * brightens and dims with it, like a breathing lamp. Never a hard
             * flash, and no warning red. */
            uint8_t level = (uint8_t)breath_level(BREATH_FAST_MS, 255);
            brain_breathe(BREATH_FAST_MS, 26);
            apply_background(level);
            lv_obj_set_style_opa(ui.ring, (lv_opa_t)(190 + level * 65 / 255), 0);
        } else {
            brain_breathe(BREATH_MS, 12);
            apply_background(0);
            lv_obj_set_style_opa(ui.ring, LV_OPA_COVER, 0);
        }

        if (ui.holding && !ui.end_sent) {
            ui.hold_ms += 100;
            uint32_t progress = ui.hold_ms > HOLD_MS ? 100 : ui.hold_ms * 100 / HOLD_MS;
            lv_obj_set_width(ui.hold_progress, (lv_coord_t)(174 * progress / 100));
            if (progress >= 100) {
                ui.end_sent = true;
                ui.holding = false;
                /* Remember where the countdown stopped so 取消 can resume it.
                 * END_REQUEST is only sent after the user confirms on the
                 * decision page, not the moment the hold completes. */
                ui.paused_remaining = remaining;
                show_decision(DECISION_EARLY);
                return;
            }
        }

        if (remaining == 0) {
            /* Second time around there is nothing to choose any more. */
            show_decision(ui.focus_extended ? DECISION_EXTENDED : DECISION_DUE);
            emit(BRAIN_UI_EVENT_TIME_UP, NULL);
        }
        return;
    }

    if (ui.screen == SCREEN_BREAK) {
        if (lv_tick_elaps(ui.break_started) >= BREAK_MS) {
            show_realtime();
            return;
        }
    }

    /* Every page that shows a brain breathes, not just the idle one. */
    brain_breathe(BREATH_MS, 12);
}

void brain_ui_init(void)
{
    ui.root = lv_screen_active();
    show_realtime();
    if (ui.clock == NULL) {
        ui.clock = lv_timer_create(tick, 100, NULL);
    }
}

void brain_ui_set_action_callback(brain_ui_action_cb_t callback, void *user_data)
{
    ui.callback = callback;
    ui.user_data = user_data;
}

void brain_ui_set_offline_demo(bool enabled)
{
    ui.offline_demo = enabled;
}

void brain_ui_set_brain_score(uint8_t score)
{
    uint8_t closest = 0;
    for (uint8_t i = 1; i < 6; ++i) {
        int difference = (int)score - (int)SCORES[i];
        int previous = (int)score - (int)SCORES[closest];
        if (difference < 0) difference = -difference;
        if (previous < 0) previous = -previous;
        if (difference < previous) closest = i;
    }
    ui.score = SCORES[closest];

    /* The realtime page is already on screen: patch it in place. Rebuilding the
     * whole tree on every state sync would flash the panel for a value change. */
    if (ui.screen == SCREEN_REALTIME && ui.image != NULL &&
        ui.score_label != NULL && ui.tag_label != NULL) {
        char text[24];
        snprintf(text, sizeof(text), "脑力 %u", ui.score);
        lv_label_set_text(ui.score_label, text);
        lv_label_set_text(ui.tag_label, current_tag());
        lv_image_set_src(ui.image, current_image());
    }
}

void brain_ui_show_realtime(void)
{
    show_realtime();
}

bool brain_ui_is_focusing(void)
{
    return ui.screen == SCREEN_FOCUS;
}

bool brain_ui_is_realtime(void)
{
    return ui.screen == SCREEN_REALTIME;
}

bool brain_ui_is_session_page(void)
{
    return ui.screen == SCREEN_FOCUS || ui.screen == SCREEN_DUE ||
           ui.screen == SCREEN_BREAK_MENU || ui.screen == SCREEN_BREAK;
}

void brain_ui_start_focus(uint32_t duration_seconds)
{
    if (duration_seconds == 0) duration_seconds = 1;
    if (duration_seconds > MAX_MINUTES) duration_seconds = MAX_MINUTES;
    prepare_screen(SCREEN_FOCUS);
    ui.focus_seconds = duration_seconds;
    ui.focus_extended = false;
    ui.focus_started = lv_tick_get();
    ui.last_remaining = duration_seconds + 1;
    make_ring(9, MINT);
    label(ui.root, "陪伴中", &brain_font_22, DEEP, 34);
    ui.image = brain(ui.root, current_image(), 204, 82);
    ui.countdown = label(ui.root, "00:00", &brain_font_44, INK, 292);
    lv_obj_set_style_text_letter_space(ui.countdown, 3, 0);

    /* Hold feedback lives above the button: the finger covers the button itself,
     * so an inner bar would never be seen. */
    lv_obj_t *track = lv_obj_create(ui.root);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, 174, 6);
    lv_obj_align(track, LV_ALIGN_TOP_MID, 0, 360);
    lv_obj_set_style_bg_color(track, lv_color_hex(TRACK), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    ui.hold_progress = lv_obj_create(track);
    lv_obj_remove_style_all(ui.hold_progress);
    lv_obj_set_size(ui.hold_progress, 0, 6);
    lv_obj_align(ui.hold_progress, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(ui.hold_progress, lv_color_hex(MINT), 0);
    lv_obj_set_style_bg_opa(ui.hold_progress, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui.hold_progress, 3, 0);
    lv_obj_clear_flag(ui.hold_progress, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *button = soft_button(ui.root, "提前结束", 174, 52, &brain_font_22);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 374);
    lv_obj_add_event_cb(button, hold_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(button, hold_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(button, hold_released, LV_EVENT_PRESS_LOST, NULL);
    ui.hold_label = lv_obj_get_child(button, 0);
    tick(NULL);
}

void brain_ui_extend_focus(uint32_t duration_seconds)
{
    brain_ui_start_focus(duration_seconds);
    /* The next time-up offers only 确定: the session already used its extension. */
    ui.focus_extended = true;
}

void brain_ui_end_focus(uint8_t latest_brain_score)
{
    brain_ui_set_brain_score(latest_brain_score);
    show_realtime();
}

void brain_ui_show_due(void)
{
    show_decision(DECISION_DUE);
}

void brain_ui_show_break_menu(void)
{
    prepare_screen(SCREEN_BREAK_MENU);
    ui.break_menu_at = lv_tick_get();

    label(ui.root, "接下来，做点小事", &brain_font_22, DEEP, 24);

    /* One option per screen, swipe sideways. A 2x2 grid forced the copy and the
     * button into the round screen's corners; a pager keeps everything centred. */
    ui.break_pager = lv_tileview_create(ui.root);
    lv_obj_set_size(ui.break_pager, 466, 246);
    lv_obj_align(ui.break_pager, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_opa(ui.break_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.break_pager, 0, 0);
    lv_obj_set_style_pad_all(ui.break_pager, 0, 0);
    lv_obj_set_scrollbar_mode(ui.break_pager, LV_SCROLLBAR_MODE_OFF);

    const lv_image_dsc_t *images[] = {&brain_water, &brain_move, &brain_sleep, NULL};

    for (uint8_t i = 0; i < 4; ++i) {
        lv_obj_t *tile = lv_tileview_add_tile(ui.break_pager, i, 0, LV_DIR_ALL);
        ui.break_tiles[i] = tile;
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        const lv_image_dsc_t *source =
            (i == BRAIN_UI_BREAK_RETURN) ? current_image() : images[i];

        lv_obj_t *picture = lv_image_create(tile);
        lv_image_set_src(picture, source);
        lv_obj_set_size(picture, 150, 150);
        lv_image_set_scale(picture, 150);
        lv_obj_align(picture, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_clear_flag(picture, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *copy = lv_label_create(tile);
        lv_label_set_text(copy, BREAK_COPY[i]);
        lv_obj_set_style_text_font(copy, &brain_font_22, 0);
        lv_obj_set_style_text_color(copy, lv_color_hex(DEEP), 0);
        lv_obj_align(copy, LV_ALIGN_TOP_MID, 0, 170);
    }

    lv_obj_t *button = soft_button(ui.root, "就选它", 174, 52, &brain_font_22);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 322);
    lv_obj_add_event_cb(button, break_confirm_clicked, LV_EVENT_CLICKED, NULL);

    label(ui.root, "左右滑动，选一件", &brain_font_16, MUTED, 386);
}

void brain_ui_show_break(brain_ui_break_t type)
{
    if (type > BRAIN_UI_BREAK_RETURN) return;
    prepare_screen(SCREEN_BREAK);
    ui.break_type = type;
    ui.break_started = lv_tick_get();
    const lv_image_dsc_t *images[] = {&brain_water, &brain_move, &brain_sleep, NULL};
    const lv_image_dsc_t *source = type == BRAIN_UI_BREAK_RETURN ? current_image() : images[type];
    label(ui.root, "陪你做件小事", &brain_font_30, DEEP, 34);
    ui.image = brain(ui.root, source, 240, 92);
    label(ui.root, BREAK_COPY[type], &brain_font_30, DEEP, 344);
    label(ui.root, "15秒后，我回到这里陪你", &brain_font_18, MUTED, 404);
}
