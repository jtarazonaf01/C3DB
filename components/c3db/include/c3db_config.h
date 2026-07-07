/*
 * C3DB - Embedded Persistent Database for IoT Systems
 *
 * Copyright (c) 2026 Jose Tarazona Franco
 *
 * Licensed under the Apache License, Version 2.0.
 * You may obtain a copy of the License at:
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 *
 * This file is part of the C3DB project developed as part of a
 * Bachelor's Thesis (TFG).
 */

#pragma once

#include <cstddef>
#include <cstdint>

//! Size of the shared scratch buffer used for bounded temporary copies.
static constexpr size_t C3DB_SHARED_BUFFER_SIZE = 4096;

//! Shared scratch buffer. It is temporary workspace, not persistent cache state.
extern uint8_t c3db_shared_buffer[C3DB_SHARED_BUFFER_SIZE];
