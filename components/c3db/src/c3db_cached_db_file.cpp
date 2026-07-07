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

#include "c3db_cached_db_file.h"

#include <cstring>
#include <new>

#include "c3db_config.h"
#include "c3db_defs.h"
#include "c3db_utils.h"

static size_t weighted_capacity(size_t total, size_t weight) {
  return (total * weight) / 100;
}

/* ==========================================================================
 * c3db_cached_rec_t
 * ==========================================================================
 */

c3db_cached_rec_t::c3db_cached_rec_t(uint8_t* raw_data, size_t data_size)
  : c3db_rec_t(raw_data, data_size) {
}

size_t c3db_cached_rec_t::rec_size() const {
  return rec_size(data_size_);
}

size_t c3db_cached_rec_t::rec_size(size_t data_size) {
  return PAYLOAD_OFFSET + data_size;
}

c3db_err_t c3db_cached_rec_t::initialize(
  uint8_t state,
  uint8_t data_slot,
  const del_block_t &delete_block_value,
  const data_block_t &data_block_value,
  const uint8_t* payload
) {
  if (!raw_data_) return C3DB_GENERIC_ERR;
  if (!payload) return C3DB_INVALID_ARG_ERR;
  if (state == C3DB_REC_CORRUPT) return C3DB_INVALID_ARG_ERR;
  if (state != C3DB_REC_ACTIVE &&
      state != C3DB_REC_DELETED) {
    return C3DB_INVALID_ARG_ERR;
  }
  if (data_slot != C3DB_REC_SLOT_0 && data_slot != C3DB_REC_SLOT_1) {
    return C3DB_INVALID_ARG_ERR;
  }

  std::memset(raw_data_, 0, rec_size());
  set_state(state);
  set_data_slot(data_slot);
  *del_block() = delete_block_value;
  *data_block() = data_block_value;
  std::memcpy(const_cast<uint8_t*>(this->payload()), payload, data_size_);
  return C3DB_OK;
}

c3db_err_t c3db_cached_rec_t::save_payload(c3db_file_t* file, size_t row_id, const uint8_t* payload) {
  if (!file) return C3DB_INVALID_ARG_ERR;
  if (!payload) return C3DB_INVALID_ARG_ERR;
  if (!raw_data_) return C3DB_GENERIC_ERR;
  if (data_slot() != C3DB_REC_SLOT_0 && data_slot() != C3DB_REC_SLOT_1) return C3DB_REC_CORRUPT_ERR;

  const uint8_t write_slot = next_data_slot();
  ON_ERR_RETURN(set_payload(payload));
  ON_ERR_RETURN(write_payload(file, row_id));
  set_data_slot(write_slot);
  set_state(C3DB_REC_ACTIVE);
  return C3DB_OK;
}

c3db_err_t c3db_cached_rec_t::get_payload(uint8_t* buffer) const {
  // The caller must ensure reading this record state is valid for the operation.
  if (!buffer) return C3DB_INVALID_ARG_ERR;
  if (!is_valid()) return C3DB_REC_CORRUPT_ERR;

  std::memcpy(buffer, payload(), data_size_);
  return C3DB_OK;
}

c3db_err_t c3db_cached_rec_t::export_payload(c3db_file_t* target_file) const {
  if (!target_file) return C3DB_INVALID_ARG_ERR;
  if (!is_active()) return C3DB_REC_CORRUPT_ERR;
  return target_file->write(payload(), data_size_);
}

c3db_err_t c3db_cached_rec_t::export_to(c3db_db_rec_t &db_rec) const {
  if (!raw_data_) return C3DB_GENERIC_ERR;
  if (!is_active()) return C3DB_REC_CORRUPT_ERR;
  return db_rec.initialize(payload());
}

c3db_err_t c3db_cached_rec_t::set_payload(uint32_t seq, uint32_t cycle, const uint8_t* payload) {
  // The caller must ensure this state transition is valid for the operation.
  if (!payload) return C3DB_INVALID_ARG_ERR;
  if (!raw_data_) return C3DB_GENERIC_ERR;

  set_state(C3DB_REC_ACTIVE);
  data_block_t* block = reinterpret_cast<data_block_t*>(raw_data_ + DATA_BLOCK_OFFSET);
  block->seq = seq;
  block->cycle = cycle;
  block->crc = data_block_crc(seq, cycle, payload, data_size_);
  std::memcpy(const_cast<uint8_t*>(this->payload()), payload, data_size_);

  return C3DB_OK;
}

