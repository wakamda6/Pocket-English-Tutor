#include <assert.h>
#include <string.h>
#include "tutor_setup_form.h"

static bool parse(const char *form, tutor_setup_request_t *out)
{
    char body[TUTOR_FORM_CAP + 1];
    assert(strlen(form) < sizeof(body));
    strcpy(body, form);
    return tutor_setup_parse_form(body, "test-token", out);
}

int main(void)
{
    tutor_setup_request_t request;
    assert(parse("token=test-token&ssid=%E4%B8%AD+2.4G&password=pass%2Bword&key=test%26key", &request));
    assert(strcmp(request.config.ssid, "\xE4\xB8\xAD 2.4G") == 0);
    assert(strcmp(request.config.password, "pass+word") == 0);
    assert(strcmp(request.config.key, "test&key") == 0);
    assert(parse("key=key&password=&ssid=open&token=test-token", &request));
    assert(!request.clear && request.config.password[0] == 0);
    assert(parse("token=test-token&clear=1", &request) && request.clear);

    assert(!parse("token=wrong&clear=1", &request));
    assert(!parse("clear=1", &request));
    assert(!parse("token=test-token&clear=1&clear=1", &request));
    assert(!parse("token=test-token&clear=1&ssid=extra", &request));
    assert(!parse("token=test-token&ssid=abc%00def&password=&key=key", &request));
    assert(!parse("token=test-token&ssid=abc%&password=&key=key", &request));
    assert(!parse("token=test-token&ssid=w&password=short&key=key", &request));
    assert(!parse("token=test-token&ssid=w&password=&key=key%0d", &request));
    assert(!parse("token=test-token&ssid=w&password=&key=key&", &request));
    assert(parse("token=test-token&ssid=12345678901234567890123456789012&password=&key=test", &request));
    assert(!parse("token=test-token&ssid=123456789012345678901234567890123&password=&key=test", &request));
    return 0;
}
