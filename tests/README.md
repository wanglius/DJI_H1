# Hardware tests

These sources are integration harnesses, not production modules. The active
milestone firmware selects them explicitly from `main/CMakeLists.txt`.

- `coexistence_test.c` runs dual-spectrometer acquisition while continuously
  writing and periodically flushing `COEXIST.BIN` on the SD card. It overwrites
  that file on every run.
- `sd_card_test.c` overwrites `H1TEST.TXT`, then reads it back.
- `data_pipeline_test.c` validates the documented 30-byte drone payload decoder
  and common in-memory record metadata.
- `h1_single_frame_test.c` is the original standalone `app_main` retained for
  regression diagnosis; it is not part of the normal build.

Hardware tests may write to the inserted card. Never run them with irreplaceable
media unless the named test files have been backed up.