c3db_err_t c3db_cached_rec_t::write_payload(c3db_file_t* file, size_t row_id) {
  if (!file) return C3DB_INVALID_ARG_ERR;
  if (!raw_data_ || !is_active()) return C3DB_REC_CORRUPT_ERR;
  if (data_slot() != C3DB_REC_SLOT_0 && data_slot() != C3DB_REC_SLOT_1) return C3DB_REC_CORRUPT_ERR;

  const uint8_t slot = next_data_slot();
  const size_t data_block_size = sizeof(data_block_t) + data_size_;
  const size_t offset = sizeof(del_block_t) + (slot * data_block_size);
  return file->write_rec(
    row_id,
    offset,
    reinterpret_cast<const uint8_t*>(data_block()),
    data_block_size
  );
}

const uint8_t* c3db_cached_rec_t::payload() const {
  return raw_data_ + PAYLOAD_OFFSET;
}

uint8_t c3db_cached_rec_t::state() const {
  return raw_data_ ? raw_data_[STATE_OFFSET] : C3DB_REC_CORRUPT;
}

auto c3db_cached_rec_t::del_block() -> del_block_t* {
  return reinterpret_cast<del_block_t*>(raw_data_ + DEL_BLOCK_OFFSET);
}

auto c3db_cached_rec_t::del_block() const -> const del_block_t* {
  return reinterpret_cast<const del_block_t*>(raw_data_ + DEL_BLOCK_OFFSET);
}

auto c3db_cached_rec_t::data_block() -> data_block_t* {
  const uint8_t slot = data_slot();
  if (slot != C3DB_REC_SLOT_0 && slot != C3DB_REC_SLOT_1) return nullptr;
  if (!is_valid()) return nullptr;
  return reinterpret_cast<data_block_t*>(raw_data_ + DATA_BLOCK_OFFSET);
}

auto c3db_cached_rec_t::data_block() const -> const data_block_t* {
  const uint8_t slot = data_slot();
  if (slot != C3DB_REC_SLOT_0 && slot != C3DB_REC_SLOT_1) return nullptr;
  if (!is_valid()) return nullptr;
  return reinterpret_cast<const data_block_t*>(raw_data_ + DATA_BLOCK_OFFSET);
}

uint8_t c3db_cached_rec_t::data_slot() const {
  return raw_data_ ? raw_data_[DATA_SLOT_OFFSET] : C3DB_REC_NO_SLOT;
}

void c3db_cached_rec_t::set_state(uint8_t state) {
  if (raw_data_) raw_data_[STATE_OFFSET] = state;
}

void c3db_cached_rec_t::set_data_slot(uint8_t data_slot) {
  if (raw_data_) raw_data_[DATA_SLOT_OFFSET] = data_slot;
}

/* ==========================================================================
 * c3db_cached_db_file_t
 * ==========================================================================
 */

c3db_cached_db_file_t::c3db_cached_db_file_t(size_t data_size, size_t memory_size)
  : c3db_db_file_t(data_size),
    autocommit(false),
    memory_size_(memory_size),
    entry_size_(sizeof(cache_meta_t) + c3db_cached_rec_t::rec_size(data_size)),
    total_capacity_(0),
    mode_(C3DB_BALANCED_MODE),
    cache_memory_(nullptr),
    seq_capacity_(0),
    seq_first_row_(0),
    seq_rows_loaded_(0),
    hist_pos_(0),
    hist_rows_loaded_(0),
    write_capacity_(0),
    write_first_row_(0),
    write_rows_pending_(0),
    seq_access_(nullptr),
    seq_copied_(nullptr),
    seq_access_pos_(0),
    seq_access_count_(0) {
}

c3db_cached_db_file_t::~c3db_cached_db_file_t() {
  end();
}

c3db_err_t c3db_cached_db_file_t::create(
  const char* base_file_name,
  c3db_cache_mode_t mode,
  const char* extension
) {
  ON_ERR_RETURN(allocate_cache(mode));
  c3db_err_t err = c3db_db_file_t::create(base_file_name, extension);
  if (IS_ERR(err)) release_cache();
  return err;
}

