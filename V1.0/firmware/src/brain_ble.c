#include "brain_ble.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "tal_log.h"
#include "tal_system.h"
#include "tal_time_service.h"
#include "tal_workq_service.h"
#include "tal_bluetooth.h"

#include "ble_hs.h"
#include "ble_hs_mbuf.h"
#include "tuya_ble_mbuf.h"

#define BRAIN_BLE_CONN_INVALID 0xFFFF

#define BRAIN_BLE_ACK_OK             0x00
#define BRAIN_BLE_ACK_INVALID_LENGTH 0x01
#define BRAIN_BLE_ACK_INVALID_SCORE  0x02

#define BRAIN_PROTO_VERSION 1
#define BRAIN_ID_LEN        40
#define BRAIN_TX_LEN        320

/* 12345678-1234-5678-9abc-def012345601/02/03, little-endian for NimBLE. */
#define BRAIN_SVC_UUID_LE \
    0x01, 0x56, 0x34, 0x12, 0xF0, 0xDE, 0xBC, 0x9A, \
    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12
#define BRAIN_WRITE_UUID_LE \
    0x02, 0x56, 0x34, 0x12, 0xF0, 0xDE, 0xBC, 0x9A, \
    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12
#define BRAIN_ACK_UUID_LE \
    0x03, 0x56, 0x34, 0x12, 0xF0, 0xDE, 0xBC, 0x9A, \
    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12

static brain_ble_score_cb_t g_score_cb;
static brain_ble_link_cb_t g_link_cb;
static brain_ble_frame_cb_t g_frame_cb;
static uint16_t g_conn_handle = BRAIN_BLE_CONN_INVALID;
static uint16_t g_ack_val_handle;
static bool g_ack_notify_enabled;

/* Protocol state. Single pending transmit slot: UI events are seconds apart, so
 * dropping a duplicate is better than letting two tasks share one buffer. */
static char g_session_id[BRAIN_ID_LEN];
/* Small ring instead of one slot: a FOCUS_START between two repeats would
 * otherwise evict the ID we still need to recognise as a duplicate. */
#define BRAIN_ID_RING 6
static char g_recent_ids[BRAIN_ID_RING][BRAIN_ID_LEN];
static uint8_t g_recent_pos;
/* Last score actually applied to the screen, so ACKs report device truth. */
static uint8_t g_state_score = 80;
static char g_tx_buf[BRAIN_TX_LEN];
static volatile bool g_tx_busy;

static const ble_uuid128_t g_brain_svc_uuid = BLE_UUID128_INIT(BRAIN_SVC_UUID_LE);
static const ble_uuid128_t g_brain_write_uuid = BLE_UUID128_INIT(BRAIN_WRITE_UUID_LE);
static const ble_uuid128_t g_brain_ack_uuid = BLE_UUID128_INIT(BRAIN_ACK_UUID_LE);

/* Flags + complete name: 20 bytes, within the legacy 31-byte limit. */
static uint8_t g_adv_data[] = {
    0x02, 0x01, 0x06,
    0x10, 0x09,
    'S', 'l', 'o', 'w', 'H', 'o', 'u', 's', 'e', '-', 'B', 'r', 'a', 'i', 'n',
};

/* The 128-bit service UUID goes in scan response to keep advertising small. */
static uint8_t g_scan_rsp_data[] = {
    0x11, 0x07, BRAIN_SVC_UUID_LE,
};

static bool score_to_index(uint8_t score, uint8_t *state_index)
{
    if (score > 100 || (score % 20) != 0) {
        return false;
    }

    *state_index = score / 20;
    return true;
}

static void brain_send_ack(uint16_t conn_handle, uint8_t status, uint8_t score)
{
    struct os_mbuf *om;
    uint8_t ack[] = {status, score};
    int rc;

    if (conn_handle != g_conn_handle || !g_ack_notify_enabled || g_ack_val_handle == 0) {
        PR_NOTICE("[brain_ble] ACK not subscribed: status=%u score=%u", status, score);
        return;
    }

    om = ble_hs_mbuf_from_flat(ack, sizeof(ack));
    if (om == NULL) {
        PR_ERR("[brain_ble] no mbuf for ACK");
        return;
    }

    /* notify_custom consumes om on both success and failure. */
    rc = ble_gattc_notify_custom(conn_handle, g_ack_val_handle, om);
    if (rc != 0) {
        PR_ERR("[brain_ble] ACK notify failed: rc=%d", rc);
    } else {
        PR_NOTICE("[brain_ble] ACK sent: status=%u score=%u", status, score);
    }
}

