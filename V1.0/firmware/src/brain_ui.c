#include "brain_ui.h"

#include <stdio.h>
#include <string.h>

#include "brain_images.h"
#include "brain_voice.h"
#include "lvgl.h"
#include "tal_api.h"

LV_FONT_DECLARE(brain_font_16);
LV_FONT_DECLARE(brain_font_18);
LV_FONT_DECLARE(brain_font_22);
LV_FONT_DECLARE(brain_font_30);
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
#define BG_TOP      0xF0F8F4
#define BG_BOTTOM   0xF7FCF9
#define BG_PULSE    0xE4F5EE
#define BG_PULSE_LO 0xF3FBF7
#define BRAIN_SIZE  236
#define TOOL_TEXT_MS 10000
#define NOTE        0xFFF0C9
#define NOTE_EDGE   0xE7D79E
#define HOLD_MS     2000
#define SKIN_COUNT  8
#define CARD_COUNT  4
#define INSIGHT_COUNT 2
#define NO_SKIN     0xFF
#define BREAK_MS    15000
#define MAX_MINUTES (99 * 60 + 59)
/* One breath: 2.8s calm, 2.0s during the last five seconds. */
#define BREATH_MS      2800
#define BREATH_FAST_MS 2000

typedef enum {
    SCREEN_REALTIME,
    SCREEN_BRAIN_DEMO,
    SCREEN_FOCUS,
    SCREEN_BREAK_MENU,
    SCREEN_BREAK,
    SCREEN_WARDROBE,
    SCREEN_MONOLOGUES,
    SCREEN_MONOLOGUE,
    SCREEN_INSIGHTS,
} screen_state_t;

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
static const char *const SKIN_NAMES[] = {
    "sleep", "love", "dizzy", "hurt", "cool", "study", "health", "sport",
};
static const char *const SKIN_TITLES[] = {
    "安睡脑", "恋爱脑", "糊涂脑", "难受脑", "酷酷脑", "学习脑", "健康脑", "运动脑",
};
static const lv_image_dsc_t *const SKIN_IMAGES[] = {
    &brain_sleep, &brain_love, &brain_dizzy, &brain_hurt,
    &brain_cool, &brain_study, &brain_health, &brain_sport,
};

/* The four monologue cards come straight from the App's design library. */
static const lv_image_dsc_t *const POSTCARD_IMAGES[CARD_COUNT] = {
    &monologue_power, &monologue_heart, &monologue_coffee, &monologue_stream,
};

/* The five realtime tools: two local, three collection pages. They render as
 * icons by default and as text after a tap on the panel body. */
static const char *const TOOL_SYMBOLS[] = {
    LV_SYMBOL_PLAY, LV_SYMBOL_LIST, LV_SYMBOL_IMAGE, LV_SYMBOL_FILE, LV_SYMBOL_CHARGE,
};
static const char *const TOOL_LABELS[] = {"演示", "测试流程", "皮肤", "独白", "成就"};
static const lv_coord_t TOOL_X[] = {16, 44, -50, -18, -4};
static const lv_coord_t TOOL_Y[] = {258, 316, 92, 150, 208};
#define TOOL_COUNT ((int)(sizeof(TOOL_SYMBOLS) / sizeof(TOOL_SYMBOLS[0])))

static struct {
    lv_obj_t *root;
    lv_obj_t *image;
    lv_obj_t *ring;
    lv_obj_t *countdown;
    lv_obj_t *hold_label;
    lv_obj_t *hold_progress;
    lv_obj_t *score_label;
    lv_obj_t *tag_label;
    lv_obj_t *hint_label;
    lv_obj_t *demo_score;
    lv_obj_t *demo_tag;
    lv_obj_t *break_pager;
    lv_obj_t *break_tiles[4];
    lv_obj_t *library_pager;
    lv_obj_t *library_tiles[SKIN_COUNT];
    lv_obj_t *library_action;
    lv_obj_t *tool_buttons[5];
    lv_timer_t *clock;
    screen_state_t screen;
    uint8_t score;
    uint8_t demo_index;
    uint8_t break_type;
    uint8_t bg_level;
    uint32_t image_scale;
    uint32_t focus_started;
    uint32_t break_started;
    uint32_t break_menu_at;
    uint32_t tool_text_until;
    uint32_t focus_seconds;
    uint32_t last_remaining;
    uint32_t hold_ms;
    bool holding;
    bool end_sent;
    bool offline_demo;
    bool testing;
    bool tool_text;
    uint8_t unlocked_skins;
    uint8_t wearing;
    uint8_t library_items[SKIN_COUNT];
    uint8_t library_count;
    uint8_t card_reading;
    char card_titles[CARD_COUNT][40];
    char card_lines[CARD_COUNT][96];
    uint8_t card_images[CARD_COUNT];
    char insight_titles[INSIGHT_COUNT][96];
    char insight_bodies[INSIGHT_COUNT][96];
    bool card_valid[CARD_COUNT];
    bool insight_valid[INSIGHT_COUNT];
    brain_ui_action_cb_t callback;
    void *user_data;
} ui = {.score = 80, .demo_index = 5, .offline_demo = true, .wearing = NO_SKIN};

