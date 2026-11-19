#include "tutor_setup_dns.h"

#include <stdbool.h>
#include <string.h>

size_t tutor_setup_dns_reply(uint8_t *packet, size_t length, size_t capacity,
                             const uint8_t address[4])
{
    enum { HEADER = 12, ANSWER = 16 };
    if (length < HEADER || length > capacity || (packet[2] & 0xf8) ||
        packet[4] != 0 || packet[5] != 1) {
        return 0;
    }

    size_t end = HEADER;
    while (end < length && packet[end]) {
        size_t label = packet[end++];
        if (label > 63 || label > length - end || end + label - HEADER > 254) {
            return 0;
        }
        end += label;
    }
    if (length - end < 5) {
        return 0;
    }
    end++;
    bool answer_a = packet[end] == 0 && packet[end + 1] == 1 &&
                    packet[end + 2] == 0 && packet[end + 3] == 1;
    end += 4;
    if (answer_a && capacity - end < ANSWER) {
        return 0;
    }

    packet[2] = 0x84 | (packet[2] & 1); // Response, authoritative, preserve RD.
    packet[3] = 0;
    memset(&packet[6], 0, 6);
    if (answer_a) {
        static const uint8_t prefix[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4 };
        packet[7] = 1;
        memcpy(&packet[end], prefix, sizeof(prefix));
        memcpy(&packet[end + sizeof(prefix)], address, 4);
        end += ANSWER;
    }
    return end;
}
