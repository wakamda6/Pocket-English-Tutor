#pragma once

#include "esp_err.h"
#include "tutor_setup_form.h"

#define TUTOR_SETUP_IP "192.168.4.1"

typedef enum {
    TUTOR_SETUP_FORM,
    TUTOR_SETUP_CHECKING,
    TUTOR_SETUP_WIFI_FAILED,
    TUTOR_SETUP_SAVE_FAILED,
    TUTOR_SETUP_SAVED,
    TUTOR_SETUP_CLEARED,
} tutor_setup_status_t;

// Wi-Fi must be initialized and stopped when entering setup.
esp_err_t tutor_setup_start(void);
void tutor_setup_stop(void);
bool tutor_setup_take_request(tutor_setup_request_t *request);
void tutor_setup_report(tutor_setup_status_t status);
const char *tutor_setup_ssid(void);
