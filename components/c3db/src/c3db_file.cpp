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

#include "c3db_file.h"

#include <cstring>
#include <stdio.h>
#include <unistd.h>

#include "c3db_config.h"
#include "c3db_defs.h"

static c3db_err_t open_fn(const char* file_name, FILE* &file, bool read_only) {
  file = nullptr;
  if (!file_name || file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  file = fopen(file_name, read_only ? "r" : "r+");
  if (!file) return C3DB_FILE_OPEN_ERR;
  return C3DB_OK;
}

static c3db_err_t close_fn(FILE* file) {
  if (!file) return C3DB_FILE_NOT_OPEN_ERR;
  if (fclose(file)) return C3DB_FILE_CLOSE_ERR;
  return C3DB_OK;
}

static c3db_err_t create_fn(const char* file_name, FILE* &file) {
  file = nullptr;
  if (!file_name || file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  FILE* existing_file = fopen(file_name, "r");
  if (existing_file) {
    close_fn(existing_file);
    return C3DB_FILE_ALREADY_EXISTS_ERR;
  }

  file = fopen(file_name, "w+");
  if (!file) return C3DB_FILE_OPEN_ERR;
  return C3DB_OK;
}

static c3db_err_t seek_fn(FILE* file, size_t pos) {
  if (!file) return C3DB_FILE_NOT_OPEN_ERR;
  if (fseek(file, pos, SEEK_SET)) return C3DB_FILE_SEEK_ERR;
  return C3DB_OK;
}

static c3db_err_t read_fn(FILE* file, uint8_t* buf, size_t len) {
  if (!file) return C3DB_FILE_NOT_OPEN_ERR;
  if (fread(buf, 1, len, file) != len) {
    return feof(file) == 0 ? C3DB_FILE_READ_ERR : C3DB_EOF_ERR;
  }
  return C3DB_OK;
}

static c3db_err_t tell_fn(FILE* file, size_t &pos) {
  if (!file) return C3DB_FILE_NOT_OPEN_ERR;
  const long file_pos = ftell(file);
  if (file_pos < 0) return C3DB_FILE_TELL_ERR;
  pos = static_cast<size_t>(file_pos);
  return C3DB_OK;
}

static c3db_err_t size_fn(FILE* file, size_t &size) {
  if (!file) return C3DB_FILE_NOT_OPEN_ERR;
  if (fseek(file, 0, SEEK_END)) return C3DB_FILE_SEEK_ERR;
  return tell_fn(file, size);
}

static c3db_err_t flush_fn(FILE* file) {
  if (!file) return C3DB_FILE_NOT_OPEN_ERR;
  if (fflush(file)) return C3DB_FILE_FLUSH_ERR;
  fsync(fileno(file));
  return C3DB_OK;
}

static c3db_err_t write_fn(FILE* file, const uint8_t* buf, size_t len, bool flush) {
  if (!file) return C3DB_FILE_NOT_OPEN_ERR;
  if (fwrite(buf, 1, len, file) != len) return C3DB_FILE_WRITE_ERR;
  if (!flush) return C3DB_OK;
  return flush_fn(file);
}

c3db_file_t::c3db_file_t()
  : file_(nullptr),
    base_addr_(0),
    hdr_size_(0),
    rec_size_(0),
    file_size_(0),
    read_only_(false),
    own_file_(false) {
}

c3db_file_t::~c3db_file_t() {
  end();
}

c3db_err_t c3db_file_t::create(
  const char* file_name,
  size_t base_addr,
  size_t header_size,
  size_t rec_size
) {
  if (file_) return C3DB_FILE_ALREADY_OPEN_ERR;

  FILE* file = nullptr;
  c3db_err_t err = create_fn(file_name, file);
  if (IS_ERR(err)) return err;

  err = begin(file, false, base_addr, header_size, rec_size);
  if (IS_ERR(err)) {
    close_fn(file);
    return err;
  }

  own_file_ = true;
  return C3DB_OK;
}

c3db_err_t c3db_file_t::delete_file(const char* file_name) {
  if (!file_name || file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;
  if (remove(file_name)) return C3DB_FILE_DELETE_ERR;
  return C3DB_OK;
}

bool c3db_file_t::exists(const char* file_name) {
  if (!file_name || file_name[0] == '\0') return false;

  FILE* file = fopen(file_name, "r");
  if (!file) return false;
  close_fn(file);
  return true;
}

c3db_err_t c3db_file_t::rename_file(const char* old_file_name, const char* new_file_name) {
  if (!old_file_name || old_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;
  if (!new_file_name || new_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;
  if (rename(old_file_name, new_file_name)) return C3DB_FILE_RENAME_ERR;
  return C3DB_OK;
}

c3db_err_t c3db_file_t::copy_file(const char* source_file_name, const char* target_file_name) {
  if (!source_file_name || source_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;
  if (!target_file_name || target_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  FILE* source = nullptr;
  c3db_err_t err = open_fn(source_file_name, source, true);
  if (IS_ERR(err)) return err;

  FILE* target = nullptr;
  err = create_fn(target_file_name, target);
  if (IS_ERR(err)) {
    close_fn(source);
    return err;
  }

  size_t source_size = 0;
  err = size_fn(source, source_size);
  size_t offset = 0;

  /*
   * File-to-file copy is intentionally kept in this low-level wrapper so upper
   * layers never need fopen/fread/fwrite directly. The shared buffer bounds RAM
   * usage during backup, import/export and defrag support operations.
   */
  while (OK(err) && offset < source_size) {
    const size_t chunk_len = (source_size - offset) < C3DB_SHARED_BUFFER_SIZE
      ? (source_size - offset)
      : C3DB_SHARED_BUFFER_SIZE;

    err = seek_fn(source, offset);
    if (OK(err)) err = read_fn(source, c3db_shared_buffer, chunk_len);
    if (OK(err)) err = write_fn(target, c3db_shared_buffer, chunk_len, true);
    offset += chunk_len;
  }

  c3db_err_t close_source_err = close_fn(source);
  c3db_err_t close_target_err = close_fn(target);
  if (IS_ERR(err)) return err;
  if (IS_ERR(close_source_err)) return close_source_err;
  if (IS_ERR(close_target_err)) return close_target_err;
  return KO(close_source_err) ? close_source_err : close_target_err;
}

c3db_err_t c3db_file_t::begin(
  const char* file_name,
  bool read_only,
  size_t base_addr,
  size_t header_size,
  size_t rec_size
) {
  if (file_) return C3DB_FILE_ALREADY_OPEN_ERR;

  FILE* file = nullptr;
  c3db_err_t err = open_fn(file_name, file, read_only);
  if (IS_ERR(err)) return err;

  err = begin(file, read_only, base_addr, header_size, rec_size);
  if (IS_ERR(err)) {
    close_fn(file);
    return err;
  }

  own_file_ = true;
  return C3DB_OK;
}

c3db_err_t c3db_file_t::begin(
  FILE* file,
  bool read_only,
  size_t base_addr,
  size_t header_size,
  size_t rec_size
) {
  if (file_) return C3DB_FILE_ALREADY_OPEN_ERR;
  if (!file) return C3DB_INVALID_ARG_ERR;

  file_ = file;
  base_addr_ = base_addr;
  hdr_size_ = header_size;
  rec_size_ = rec_size;
  file_size_ = 0;
  read_only_ = read_only;
  own_file_ = false;

  c3db_err_t err = init_file_size();
  if (IS_ERR(err)) reset_state();
  return err;
}

c3db_err_t c3db_file_t::end() {
  c3db_err_t err = C3DB_OK;
  if (file_ && own_file_) err = close_fn(file_);
  reset_state();
  return err;
}

c3db_err_t c3db_file_t::seek(size_t addr) {
  return seek_fn(file_, abs_addr(addr));
}

c3db_err_t c3db_file_t::read(uint8_t* buf, size_t len) {
  if (!buf && len > 0) return C3DB_INVALID_ARG_ERR;
  if (len == 0) return C3DB_OK;
  return read_fn(file_, buf, len);
}

c3db_err_t c3db_file_t::read(size_t addr, uint8_t* buf, size_t len) {
  if (!file_) return C3DB_FILE_NOT_OPEN_ERR;
  ON_ERR_RETURN(seek(addr));
  return read(buf, len);
}

c3db_err_t c3db_file_t::write(const uint8_t* buf, size_t len, bool flush) {
  if (!buf && len > 0) return C3DB_INVALID_ARG_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (len == 0) return C3DB_OK;
  return write_fn(file_, buf, len, flush);
}

c3db_err_t c3db_file_t::write(size_t addr, const uint8_t* buf, size_t len, bool flush) {
  if (!buf && len > 0) return C3DB_INVALID_ARG_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (len == 0) return C3DB_OK;

  const size_t pos = abs_addr(addr);
  ON_ERR_RETURN(seek(addr));
  c3db_err_t err = write_fn(file_, buf, len, flush);
  if (IS_ERR(err)) return err;
  if (pos + len > file_size_) file_size_ = pos + len;
  return C3DB_OK;
}

c3db_err_t c3db_file_t::flush() {
  return flush_fn(file_);
}

c3db_err_t c3db_file_t::read_hdr(size_t offset, uint8_t* buf, size_t len) {
  if (offset > hdr_size_ || len > hdr_size_ - offset) return C3DB_EOF_ERR;
  return read(offset, buf, len);
}

c3db_err_t c3db_file_t::write_hdr(size_t offset, const uint8_t* buf, size_t len, bool flush) {
  if (offset > hdr_size_ || len > hdr_size_ - offset) return C3DB_EOF_ERR;
  return write(offset, buf, len, flush);
}

c3db_err_t c3db_file_t::select(size_t row_id, uint8_t* buf) {
  return read_rec(row_id, 0, buf, rec_size_);
}

c3db_err_t c3db_file_t::select(size_t first_row_id, size_t rows, uint8_t* buf) {
  if (rec_size_ == 0) return C3DB_NO_RECORDS_ERR;
  if (!buf && rows > 0) return C3DB_INVALID_ARG_ERR;
  if (rows == 0) return C3DB_OK;

  const size_t current_count = rec_count();
  if (first_row_id >= current_count || rows > current_count - first_row_id) return C3DB_EOF_ERR;
  return read(rec_rel_addr(first_row_id), buf, rows * rec_size_);
}

c3db_err_t c3db_file_t::update(size_t row_id, const uint8_t* buf, bool flush) {
  return write_rec(row_id, 0, buf, rec_size_, flush);
}

c3db_err_t c3db_file_t::append(const uint8_t* buf, size_t &row_id) {
  if (rec_size_ == 0) return C3DB_NO_RECORDS_ERR;
  if (!buf) return C3DB_INVALID_ARG_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;

  size_t rows_added = 0;
  ON_ERR_RETURN(extend(buf, rec_size_, row_id, rows_added));
  return rows_added == 1 ? C3DB_OK : C3DB_FILE_WRITE_ERR;
}

c3db_err_t c3db_file_t::extend(
  const uint8_t* buf,
  size_t len,
  size_t &first_row_id,
  size_t &rows_added
) {
  if (!buf && len > 0) return C3DB_INVALID_ARG_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;

  first_row_id = rec_count();
  rows_added = 0;
  if (len == 0) return C3DB_OK;

  if (rec_size_ == 0) {
    if (header_only()) return C3DB_NO_RECORDS_ERR;
    // No header and no record size means raw byte-stream mode.
    return write(file_size_ - base_addr_, buf, len);
  }

  if (len % rec_size_ != 0) return C3DB_INVALID_ARG_ERR;
  rows_added = len / rec_size_;

  /*
   * Interrupted appends can leave a partial tail. rec_count() ignores that
   * tail, so extending from the logical end overwrites it.
   */
  return write(rec_rel_addr(first_row_id), buf, len);
}

c3db_err_t c3db_file_t::import_file(
  const char* source_file_name,
  size_t &first_row_id,
  size_t &rows_added
) {
  if (!source_file_name || source_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (header_only()) return C3DB_NO_RECORDS_ERR;

  FILE* source_file = nullptr;
  c3db_err_t err = open_fn(source_file_name, source_file, true);
  if (IS_ERR(err)) return err;

  err = import_file(source_file, first_row_id, rows_added);
  c3db_err_t close_err = close_fn(source_file);
  if (IS_ERR(err)) return err;
  if (IS_ERR(close_err)) return close_err;
  return KO(err) ? err : close_err;
}

c3db_err_t c3db_file_t::import_file(FILE* source_file, size_t &first_row_id, size_t &rows_added) {
  if (!source_file) return C3DB_INVALID_ARG_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (header_only()) return C3DB_NO_RECORDS_ERR;

  size_t source_size = 0;
  c3db_err_t err = size_fn(source_file, source_size);
  if (IS_ERR(err)) return err;

  if (rec_size_ > 0 && (source_size % rec_size_) != 0) {
    return C3DB_FILE_SIZE_ERR;
  }

  first_row_id = rec_count();
  rows_added = rec_size_ > 0 ? source_size / rec_size_ : 0;

  size_t remaining = source_size;
  size_t source_offset = 0;
  size_t target_offset = rec_size_ > 0 ? rec_rel_addr(first_row_id) : file_size_ - base_addr_;

  while (remaining > 0) {
    const size_t chunk_len = remaining < C3DB_SHARED_BUFFER_SIZE ? remaining : C3DB_SHARED_BUFFER_SIZE;

    err = seek_fn(source_file, source_offset);
    if (OK(err)) err = read_fn(source_file, c3db_shared_buffer, chunk_len);
    if (OK(err)) err = write(target_offset, c3db_shared_buffer, chunk_len);
    if (IS_ERR(err)) {
      return err;
    }

    source_offset += chunk_len;
    target_offset += chunk_len;
    remaining -= chunk_len;
  }

  return C3DB_OK;
}

c3db_err_t c3db_file_t::read_rec(size_t row_id, size_t offset, uint8_t* buf, size_t len) {
  if (rec_size_ == 0) return C3DB_NO_RECORDS_ERR;
  if (offset > rec_size_ || len > rec_size_ - offset) return C3DB_EOF_ERR;
  if (row_id >= rec_count()) return C3DB_EOF_ERR;
  return read(rec_rel_addr(row_id) + offset, buf, len);
}

c3db_err_t c3db_file_t::write_rec(
  size_t row_id,
  size_t offset,
  const uint8_t* buf,
  size_t len,
  bool flush
) {
  if (rec_size_ == 0) return C3DB_NO_RECORDS_ERR;
  if (offset > rec_size_ || len > rec_size_ - offset) return C3DB_EOF_ERR;
  if (row_id >= rec_count()) return C3DB_EOF_ERR;
  return write(rec_rel_addr(row_id) + offset, buf, len, flush);
}

size_t c3db_file_t::base_addr() const {
  return base_addr_;
}

size_t c3db_file_t::hdr_size() const {
  return hdr_size_;
}

size_t c3db_file_t::rec_size() const {
  return rec_size_;
}

size_t c3db_file_t::file_size() const {
  return file_size_;
}

size_t c3db_file_t::rec_count() const {
  if (rec_size_ == 0) return 0;
  const size_t first_record = rec_addr(0);
  if (file_size_ <= first_record) return 0;
  return (file_size_ - first_record) / rec_size_;
}

bool c3db_file_t::is_open() const {
  return file_ != nullptr;
}

bool c3db_file_t::is_read_only() const {
  return read_only_;
}

size_t c3db_file_t::abs_addr(size_t rel_addr) const {
  return base_addr_ + rel_addr;
}

size_t c3db_file_t::rec_addr(size_t rec_num) const {
  return base_addr_ + hdr_size_ + (rec_num * rec_size_);
}

size_t c3db_file_t::rec_rel_addr(size_t rec_num) const {
  return hdr_size_ + (rec_num * rec_size_);
}

bool c3db_file_t::header_only() const {
  return hdr_size_ > 0 && rec_size_ == 0;
}

c3db_err_t c3db_file_t::init_file_size() {
  c3db_err_t err = size_fn(file_, file_size_);
  if (IS_ERR(err)) return err;
  if (file_size_ < base_addr_) return C3DB_FILE_SIZE_ERR;

  const size_t min_size = base_addr_ + hdr_size_;
  if (file_size_ < min_size) {
    if (read_only_) return C3DB_FILE_SIZE_ERR;
    err = fill_hdr();
    if (IS_ERR(err)) return err;
  }

  return C3DB_OK;
}

c3db_err_t c3db_file_t::fill_hdr() {
  if (read_only_) return C3DB_READ_ONLY_ERR;

  const size_t min_size = base_addr_ + hdr_size_;
  if (file_size_ >= min_size) return C3DB_OK;

  ON_ERR_RETURN(seek_fn(file_, file_size_));
  std::memset(c3db_shared_buffer, 0, C3DB_SHARED_BUFFER_SIZE);

  size_t remaining = min_size - file_size_;
  while (remaining > 0) {
    const size_t chunk = remaining < C3DB_SHARED_BUFFER_SIZE ? remaining : C3DB_SHARED_BUFFER_SIZE;
    if (fwrite(c3db_shared_buffer, 1, chunk, file_) != chunk) return C3DB_FILE_WRITE_ERR;
    remaining -= chunk;
  }

  ON_ERR_RETURN(flush_fn(file_));
  file_size_ = min_size;
  return C3DB_OK;
}

void c3db_file_t::reset_state() {
  file_ = nullptr;
  base_addr_ = 0;
  hdr_size_ = 0;
  rec_size_ = 0;
  file_size_ = 0;
  read_only_ = false;
  own_file_ = false;
}