static void show_realtime(void);
static void show_library(screen_state_t state);

static const lv_image_dsc_t *current_image(void)
{
    if (ui.wearing < SKIN_COUNT) {
        return SKIN_IMAGES[ui.wearing];
    }
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
    /* The local test walkthrough must never drive a real App session. */
    if (ui.testing) return;
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
    ui.hint_label = NULL;
    ui.demo_score = NULL;
    ui.demo_tag = NULL;
    ui.break_pager = NULL;
    ui.library_pager = NULL;
    ui.library_action = NULL;
    ui.tool_text = false;
    ui.tool_text_until = 0;
    ui.image_scale = 0;
    for (uint8_t i = 0; i < (uint8_t)TOOL_COUNT; ++i) {
        ui.tool_buttons[i] = NULL;
    }
    for (uint8_t i = 0; i < SKIN_COUNT; ++i) {
        ui.library_tiles[i] = NULL;
    }
    for (uint8_t i = 0; i < 4; ++i) {
        ui.break_tiles[i] = NULL;
    }
    lv_obj_clean(ui.root);
    ui.bg_level = 0xFF; /* force the gradient to be written after the clean */
    apply_background(0);
    lv_obj_clear_flag(ui.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui.root, LV_OBJ_FLAG_CLICKABLE);
}

/* Images are stored at two resolutions: states are 256px, skins are 160px.
 * A fixed LVGL zoom therefore makes the same widget visibly different sizes.
 * Always derive zoom from the source width so the on-screen size is identical. */
static uint32_t image_zoom_for(const lv_image_dsc_t *source, lv_coord_t size)
{
    uint16_t width = source != NULL ? source->header.w : 0;
    return width == 0 ? 256 : (uint32_t)size * 256 / width;
}

static void fit_brain_image(lv_obj_t *obj, const lv_image_dsc_t *source, lv_coord_t size)
{
    lv_obj_set_size(obj, size, size);
    lv_image_set_scale(obj, image_zoom_for(source, size));
    lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_CENTER);
}

/* Design-library cards are 4:3, so scale by width and keep the source aspect. */
static void fit_design_image(lv_obj_t *obj, const lv_image_dsc_t *source, lv_coord_t width)
{
    uint16_t w = source->header.w;
    uint16_t h = source->header.h;
    lv_coord_t height = (lv_coord_t)((uint32_t)width * h / w);
    lv_obj_set_size(obj, width, height);
    lv_image_set_scale(obj, (uint32_t)width * 256 / w);
    lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_CENTER);
}

