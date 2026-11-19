#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tutor_cloud_message.h"

typedef struct {
    size_t count;
    size_t before_type;
    bool zeroes;
    uint8_t first[3];
    tutor_cloud_message_t *reader;
} received_t;

static bool receive(void *context, const uint8_t *bytes, size_t count)
{
    received_t *received = context;
    if (!received->count) memcpy(received->first, bytes, count < 3 ? count : 3);
    if (!received->reader->type[0]) received->before_type += count;
    if (received->zeroes) {
        for (size_t i = 0; i < count; i++) assert(bytes[i] == 0);
    }
    received->count += count;
    return true;
}

static bool parse(tutor_cloud_message_t *reader, const char *text, size_t fragment, received_t *received)
{
    *reader = (tutor_cloud_message_t){0};
    *received = (received_t){ .zeroes = received->zeroes, .reader = reader };
    size_t left = strlen(text);
    while (left) {
        size_t size = left > fragment ? fragment : left;
        if (!tutor_cloud_message_feed(reader, text, size, receive, received)) return false;
        text += size;
        left -= size;
    }
    return tutor_cloud_message_end(reader);
}

int main(void)
{
    tutor_cloud_message_t reader;
    received_t received = {0};
    const char *control = "{\"event\":{\"type\":\"nested\",\"items\":[\"quote \\\" and }\",{}]},"
                          "\"type\":\"session.created\"}";
    assert(parse(&reader, control, 1, &received));
    assert(strcmp(reader.type, "session.created") == 0 && received.count == 0);
    assert(parse(&reader, "{\"delta\":\"AB\\/C\",\"type\":\"response.output_audio.delta\"}", 1, &received));
    assert(received.count == 3 && received.first[0] == 0 && received.first[1] == 31 &&
           received.first[2] == 194 && received.before_type == 3);
    assert(parse(&reader, "{\"type\":\"response.output_audio.delta\",\"delta\":\"AQI=\"}", 2, &received));
    assert(received.count == 2 && received.first[0] == 1 && received.first[1] == 2 &&
           received.before_type == 0);
    assert(parse(&reader, "{\"type\":\"response.output_text.delta\",\"delta\":\"I went.\\n\"}", 3, &received));
    assert(received.count == 0);
    assert(parse(&reader, "{\"delta\":\"I went.\",\"type\":\"response.output_text.delta\"}", 4, &received));
    assert(!tutor_cloud_message_is_audio(&reader));

    static char large[TUTOR_CLOUD_MESSAGE_MAX + 1];
    const size_t encoded = 76 * 1024; // Larger than the observed 60,147-byte reply.
    const char *prefix = "{\"delta\":\"";
    size_t start = strlen(prefix);
    memcpy(large, prefix, start);
    memset(large + start, 'A', encoded);
    strcpy(large + start + encoded, "\",\"event_id\":\"sample\",\"type\":\"response.output_audio.delta\"}");
    received.zeroes = true;
    assert(parse(&reader, large, 2047, &received));
    assert(received.count == encoded / 4 * 3 && received.before_type == received.count);
    int header = snprintf(large, sizeof(large), "{\"type\":\"response.output_audio.delta\",\"delta\":\"");
    assert(header > 0);
    memset(large + header, 'A', encoded);
    strcpy(large + header + encoded, "\"}");
    assert(parse(&reader, large, 2048, &received));
    assert(received.count == encoded / 4 * 3 && received.before_type == 0);

    assert(!parse(&reader, "{\"type\":\"response.output_audio.delta\"}", 1, &received));
    assert(!parse(&reader, "{\"type\":\"response.output_audio.delta\",\"delta\":\"AAA\"}", 1, &received));
    assert(!parse(&reader, "{\"type\":\"session.created\",\"session\":{\"id\":\"unfinished", 1, &received));
    assert(!tutor_cloud_message_feed(&reader, large, TUTOR_CLOUD_MESSAGE_MAX + 1, receive, &received));
    return 0;
}