c3db_err_t c3db_cached_db_file_t::begin(
  const char* base_file_name,
  bool read_only,
  c3db_cache_mode_t mode,
  const char* extension
) {
  ON_ERR_RETURN(allocate_cache(mode));
  c3db_err_t err = c3db_db_file_t::begin(base_file_name, read_only, extension);
  if (IS_ERR(err)) release_cache();
  return err;
}

c3db_err_t c3db_cached_db_file_t::begin(
  FILE* file,
  bool read_only,
  c3db_cache_mode_t mode
) {
  ON_ERR_RETURN(allocate_cache(mode));
  c3db_err_t err = c3db_db_file_t::begin(file, read_only);
  if (IS_ERR(err)) release_cache();
  return err;
}

c3db_err_t c3db_cached_db_file_t::end() {
  if (is_open() && !is_read_only()) {
    ON_ERR_RETURN(commit());
  }

  c3db_err_t err = c3db_db_file_t::end();
  release_cache();
  return err;
}

c3db_err_t c3db_cached_db_file_t::commit() {
  if (write_rows_pending_ == 0) return C3DB_OK;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;

  const size_t physical_size = rec_size();
  size_t rows_done = 0;

  /*
   * Write cache stores compact logical entries. Commit expands them to DBF
   * records only inside the scratch buffer, so normal cache memory is not
   * inflated by duplicated CRC/slot metadata.
   */
  const size_t rows_per_chunk = C3DB_SHARED_BUFFER_SIZE / physical_size;
  while (rows_done < write_rows_pending_) {
    const size_t rows_now = (write_rows_pending_ - rows_done) < rows_per_chunk
      ? (write_rows_pending_ - rows_done)
      : rows_per_chunk;

    for (size_t i = 0; i < rows_now; ++i) {
      c3db_cached_rec_t cached_rec = get_rec(get_entry_idx_from_write_pos(rows_done + i));
      c3db_db_rec_t db_rec(c3db_shared_buffer + (i * physical_size), data_size());
      ON_ERR_RETURN(cached_rec.export_to(db_rec));
    }

    size_t first_row = 0;
    size_t rows_added = 0;
    ON_ERR_RETURN(file_.extend(c3db_shared_buffer, rows_now * physical_size, first_row, rows_added));
    if (rows_added != rows_now || first_row != (write_first_row_ + rows_done)) return C3DB_FILE_CORRUPT_ERR;

    rows_done += rows_now;
  }

  clear_write_cache();
  return C3DB_OK;
}

c3db_cache_mode_t c3db_cached_db_file_t::mode() const {
  return mode_;
}

c3db_err_t c3db_cached_db_file_t::mode(c3db_cache_mode_t new_mode) {
  if (!cache_memory_) return C3DB_FILE_NOT_OPEN_ERR;
  if (mode_ == new_mode) return C3DB_OK;

  /*
   * Mode changes move boundaries inside one contiguous memory block. Pending
   * appends must be published first, otherwise repartitioning would discard
   * logical records that are not on disk yet.
   */
  if (is_open() && !is_read_only()) {
    ON_ERR_RETURN(commit());
  }

  configure_cache(new_mode);
  return C3DB_OK;
}

c3db_err_t c3db_cached_db_file_t::import_file(FILE* source_file, c3db_id_t &first_id, size_t &rows_added) {
  ON_ERR_RETURN(commit());
  c3db_err_t err = c3db_db_file_t::import_file(source_file, first_id, rows_added);
  if (OK(err)) {
    clear_seq_cache();
    clear_hist_cache();
  }
  return err;
}

c3db_err_t c3db_cached_db_file_t::import_file(
  const char* source_file_name,
  c3db_id_t &first_id,
  size_t &rows_added
) {
  ON_ERR_RETURN(commit());
  c3db_err_t err = c3db_db_file_t::import_file(source_file_name, first_id, rows_added);
  if (OK(err)) {
    clear_seq_cache();
    clear_hist_cache();
  }
  return err;
}

c3db_err_t c3db_cached_db_file_t::export_file(FILE* target_file, size_t &rows_exported) {
  ON_ERR_RETURN(commit());
  return c3db_db_file_t::export_file(target_file, rows_exported);
}

