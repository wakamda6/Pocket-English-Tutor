#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TUTOR_PCM_RATE 16000
#define TUTOR_CLOUD_PCM_RATE 24000
#define TUTOR_FRAME_SAMPLES 320

typedef struct {
    int16_t window[3];
    unsigned count;
} tutor_pcm_t;

typedef struct {
    tutor_pcm_t resampler;
    bool have_low_byte;
    uint8_t low_byte;
} tutor_pcm_stream_t;

// Maximum output samples given new bytes and any partial sample/window.
#define TUTOR_PCM_OUTPUT_CAP(bytes) (2 * (((bytes) + 5) / 6))

// Area-average three 24 kHz samples into two 16 kHz samples.
bool tutor_pcm_push(tutor_pcm_t *state, int16_t input, int16_t output[2]);

// Drain the final partial window, if any, at the end of a reply.
unsigned tutor_pcm_finish(tutor_pcm_t *state, int16_t output[2]);

size_t tutor_pcm_convert_le(tutor_pcm_stream_t *state, const uint8_t *pcm, size_t length,
                            int16_t *output);
