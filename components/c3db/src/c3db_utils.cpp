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

#include "c3db_utils.h"

#include <cstring>
#include <new>

c3db_err_t c3db_make_file_name(const char* base_file_name, const char* extension, char* &file_name) {
  file_name = nullptr;
  if (!base_file_name || base_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  const char* file_extension = extension ? extension : "";
  const size_t name_len = std::strlen(base_file_name);
  const size_t ext_len = std::strlen(file_extension);

  file_name = new (std::nothrow) char[name_len + ext_len + 1];
  if (!file_name) return C3DB_GENERIC_ERR;

  std::memcpy(file_name, base_file_name, name_len);
  std::memcpy(file_name + name_len, file_extension, ext_len + 1);
  return C3DB_OK;
}

uint32_t c3db_crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
  if (!data && len > 0) return crc;

  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }

  return crc;
}

uint32_t c3db_crc32_finish(uint32_t crc) {
  return ~crc;
}

uint32_t c3db_crc32(const uint8_t* data, size_t len) {
  return c3db_crc32_finish(c3db_crc32_update(C3DB_CRC32_INIT, data, len));
}