c3db_err_t c3db_cached_db_file_t::export_file(const char* target_file_name, size_t &rows_exported) {
  ON_ERR_RETURN(commit());
  return c3db_db_file_t::export_file(target_file_name, rows_exported);
}

c3db_err_t c3db_cached_db_file_t::append(const uint8_t* data, c3db_id_t &id) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!data) return C3DB_INVALID_ARG_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;

  if (write_rows_pending_ == write_capacity_) ON_ERR_RETURN(commit());
  if (write_rows_pending_ == 0) write_first_row_ = static_cast<uint32_t>(file_.rec_count());

  id = c3db_id(write_first_row_ + static_cast<uint32_t>(write_rows_pending_), 0);
  set_entry(get_entry_idx_from_write_pos(write_rows_pending_), C3DB_CACHE_VALID, id, data);
  ++write_rows_pending_;

  /*
   * In normal cached mode the id is reserved before persistence. autocommit
   * keeps the same API but narrows the durability window to this append call.
   */
  if (autocommit) return commit();
  return C3DB_OK;
}

c3db_err_t c3db_cached_db_file_t::insert(const uint8_t* data, c3db_id_t &id) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!data) return C3DB_INVALID_ARG_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;

  ON_ERR_RETURN(commit());
  c3db_err_t err = c3db_db_file_t::insert(data, id);
  if (OK(err) || IS_WNG(err)) mark_row_empty(c3db_row(id));
  return err;
}

c3db_err_t c3db_cached_db_file_t::select(c3db_id_t id, uint8_t* data) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!data) return C3DB_INVALID_ARG_ERR;

  const uint32_t row_id = c3db_row(id);

  /*
   * The lookup order mirrors expected freshness: pending appends are newer
   * than anything persisted, the sequential window is the cheapest disk-backed
   * read path, and history is only useful for recently evicted valid rows.
  */
  if (row_in_write_cache(row_id)) {
    const size_t entry_num = get_entry_idx_from_write_pos(row_id - write_first_row_);
    if (matchs_entry(entry_num, id)) return read_entry_payload(entry_num, data);
  }

  if (row_in_seq_cache(row_id)) {
    const size_t index = row_id - seq_first_row_;
    const size_t entry_num = get_entry_idx_from_seq_pos(index);
    const cache_meta_t* meta = get_meta(entry_num);
    if (matchs_entry(entry_num, id)) {
      register_seq_access(index);
      return read_entry_payload(entry_num, data);
    }

    if (meta->state != C3DB_CACHE_EMPTY) {
      register_seq_access(index);
      return C3DB_REC_NOT_FOUND_ERR;
    }
  } else {
    size_t hist_index = 0;
    if (find_hist_entry(id, hist_index)) {
      const size_t entry_num = get_entry_idx_from_hist_pos(hist_index);
      if (matchs_entry(entry_num, id)) return read_entry_payload(entry_num, data);
    }
  }

  /*
   * A miss loads a new persisted window starting at the requested physical row.
   * If the requested row is still not visible after decoding the window, DBF is
   * consulted as a final authority for the public result.
   */
  ON_ERR_RETURN(load_seq_cache(row_id));

  if (row_in_seq_cache(row_id)) {
    const size_t index = row_id - seq_first_row_;
    const size_t entry_num = get_entry_idx_from_seq_pos(index);
    const cache_meta_t* meta = get_meta(entry_num);
    if (matchs_entry(entry_num, id)) {
      register_seq_access(index);
      return read_entry_payload(entry_num, data);
    }

    if (meta->state != C3DB_CACHE_EMPTY) {
      register_seq_access(index);
      return C3DB_REC_NOT_FOUND_ERR;
    }
  }

  return c3db_db_file_t::select(id, data);
}

c3db_err_t c3db_cached_db_file_t::update(c3db_id_t id, const uint8_t* payload) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!payload) return C3DB_INVALID_ARG_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;

  ON_ERR_RETURN(commit());
  const uint32_t row_id = c3db_row(id);

  if (row_in_seq_cache(row_id)) {
    const size_t entry_num = get_entry_idx_from_seq_pos(row_id - seq_first_row_);
    cache_meta_t* meta = get_meta(entry_num);
    if (meta->state != C3DB_CACHE_EMPTY) return update_from_cached_entry(entry_num, id, payload);
  } else {
    size_t hist_index = 0;
    if (find_hist_entry(id, hist_index)) {
      return update_from_cached_entry(get_entry_idx_from_hist_pos(hist_index), id, payload);
    }
  }

  return c3db_db_file_t::update(id, payload);
}

