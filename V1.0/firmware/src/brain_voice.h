#ifndef BRAIN_VOICE_H
#define BRAIN_VOICE_H

#include <stdbool.h>

typedef enum {
    BRAIN_VOICE_NONE = 0,
    BRAIN_VOICE_FIVE_SECONDS,
    BRAIN_VOICE_TIME_UP,
} brain_voice_t;

/* Finds the board codec and starts the low-priority playback worker. */
bool brain_voice_init(void);

/* Non-blocking: the worker owns the codec, so the LVGL task never stalls. */
void brain_voice_play(brain_voice_t clip);

#endif
