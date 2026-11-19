#include <assert.h>
#include <stdint.h>
#include "tutor_pcm.h"

int main(void)
{
    tutor_pcm_t state = {0};
    int16_t samples[2];
    assert(!tutor_pcm_push(&state, 1200, samples));
    assert(!tutor_pcm_push(&state, 2400, samples));
    assert(tutor_pcm_push(&state, 3600, samples));
    assert(samples[0] == 1600 && samples[1] == 3200);
    assert(tutor_pcm_finish(&state, samples) == 0);

    assert(!tutor_pcm_push(&state, INT16_MIN, samples));
    assert(!tutor_pcm_push(&state, INT16_MIN, samples));
    assert(tutor_pcm_push(&state, INT16_MIN, samples));
    assert(samples[0] == INT16_MIN && samples[1] == INT16_MIN);

    assert(!tutor_pcm_push(&state, INT16_MAX, samples));
    assert(tutor_pcm_finish(&state, samples) == 1 && samples[0] == INT16_MAX);
    assert(!tutor_pcm_push(&state, 600, samples));
    assert(!tutor_pcm_push(&state, 300, samples));
    assert(tutor_pcm_finish(&state, samples) == 1 && samples[0] == 500);

    unsigned written = 0;
    for (unsigned i = 0; i < TUTOR_CLOUD_PCM_RATE; i++) {
        if (tutor_pcm_push(&state, -1200, samples)) {
            assert(samples[0] == -1200 && samples[1] == -1200);
            written += 2;
        }
    }
    assert(written == TUTOR_PCM_RATE && tutor_pcm_finish(&state, samples) == 0);

    // Signed PCM split between callbacks must land at the same sample boundaries.
    const uint8_t pcm[] = { 0xb0, 0x04, 0x60, 0x09, 0x10, 0x0e,
                            0x00, 0x80, 0x00, 0x80, 0x00, 0x80 };
    tutor_pcm_stream_t stream = {0};
    int16_t split[TUTOR_PCM_OUTPUT_CAP(sizeof(pcm))];
    assert(tutor_pcm_convert_le(&stream, pcm, 5, split) == 0 && stream.have_low_byte);
    tutor_pcm_stream_t candidate = stream;
    assert(tutor_pcm_convert_le(&candidate, pcm + 5, 1, split) == 2);
    assert(stream.have_low_byte && stream.resampler.count == 2);
    assert(tutor_pcm_convert_le(&stream, pcm + 5, 7, split) == 4);
    assert(split[0] == 1600 && split[1] == 3200);
    assert(split[2] == INT16_MIN && split[3] == INT16_MIN);
    assert(!stream.have_low_byte && stream.resampler.count == 0);
    return 0;
}