c3db_err_t c3db_cached_db_file_t::remove(c3db_id_t id) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;

  ON_ERR_RETURN(commit());
  const uint32_t row_id = c3db_row(id);

  if (row_in_seq_cache(row_id)) {
    const size_t entry_num = get_entry_idx_from_seq_pos(row_id - seq_first_row_);
    cache_meta_t* meta = get_meta(entry_num);
    if (meta->state != C3DB_CACHE_EMPTY) return remove_from_cached_entry(entry_num, id);
  } else {
    size_t hist_index = 0;
    if (find_hist_entry(id, hist_index)) {
      return remove_from_cached_entry(get_entry_idx_from_hist_pos(hist_index), id);
    }
  }

  return c3db_db_file_t::remove(id);
}

c3db_err_t c3db_cached_db_file_t::max_rec_count(size_t &count) const {
  c3db_err_t err = c3db_db_file_t::max_rec_count(count);
  if (IS_ERR(err)) return err;
  count += write_rows_pending_;
  return err;
}

c3db_err_t c3db_cached_db_file_t::allocate_cache(c3db_cache_mode_t mode) {
  if (cache_memory_) return C3DB_FILE_ALREADY_OPEN_ERR;
  if (entry_size_ == 0) return C3DB_INVALID_ARG_ERR;

  /*
   * Cache memory is caller-bounded. Any tail bytes that cannot hold a complete
   * entry are ignored, avoiding partial-entry bookkeeping in constrained RAM.
   */
  total_capacity_ = memory_size_ / entry_size_;
  if (total_capacity_ < 3) return C3DB_INVALID_ARG_ERR;

  memory_size_ = total_capacity_ * entry_size_;
  cache_memory_ = new (std::nothrow) uint8_t[memory_size_];
  seq_access_ = new (std::nothrow) size_t[total_capacity_];
  seq_copied_ = new (std::nothrow) uint8_t[total_capacity_];

  if (!cache_memory_ || !seq_access_ || !seq_copied_) {
    release_cache();
    return C3DB_GENERIC_ERR;
  }

  configure_cache(mode);
  return C3DB_OK;
}

void c3db_cached_db_file_t::configure_cache(c3db_cache_mode_t mode) {
  size_t seq_weight = 20;
  size_t write_weight = 50;

  if (mode == C3DB_SEQUENTIAL_ACCESS_MODE) {
    seq_weight = 70;
    write_weight = 15;
  } else if (mode == C3DB_BULK_INSERT_MODE) {
    seq_weight = 10;
    write_weight = 80;
  }

  seq_capacity_ = weighted_capacity(total_capacity_, seq_weight);
  write_capacity_ = weighted_capacity(total_capacity_, write_weight);

  /*
   * The historical capacity is implicit. Keeping at least one entry for seq
   * and write, and leaving total > seq + write, guarantees all three cache
   * zones exist even with the minimum supported memory.
   */
  if (seq_capacity_ == 0) seq_capacity_ = 1;
  if (write_capacity_ == 0) write_capacity_ = 1;

  while (seq_capacity_ + write_capacity_ >= total_capacity_) {
    if (write_capacity_ >= seq_capacity_ && write_capacity_ > 1) {
      --write_capacity_;
    } else if (seq_capacity_ > 1) {
      --seq_capacity_;
    } else {
      break;
    }
  }

  mode_ = mode;

  clear_seq_cache();
  clear_hist_cache();
  clear_write_cache();
}

void c3db_cached_db_file_t::release_cache() {
  delete[] cache_memory_;
  delete[] seq_access_;
  delete[] seq_copied_;

  cache_memory_ = nullptr;
  seq_access_ = nullptr;
  seq_copied_ = nullptr;
  total_capacity_ = 0;
  seq_capacity_ = 0;
  write_capacity_ = 0;

  clear_seq_cache();
  clear_hist_cache();
  clear_write_cache();
}

size_t c3db_cached_db_file_t::hist_capacity() const {
  return total_capacity_ - seq_capacity_ - write_capacity_;
}

size_t c3db_cached_db_file_t::get_entry_idx_from_seq_pos(size_t pos) const {
  return pos;
}