/* -------------------- protocol frames (plan section 5) -------------------- */

/* IDs travel in JSON, so keep only characters that need no escaping. */
static void copy_id(char *dst, size_t cap, const char *src)
{
    size_t n = 0;

    if (dst == NULL || cap == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }
    for (; *src != '\0' && n + 1 < cap; ++src) {
        char c = *src;
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (safe) {
            dst[n++] = c;
        }
    }
    dst[n] = '\0';
}

static void brain_notify_bytes(const uint8_t *data, uint16_t len)
{
    struct os_mbuf *om;
    int rc;

    if (g_conn_handle == BRAIN_BLE_CONN_INVALID || !g_ack_notify_enabled ||
        g_ack_val_handle == 0) {
        return;
    }

    om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        PR_ERR("[brain_ble] no mbuf for frame len=%u", len);
        return;
    }

    rc = ble_gattc_notify_custom(g_conn_handle, g_ack_val_handle, om);
    if (rc != 0) {
        PR_ERR("[brain_ble] frame notify failed: rc=%d", rc);
    }
}

static void brain_send_ack_frame(const char *ack_event_id, uint8_t status, uint8_t score)
{
    char buf[BRAIN_TX_LEN];
    char id[BRAIN_ID_LEN];

    copy_id(id, sizeof(id), ack_event_id);
    snprintf(buf, sizeof(buf),
             "{\"protocolVersion\":%d,\"eventType\":\"STATE_ACK\",\"sessionID\":\"%s\","
             "\"timestamp\":%ld,\"payload\":{\"ackEventID\":\"%s\",\"status\":%u,"
             "\"brainScore\":%u,\"capabilities\":[\"CONTENT_SYNC\",\"SKIN_SET\"]}}",
             BRAIN_PROTO_VERSION, g_session_id, (long)tal_time_get_posix(), id,
             (unsigned)status, (unsigned)score);
    brain_notify_bytes((const uint8_t *)buf, (uint16_t)strlen(buf));
}

