#include <assert.h>
#include <string.h>
#include "tutor_setup_dns.h"

int main(void)
{
    static const uint8_t query[] = {
        0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
        3, 'a', 'p', 'p', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1,
    };
    static const uint8_t address[] = { 192, 168, 4, 1 };
    uint8_t packet[64];

    memcpy(packet, query, sizeof(query));
    size_t size = tutor_setup_dns_reply(packet, sizeof(query), sizeof(packet), address);
    assert(size == sizeof(query) + 16);
    assert(packet[0] == 0x12 && packet[1] == 0x34 && packet[2] == 0x85 && packet[7] == 1);
    assert(memcmp(packet + 12, query + 12, sizeof(query) - 12) == 0);
    assert(memcmp(packet + size - 4, address, sizeof(address)) == 0);
    assert(tutor_setup_dns_reply(packet, size, sizeof(packet), address) == 0);

    memcpy(packet, query, sizeof(query));
    packet[sizeof(query) - 3] = 28; // AAAA gets a valid empty answer.
    assert(tutor_setup_dns_reply(packet, sizeof(query), sizeof(packet), address) == sizeof(query));
    assert(packet[7] == 0);

    memcpy(packet, query, sizeof(query));
    for (size_t n = 0; n < sizeof(query); n++) {
        assert(tutor_setup_dns_reply(packet, n, sizeof(packet), address) == 0);
    }
    assert(tutor_setup_dns_reply(packet, sizeof(query), sizeof(query), address) == 0);
    packet[12] = 0xc0;
    assert(tutor_setup_dns_reply(packet, sizeof(query), sizeof(packet), address) == 0);
    return 0;
}
