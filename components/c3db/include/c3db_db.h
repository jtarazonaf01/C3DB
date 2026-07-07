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
#include <stdio.h>

#include "c3db_db_file.h"
#include "c3db_index.h"
#include "c3db_types.h"

//! Maximum number of user-defined indexes stored in the DB header.
static constexpr size_t C3DB_IDX_CAPACITY = 8;

//! One index slot stored in the optional ".db" file.
struct __attribute__((packed)) c3db_idx_slot_t
{
  uint32_t offset = 0;
  uint32_t len = 0;
};

//! Persistent index table stored in record 0 of the optional ".db" file.
struct __attribute__((packed)) c3db_idx_hdr_t
{
  c3db_idx_slot_t slots[C3DB_IDX_CAPACITY] = {};
};

/**
 * Base class for user-facing C3DB database wrappers.
 *
 * The base class owns common high-level state such as the database base name
 * and optional metadata file used by indexed databases. Concrete subclasses
 * decide which storage engine backs the payload records.
 */
class c3db_db_t
{
public:
  virtual ~c3db_db_t();

  virtual c3db_err_t create(const char* base_file_name);
  virtual c3db_err_t begin(const char* base_file_name, bool read_only = false);
  virtual c3db_err_t end();

  //! Inserts a record and updates every active index.
  virtual c3db_err_t insert(const uint8_t* data, c3db_id_t &id);

  //! Reads one record by its logical id.
  virtual c3db_err_t select(c3db_id_t id, uint8_t* data);

  //! Updates a record and adds new index references for the new payload.
  virtual c3db_err_t update(c3db_id_t id, const uint8_t* data);

  //! Removes one record by id. Index references are cleaned lazily.
  virtual c3db_err_t remove(c3db_id_t id);

  //! Finds records through an index. Reuse the same cursor to continue reading.
  c3db_err_t select(size_t idx_num, const uint8_t* value, uint8_t* data, c3db_idx_cursor_t &cursor);

  //! Removes every currently matching record for an indexed value.
  c3db_err_t remove(size_t idx_num, const uint8_t* value, size_t &removed_count);

  virtual c3db_err_t import_file(FILE* source_file, c3db_id_t &first_id, size_t &rows_added);
  virtual c3db_err_t import_file(const char* source_file_name, c3db_id_t &first_id, size_t &rows_added);
  virtual c3db_err_t export_file(FILE* target_file, size_t &rows_exported);
  virtual c3db_err_t export_file(const char* target_file_name, size_t &rows_exported);

  //! Creates a persistent index over payload[offset, offset + len).
  c3db_err_t create_idx(uint32_t offset, uint32_t len, size_t &idx_num, bool index_content = false);

  //! Deletes one index and its physical files.
  c3db_err_t delete_idx(size_t idx_num);

  //! Rebuilds one index from the current storage contents.
  c3db_err_t index_content(size_t idx_num);

  virtual c3db_err_t max_rec_count(size_t &count) const;
  virtual bool is_open() const;
  virtual bool is_read_only() const;

protected:
  c3db_db_t();

  virtual c3db_db_file_t& storage() = 0;
  virtual const c3db_db_file_t& storage() const = 0;
  virtual size_t payload_size() const = 0;

  c3db_err_t set_base_file_name(const char* base_file_name);
  c3db_err_t load_idxs();
  c3db_err_t make_idx_base_name(size_t idx_num, char* &idx_base_name) const;
  c3db_err_t open_idxs_file(c3db_db_file_t &idxs);
  c3db_err_t build_idx_cache(c3db_db_file_t &idxs);
  void reset_state();

  char* base_file_name_;
  size_t idx_count_;
  c3db_idx_hdr_t idx_hdr_;
  c3db_index_t* indexes_[C3DB_IDX_CAPACITY];
};
