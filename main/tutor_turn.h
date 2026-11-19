#pragma once

#include <stdbool.h>

typedef enum {
    TUTOR_IDLE,
    TUTOR_CONNECTING,
    TUTOR_READY,
    TUTOR_ARMING,
    TUTOR_LISTENING,
    TUTOR_SENDING,
    TUTOR_THINKING,
    TUTOR_SPEAKING,
    TUTOR_CLOSING,
    TUTOR_CLOUD_FAILED,
    TUTOR_AUDIO_FAILED,
} tutor_status_t;

static inline bool tutor_turn_can_start(tutor_status_t cloud, bool streaming, bool playing)
{
    return cloud == TUTOR_READY && !streaming && !playing;
}

static inline bool tutor_turn_accepts_reply(tutor_status_t cloud)
{
    return cloud == TUTOR_LISTENING || cloud == TUTOR_THINKING || cloud == TUTOR_SPEAKING;
}

static inline tutor_status_t tutor_turn_view(tutor_status_t cloud, bool requested,
                                            bool streaming, bool playing)
{
    if (cloud >= TUTOR_READY && cloud <= TUTOR_SPEAKING) {
        if (requested) return streaming ? TUTOR_LISTENING : TUTOR_ARMING;
        if (streaming) return TUTOR_SENDING;
        if (cloud == TUTOR_READY && playing) return TUTOR_SPEAKING;
    }
    return cloud;
}
