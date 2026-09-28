#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Single lifecycle-owner state. Driver counters may wrap, but must not be
 * reset between begin and freeze. Cleanup retries cannot recount teardown. */
typedef struct {
    uint32_t overruns, drops;
    bool active, notified;
} acquisition_rx_loss_t;

static inline void acquisition_rx_loss_begin(acquisition_rx_loss_t *state,
                                             uint32_t overruns, uint32_t drops)
{
    *state = (acquisition_rx_loss_t){overruns, drops, true, false};
}

static inline bool acquisition_rx_loss_sample(acquisition_rx_loss_t *state,
    uint32_t overruns, uint32_t drops, bool freeze,
    uint32_t *overrun_delta, uint32_t *drop_delta)
{
    *overrun_delta = *drop_delta = 0;
    if (!state->active) return false;
    *overrun_delta = overruns - state->overruns;
    *drop_delta = drops - state->drops;
    state->overruns = overruns;
    state->drops = drops;
    if (freeze) state->active = false;
    bool first = !state->notified && (*overrun_delta || *drop_delta);
    if (first) state->notified = true;
    return first;
}
