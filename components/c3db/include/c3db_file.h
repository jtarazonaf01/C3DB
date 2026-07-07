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

#include "c3db_types.h"

/**
 * Low-level file wrapper used by C3DB to isolate disk access.
 *
 * The class intentionally has no database semantics: no log, no cache and no
 * record validation. Higher layers decide how records are interpreted and which
 * partial writes are safe.
 *
 * When both header_size and rec_size are zero, the wrapper behaves as a raw
 * byte stream starting at base_addr.
 */
class c3db_file_t
{
public:
  //! Creates an empty file wrapper.
  c3db_file_t();

  //! Closes the file if this wrapper owns it.
  ~c3db_file_t();

  //! Creates and opens a physical file in read/write mode.
  c3db_err_t create(
    const char* file_name,
    size_t base_addr = 0,
    size_t header_size = 0,
    size_t rec_size = 0
  );

  //! Deletes a physical file.
  static c3db_err_t delete_file(const char* file_name);

  //! Returns true when a physical file can be opened for reading.
  static bool exists(const char* file_name);

  //! Renames a physical file.
  static c3db_err_t rename_file(const char* old_file_name, const char* new_file_name);

  //! Copies a physical file using the shared scratch buffer.
  static c3db_err_t copy_file(const char* source_file_name, const char* target_file_name);

  //! Opens a physical file.
  c3db_err_t begin(
    const char* file_name,
    bool read_only = false,
    size_t base_addr = 0,
    size_t header_size = 0,
    size_t rec_size = 0
  );

  /**
   * Uses an already opened physical file.
   *
   * The caller keeps ownership of the FILE pointer, but access must be
   * exclusive while this wrapper is active. Cursor-based read/write operations
   * assume the current file position is valid.
   */
  c3db_err_t begin(
    FILE* file,
    bool read_only = false,
    size_t base_addr = 0,
    size_t header_size = 0,
    size_t rec_size = 0
  );

  //! Ends access to the current physical file.
  c3db_err_t end();

  //! Moves the cursor to an address relative to base_addr.
  c3db_err_t seek(size_t addr);

  /**
   * Reads bytes from the current cursor position.
   *
   * Low-level byte read. The cursor is assumed to be valid.
   */
  c3db_err_t read(uint8_t* buf, size_t len);

  //! Reads bytes from an address relative to base_addr.
  c3db_err_t read(size_t addr, uint8_t* buf, size_t len);

  //! Reads bytes from the fixed header area.
  c3db_err_t read_hdr(size_t offset, uint8_t* buf, size_t len);

  //! Reads a byte range inside a single fixed-size record.
  c3db_err_t read_rec(size_t row_id, size_t offset, uint8_t* buf, size_t len);

  //! Reads one complete fixed-size record.
  c3db_err_t select(size_t row_id, uint8_t* buf);

  //! Reads a contiguous range of complete fixed-size records.
  c3db_err_t select(size_t first_row_id, size_t rows, uint8_t* buf);

  /**
   * Writes bytes at the current cursor position.
   *
   * Low-level byte write. The cursor is assumed to be valid.
   */
  c3db_err_t write(const uint8_t* buf, size_t len, bool flush = true);

  //! Writes bytes at an address relative to base_addr.
  c3db_err_t write(size_t addr, const uint8_t* buf, size_t len, bool flush = true);

  //! Writes bytes to the fixed header area.
  c3db_err_t write_hdr(size_t offset, const uint8_t* buf, size_t len, bool flush = true);

  //! Writes a byte range inside a single fixed-size record.
  c3db_err_t write_rec(size_t row_id, size_t offset, const uint8_t* buf, size_t len, bool flush = true);

  //! Writes one complete fixed-size record.
  c3db_err_t update(size_t row_id, const uint8_t* buf, bool flush = true);

  //! Appends one complete fixed-size record and returns its row.
  c3db_err_t append(const uint8_t* buf, size_t &row_id);

  //! Extends the logical record area with complete fixed-size records.
  c3db_err_t extend(const uint8_t* buf, size_t len, size_t &first_row_id, size_t &rows_added);

  //! Flushes pending stdio data and forces it to the storage device.
  c3db_err_t flush();

  //! Imports records from an already opened file into the logical record area.
  c3db_err_t import_file(FILE* source_file, size_t &first_row_id, size_t &rows_added);

  //! Imports records from a physical file into the logical record area.
  c3db_err_t import_file(const char* source_file_name, size_t &first_row_id, size_t &rows_added);

  //! Returns the base address reserved before the C3DB-managed area.
  size_t base_addr() const;

  //! Returns the fixed header size.
  size_t hdr_size() const;

  //! Returns the fixed record size.
  size_t rec_size() const;

  //! Returns the cached physical file size.
  size_t file_size() const;

  //! Returns the number of complete fixed-size records.
  size_t rec_count() const;

  //! Returns true when a physical file is currently open.
  bool is_open() const;

  //! Returns true when the current physical file is read-only.
  bool is_read_only() const;

private:
  size_t abs_addr(size_t rel_addr) const;
  size_t rec_addr(size_t rec_num) const;
  size_t rec_rel_addr(size_t rec_num) const;
  bool header_only() const;
  c3db_err_t init_file_size();
  c3db_err_t fill_hdr();
  void reset_state();

  FILE* file_;
  size_t base_addr_;
  size_t hdr_size_;
  size_t rec_size_;
  size_t file_size_;
  bool read_only_;
  bool own_file_;
};
