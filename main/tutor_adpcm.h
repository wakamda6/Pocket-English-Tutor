#pragma once

#include "tutor_pcm.h"

typedef struct {
    int16_t first_sample;
    uint16_t count;
    uint8_t step_index;
    uint8_t data[TUTOR_FRAME_SAMPLES / 2];
} tutor_adpcm_block_t;

// Each block begins with a predictor and index so dropped blocks do not corrupt the next one.
void tutor_adpcm_encode(tutor_adpcm_block_t *block, const int16_t *samples,
                         size_t count, unsigned *step_index);
size_t tutor_adpcm_decode(const tutor_adpcm_block_t *block, int16_t samples[TUTOR_FRAME_SAMPLES]);