static bool recent_seen(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    for (uint8_t i = 0; i < BRAIN_ID_RING; ++i) {
        if (strcmp(g_recent_ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

static void recent_add(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return;
    }
    copy_id(g_recent_ids[g_recent_pos], BRAIN_ID_LEN, id);
    g_recent_pos = (uint8_t)((g_recent_pos + 1) % BRAIN_ID_RING);
}

void brain_ble_note_score(uint8_t score)
{
    if (score <= 100) {
        g_state_score = score;
    }
}

static const char *json_str(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return (cJSON_IsString(item) && item->valuestring != NULL) ? item->valuestring : NULL;
}

static int json_int(const cJSON *root, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

/* JSONSerialization emits Swift Bool as `true`/`false`, not 1/0. */
static bool json_bool(const cJSON *root, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) return item->valueint != 0;
    return fallback;
}

/* Returns false when the buffer is not a JSON frame, so the caller can fall
 * back to the one-byte debug write path. */
static bool brain_dispatch_json(const uint8_t *data, uint16_t len)
{
    /* One complete UTF-8 content frame, including envelope and NUL. */
    char body[384];
    cJSON *root;
    const cJSON *payload;
    brain_ble_frame_t frame;
    const char *type;
    char session[BRAIN_ID_LEN];
    char event_id[BRAIN_ID_LEN];
    int score = -1;
    int seconds = 0;
    bool starts_session;
    bool duplicate;
    bool stale;

    if (len == 0 || data[0] != '{' || len >= sizeof(body)) {
        return false;
    }

    memcpy(body, data, len);
    body[len] = '\0';

    root = cJSON_Parse(body);
    if (root == NULL) {
        PR_ERR("[brain_ble] malformed json (%u bytes)", len);
        return false;
    }

    type = json_str(root, "eventType");
    if (type == NULL) {
        PR_ERR("[brain_ble] frame without eventType");
        cJSON_Delete(root);
        return true;
    }

    payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (payload != NULL) {
        score = json_int(payload, "brainScore", -1);
        seconds = json_int(payload, "durationSeconds", 0);
        if (seconds <= 0) {
            /* Absolute schedule: derive the remaining span from endsAt. */
            int ends_at = json_int(payload, "endsAt", 0);
            int stamp = json_int(root, "timestamp", 0);
            if (ends_at > 0 && stamp > 0 && ends_at > stamp) {
                seconds = ends_at - stamp;
            }
        }
    }

    copy_id(event_id, sizeof(event_id), json_str(root, "eventID"));
    copy_id(session, sizeof(session), json_str(root, "sessionID"));

    starts_session = strcmp(type, "FOCUS_START") == 0;
    if (starts_session) {
        copy_id(g_session_id, sizeof(g_session_id), session);
    }

    duplicate = recent_seen(event_id);
    stale = !starts_session && session[0] != '\0' && g_session_id[0] != '\0' &&
            strcmp(session, g_session_id) != 0;

    if (duplicate || stale) {
        PR_NOTICE("[brain_ble] %s ignored (%s) session=%s", type,
                  duplicate ? "duplicate" : "stale session", session);
        /* Report device truth, not the value the phone just asked for. */
        brain_send_ack_frame(event_id, BRAIN_BLE_ACK_OK, g_state_score);
        cJSON_Delete(root);
        return true;
    }

    recent_add(event_id);

    frame.type = type;
    frame.session_id = session;
    frame.event_id = event_id;
    frame.brain_score = (score >= 0 && score <= 100) ? (uint8_t)score : 0xFF;
    frame.seconds = (uint32_t)(seconds > 0 ? seconds : 0);
    frame.break_type = payload != NULL ? json_str(payload, "breakType") : NULL;
    frame.skin_mask = payload != NULL ? (uint8_t)(json_int(payload, "unlockedSkinMask", 0) & 0xFF) : 0;
    frame.wearing = payload != NULL ? json_str(payload, "wearingSkin") : NULL;
    frame.content_kind = payload != NULL ? json_str(payload, "contentKind") : NULL;
    frame.content_index = payload != NULL ? json_int(payload, "index", -1) : -1;
    frame.content_present = payload != NULL && json_bool(payload, "present", false);
    frame.title = payload != NULL ? json_str(payload, "title") : NULL;
    frame.text = payload != NULL ? json_str(payload, "text") : NULL;
    frame.image_index = payload != NULL ? json_int(payload, "img", -1) : -1;

    PR_NOTICE("[brain_ble] %s score=%d seconds=%u", type, score, frame.seconds);
    if (g_frame_cb != NULL) {
        g_frame_cb(&frame);
    }
    /* Content is fire-and-forget and arrives in bursts. An ACK per frame doubled
     * the HCI traffic and once exhausted the 13-buffer ACL pool, resetting the
     * whole application. Keep ACKs for state-bearing commands instead. */
    if (strcmp(type, "CONTENT_SYNC") != 0) {
        brain_send_ack_frame(event_id, BRAIN_BLE_ACK_OK, g_state_score);
    }
    cJSON_Delete(root);
    return true;
}

static void brain_emit_worker(void *arg)
{
    (void)arg;
    brain_notify_bytes((const uint8_t *)g_tx_buf, (uint16_t)strlen(g_tx_buf));
    g_tx_busy = false;
}

void brain_ble_emit(const char *event_type, const char *detail)
{
    static uint32_t counter;
    char event_id[BRAIN_ID_LEN];

    if (event_type == NULL || !g_ack_notify_enabled || g_tx_busy) {
        return;
    }

    snprintf(event_id, sizeof(event_id), "dev-%08lx",
             (unsigned long)(tal_system_get_millisecond() ^ (counter++ << 16)));

    if (detail != NULL) {
        snprintf(g_tx_buf, sizeof(g_tx_buf),
                 "{\"protocolVersion\":%d,\"eventID\":\"%s\",\"eventType\":\"%s\","
                 "\"sessionID\":\"%s\",\"timestamp\":%ld,\"payload\":{\"detail\":\"%s\"}}",
                 BRAIN_PROTO_VERSION, event_id, event_type, g_session_id,
                 (long)tal_time_get_posix(), detail);
    } else {
        snprintf(g_tx_buf, sizeof(g_tx_buf),
                 "{\"protocolVersion\":%d,\"eventID\":\"%s\",\"eventType\":\"%s\","
                 "\"sessionID\":\"%s\",\"timestamp\":%ld,\"payload\":{}}",
                 BRAIN_PROTO_VERSION, event_id, event_type, g_session_id,
                 (long)tal_time_get_posix());
    }

    /* Outside the BLE host task: let the work queue own the mbuf and radio. */
    g_tx_busy = true;
    if (tal_workq_schedule(WORKQ_SYSTEM, brain_emit_worker, NULL) != OPRT_OK) {
        g_tx_busy = false;
        PR_ERR("[brain_ble] emit queue full, dropped %s", event_type);
    }
}

void brain_ble_set_frame_callback(brain_ble_frame_cb_t frame_cb)
{
    g_frame_cb = frame_cb;
}

static int brain_ack_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;

    /* ACK is notify-only. NimBLE still requires a non-NULL access callback
     * for every characteristic, otherwise GATT registration returns EINVAL. */
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

static int brain_write_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t state_index;
    uint8_t score = 0xFF;
    uint8_t frame[384];
    uint16_t plen;
    int rc;

    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || conn_handle != g_conn_handle) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    plen = OS_MBUF_PKTLEN(ctxt->om);

    /* Anything longer than one byte is a protocol frame; the one-byte form stays
     * as the manual debug path used from nRF Connect. */
    if (plen > 1) {
        if (plen > sizeof(frame)) {
            brain_send_ack_frame("", BRAIN_BLE_ACK_INVALID_LENGTH, 0);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (os_mbuf_copydata(ctxt->om, 0, plen, frame) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (brain_dispatch_json(frame, plen)) {
            return 0;
        }
        PR_ERR("[brain_ble] unsupported write of %u bytes", plen);
        brain_send_ack_frame("", BRAIN_BLE_ACK_INVALID_LENGTH, 0);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (plen != 1) {
        if (OS_MBUF_PKTLEN(ctxt->om) > 0) {
            (void)os_mbuf_copydata(ctxt->om, 0, 1, &score);
        }
        brain_send_ack(conn_handle, BRAIN_BLE_ACK_INVALID_LENGTH, score);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    rc = os_mbuf_copydata(ctxt->om, 0, 1, &score);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (!score_to_index(score, &state_index)) {
        brain_send_ack(conn_handle, BRAIN_BLE_ACK_INVALID_SCORE, score);
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    /* Short callback: update LVGL under its lock, then return to the BLE host. */
    g_score_cb(state_index, score);
    brain_send_ack(conn_handle, BRAIN_BLE_ACK_OK, score);
    return 0;
}

static struct ble_gatt_chr_def g_brain_chrs[] = {
    {
        .uuid = &g_brain_write_uuid.u,
        .access_cb = brain_write_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &g_brain_ack_uuid.u,
        .access_cb = brain_ack_access,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_ack_val_handle,
    },
    {
        .uuid = NULL,
    },
};

static const struct ble_gatt_svc_def g_brain_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_brain_svc_uuid.u,
        .characteristics = g_brain_chrs,
    },
    {
        .type = 0,
    },
};

static OPERATE_RET brain_start_advertising(void)
{
    TAL_BLE_DATA_T adv = {
        .len = sizeof(g_adv_data),
        .p_data = g_adv_data,
    };
    TAL_BLE_DATA_T rsp = {
        .len = sizeof(g_scan_rsp_data),
        .p_data = g_scan_rsp_data,
    };
    OPERATE_RET ret;

    ret = tal_ble_advertising_data_set(&adv, &rsp);
    if (ret != OPRT_OK) {
        PR_ERR("[brain_ble] set advertising data failed: %d", ret);
        return ret;
    }

    ret = tal_ble_advertising_start(TUYAOS_BLE_DEFAULT_ADV_PARAM);
    if (ret != OPRT_OK) {
        PR_ERR("[brain_ble] start advertising failed: %d", ret);
        return ret;
    }

    if (!ble_gap_adv_active()) {
        PR_ERR("[brain_ble] advertising API succeeded but radio is not advertising");
        return OPRT_COM_ERROR;
    }

    PR_NOTICE("[brain_ble] advertising active as SlowHouse-Brain");
    return OPRT_OK;
}

static void brain_ble_event_cb(TAL_BLE_EVT_PARAMS_T *event)
{
    switch (event->type) {
    case TAL_BLE_STACK_INIT:
        PR_NOTICE("[brain_ble] stack init event: result=%u enabled=%d",
                  event->ble_event.init, ble_hs_is_enabled());
        if (event->ble_event.init == 0) {
            /* Match the official peripheral example: wait for host/controller sync. */
            tal_system_sleep(1000);
            PR_NOTICE("[brain_ble] starting advertising: enabled=%d", ble_hs_is_enabled());
            (void)brain_start_advertising();
        } else {
            PR_ERR("[brain_ble] stack init failed: %u", event->ble_event.init);
        }
        break;

    case TAL_BLE_EVT_PERIPHERAL_CONNECT:
        if (event->ble_event.connect.result == 0) {
            g_conn_handle = event->ble_event.connect.peer.conn_handle;
            g_ack_notify_enabled = false;
            PR_NOTICE("[brain_ble] connected: handle=0x%04x", g_conn_handle);
            if (g_link_cb != NULL) {
                g_link_cb(true);
            }
        } else {
            g_conn_handle = BRAIN_BLE_CONN_INVALID;
            g_ack_notify_enabled = false;
            if (g_link_cb != NULL) {
                g_link_cb(false);
            }
            (void)brain_start_advertising();
        }
        break;

    case TAL_BLE_EVT_DISCONNECT:
        PR_NOTICE("[brain_ble] disconnected: reason=%u", event->ble_event.disconnect.reason);
        g_conn_handle = BRAIN_BLE_CONN_INVALID;
        g_ack_notify_enabled = false;
        if (g_link_cb != NULL) {
            g_link_cb(false);
        }
        (void)brain_start_advertising();
        break;

    case TAL_BLE_EVT_SUBSCRIBE: {
        TAL_BLE_SUBSCRBE_EVT_T *sub = &event->ble_event.subscribe;
        PR_NOTICE("[brain_ble] subscribe: conn=0x%04x char=0x%04x ack=0x%04x notify=%u",
                  sub->conn_handle, sub->char_handle, g_ack_val_handle, sub->cur_notify);
        if (sub->conn_handle == g_conn_handle && g_ack_val_handle != 0 &&
            (sub->char_handle == g_ack_val_handle ||
             sub->char_handle == g_ack_val_handle + 1)) {
            g_ack_notify_enabled = sub->cur_notify != 0;
            PR_NOTICE("[brain_ble] ACK notify=%u", g_ack_notify_enabled);
            if (g_ack_notify_enabled) {
                /* Plan: after a reconnect the device asks the phone for the
                 * current truth instead of trusting a stale local screen. */
                brain_ble_emit("STATE_REQUEST", NULL);
            }
        }
        break;
    }

    case TAL_BLE_EVT_MTU_REQUEST:
        PR_NOTICE("[brain_ble] MTU changed: %u", event->ble_event.exchange_mtu.mtu);
        break;

    default:
        break;
    }
}

void brain_ble_set_link_callback(brain_ble_link_cb_t link_cb)
{
    g_link_cb = link_cb;
}

OPERATE_RET brain_ble_init(brain_ble_score_cb_t score_cb)
{
    OPERATE_RET ret;
    int rc;

    if (score_cb == NULL) {
        return OPRT_INVALID_PARM;
    }
    g_score_cb = score_cb;

    /* Custom services must be queued before the NimBLE host starts. */
    rc = ble_gatts_count_cfg(g_brain_svcs);
    if (rc != 0) {
        PR_ERR("[brain_ble] count GATT config failed: %d", rc);
        return OPRT_COM_ERROR;
    }

    rc = ble_gatts_add_svcs(g_brain_svcs);
    if (rc != 0) {
        PR_ERR("[brain_ble] add GATT services failed: %d", rc);
        return OPRT_COM_ERROR;
    }

    ret = tal_ble_bt_init(TAL_BLE_ROLE_PERIPERAL, brain_ble_event_cb);
    if (ret != OPRT_OK) {
        PR_ERR("[brain_ble] BLE init failed: %d", ret);
    }
    return ret;
}
