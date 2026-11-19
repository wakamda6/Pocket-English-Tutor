#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TUTOR_CLOUD_MESSAGE_MAX (96 * 1024)
#define TUTOR_CLOUD_PCM_CHUNK 768

typedef bool (*tutor_cloud_pcm_fn)(void *context, const uint8_t *pcm, size_t size);

typedef struct {
    char type[64];
    char key[6];
    size_t token_len;
    size_t length;
    unsigned depth;
    unsigned phase;
    unsigned field;
    bool in_string;
    bool escape;
    bool bare_value;
    bool saw_delta;
    bool valid_delta;
    bool padded;
    uint8_t base64[4];
    unsigned base64_len;
    uint8_t pcm[TUTOR_CLOUD_PCM_CHUNK];
    size_t pcm_len;
} tutor_cloud_message_t;

bool tutor_cloud_message_feed(tutor_cloud_message_t *reader, const char *data, size_t size,
                             tutor_cloud_pcm_fn write_pcm, void *context);
bool tutor_cloud_message_end(const tutor_cloud_message_t *reader);
bool tutor_cloud_message_is_audio(const tutor_cloud_message_t *reader);
