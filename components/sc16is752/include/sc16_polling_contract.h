#pragma once

/* Include after FreeRTOS.h. A one-tick sleep must leave time to service both
 * 64-byte UART FIFOs at 115200 baud (8N1). Reject incompatible builds rather
 * than discovering a 10 ms polling interval as corrupt spectra in flight. */
_Static_assert(configTICK_RATE_HZ == 1000,
               "SC16 dual-RX polling requires CONFIG_FREERTOS_HZ=1000");