size_t c3db_cached_db_file_t::get_entry_idx_from_hist_pos(size_t pos) const {
  return seq_capacity_ + pos;
}

size_t c3db_cached_db_file_t::get_entry_idx_from_write_pos(size_t pos) const {
  return seq_capacity_ + hist_capacity() + pos;
}

auto c3db_cached_db_file_t::get_meta(size_t entry_num) -> cache_meta_t* {
  return reinterpret_cast<cache_meta_t*>(cache_memory_ + (entry_num * entry_size_));
}

auto c3db_cached_db_file_t::get_meta(size_t entry_num) const -> const cache_meta_t* {
  return reinterpret_cast<const cache_meta_t*>(cache_memory_ + (entry_num * entry_size_));
}

c3db_cached_rec_t c3db_cached_db_file_t::get_rec(size_t entry_num) {
  return c3db_cached_rec_t(
    cache_memory_ + (entry_num * entry_size_) + sizeof(cache_meta_t),
    data_size()
  );
}

c3db_cached_rec_t c3db_cached_db_file_t::get_rec(size_t entry_num) const {
  return c3db_cached_rec_t(
    const_cast<uint8_t*>(cache_memory_ + (entry_num * entry_size_) + sizeof(cache_meta_t)),
    data_size()
  );
}

void c3db_cached_db_file_t::set_entry(
  size_t entry_num,
  cache_entry_state_t state,
  c3db_id_t id,
  const uint8_t* payload
) {
  cache_meta_t* meta = get_meta(entry_num);
  meta->state = static_cast<uint8_t>(state);
  meta->id = id;

  c3db_cached_rec_t rec = get_rec(entry_num);
  rec.initialize(payload);
}

c3db_err_t c3db_cached_db_file_t::set_entry(size_t entry_num, c3db_id_t id, const c3db_db_rec_t &rec) {
  if (!rec.is_active()) return C3DB_REC_NOT_FOUND_ERR;

  cache_meta_t* meta = get_meta(entry_num);
  meta->state = C3DB_CACHE_VALID;
  meta->id = id;

  c3db_cached_rec_t cached = get_rec(entry_num);
  return rec.export_to(cached);
}

c3db_err_t c3db_cached_db_file_t::update_from_cached_entry(
  size_t entry_num,
  c3db_id_t id,
  const uint8_t* payload
) {
  cached_row_ = C3DB_NULL_REF;
  cache_meta_t* meta = get_meta(entry_num);
  if (!meta || meta->state != C3DB_CACHE_VALID) return C3DB_REC_NOT_FOUND_ERR;

  c3db_cached_rec_t rec = get_rec(entry_num);
  if (!rec.is_active() || rec.cycle() != c3db_cycle(id)) return C3DB_REC_NOT_FOUND_ERR;

  ON_ERR_RETURN(rec.save_payload(&file_, c3db_row(id), payload));
  return C3DB_OK;
}

c3db_err_t c3db_cached_db_file_t::remove_from_cached_entry(size_t entry_num, c3db_id_t id) {
  cached_row_ = C3DB_NULL_REF;
  cache_meta_t* meta = get_meta(entry_num);
  if (!meta || meta->state != C3DB_CACHE_VALID) return C3DB_REC_NOT_FOUND_ERR;

  c3db_cached_rec_t rec = get_rec(entry_num);
  if (!rec.is_active() || rec.cycle() != c3db_cycle(id)) return C3DB_REC_NOT_FOUND_ERR;

  ON_ERR_RETURN(rec.save_deleted(&file_, c3db_row(id), first_free_));
  ON_ERR_RETURN(add_free(c3db_row(id)));
  meta->id = id;
  meta->state = C3DB_CACHE_DELETED;
  return C3DB_OK;
}

bool c3db_cached_db_file_t::row_in_seq_cache(uint32_t row_id) const {
  return seq_rows_loaded_ > 0 &&
         row_id >= seq_first_row_ &&
         row_id < seq_first_row_ + seq_rows_loaded_;
}

bool c3db_cached_db_file_t::row_in_write_cache(uint32_t row_id) const {
  return write_rows_pending_ > 0 &&
         row_id >= write_first_row_ &&
         row_id < write_first_row_ + write_rows_pending_;
}

