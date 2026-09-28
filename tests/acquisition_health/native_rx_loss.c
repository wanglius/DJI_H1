#include <assert.h>
#include "acquisition_rx_loss.h"

int main(void)
{
    acquisition_rx_loss_t state = {0};
    uint32_t over, drop;
    assert(!acquisition_rx_loss_sample(&state, 1, 10, false, &over, &drop));
    assert(over == 0 && drop == 0); /* not armed */
    acquisition_rx_loss_begin(&state, 7, 90);
    assert(!acquisition_rx_loss_sample(&state, 7, 90, false, &over, &drop));
    assert(acquisition_rx_loss_sample(&state, 8, 95, false, &over, &drop));
    assert(over == 1 && drop == 5);
    assert(!acquisition_rx_loss_sample(&state, 10, 100, true, &over, &drop));
    assert(over == 2 && drop == 5); /* final snapshot includes last loss */
    assert(!acquisition_rx_loss_sample(&state, 15, 500, true, &over, &drop));
    assert(over == 0 && drop == 0); /* teardown and cleanup retry excluded */
    acquisition_rx_loss_begin(&state, 0, 0);
    assert(acquisition_rx_loss_sample(&state, 0, 9, true, &over, &drop));
    assert(over == 0 && drop == 9); /* partial-start first loss at freeze */
    acquisition_rx_loss_begin(&state, UINT32_MAX, UINT32_MAX - 2);
    assert(acquisition_rx_loss_sample(&state, 0, 1, false, &over, &drop));
    assert(over == 1 && drop == 4); /* driver-counter wrap */
    return 0;
}
