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

#include "c3db_cached_db_file.h"
#include "c3db_db.h"

/**
 * User-facing database wrapper for small fixed-size records.
 */
class c3db_cached_db_t : public c3db_db_t
{
public:
  //! Creates a cached database wrapper for fixed-size records.
  c3db_cached_db_t(size_t data_size, size_t memory_size);

  using c3db_db_t::begin;
  using c3db_db_t::create;

  //! Creates a new cached database using a specific cache distribution mode.
  c3db_err_t create(const char* base_file_name, c3db_cache_mode_t mode);

  //! Opens an existing cached database using a specific cache distribution mode.
  c3db_err_t begin(const char* base_file_name, bool read_only, c3db_cache_mode_t mode);

  //! Flushes pending cached writes to storage.
  c3db_err_t commit();

  //! Returns the current cache distribution mode.
  c3db_cache_mode_t mode() const;

  //! Changes the cache distribution mode, flushing or repartitioning as needed.
  c3db_err_t mode(c3db_cache_mode_t new_mode);

  //! Returns true when mutations are committed automatically.
  bool autocommit() const;

  //! Enables or disables automatic commit after write operations.
  void autocommit(bool value);

private:
  c3db_db_file_t& storage() override;
  const c3db_db_file_t& storage() const override;
  size_t payload_size() const override;

  c3db_cached_db_file_t storage_;
};