bool c3db_cached_db_file_t::find_hist_entry(c3db_id_t id, size_t &index) const {
  for (size_t n = 0; n < hist_rows_loaded_; ++n) {
    const size_t i = (hist_pos_ + hist_capacity() - 1 - n) % hist_capacity();
    const cache_meta_t* meta = get_meta(get_entry_idx_from_hist_pos(i));
    if ((meta->state == C3DB_CACHE_VALID || meta->state == C3DB_CACHE_DELETED) &&
        meta->id == id) {
      index = i;
      return true;
    }
  }
  return false;
}

bool c3db_cached_db_file_t::matchs_entry(size_t entry_num, c3db_id_t id) const {
  const cache_meta_t* meta = get_meta(entry_num);
  if (!meta || meta->state == C3DB_CACHE_EMPTY) return false;

  /*
   * Valid entries must match the full id, including cycle. Negative entries
   * only represent the physical row state loaded from disk; they intentionally
   * hide any id for that row as DBF would do.
   */
  const uint32_t entry_row = c3db_row(meta->id);
  const uint32_t requested_row = c3db_row(id);
  if (meta->state == C3DB_CACHE_VALID) return meta->id == id;
  return entry_row == requested_row;
}

c3db_err_t c3db_cached_db_file_t::read_entry_payload(size_t entry_num, uint8_t* payload) const {
  if (!payload) return C3DB_INVALID_ARG_ERR;

  const cache_meta_t* meta = get_meta(entry_num);
  if (!meta || meta->state != C3DB_CACHE_VALID) return C3DB_REC_NOT_FOUND_ERR;

  c3db_cached_rec_t rec = get_rec(entry_num);
  if (!rec.is_active()) return C3DB_REC_NOT_FOUND_ERR;
  return rec.get_payload(payload);
}

c3db_err_t c3db_cached_db_file_t::load_seq_cache(uint32_t row_id) {
  ON_ERR_RETURN(move_seq_to_hist());
  clear_seq_cache();

  const size_t persisted_count = file_.rec_count();
  if (row_id >= persisted_count) return C3DB_REC_NOT_FOUND_ERR;

  const size_t rows_to_load = seq_capacity_ < (persisted_count - row_id)
    ? seq_capacity_
    : (persisted_count - row_id);
  const size_t physical_size = rec_size();
  if (physical_size == 0) return C3DB_FILE_SIZE_ERR;

  const size_t rows_per_read = C3DB_SHARED_BUFFER_SIZE / physical_size;
  size_t rows_loaded = 0;

  while (rows_loaded < rows_to_load) {
    const size_t rows_now = (rows_to_load - rows_loaded) < rows_per_read
      ? (rows_to_load - rows_loaded)
      : rows_per_read;
    ON_ERR_RETURN(file_.select(row_id + rows_loaded, rows_now, c3db_shared_buffer));

    for (size_t i = 0; i < rows_now; ++i) {
      const size_t cache_index = rows_loaded + i;
      const uint32_t current_row = row_id + static_cast<uint32_t>(cache_index);
      cache_meta_t* meta = get_meta(get_entry_idx_from_seq_pos(cache_index));
      meta->state = C3DB_CACHE_EMPTY;

      c3db_db_rec_t rec(c3db_shared_buffer + (i * physical_size), data_size());
      c3db_err_t err = rec.parse();
      if (OK(err) && rec.is_active()) {
        const c3db_id_t id = c3db_id(current_row, rec.cycle());
        err = set_entry(get_entry_idx_from_seq_pos(cache_index), id, rec);
      }
      if (OK(err) && meta->state == C3DB_CACHE_VALID) {
        continue;
      } else if (err == C3DB_REC_NOT_FOUND_ERR) {
        /*
         * Deleted rows are cached as negative hits. Re-reading the same old id
         * can then return not-found without touching the microSD again.
         */
        meta->id = c3db_id(current_row, 0);
        meta->state = C3DB_CACHE_DELETED;
      } else if ((OK(err) && !rec.is_active()) || err == C3DB_REC_CORRUPT_ERR) {
        /*
         * Corrupt rows are hidden like DBF select() hides them. They are kept
         * out of history because only valid payloads are useful there.
         */
        meta->id = c3db_id(current_row, 0);
        meta->state = rec.is_deleted() ? C3DB_CACHE_DELETED : C3DB_CACHE_CORRUPT;
      } else {
        return err;
      }
    }

    rows_loaded += rows_now;
  }

  seq_first_row_ = row_id;
  seq_rows_loaded_ = rows_loaded;
  return C3DB_OK;
}

