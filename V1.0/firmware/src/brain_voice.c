#include "brain_voice.h"

#include "tal_api.h"
#include "tdl_audio_manage.h"

extern const uint8_t voice_five_seconds[];
extern const uint32_t voice_five_seconds_len;
extern const uint8_t voice_time_up[];
extern const uint32_t voice_time_up_len;

#define VOICE_STACK 4096
#define VOICE_PRIO  2

static TDL_AUDIO_HANDLE_T s_audio;
static THREAD_HANDLE s_thread;
static volatile brain_voice_t s_request = BRAIN_VOICE_NONE;
static bool s_opened;
static TDL_AUDIO_INFO_T s_info;

static bool ensure_open(void)
{
    if (s_opened) return true;
    if (s_audio == NULL) return false;
    if (tdl_audio_open(s_audio, NULL) != OPRT_OK) {
        PR_ERR("[voice] codec open failed");
        return false;
    }
    if (tdl_audio_get_info(s_audio, &s_info) != OPRT_OK || s_info.frame_size == 0) {
        PR_ERR("[voice] codec info unavailable");
        return false;
    }
    (void)tdl_audio_volume_set(s_audio, 80);
    s_opened = true;
    return true;
}

static void play_clip(brain_voice_t clip)
{
    const uint8_t *data = clip == BRAIN_VOICE_FIVE_SECONDS ? voice_five_seconds : voice_time_up;
    uint32_t total = clip == BRAIN_VOICE_FIVE_SECONDS ? voice_five_seconds_len : voice_time_up_len;

    if (total == 0 || !ensure_open()) return;
    PR_NOTICE("[voice] play clip=%d bytes=%lu", clip, (unsigned long)total);

    /* Keep the codec open for the product lifetime. The T5AI audio driver
     * cannot re-open its ADC pipeline: a second open reports aud_adc_init fail
     * and then faults. Only clear the queued PCM between phrases. */
    (void)tdl_audio_play_stop(s_audio);
    for (uint32_t offset = 0; offset < total; offset += s_info.frame_size) {
        uint32_t length = total - offset;
        if (length > s_info.frame_size) length = s_info.frame_size;
        uint8_t *frame = (uint8_t *)(data + offset);
        if (tdl_audio_play(s_audio, frame, length) != OPRT_OK) break;
        tal_system_sleep(s_info.sample_tm_ms > 0 ? s_info.sample_tm_ms : 20);
    }
    tal_system_sleep(80);
}

static void voice_task(void *arg)
{
    (void)arg;
    for (;;) {
        brain_voice_t clip = s_request;
        if (clip != BRAIN_VOICE_NONE) {
            s_request = BRAIN_VOICE_NONE;
            play_clip(clip);
        }
        tal_system_sleep(50);
    }
}

bool brain_voice_init(void)
{
    if (tdl_audio_find(AUDIO_CODEC_NAME, &s_audio) != OPRT_OK || s_audio == NULL) {
        PR_ERR("[voice] audio codec not found");
        return false;
    }

    THREAD_CFG_T cfg = {
        .stackDepth = VOICE_STACK,
        .priority = VOICE_PRIO,
        .thrdname = "brain_voice",
    };
    if (tal_thread_create_and_start(&s_thread, NULL, NULL, voice_task, NULL, &cfg) != OPRT_OK) {
        PR_ERR("[voice] worker create failed");
        return false;
    }
    PR_NOTICE("[voice] ready");
    return true;
}

void brain_voice_play(brain_voice_t clip)
{
    if (clip != BRAIN_VOICE_NONE) s_request = clip;
}
