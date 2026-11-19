#pragma once

#include <stdbool.h>

#define TUTOR_KEY_MAX 128
#define TUTOR_FORM_CAP 768

typedef struct {
    char ssid[33];
    char password[64];
    char key[TUTOR_KEY_MAX + 1];
} tutor_config_t;

typedef struct {
    tutor_config_t config;
    bool clear;
} tutor_setup_request_t;

bool tutor_config_valid(const tutor_config_t *config);
bool tutor_setup_parse_form(char *body, const char *token, tutor_setup_request_t *request);
