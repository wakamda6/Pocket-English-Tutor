#include <assert.h>
#include "tutor_turn.h"

int main(void)
{
    assert(tutor_turn_view(TUTOR_CONNECTING, true, false, false) == TUTOR_CONNECTING);
    assert(tutor_turn_view(TUTOR_READY, true, false, false) == TUTOR_ARMING);
    assert(tutor_turn_view(TUTOR_LISTENING, true, true, false) == TUTOR_LISTENING);
    assert(tutor_turn_view(TUTOR_SPEAKING, true, true, true) == TUTOR_LISTENING);
    assert(tutor_turn_view(TUTOR_READY, true, true, true) == TUTOR_LISTENING);
    assert(tutor_turn_view(TUTOR_LISTENING, false, true, false) == TUTOR_SENDING);
    assert(tutor_turn_view(TUTOR_THINKING, false, false, false) == TUTOR_THINKING);
    assert(tutor_turn_view(TUTOR_READY, false, false, true) == TUTOR_SPEAKING);
    assert(tutor_turn_view(TUTOR_READY, false, false, false) == TUTOR_READY);
    assert(tutor_turn_view(TUTOR_CLOUD_FAILED, true, true, true) == TUTOR_CLOUD_FAILED);
    assert(tutor_turn_can_start(TUTOR_READY, false, false));
    assert(!tutor_turn_can_start(TUTOR_READY, true, false));
    assert(!tutor_turn_can_start(TUTOR_READY, false, true));
    assert(!tutor_turn_can_start(TUTOR_THINKING, false, false));
    assert(!tutor_turn_accepts_reply(TUTOR_READY));
    assert(tutor_turn_accepts_reply(TUTOR_LISTENING));
    assert(tutor_turn_accepts_reply(TUTOR_THINKING));
    assert(tutor_turn_accepts_reply(TUTOR_SPEAKING));
    return 0;
}
