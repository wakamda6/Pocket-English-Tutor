#include "tutor_pcm.h"

bool tutor_pcm_push(tutor_pcm_t *state, int16_t input, int16_t output[2])
{
    state->window[state->count++] = input;
    if (state->count < 3) {
        return false;
    }

    output[0] = (2 * (int32_t)state->window[0] + state->window[1]) / 3;
    output[1] = (state->window[1] + 2 * (int32_t)state->window[2]) / 3;
    state->count = 0;
    return true;
}

unsigned tutor_pcm_finish(tutor_pcm_t *state, int16_t output[2])
{
    if (state->count == 0) {
        return 0;
    }
    if (state->count == 1) {
        output[0] = state->window[0];
    } else {
        output[0] = (2 * (int32_t)state->window[0] + state->window[1]) / 3;
    }
    state->count = 0;
    return 1;
}

size_t tutor_pcm_convert_le(tutor_pcm_stream_t *state, const uint8_t *pcm, size_t length,
                            int16_t *output)
{
    size_t count = 0;
    for (size_t i = 0; i < length; i++) {
        if (!state->have_low_byte) {
            state->low_byte = pcm[i];
            state->have_low_byte = true;
            continue;
        }
        int16_t sample = (int16_t)((uint16_t)pcm[i] << 8 | state->low_byte);
        state->have_low_byte = false;
        if (tutor_pcm_push(&state->resampler, sample, output + count)) count += 2;
    }
    return count;
}
