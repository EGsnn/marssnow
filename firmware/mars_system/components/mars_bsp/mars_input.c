#include "mars_board.h"

mars_input_action_t mars_input_update(mars_input_state_t *s, int raw, int64_t now)
{
    if (!s || raw < 0 || raw > 3) return MARS_INPUT_NONE;
    if (raw != s->candidate) {
        s->candidate = raw;
        s->candidate_since_ms = now;
    }
    if (raw != s->stable && now - s->candidate_since_ms >= 30) {
        int old = s->stable;
        s->stable = raw;
        if (old == 3 && raw == 0 && !s->long_sent) {
            s->long_sent = false;
            s->pressed_since_ms = now;
            return MARS_INPUT_SELECT;
        }
        s->pressed_since_ms = now;
        s->long_sent = false;
        if (raw == 1) return MARS_INPUT_PREVIOUS;
        if (raw == 2) return MARS_INPUT_NEXT;
    }
    if (s->stable == 3 && raw == 3 && !s->long_sent && now - s->pressed_since_ms >= 800) {
        s->long_sent = true;
        return MARS_INPUT_BACK;
    }
    return MARS_INPUT_NONE;
}
