#include <assert.h>
#include <stdint.h>
#include "tutor_adpcm.h"

int main(void)
{
    tutor_adpcm_block_t block = {0};
    int16_t input[TUTOR_FRAME_SAMPLES] = {0};
    int16_t output[TUTOR_FRAME_SAMPLES];
    unsigned index = 0;
    tutor_adpcm_encode(&block, input, TUTOR_FRAME_SAMPLES, &index);
    assert(tutor_adpcm_decode(&block, output) == TUTOR_FRAME_SAMPLES);
    for (size_t i = 0; i < TUTOR_FRAME_SAMPLES; i++) assert(output[i] == 0);

    const int16_t tail[] = { -1000, -1003, -1007, -1010, -1005 };
    tutor_adpcm_encode(&block, tail, sizeof(tail) / sizeof(tail[0]), &index);
    assert(tutor_adpcm_decode(&block, output) == sizeof(tail) / sizeof(tail[0]));
    assert(output[0] == tail[0]);

    // A steady voiced tone should retain at least 20 dB of signal-to-error ratio.
    static const int16_t wave[] = {
        0, 1561, 3061, 4445, 5657, 6652, 7391, 7846,
        8000, 7846, 7391, 6652, 5657, 4445, 3061, 1561,
        0, -1561, -3061, -4445, -5657, -6652, -7391, -7846,
        -8000, -7846, -7391, -6652, -5657, -4445, -3061, -1561,
    };
    for (size_t i = 0; i < TUTOR_FRAME_SAMPLES; i++) input[i] = wave[i % 32];
    // Warm up the encoder, then decode a separate block on its own.
    tutor_adpcm_encode(&block, input, TUTOR_FRAME_SAMPLES, &index);
    tutor_adpcm_encode(&block, input, TUTOR_FRAME_SAMPLES, &index);
    assert(tutor_adpcm_decode(&block, output) == TUTOR_FRAME_SAMPLES);
    int64_t signal = 0;
    int64_t error = 0;
    for (size_t i = 0; i < TUTOR_FRAME_SAMPLES; i++) {
        int64_t sample = input[i];
        int64_t difference = sample - output[i];
        signal += sample * sample;
        error += difference * difference;
    }
    assert(signal >= 100 * error);

    tutor_adpcm_encode(&block, input, 0, &index);
    assert(tutor_adpcm_decode(&block, output) == 0);
    block.count = TUTOR_FRAME_SAMPLES + 1;
    assert(tutor_adpcm_decode(&block, output) == 0);
    return 0;
}
