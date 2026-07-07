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

#include "c3db_data_db.h"

#include "c3db_defs.h"

c3db_data_db_t::c3db_data_db_t(size_t data_size)
  : c3db_db_t(),
    storage_(data_size) {
}

c3db_err_t c3db_data_db_t::create(const char* base_file_name) {
  if (storage_.is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;
  ON_ERR_RETURN(set_base_file_name(base_file_name));

  c3db_err_t err = storage_.create(base_file_name);
  if (IS_ERR(err)) reset_state();
  return err;
}

c3db_err_t c3db_data_db_t::begin(const char* base_file_name, bool read_only) {
  if (storage_.is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;
  ON_ERR_RETURN(set_base_file_name(base_file_name));

  c3db_err_t err = storage_.begin(base_file_name, read_only);
  if (OK(err)) err = load_idxs();

  if (IS_ERR(err)) {
    storage_.end();
    reset_state();
  }
  return err;
}

c3db_err_t c3db_data_db_t::defrag() {
  return storage_.defrag();
}

c3db_db_file_t& c3db_data_db_t::storage() {
  return storage_;
}

const c3db_db_file_t& c3db_data_db_t::storage() const {
  return storage_;
}

size_t c3db_data_db_t::payload_size() const {
  return storage_.payload_size();
}
