# C3DB

C3DB is a lightweight persistent database engine for resource-constrained embedded and IoT systems.

The project is designed for ESP32-class microcontrollers using external microSD storage. It focuses on fixed-size records, consistency after unexpected resets, bounded RAM usage, optional caching, space reuse and persistent hash indexes.

This repository was developed as part of a Bachelor's Thesis (TFG) using ESP32, ESP-IDF and C++.

## Author

Copyright (c) 2026 Jose Tarazona Franco.

## Current Scope

The current implementation includes:

- `c3db_file_t`: low-level physical file wrapper.
- `c3db_db_file_t`: consistent fixed-size record file with validated records and free-list reuse.
- `c3db_cached_db_file_t`: cached fixed-size record storage for small payloads.
- `c3db_data_file_t`: metadata plus data-file storage for larger payloads.
- `c3db_index_t`: persistent hash index based on Linear Hashing.
- `c3db_cached_db_t`: high-level user-facing database for small cached records.
- `c3db_data_db_t`: high-level user-facing database for larger payloads.

The public demo in `main/main.cpp` mounts the microSD card, creates a small cached database, creates an index, inserts one record and reads it back both by id and through the index.

## Build Environment

The project is configured for ESP-IDF development. The standard ESP-IDF workflow is:

```powershell
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

If using Visual Studio Code with the ESP-IDF extension, build and flash can also be launched from the configured ESP-IDF commands.

## Runtime Target

The demo expects a microSD card mounted at:

```text
/sdcard
```

The current `main.cpp` is configured for SDSPI on the LilyGO T8-style pinout used during development:

```text
MOSI: GPIO15
MISO: GPIO2
SCLK: GPIO14
CS:   GPIO13
```

## Tests And Benchmarks

Manual tests are kept under `test/`.

Benchmarks are kept under:

```text
components/c3db/benchmarks
```

They are not compiled or executed by the default presentation firmware. To run them, add the relevant source files back to `main/CMakeLists.txt` and call the desired function from `run_c3db_manual_tests()`.

## License

C3DB is licensed under the Apache License, Version 2.0. See `LICENSE` for details.
