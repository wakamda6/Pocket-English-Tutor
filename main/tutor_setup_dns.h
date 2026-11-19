#pragma once

#include <stddef.h>
#include <stdint.h>

// Build a captive portal DNS response in place. Return 0 for malformed queries.
size_t tutor_setup_dns_reply(uint8_t *packet, size_t length, size_t capacity,
                             const uint8_t address[4]);
