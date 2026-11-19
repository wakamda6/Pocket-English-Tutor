#pragma once

#include <stdbool.h>
#include <stddef.h>

// Each block is 20 ms of PCM.
#define TUTOR_PLAYBACK_BLOCKS 170
#define TUTOR_PREBUFFER_BLOCKS 125

static inline bool tutor_playback_can_start(bool submitted, size_t queued, bool final)
{
    return submitted && queued > 0 && (final || queued >= TUTOR_PREBUFFER_BLOCKS);
}
