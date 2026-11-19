#include "tutor_setup_form.h"

#include <stddef.h>
#include <string.h>

static size_t field_length(const char *field, size_t capacity)
{
    const char *end = memchr(field, 0, capacity);
    return end ? (size_t)(end - field) : capacity;
}

bool tutor_config_valid(const tutor_config_t *config)
{
    size_t ssid = field_length(config->ssid, sizeof(config->ssid));
    size_t password = field_length(config->password, sizeof(config->password));
    size_t key = field_length(config->key, sizeof(config->key));
    if (ssid == 0 || ssid >= sizeof(config->ssid) || password >= sizeof(config->password) ||
        (password > 0 && password < 8) || key == 0 || key >= sizeof(config->key)) {
        return false;
    }
    for (size_t i = 0; i < key; i++) {
        if ((unsigned char)config->key[i] <= 0x20 || (unsigned char)config->key[i] >= 0x7f) {
            return false;
        }
    }
    return true;
}

static int hex_digit(char byte)
{
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

static bool decode(const char *value, char *dest, size_t capacity)
{
    size_t len = 0;
    while (*value) {
        unsigned char byte = *value++;
        if (byte == '%') {
            if (!value[0] || !value[1]) return false;
            int high = hex_digit(value[0]);
            int low = hex_digit(value[1]);
            if (high < 0 || low < 0) return false;
            byte = (high << 4) | low;
            value += 2;
        } else if (byte == '+') {
            byte = ' ';
        }
        if (byte == 0 || len + 1 == capacity) return false;
        dest[len++] = byte;
    }
    dest[len] = 0;
    return true;
}

bool tutor_setup_parse_form(char *body, const char *token, tutor_setup_request_t *request)
{
    enum { TOKEN = 1, SSID = 2, PASSWORD = 4, KEY = 8, CLEAR = 16 };
    *request = (tutor_setup_request_t){0};
    unsigned fields = 0;
    for (char *field = body; field;) {
        char *next = strchr(field, '&');
        if (next) *next++ = 0;
        char *value = strchr(field, '=');
        if (!value) return false;
        *value++ = 0;
        unsigned bit;
        bool valid;
        if (strcmp(field, "token") == 0) {
            bit = TOKEN;
            valid = strcmp(value, token) == 0;
        } else if (strcmp(field, "ssid") == 0) {
            bit = SSID;
            valid = decode(value, request->config.ssid, sizeof(request->config.ssid));
        } else if (strcmp(field, "password") == 0) {
            bit = PASSWORD;
            valid = decode(value, request->config.password, sizeof(request->config.password));
        } else if (strcmp(field, "key") == 0) {
            bit = KEY;
            valid = decode(value, request->config.key, sizeof(request->config.key));
        } else if (strcmp(field, "clear") == 0) {
            bit = CLEAR;
            valid = strcmp(value, "1") == 0;
        } else {
            return false;
        }
        if (!valid || (fields & bit)) return false;
        fields |= bit;
        field = next;
    }
    if (fields == (TOKEN | CLEAR)) {
        request->clear = true;
        return true;
    }
    return fields == (TOKEN | SSID | PASSWORD | KEY) && tutor_config_valid(&request->config);
}
