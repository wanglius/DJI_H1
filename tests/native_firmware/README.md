# Native firmware regression tests

These host-only tests compile the actual `m100m.c` and `telemetry.c` into isolated
test executables. Only ESP-IDF/FreeRTOS services are replaced by deterministic
UART, time, and queue stubs. No serial port, broker, or device is accessed.
Unrelated dependencies retained by the host linker have abort-only guards in
`pool_unused_dependencies.h`; reaching any such guard fails the test immediately.

Use a host C compiler through `CC`, or install the optional pinned Zig compiler
inside a test virtual environment (not the ground application's runtime):

```powershell
python -m pip install -r tests/native_firmware/requirements.txt
python -B -m unittest discover -s tests/native_firmware -v
```

The repository-wide `python -B tests/run_host_tests.py` also discovers this group.
If no host compiler is available, this group reports a skip, not a successful C
verification. ESP-only Clang distributions cannot build Windows executables.
Executables are temporary and removed after testing. Zig's reusable compiler
cache is kept in the ignored `build-native-tests/` directory.

Coverage includes missing prompt/completion responses, cancellation, short UART
writes, explicit modem errors, missing PUBACK, binary ACK bytes across UART chunk
boundaries, repeated offline pool expiry, FIFO preservation, producer publication
ownership, and a shutdown queue reset between peek and dequeue. Queue races are
injected at explicit scheduling points; these are not a real FreeRTOS concurrency
test or proof of modem timing. Firmware builds and hardware missions remain
separate verification steps.
