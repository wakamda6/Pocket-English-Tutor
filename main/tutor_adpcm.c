#include "tutor_adpcm.h"

static const int16_t step_sizes[] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

enum { LAST_STEP = sizeof(step_sizes) / sizeof(step_sizes[0]) - 1 };

static int32_t decode_nibble(unsigned nibble, int32_t predictor, unsigned *index)
{
    static const int8_t steps[] = { -1, -1, -1, -1, 2, 4, 6, 8 };
    int32_t step = step_sizes[*index];
    int32_t difference = step >> 3;
    if (nibble & 4) difference += step;
    if (nibble & 2) difference += step >> 1;
    if (nibble & 1) difference += step >> 2;
    predictor += nibble & 8 ? -difference : difference;
    if (predictor > INT16_MAX) predictor = INT16_MAX;
    if (predictor < INT16_MIN) predictor = INT16_MIN;

    int new_index = (int)*index + steps[nibble & 7];
    if (new_index < 0) new_index = 0;
    if (new_index > LAST_STEP) new_index = LAST_STEP;
    *index = new_index;
    return predictor;
}

void tutor_adpcm_encode(tutor_adpcm_block_t *block, const int16_t *samples,
                         size_t count, unsigned *step_index)
{
    if (count == 0 || count > TUTOR_FRAME_SAMPLES) {
        block->count = 0;
        return;
    }
    unsigned index = *step_index > LAST_STEP ? LAST_STEP : *step_index;
    int32_t predictor = samples[0];
    block->first_sample = predictor;
    block->count = count;
    block->step_index = index;
    for (size_t i = 1; i < count; i++) {
        int32_t difference = samples[i] - predictor;
        unsigned nibble = difference < 0 ? 8 : 0;
        if (difference < 0) difference = -difference;
        int32_t step = step_sizes[index];
        if (difference >= step) {
            nibble |= 4;
            difference -= step;
        }
        if (difference >= (step >> 1)) {
            nibble |= 2;
            difference -= step >> 1;
        }
        if (difference >= (step >> 2)) nibble |= 1;
        predictor = decode_nibble(nibble, predictor, &index);

        size_t pos = (i - 1) / 2;
        if (i & 1) {
            block->data[pos] = nibble;
        } else {
            block->data[pos] |= nibble << 4;
        }
    }
    *step_index = index;
}

size_t tutor_adpcm_decode(const tutor_adpcm_block_t *block, int16_t samples[TUTOR_FRAME_SAMPLES])
{
    unsigned index = block->step_index;
    if (!block->count || block->count > TUTOR_FRAME_SAMPLES || index > LAST_STEP) return 0;
    int32_t predictor = block->first_sample;
    samples[0] = predictor;
    for (size_t i = 1; i < block->count; i++) {
        unsigned pair = block->data[(i - 1) / 2];
        unsigned nibble = (i & 1) ? pair & 15 : pair >> 4;
        predictor = decode_nibble(nibble, predictor, &index);
        samples[i] = predictor;
    }
    return block->count;
}
