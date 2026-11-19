#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "tutor_setup_form.h"
#include "tutor_turn.h"

#define TUTOR_DEFAULT_VOLUME 100
#define TUTOR_SPEED_STEP 10
#define TUTOR_SPEED_MIN (-30)
#define TUTOR_SPEED_MAX 30

static inline bool tutor_speed_valid(int speed)
{
    return speed >= TUTOR_SPEED_MIN && speed <= TUTOR_SPEED_MAX && speed % TUTOR_SPEED_STEP == 0;
}

typedef enum {
    TUTOR_MODE_ENGLISH_CHAT,
    TUTOR_MODE_TEACHING,
} tutor_mode_t;

esp_err_t tutor_session_start(const char *key, tutor_mode_t mode, int8_t speed);
void tutor_session_toggle_mic(void);
void tutor_session_stop(void);
bool tutor_session_active(void);
tutor_status_t tutor_session_status(void);
void tutor_session_set_volume(uint8_t percent);
