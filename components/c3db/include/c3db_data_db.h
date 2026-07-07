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

#include "c3db_data_file.h"
#include "c3db_db.h"

/**
 * User-facing database wrapper for large fixed-size payloads.
 */
class c3db_data_db_t : public c3db_db_t
{
public:
  //! Creates a data database wrapper for fixed-size large payloads.
  explicit c3db_data_db_t(size_t data_size);

  //! Creates "<base>.met" and "<base>.dat" through the data storage engine.
  c3db_err_t create(const char* base_file_name) override;

  //! Opens an existing data database and loads any persistent indexes.
  c3db_err_t begin(const char* base_file_name, bool read_only = false) override;

  //! Compacts the data file while preserving external logical record ids.
  c3db_err_t defrag();

private:
  c3db_db_file_t& storage() override;
  const c3db_db_file_t& storage() const override;
  size_t payload_size() const override;

  c3db_data_file_t storage_;
};
