#include <assert.h>
#include "tutor_playback.h"

int main(void)
{
    assert(!tutor_playback_can_start(false, TUTOR_PLAYBACK_BLOCKS, false));
    assert(!tutor_playback_can_start(false, TUTOR_PLAYBACK_BLOCKS, true));
    assert(!tutor_playback_can_start(true, 0, true));
    assert(!tutor_playback_can_start(true, 1, false));
    assert(!tutor_playback_can_start(true, TUTOR_PREBUFFER_BLOCKS - 1, false));
    assert(tutor_playback_can_start(true, TUTOR_PREBUFFER_BLOCKS, false));
    assert(tutor_playback_can_start(true, 1, true));
    assert(TUTOR_PLAYBACK_BLOCKS > TUTOR_PREBUFFER_BLOCKS);
    return 0;
}