static lv_obj_t *brain(lv_obj_t *parent, const lv_image_dsc_t *source, lv_coord_t size, lv_coord_t y)
{
    lv_obj_t *obj = lv_image_create(parent);
    lv_image_set_src(obj, source);
    fit_brain_image(obj, source, size);
    /* Base for the breathing animation in tick(). */
    ui.image_scale = image_zoom_for(source, size);
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

static lv_obj_t *icon_button(lv_obj_t *parent, const char *symbol, lv_coord_t size)
{
    lv_obj_t *button = soft_button(parent, "", size, size, &brain_font_16);
    lv_obj_t *icon = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_font(icon, LV_FONT_DEFAULT, 0);
    lv_label_set_text(icon, symbol);
    return button;
}

static lv_obj_t *note_card(lv_obj_t *parent, lv_coord_t width, lv_coord_t height, lv_coord_t y)
{
    lv_obj_t *note = lv_obj_create(parent);
    lv_obj_set_size(note, width, height);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(note, 12, 0);
    lv_obj_set_style_bg_color(note, lv_color_hex(NOTE), 0);
    lv_obj_set_style_bg_opa(note, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(note, lv_color_hex(NOTE_EDGE), 0);
    lv_obj_set_style_border_width(note, 1, 0);
    lv_obj_set_style_shadow_width(note, 0, 0);
    lv_obj_set_style_pad_all(note, 0, 0);
    lv_obj_clear_flag(note, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return note;
}

static void wrapped_label(lv_obj_t *parent, const char *text, lv_coord_t width,
                          lv_coord_t y, const lv_font_t *font, uint32_t colour)
{
    lv_obj_t *obj = label(parent, text, font, colour, y);
    lv_obj_set_width(obj, width);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
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

static void test_clicked(lv_event_t *event)
{
    (void)event;
    if (ui.screen == SCREEN_REALTIME) {
        ui.testing = true;
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

static void library_exit_clicked(lv_event_t *event)
{
    (void)event;
    show_realtime();
}

static lv_obj_t *library_add_close(void)
{
    lv_obj_t *close = icon_button(ui.root, LV_SYMBOL_CLOSE, 44);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -77, 60);
    lv_obj_add_event_cb(close, library_exit_clicked, LV_EVENT_CLICKED, NULL);
    return close;
}

static uint8_t library_selected(void)
{
    lv_obj_t *active = lv_tileview_get_tile_active(ui.library_pager);
    for (uint8_t i = 0; i < ui.library_count; ++i) {
        if (ui.library_tiles[i] == active) return ui.library_items[i];
    }
    return 0;
}

static void library_action_refresh(void)
{
    if (ui.library_action == NULL) return;
    lv_obj_t *caption = lv_obj_get_child(ui.library_action, 0);
    uint8_t selected = library_selected();
    if (ui.screen == SCREEN_WARDROBE) {
        bool unlocked = selected < SKIN_COUNT && (ui.unlocked_skins & (1U << selected)) != 0;
        if (!unlocked) {
            lv_obj_add_flag(ui.library_action, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_clear_flag(ui.library_action, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(caption, ui.wearing == selected ? "脱下" : "穿上");
    } else if (ui.screen == SCREEN_MONOLOGUES) {
        bool unlocked = selected < CARD_COUNT && ui.card_valid[selected];
        if (!unlocked) {
            lv_obj_add_flag(ui.library_action, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_clear_flag(ui.library_action, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(caption, "阅读");
    }
}

static void library_changed(lv_event_t *event)
{
    (void)event;
    library_action_refresh();
}

static void monologue_exit_clicked(lv_event_t *event)
{
    (void)event;
    show_library(SCREEN_MONOLOGUES);
}

static void library_action_clicked(lv_event_t *event)
{
    (void)event;
    uint8_t selected = library_selected();
    if (ui.screen == SCREEN_WARDROBE) {
        if (selected >= SKIN_COUNT || (ui.unlocked_skins & (1U << selected)) == 0) return;
        if (ui.wearing == selected) {
            /* Same skin tapped again: take it off. The realtime page falls back
             * to the six score images as soon as the user returns. */
            ui.wearing = NO_SKIN;
            (void)tal_kv_set("brain_skin", &ui.wearing, 1);
            emit(BRAIN_UI_EVENT_SKIN_SET_REQUEST, "");
        } else {
            ui.wearing = selected;
            (void)tal_kv_set("brain_skin", &ui.wearing, 1);
            emit(BRAIN_UI_EVENT_SKIN_SET_REQUEST, SKIN_NAMES[selected]);
        }
        library_action_refresh();
        return;
    }
    if (ui.screen == SCREEN_MONOLOGUES && selected < CARD_COUNT && ui.card_valid[selected]) {
        ui.card_reading = selected;
        prepare_screen(SCREEN_MONOLOGUE);
        label(ui.root, "脑子的独白", &brain_font_22, DEEP, 30);
        lv_obj_t *close = icon_button(ui.root, LV_SYMBOL_CLOSE, 44);
        lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -77, 60);
        lv_obj_add_event_cb(close, monologue_exit_clicked, LV_EVENT_CLICKED, NULL);
        /* Reading view: design-library artwork with the title beneath it. */
        lv_obj_t *picture = lv_image_create(ui.root);
        lv_image_set_src(picture, POSTCARD_IMAGES[selected]);
        fit_design_image(picture, POSTCARD_IMAGES[selected], BRAIN_SIZE);
        lv_obj_align(picture, LV_ALIGN_TOP_MID, 0, 88);
        lv_obj_clear_flag(picture, LV_OBJ_FLAG_CLICKABLE);
        wrapped_label(ui.root, ui.card_titles[selected], 380, 300, &brain_font_22, DEEP);
        label(ui.root, "这段话，一直留在这里", &brain_font_16, MUTED, 384);
    }
}

/* Skins and monologues always page every entry; the lock icon and the hidden
 * action button keep locked items read-only. Insights stay presence-driven. */
static void show_library(screen_state_t state)
{
    prepare_screen(state);
    ui.library_count = 0;
    const char *heading = state == SCREEN_WARDROBE ? "皮肤" :
                          state == SCREEN_MONOLOGUES ? "独白" : "成就";
    label(ui.root, heading, &brain_font_22, DEEP, 30);
    lv_obj_t *close = library_add_close();

    if (state == SCREEN_WARDROBE) {
        for (uint8_t i = 0; i < SKIN_COUNT; ++i) ui.library_items[ui.library_count++] = i;
    } else if (state == SCREEN_MONOLOGUES) {
        for (uint8_t i = 0; i < CARD_COUNT; ++i) ui.library_items[ui.library_count++] = i;
    } else {
        for (uint8_t i = 0; i < INSIGHT_COUNT; ++i) {
            if (ui.insight_valid[i]) ui.library_items[ui.library_count++] = i;
        }
    }

    if (ui.library_count == 0) {
        label(ui.root, "暂时没有新成就", &brain_font_22, DEEP, 194);
        label(ui.root, "回 App 看看，慢慢来", &brain_font_16, MUTED, 240);
        return;
    }

    ui.library_pager = lv_tileview_create(ui.root);
    lv_obj_set_size(ui.library_pager, 466, state == SCREEN_INSIGHTS ? 344 : 304);
    lv_obj_align(ui.library_pager, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_set_style_bg_opa(ui.library_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.library_pager, 0, 0);
    lv_obj_set_style_pad_all(ui.library_pager, 0, 0);
    lv_obj_set_scrollbar_mode(ui.library_pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(ui.library_pager, library_changed, LV_EVENT_VALUE_CHANGED, NULL);

    for (uint8_t pos = 0; pos < ui.library_count; ++pos) {
        uint8_t index = ui.library_items[pos];
        lv_obj_t *tile = lv_tileview_add_tile(ui.library_pager, pos, 0, LV_DIR_HOR);
        ui.library_tiles[pos] = tile;
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        if (state == SCREEN_WARDROBE) {
            bool unlocked = (ui.unlocked_skins & (1U << index)) != 0;
            lv_obj_t *picture = lv_image_create(tile);
            lv_image_set_src(picture, SKIN_IMAGES[index]);
            fit_brain_image(picture, SKIN_IMAGES[index], BRAIN_SIZE);
            lv_obj_align(picture, LV_ALIGN_TOP_MID, 0, 0);
            lv_obj_clear_flag(picture, LV_OBJ_FLAG_CLICKABLE);
            label(tile, SKIN_TITLES[index], &brain_font_22, DEEP, 244);
            lv_obj_t *badge = icon_button(tile, unlocked ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, 36);
            lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -18, 236);
        } else if (state == SCREEN_MONOLOGUES) {
            bool unlocked = ui.card_valid[index];
            lv_obj_t *picture = lv_image_create(tile);
            lv_image_set_src(picture, POSTCARD_IMAGES[index]);
            fit_design_image(picture, POSTCARD_IMAGES[index], BRAIN_SIZE);
            lv_obj_align(picture, LV_ALIGN_TOP_MID, 0, 0);
            lv_obj_clear_flag(picture, LV_OBJ_FLAG_CLICKABLE);
            label(tile, ui.card_titles[index], &brain_font_22, DEEP, 188);
            lv_obj_t *badge = icon_button(tile, unlocked ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, 36);
            lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -18, 180);
        } else {
            lv_obj_t *note = note_card(tile, 330, 260, 28);
            wrapped_label(note, ui.insight_titles[index], 280, 42, &brain_font_22, INK);
            const char *body = ui.insight_bodies[index];
            wrapped_label(note, body[0] != '\0' ? body : "详情请返回APP查看",
                          280, 96, &brain_font_16, INK);
        }
    }

    if (state != SCREEN_INSIGHTS) {
        ui.library_action = soft_button(ui.root, "", 174, 50, &brain_font_22);
        lv_obj_align(ui.library_action, LV_ALIGN_TOP_MID, 0, 382);
        lv_obj_add_event_cb(ui.library_action, library_action_clicked, LV_EVENT_CLICKED, NULL);
        library_action_refresh();
    }
    /* The tileview is created after the close button, so it would sit on top
     * and swallow taps. Lift the close button back to the foreground. */
    lv_obj_move_foreground(close);
    if (ui.library_count > 1) {
        label(ui.root, "左右滑动", &brain_font_16, MUTED, 438);
    }
}

static void library_open_clicked(lv_event_t *event)
{
    screen_state_t state = (screen_state_t)(uintptr_t)lv_event_get_user_data(event);
    if (ui.screen == SCREEN_REALTIME) show_library(state);
}

static void tool_button_align(uint8_t index, lv_obj_t *button)
{
    if (index < 2) {
        lv_obj_align(button, LV_ALIGN_TOP_LEFT, TOOL_X[index], TOOL_Y[index]);
    } else {
        lv_obj_align(button, LV_ALIGN_TOP_RIGHT, TOOL_X[index], TOOL_Y[index]);
    }
}

static void realtime_apply_tool_mode(void)
{
    for (uint8_t i = 0; i < (uint8_t)TOOL_COUNT; ++i) {
        lv_obj_t *button = ui.tool_buttons[i];
        if (button == NULL) continue;
        lv_obj_t *caption = lv_obj_get_child(button, 0);
        if (ui.tool_text) {
            lv_obj_set_size(button, 88, 46);
            lv_obj_set_style_radius(button, 14, 0);
            lv_obj_set_style_text_font(caption, &brain_font_16, 0);
            lv_label_set_text(caption, TOOL_LABELS[i]);
        } else {
            lv_obj_set_size(button, 48, 48);
            lv_obj_set_style_radius(button, 24, 0);
            lv_obj_set_style_text_font(caption, LV_FONT_DEFAULT, 0);
            lv_label_set_text(caption, TOOL_SYMBOLS[i]);
        }
        tool_button_align(i, button);
    }
}

static void realtime_root_clicked(lv_event_t *event)
{
    (void)event;
    if (ui.screen != SCREEN_REALTIME) return;
    ui.tool_text = !ui.tool_text;
    ui.tool_text_until = ui.tool_text ? lv_tick_get() + TOOL_TEXT_MS : 0;
    realtime_apply_tool_mode();
}

static void show_realtime(void)
{
    ui.testing = false;
    prepare_screen(SCREEN_REALTIME);
    char text[24];
    snprintf(text, sizeof(text), "脑力 %u", ui.score);
    label(ui.root, "慢想屋", &brain_font_22, DEEP, 30);
    ui.score_label = label(ui.root, text, &brain_font_44, INK, 60);
    ui.image = brain(ui.root, current_image(), 236, 126);
    lv_obj_t *pill = lv_obj_create(ui.root);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 300, 52);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 368);
    lv_obj_set_style_bg_color(pill, lv_color_hex(PALE), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 26, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    ui.tag_label = label(pill, current_tag(), &brain_font_30, DEEP, 6);
    ui.hint_label = label(ui.root, "我在这儿，慢慢陪你", &brain_font_16, MUTED, 426);

    /* Five tools hug the round edge. Icons keep the page calm; one tap on the
     * panel body temporarily expands them into labelled buttons. */
    for (uint8_t i = 0; i < (uint8_t)TOOL_COUNT; ++i) {
        lv_obj_t *button = icon_button(ui.root, TOOL_SYMBOLS[i], 48);
        ui.tool_buttons[i] = button;
        tool_button_align(i, button);
        if (i == 0) {
            lv_obj_add_event_cb(button, demo_open_clicked, LV_EVENT_CLICKED, NULL);
        } else if (i == 1) {
            lv_obj_add_event_cb(button, test_clicked, LV_EVENT_CLICKED, NULL);
        } else {
            const screen_state_t states[] = {SCREEN_REALTIME, SCREEN_REALTIME,
                                              SCREEN_WARDROBE, SCREEN_MONOLOGUES, SCREEN_INSIGHTS};
            lv_obj_add_event_cb(button, library_open_clicked, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)states[i]);
        }
    }
    realtime_apply_tool_mode();
    lv_obj_add_event_cb(ui.root, realtime_root_clicked, LV_EVENT_CLICKED, NULL);
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

/* Early end no longer has a confirmation page. The 2-second hold is the only
 * guard: once it completes, the panel lands directly on a small thing. */
static void tick(lv_timer_t *timer)
{
    (void)timer;

    if (ui.screen == SCREEN_REALTIME && ui.tool_text && lv_tick_get() >= ui.tool_text_until) {
        ui.tool_text = false;
        ui.tool_text_until = 0;
        realtime_apply_tool_mode();
    }

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
            if (remaining == 5) brain_voice_play(BRAIN_VOICE_FIVE_SECONDS);
        }

        if (remaining <= 5 && remaining > 0) {
            /* Last five seconds: the breath gets deeper and the whole panel
             * brightens and dims with it, like a breathing lamp. Never a hard
             * flash, and no warning red. */
            uint8_t level = (uint8_t)breath_level(BREATH_FAST_MS, 255);
            brain_breathe(BREATH_FAST_MS, 26);
            apply_background(level);
            lv_obj_set_style_opa(ui.ring, (lv_opa_t)(190 + level * 65 / 255), 0);
            /* The style changes are cheap but LVGL needs the refresh requested
             * on every 100 ms tick for the pulse to become a lamp. */
            lv_obj_invalidate(ui.root);
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
                emit(BRAIN_UI_EVENT_END_REQUEST, NULL);
                brain_ui_show_break_menu();
                return;
            }
        }

        if (remaining == 0) {
            brain_voice_play(BRAIN_VOICE_TIME_UP);
            emit(BRAIN_UI_EVENT_TIME_UP, NULL);
            brain_ui_show_break_menu();
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
    uint8_t *saved = NULL;
    size_t length = 0;
    if (tal_kv_get("brain_skin", &saved, &length) == OPRT_OK && saved != NULL) {
        if (length == 1 && saved[0] < SKIN_COUNT) ui.wearing = saved[0];
        tal_kv_free(saved);
    }
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
    /* Kept for the existing BLE callback; the screen no longer changes on link
     * events, which is what used to make the panel flash on every reconnect. */
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
    return ui.screen == SCREEN_FOCUS || ui.screen == SCREEN_BREAK_MENU ||
           ui.screen == SCREEN_BREAK;
}

void brain_ui_start_focus(uint32_t duration_seconds)
{
    if (duration_seconds == 0) duration_seconds = 1;
    if (duration_seconds > MAX_MINUTES) duration_seconds = MAX_MINUTES;
    prepare_screen(SCREEN_FOCUS);
    ui.focus_seconds = duration_seconds;
    ui.focus_started = lv_tick_get();
    ui.last_remaining = duration_seconds + 1;
    make_ring(9, MINT);
    label(ui.root, "陪伴中", &brain_font_22, DEEP, 30);
    ui.image = brain(ui.root, current_image(), BRAIN_SIZE, 64);
    ui.countdown = label(ui.root, "00:00", &brain_font_44, INK, 312);
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

void brain_ui_end_focus(uint8_t latest_brain_score)
{
    brain_ui_set_brain_score(latest_brain_score);
    show_realtime();
}

void brain_ui_show_break_menu(void)
{
    prepare_screen(SCREEN_BREAK_MENU);
    ui.break_menu_at = lv_tick_get();

    label(ui.root, "陪你做的一件小事", &brain_font_22, DEEP, 24);

    /* One option per screen, swipe sideways. A 2x2 grid forced the copy and the
     * button into the round screen's corners; a pager keeps everything centred.
     * The tile is sized for the brain plus its line: a tile clips its children,
     * so anything taller than the pager would silently disappear. */
    ui.break_pager = lv_tileview_create(ui.root);
    lv_obj_set_size(ui.break_pager, 466, 300);
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
        fit_brain_image(picture, source, BRAIN_SIZE);
        lv_obj_align(picture, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_clear_flag(picture, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *copy = lv_label_create(tile);
        lv_label_set_text(copy, BREAK_COPY[i]);
        lv_obj_set_style_text_font(copy, &brain_font_22, 0);
        lv_obj_set_style_text_color(copy, lv_color_hex(DEEP), 0);
        lv_obj_align(copy, LV_ALIGN_TOP_MID, 0, 250);
    }

    /* Confirm button: an icon in the bottom-right, hugging the round edge. */
    lv_obj_t *button = icon_button(ui.root, LV_SYMBOL_OK, 52);
    lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, -70, -70);
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
    ui.image = brain(ui.root, source, BRAIN_SIZE, 82);
    label(ui.root, BREAK_COPY[type], &brain_font_30, DEEP, 332);
    label(ui.root, "15秒后，我回到这里陪你", &brain_font_18, MUTED, 392);
}

void brain_ui_set_skins(uint8_t unlocked_mask, const char *wearing)
{
    uint8_t previous_mask = ui.unlocked_skins;
    uint8_t previous_wearing = ui.wearing;
    ui.unlocked_skins = unlocked_mask;
    ui.wearing = NO_SKIN;
    if (wearing != NULL) {
        for (uint8_t i = 0; i < SKIN_COUNT; ++i) {
            if (strcmp(wearing, SKIN_NAMES[i]) == 0 && (ui.unlocked_skins & (1U << i)) != 0) {
                ui.wearing = i;
                break;
            }
        }
    }
    if (previous_wearing != ui.wearing) (void)tal_kv_set("brain_skin", &ui.wearing, 1);
    if (ui.screen == SCREEN_REALTIME && ui.image != NULL &&
        (previous_wearing != ui.wearing || previous_mask != ui.unlocked_skins)) {
        lv_image_set_src(ui.image, current_image());
    } else if (ui.screen == SCREEN_WARDROBE &&
               (previous_wearing != ui.wearing || previous_mask != ui.unlocked_skins)) {
        show_library(SCREEN_WARDROBE);
    }
}

void brain_ui_set_postcard(uint8_t index, bool present, const char *title, const char *line, uint8_t image_index)
{
    if (index >= CARD_COUNT) return;
    /* The design-library artwork is fixed per card; the App's brain-image hint
     * stays in the protocol but no longer drives the panel. */
    (void)image_index;
    bool changed = ui.card_valid[index] != present ||
                   strcmp(ui.card_titles[index], present && title ? title : "") != 0 ||
                   strcmp(ui.card_lines[index], present && line ? line : "") != 0;
    ui.card_valid[index] = present;
    snprintf(ui.card_titles[index], sizeof(ui.card_titles[index]), "%s", present && title ? title : "");
    snprintf(ui.card_lines[index], sizeof(ui.card_lines[index]), "%s", present && line ? line : "");
    if (changed && ui.screen == SCREEN_MONOLOGUES) show_library(SCREEN_MONOLOGUES);
}

void brain_ui_set_insight(uint8_t index, bool present, const char *title, const char *body)
{
    if (index >= INSIGHT_COUNT) return;
    bool changed = ui.insight_valid[index] != present ||
                   strcmp(ui.insight_titles[index], present && title ? title : "") != 0 ||
                   strcmp(ui.insight_bodies[index], present && body ? body : "") != 0;
    ui.insight_valid[index] = present;
    snprintf(ui.insight_titles[index], sizeof(ui.insight_titles[index]), "%s", present && title ? title : "");
    snprintf(ui.insight_bodies[index], sizeof(ui.insight_bodies[index]), "%s", present && body ? body : "");
    if (changed && ui.screen == SCREEN_INSIGHTS) show_library(SCREEN_INSIGHTS);
}
