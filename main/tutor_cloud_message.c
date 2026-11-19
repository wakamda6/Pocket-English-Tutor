#include "tutor_cloud_message.h"

#include <string.h>

enum { OBJECT, KEY, COLON, VALUE, COMMA, END };
enum { IGNORE, TYPE, DELTA };

bool tutor_cloud_message_is_audio(const tutor_cloud_message_t *reader)
{
    return strcmp(reader->type, "response.output_audio.delta") == 0;
}

static int base64_digit(char byte)
{
    if (byte >= 'A' && byte <= 'Z') return byte - 'A';
    if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
    if (byte >= '0' && byte <= '9') return byte - '0' + 52;
    if (byte == '+') return 62;
    if (byte == '/') return 63;
    if (byte == '=') return 64;
    return -1;
}

static bool flush_pcm(tutor_cloud_message_t *reader, tutor_cloud_pcm_fn write_pcm, void *context)
{
    if (!reader->pcm_len) return true;
    bool ok = write_pcm(context, reader->pcm, reader->pcm_len);
    reader->pcm_len = 0;
    return ok;
}

static bool decode_byte(tutor_cloud_message_t *reader, char byte,
                        tutor_cloud_pcm_fn write_pcm, void *context)
{
    if (!reader->valid_delta) return true;
    int digit = base64_digit(byte);
    if (digit < 0 || reader->padded) {
        reader->valid_delta = false;
        return true;
    }
    reader->base64[reader->base64_len++] = digit;
    if (reader->base64_len != 4) return true;
    const uint8_t *values = reader->base64;
    reader->base64_len = 0;
    if (values[0] == 64 || values[1] == 64 || (values[2] == 64 && values[3] != 64)) {
        reader->valid_delta = false;
        return true;
    }
    reader->pcm[reader->pcm_len++] = (values[0] << 2) | (values[1] >> 4);
    if (values[2] != 64) {
        reader->pcm[reader->pcm_len++] = (values[1] << 4) | (values[2] >> 2);
        if (values[3] != 64) {
            reader->pcm[reader->pcm_len++] = (values[2] << 6) | values[3];
        }
    }
    reader->padded = values[3] == 64;
    return reader->pcm_len < sizeof(reader->pcm) || flush_pcm(reader, write_pcm, context);
}

static bool string_byte(tutor_cloud_message_t *reader, char byte,
                       tutor_cloud_pcm_fn write_pcm, void *context)
{
    bool escaped = reader->escape;
    reader->escape = false;
    if (!escaped && byte == '\\') {
        reader->escape = true;
        return true;
    }
    if (!escaped && byte == '"') {
        reader->in_string = false;
        if (reader->depth) return true;
        if (reader->phase == KEY) {
            reader->field = reader->token_len == 4 && memcmp(reader->key, "type", 4) == 0 ? TYPE :
                            reader->token_len == 5 && memcmp(reader->key, "delta", 5) == 0 ? DELTA : IGNORE;
            reader->phase = COLON;
        } else {
            reader->phase = COMMA;
            if (reader->field == DELTA && reader->valid_delta) {
                reader->valid_delta = reader->base64_len == 0 && reader->token_len != 0;
                if (reader->valid_delta) return flush_pcm(reader, write_pcm, context);
            }
        }
        return true;
    }
    if (reader->depth) return true;
    size_t offset = reader->token_len++;
    if (reader->phase == KEY && offset < sizeof(reader->key)) {
        reader->key[offset] = escaped ? 0 : byte;
    } else if (reader->phase == VALUE && reader->field == TYPE) {
        if (escaped || offset >= sizeof(reader->type) - 1) return false;
        reader->type[offset] = byte;
    } else if (reader->phase == VALUE && reader->field == DELTA) {
        if (escaped && byte != '/') reader->valid_delta = false;
        return decode_byte(reader, byte, write_pcm, context);
    }
    return true;
}

static bool json_byte(tutor_cloud_message_t *reader, char byte,
                      tutor_cloud_pcm_fn write_pcm, void *context)
{
    if (reader->in_string) return string_byte(reader, byte, write_pcm, context);
    if (reader->depth) {
        if (byte == '"') reader->in_string = true;
        if (byte == '{' || byte == '[') reader->depth++;
        if ((byte == '}' || byte == ']') && --reader->depth == 0) reader->phase = COMMA;
        return true;
    }
    const bool space = byte == ' ' || byte == '\n' || byte == '\r' || byte == '\t';
    if (reader->bare_value) {
        if (!space && byte != ',' && byte != '}') return true;
        reader->bare_value = false;
        reader->phase = COMMA;
    }
    if (space) return true;

    switch (reader->phase) {
    case OBJECT:
        if (byte != '{') return false;
        reader->phase = KEY;
        return true;
    case KEY:
        if (byte != '"') return false;
        reader->token_len = 0;
        reader->in_string = true;
        return true;
    case COLON:
        if (byte != ':') return false;
        reader->phase = VALUE;
        return true;
    case VALUE:
        if (byte == '"') {
            if (reader->field == TYPE && reader->type[0]) return false;
            if (reader->field == DELTA) {
                if (reader->saw_delta) return false;
                reader->saw_delta = true;
                reader->valid_delta = !reader->type[0] || tutor_cloud_message_is_audio(reader);
            }
            reader->token_len = 0;
            reader->in_string = true;
        } else if (reader->field == TYPE || byte == ',' || byte == '}' || byte == ']') {
            return false;
        } else if (byte == '{' || byte == '[') {
            reader->depth = 1;
        } else {
            reader->bare_value = true;
        }
        return true;
    case COMMA:
        if (byte != ',' && byte != '}') return false;
        reader->phase = byte == ',' ? KEY : END;
        return true;
    default:
        return false;
    }
}

bool tutor_cloud_message_feed(tutor_cloud_message_t *reader, const char *data, size_t size,
                             tutor_cloud_pcm_fn write_pcm, void *context)
{
    if (size > TUTOR_CLOUD_MESSAGE_MAX - reader->length) return false;
    reader->length += size;
    for (size_t i = 0; i < size; i++) {
        if (!data[i] || !json_byte(reader, data[i], write_pcm, context)) return false;
    }
    return true;
}

bool tutor_cloud_message_end(const tutor_cloud_message_t *reader)
{
    return reader->phase == END && reader->type[0] &&
           (!tutor_cloud_message_is_audio(reader) || (reader->saw_delta && reader->valid_delta));
}