c3db_err_t c3db_cached_db_file_t::move_seq_to_hist() {
  if (seq_rows_loaded_ == 0 || hist_capacity() == 0) return C3DB_OK;

  std::memset(seq_copied_, 0, seq_capacity_);
  size_t selected_count = 0;

  /*
   * The access ring is scanned from newest to oldest to keep only the most
   * useful entries when history cannot hold every accessed row. A second pass
   * inserts those selected entries oldest-to-newest, preserving recency order
   * in the history ring.
   */
  for (size_t n = 0; n < seq_access_count_ && selected_count < hist_capacity(); ++n) {
    const size_t ring_index = (seq_access_pos_ + seq_capacity_ - 1 - n) % seq_capacity_;
    const size_t seq_index = seq_access_[ring_index];
    if (seq_index >= seq_rows_loaded_ || seq_copied_[seq_index]) continue;

    const cache_meta_t* meta = get_meta(get_entry_idx_from_seq_pos(seq_index));
    if (meta->state != C3DB_CACHE_VALID) continue;

    seq_copied_[seq_index] = 1;
    ++selected_count;
  }

  /*
   * The second pass walks oldest-to-newest, so the history ring keeps the same
   * relative recency as the original sequential accesses.
   */
  size_t copied = 0;
  for (size_t n = seq_access_count_; n > 0 && copied < selected_count; --n) {
    const size_t ring_index = (seq_access_pos_ + seq_capacity_ - n) % seq_capacity_;
    const size_t seq_index = seq_access_[ring_index];
    if (seq_index < seq_rows_loaded_ && seq_copied_[seq_index]) {
      add_hist_entry(get_meta(get_entry_idx_from_seq_pos(seq_index)));
      seq_copied_[seq_index] = 0;
      ++copied;
    }
  }

  return C3DB_OK;
}

void c3db_cached_db_file_t::add_hist_entry(const cache_meta_t* meta) {
  if (!meta || meta->state != C3DB_CACHE_VALID) return;

  std::memcpy(get_meta(get_entry_idx_from_hist_pos(hist_pos_)), meta, entry_size_);
  hist_pos_ = (hist_pos_ + 1) % hist_capacity();
  if (hist_rows_loaded_ < hist_capacity()) ++hist_rows_loaded_;
}

void c3db_cached_db_file_t::register_seq_access(size_t seq_index) {
  if (seq_index >= seq_rows_loaded_ || seq_capacity_ == 0) return;

  seq_access_[seq_access_pos_] = seq_index;
  seq_access_pos_ = (seq_access_pos_ + 1) % seq_capacity_;
  if (seq_access_count_ < seq_capacity_) ++seq_access_count_;
}

void c3db_cached_db_file_t::clear_seq_cache() {
  seq_first_row_ = 0;
  seq_rows_loaded_ = 0;
  clear_seq_access();
}

void c3db_cached_db_file_t::clear_hist_cache() {
  hist_pos_ = 0;
  hist_rows_loaded_ = 0;
}

void c3db_cached_db_file_t::clear_write_cache() {
  write_first_row_ = 0;
  write_rows_pending_ = 0;
}

void c3db_cached_db_file_t::clear_seq_access() {
  seq_access_pos_ = 0;
  seq_access_count_ = 0;
}

void c3db_cached_db_file_t::mark_row_empty(uint32_t row_id) {
  /*
   * insert() may reactivate a deleted row with a new cycle. Any cached state
   * for that physical row belongs to the previous generation, so the next
   * select must miss and reload the row from DBF.
   */
  if (row_in_seq_cache(row_id)) {
    get_meta(get_entry_idx_from_seq_pos(row_id - seq_first_row_))->state = C3DB_CACHE_EMPTY;
  }

  for (size_t i = 0; i < hist_rows_loaded_; ++i) {
    cache_meta_t* meta = get_meta(get_entry_idx_from_hist_pos(i));
    if (meta->state != C3DB_CACHE_EMPTY && c3db_row(meta->id) == row_id) {
      meta->state = C3DB_CACHE_EMPTY;
    }
  }
}
