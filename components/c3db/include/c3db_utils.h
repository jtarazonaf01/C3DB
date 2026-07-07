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

#include "c3db_types.h"

static constexpr uint32_t C3DB_CRC32_INIT = 0xFFFFFFFFu;

//! Builds a logical identifier from row and cycle.
inline c3db_id_t c3db_id(uint32_t row_id, uint32_t cycle) {
  return (static_cast<c3db_id_t>(cycle) << 32) | row_id;
}

//! Returns the physical row encoded in a logical identifier.
inline uint32_t c3db_row(c3db_id_t id) {
  return static_cast<uint32_t>(id & 0xFFFFFFFFu);
}

//! Returns the generation cycle encoded in a logical identifier.
inline uint32_t c3db_cycle(c3db_id_t id) {
  return static_cast<uint32_t>(id >> 32);
}

//! Allocates "<base_file_name><extension>" in file_name.
c3db_err_t c3db_make_file_name(const char* base_file_name, const char* extension, char* &file_name);

//! Updates a running CRC32 value with len bytes.
uint32_t c3db_crc32_update(uint32_t crc, const uint8_t* data, size_t len);

//! Finalizes a running CRC32 value.
uint32_t c3db_crc32_finish(uint32_t crc);

//! Calculates CRC32 for a contiguous memory buffer.
uint32_t c3db_crc32(const uint8_t* data, size_t len);
