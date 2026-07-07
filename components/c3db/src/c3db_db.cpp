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

#include "c3db_db.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "c3db_defs.h"
#include "c3db_file.h"
#include "c3db_utils.h"

static constexpr const char* C3DB_DB_EXTENSION = ".db";
static constexpr const char* C3DB_IDX_NAME_SEPARATOR = "_i";

c3db_db_t::c3db_db_t()
  : base_file_name_(nullptr),
    idx_count_(0),
    idx_hdr_() {
  for (size_t i = 0; i < C3DB_IDX_CAPACITY; ++i) indexes_[i] = nullptr;
}

c3db_db_t::~c3db_db_t() {
  reset_state();
}

c3db_err_t c3db_db_t::create(const char* base_file_name) {
  if (storage().is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;
  ON_ERR_RETURN(set_base_file_name(base_file_name));

  c3db_err_t err = storage().create(base_file_name);
  if (IS_ERR(err)) reset_state();
  return err;
}

c3db_err_t c3db_db_t::begin(const char* base_file_name, bool read_only) {
  if (storage().is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;
  ON_ERR_RETURN(set_base_file_name(base_file_name));

  c3db_err_t err = storage().begin(base_file_name, read_only);
  if (OK(err)) err = load_idxs();

  if (IS_ERR(err)) {
    storage().end();
    reset_state();
  }
  return err;
}

c3db_err_t c3db_db_t::end() {
  c3db_err_t err = storage().end();

  for (size_t i = 0; i < C3DB_IDX_CAPACITY; ++i) {
    if (indexes_[i] && indexes_[i]->is_open()) {
      c3db_err_t idx_err = indexes_[i]->end();
      if (OK(err)) err = idx_err;
    }
  }

  reset_state();
  return err;
}

c3db_err_t c3db_db_t::insert(const uint8_t* data, c3db_id_t &id) {
  ON_ERR_RETURN(storage().insert(data, id));

  /*
   * Indexes are updated after the payload is durable. If a later index write
   * fails, the record is removed and any references already written become
   * stale references handled by the normal lazy-delete path.
   */
  for (size_t i = 0; i < C3DB_IDX_CAPACITY; ++i) {
    if (idx_hdr_.slots[i].len == 0) continue;

    const c3db_idx_slot_t &slot = idx_hdr_.slots[i];
    if (!indexes_[i]) return C3DB_FILE_NOT_OPEN_ERR;
    c3db_err_t err = indexes_[i]->index(c3db_crc32(data + slot.offset, slot.len), id);
    if (IS_ERR(err)) {
      /*
       * The record is removed if indexing fails after insertion. References
       * already written for previous indexes become harmless stale refs and
       * will be cleaned lazily.
       */
      storage().remove(id);
      return err;
    }
  }

  return C3DB_OK;
}

c3db_err_t c3db_db_t::select(c3db_id_t id, uint8_t* data) {
  return storage().select(id, data);
}

c3db_err_t c3db_db_t::update(c3db_id_t id, const uint8_t* data) {
  if (!data) return C3DB_INVALID_ARG_ERR;

  /*
   * Updates do not remove old index references immediately. They add the new
   * references first; old references are later discarded when indexed reads
   * detect that the current payload no longer matches the searched hash.
   */
  for (size_t i = 0; i < C3DB_IDX_CAPACITY; ++i) {
    if (idx_hdr_.slots[i].len == 0) continue;

    const c3db_idx_slot_t &slot = idx_hdr_.slots[i];
    if (!indexes_[i]) return C3DB_FILE_NOT_OPEN_ERR;
    ON_ERR_RETURN(indexes_[i]->index(c3db_crc32(data + slot.offset, slot.len), id));
  }

  /*
   * New index references are persisted before the payload update. If the final
   * update fails, those references point to the old payload and are discarded
   * later by the hash validation in indexed select/remove.
   */
  return storage().update(id, data);
}

c3db_err_t c3db_db_t::remove(c3db_id_t id) {
  return storage().remove(id);
}

c3db_err_t c3db_db_t::remove(size_t idx_num, const uint8_t* value, size_t &removed_count) {
  removed_count = 0;
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;
  if (!value) return C3DB_INVALID_ARG_ERR;
  if (idx_num >= C3DB_IDX_CAPACITY) return C3DB_INVALID_ARG_ERR;
  if (idx_hdr_.slots[idx_num].len == 0) return C3DB_REC_NOT_FOUND_ERR;
  if (!indexes_[idx_num] || !indexes_[idx_num]->is_open()) return C3DB_FILE_NOT_OPEN_ERR;

  const c3db_idx_slot_t &slot = idx_hdr_.slots[idx_num];
  const uint64_t hash = c3db_crc32(value, slot.len);

  c3db_id_t record_id = C3DB_NULL_ID;
  c3db_idx_cursor_t cursor = {};
  c3db_err_t err = indexes_[idx_num]->find(hash, record_id, cursor);
  if (IS_ERR(err)) return err;

  uint8_t* payload_buf = new (std::nothrow) uint8_t[payload_size()];
  if (!payload_buf) return C3DB_GENERIC_ERR;

  c3db_id_t last_ref = cursor.node;
  while (true) {
    err = storage().select(record_id, payload_buf);
    const bool matches = OK(err) && c3db_crc32(payload_buf + slot.offset, slot.len) == hash;

    /*
     * The chain may contain stale references left by updates. Only records
     * whose current indexed field still matches the requested value are removed.
     */
    if (matches) {
      err = storage().remove(record_id);
      if (OK(err)) {
        ++removed_count;
      }
      else if (err != C3DB_REC_NOT_FOUND_ERR) {
        delete[] payload_buf;
        return err;
      }
    }
    else if (err != C3DB_REC_NOT_FOUND_ERR && !OK(err)) {
      delete[] payload_buf;
      return err;
    }

    last_ref = cursor.node;
    err = indexes_[idx_num]->find_next(record_id, cursor);
    if (err == C3DB_REC_NOT_FOUND_ERR) break;
    if (IS_ERR(err)) {
      delete[] payload_buf;
      return err;
    }
  }

  delete[] payload_buf;
  return indexes_[idx_num]->remove(hash, last_ref);
}

c3db_err_t c3db_db_t::import_file(FILE* source_file, c3db_id_t &first_id, size_t &rows_added) {
  return storage().import_file(source_file, first_id, rows_added);
}

c3db_err_t c3db_db_t::import_file(
  const char* source_file_name,
  c3db_id_t &first_id,
  size_t &rows_added
) {
  return storage().import_file(source_file_name, first_id, rows_added);
}

c3db_err_t c3db_db_t::export_file(FILE* target_file, size_t &rows_exported) {
  return storage().export_file(target_file, rows_exported);
}

c3db_err_t c3db_db_t::export_file(const char* target_file_name, size_t &rows_exported) {
  return storage().export_file(target_file_name, rows_exported);
}

c3db_err_t c3db_db_t::create_idx(
  uint32_t offset,
  uint32_t len,
  size_t &idx_num,
  bool index_content_flag
) {
  idx_num = C3DB_IDX_CAPACITY;
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;
  if (len == 0) return C3DB_INVALID_ARG_ERR;
  if (offset > payload_size() || len > payload_size() - offset) return C3DB_INVALID_ARG_ERR;

  size_t slot = C3DB_IDX_CAPACITY;
  for (size_t i = 0; i < C3DB_IDX_CAPACITY; ++i) {
    if (idx_hdr_.slots[i].len == 0) {
      slot = i;
      break;
    }
  }
  if (slot == C3DB_IDX_CAPACITY) return C3DB_UNSUPPORTED_OP_ERR;

  /*
   * The optional .db file is opened only while the index table is changed.
   * Keeping it closed during normal operation saves the DBF record buffer RAM.
   */
  c3db_db_file_t idxs(sizeof(c3db_idx_hdr_t));
  ON_ERR_RETURN(open_idxs_file(idxs));

  char* idx_base_name = nullptr;
  ON_ERR_RETURN(make_idx_base_name(slot, idx_base_name));

  indexes_[slot] = new (std::nothrow) c3db_index_t();
  if (!indexes_[slot]) {
    delete[] idx_base_name;
    idxs.end();
    return C3DB_GENERIC_ERR;
  }

  c3db_err_t err = indexes_[slot]->create(idx_base_name);
  delete[] idx_base_name;
  if (IS_ERR(err)) return err;

  idx_hdr_.slots[slot].offset = offset;
  idx_hdr_.slots[slot].len = len;

  err = idxs.update(c3db_id(0, 0), reinterpret_cast<const uint8_t*>(&idx_hdr_));
  if (IS_ERR(err)) {
    indexes_[slot]->end();
    delete indexes_[slot];
    indexes_[slot] = nullptr;
    idx_hdr_.slots[slot] = c3db_idx_slot_t{};
    idxs.end();
    return err;
  }

  ++idx_count_;
  idx_num = slot;

  err = idxs.end();
  if (IS_ERR(err)) return err;

  if (!index_content_flag) return C3DB_OK;
  err = index_content(idx_num);
  if (IS_ERR(err)) {
    delete_idx(idx_num);
    idx_num = C3DB_IDX_CAPACITY;
  }
  return err;
}

c3db_err_t c3db_db_t::delete_idx(size_t idx_num) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;
  if (idx_num >= C3DB_IDX_CAPACITY) return C3DB_INVALID_ARG_ERR;
  if (idx_hdr_.slots[idx_num].len == 0) return C3DB_REC_NOT_FOUND_ERR;

  if (indexes_[idx_num] && indexes_[idx_num]->is_open()) {
    c3db_err_t close_err = indexes_[idx_num]->end();
    delete indexes_[idx_num];
    indexes_[idx_num] = nullptr;
    if (IS_ERR(close_err)) return close_err;
  }

  char* idx_base_name = nullptr;
  ON_ERR_RETURN(make_idx_base_name(idx_num, idx_base_name));

  c3db_err_t err = c3db_index_t::delete_index_files(idx_base_name);
  delete[] idx_base_name;
  if (IS_ERR(err)) return err;

  idx_hdr_.slots[idx_num] = c3db_idx_slot_t{};
  --idx_count_;

  if (idx_count_ == 0) {
    char* db_file_name = nullptr;
    ON_ERR_RETURN(c3db_make_file_name(base_file_name_, C3DB_DB_EXTENSION, db_file_name));
    err = c3db_file_t::delete_file(db_file_name);
    delete[] db_file_name;
    if (IS_ERR(err)) return err;

    idx_hdr_ = c3db_idx_hdr_t{};
    return C3DB_OK;
  }

  c3db_db_file_t idxs(sizeof(c3db_idx_hdr_t));
  ON_ERR_RETURN(open_idxs_file(idxs));
  err = idxs.update(c3db_id(0, 0), reinterpret_cast<const uint8_t*>(&idx_hdr_));
  c3db_err_t close_err = idxs.end();
  if (IS_ERR(err)) return err;
  return close_err;
}

c3db_err_t c3db_db_t::index_content(size_t idx_num) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (idx_num >= C3DB_IDX_CAPACITY) return C3DB_INVALID_ARG_ERR;
  if (idx_hdr_.slots[idx_num].len == 0) return C3DB_REC_NOT_FOUND_ERR;
  if (!indexes_[idx_num] || !indexes_[idx_num]->is_open()) return C3DB_FILE_NOT_OPEN_ERR;

  const size_t payload_len = payload_size();
  uint8_t* payload_buf = new (std::nothrow) uint8_t[payload_len];
  if (!payload_buf) return C3DB_GENERIC_ERR;

  const size_t physical_size = storage().file().rec_size();
  uint8_t* rec_buf = new (std::nothrow) uint8_t[physical_size];
  if (!rec_buf) {
    delete[] payload_buf;
    return C3DB_GENERIC_ERR;
  }

  const c3db_idx_slot_t &slot = idx_hdr_.slots[idx_num];
  const size_t rows = storage().file().rec_count();
  c3db_err_t err = C3DB_OK;

  /*
   * Index rebuilding walks physical DBF rows to recover each active logical id.
   * The public select() is then used to obtain the payload, so cached and data
   * databases keep their own consistency rules.
   */
  for (size_t row = 0; row < rows; ++row) {
    if (row > UINT32_MAX) {
      err = C3DB_FILE_SIZE_ERR;
      break;
    }

    err = storage().file().select(row, rec_buf);
    if (IS_ERR(err)) break;

    c3db_db_rec_t rec(rec_buf, storage().data_size());
    err = rec.parse();
    if (IS_ERR(err)) break;
    if (!rec.is_active()) continue;

    const c3db_id_t record_id = c3db_id(static_cast<uint32_t>(row), rec.cycle());
    err = storage().select(record_id, payload_buf);
    if (IS_ERR(err)) break;

    const uint64_t hash = c3db_crc32(payload_buf + slot.offset, slot.len);
    err = indexes_[idx_num]->index(hash, record_id);
    if (IS_ERR(err)) break;
  }

  delete[] rec_buf;
  delete[] payload_buf;

  return err;
}

c3db_err_t c3db_db_t::select(
  size_t idx_num,
  const uint8_t* value,
  uint8_t* data,
  c3db_idx_cursor_t &cursor
) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!data) return C3DB_INVALID_ARG_ERR;
  if (idx_num >= C3DB_IDX_CAPACITY) return C3DB_INVALID_ARG_ERR;
  if (idx_hdr_.slots[idx_num].len == 0) return C3DB_REC_NOT_FOUND_ERR;
  if (!indexes_[idx_num] || !indexes_[idx_num]->is_open()) return C3DB_FILE_NOT_OPEN_ERR;

  const c3db_idx_slot_t &slot = idx_hdr_.slots[idx_num];
  const bool first_call =
    cursor.prev_node == C3DB_NULL_ID &&
    cursor.node == C3DB_NULL_ID &&
    cursor.next_node == C3DB_NULL_ID;

  /*
   * A zeroed cursor starts a search; a non-empty cursor continues it. The hash
   * is stored in the cursor so later calls can validate and lazily discard
   * references made stale by updates.
   */
  c3db_id_t record_id = C3DB_NULL_ID;
  c3db_err_t err = C3DB_OK;

  if (first_call) {
    if (!value) return C3DB_INVALID_ARG_ERR;
    err = indexes_[idx_num]->find(c3db_crc32(value, slot.len), record_id, cursor);
  }
  else {
    err = indexes_[idx_num]->find_next(record_id, cursor);
  }
  if (IS_ERR(err)) return err;

  while (true) {
    err = storage().select(record_id, data);
    if (OK(err) && c3db_crc32(data + slot.offset, slot.len) == cursor.hash) {
      return C3DB_OK;
    }
    if (!OK(err) && err != C3DB_REC_NOT_FOUND_ERR) return err;

    /*
     * Index references are cleaned lazily. A reference is obsolete if the
     * record was deleted or if an update changed the indexed field so its
     * current hash no longer matches this search.
     */

    if (is_read_only()) {
      err = indexes_[idx_num]->find_next(record_id, cursor);
      if (err == C3DB_REC_NOT_FOUND_ERR) return err;
      if (IS_ERR(err)) return err;
      continue;
    }

    ON_ERR_RETURN(indexes_[idx_num]->remove_current_ref(cursor));
    if (cursor.node == C3DB_NULL_ID) return C3DB_REC_NOT_FOUND_ERR;
    ON_ERR_RETURN(indexes_[idx_num]->get_current_ref(record_id, cursor));
  }
}

c3db_err_t c3db_db_t::max_rec_count(size_t &count) const {
  return storage().max_rec_count(count);
}

bool c3db_db_t::is_open() const {
  return storage().is_open();
}

bool c3db_db_t::is_read_only() const {
  return storage().is_read_only();
}

c3db_err_t c3db_db_t::set_base_file_name(const char* base_file_name) {
  if (!base_file_name || base_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  const size_t len = std::strlen(base_file_name);
  char* new_name = new (std::nothrow) char[len + 1];
  if (!new_name) return C3DB_GENERIC_ERR;

  std::memcpy(new_name, base_file_name, len + 1);
  delete[] base_file_name_;
  base_file_name_ = new_name;
  return C3DB_OK;
}

c3db_err_t c3db_db_t::load_idxs() {
  for (size_t i = 0; i < C3DB_IDX_CAPACITY; ++i) {
    if (indexes_[i]) {
      if (indexes_[i]->is_open()) indexes_[i]->end();
      delete indexes_[i];
      indexes_[i] = nullptr;
    }
  }

  idx_count_ = 0;
  idx_hdr_ = c3db_idx_hdr_t{};

  char* db_file_name = nullptr;
  ON_ERR_RETURN(c3db_make_file_name(base_file_name_, C3DB_DB_EXTENSION, db_file_name));

  if (!c3db_file_t::exists(db_file_name)) {
    delete[] db_file_name;
    return C3DB_OK;
  }

  delete[] db_file_name;
  c3db_db_file_t idxs(sizeof(c3db_idx_hdr_t));
  c3db_err_t err = open_idxs_file(idxs);
  if (!IS_ERR(err)) {
    c3db_err_t cache_err = build_idx_cache(idxs);
    if (IS_ERR(cache_err)) err = cache_err;
  }

  c3db_err_t close_err = idxs.end();
  if (OK(err)) err = close_err;

  if (IS_ERR(err)) {
    reset_state();
  }
  return err;
}

c3db_err_t c3db_db_t::make_idx_base_name(size_t idx_num, char* &idx_base_name) const {
  idx_base_name = nullptr;
  if (!base_file_name_) return C3DB_FILE_BAD_NAME_ERR;

  const int suffix_len = std::snprintf(nullptr, 0, "%s%u", C3DB_IDX_NAME_SEPARATOR, static_cast<unsigned>(idx_num));
  if (suffix_len < 0) return C3DB_GENERIC_ERR;

  const size_t base_len = std::strlen(base_file_name_);
  idx_base_name = new (std::nothrow) char[base_len + static_cast<size_t>(suffix_len) + 1];
  if (!idx_base_name) return C3DB_GENERIC_ERR;

  std::memcpy(idx_base_name, base_file_name_, base_len);
  std::snprintf(idx_base_name + base_len, static_cast<size_t>(suffix_len) + 1, "%s%u",
                C3DB_IDX_NAME_SEPARATOR, static_cast<unsigned>(idx_num));
  return C3DB_OK;
}

c3db_err_t c3db_db_t::open_idxs_file(c3db_db_file_t &idxs) {
  if (idxs.is_open()) return C3DB_OK;
  if (!base_file_name_) return C3DB_FILE_BAD_NAME_ERR;

  char* db_file_name = nullptr;
  ON_ERR_RETURN(c3db_make_file_name(base_file_name_, C3DB_DB_EXTENSION, db_file_name));

  c3db_err_t err = C3DB_OK;
  if (c3db_file_t::exists(db_file_name)) {
    err = idxs.begin(db_file_name, storage().is_read_only(), "");
  } else {
    err = idxs.create(db_file_name, "");
    if (OK(err)) {
      c3db_id_t hdr_id = C3DB_NULL_ID;
      err = idxs.append(reinterpret_cast<const uint8_t*>(&idx_hdr_), hdr_id);
      if (OK(err) && c3db_row(hdr_id) != 0) err = C3DB_FILE_CORRUPT_ERR;
    }
  }

  delete[] db_file_name;
  return err;
}

c3db_err_t c3db_db_t::build_idx_cache(c3db_db_file_t &idxs) {
  idx_count_ = 0;
  idx_hdr_ = c3db_idx_hdr_t{};
  if (idxs.file().rec_count() == 0) return C3DB_FILE_CORRUPT_ERR;

  ON_ERR_RETURN(idxs.select(c3db_id(0, 0), reinterpret_cast<uint8_t*>(&idx_hdr_)));

  for (size_t i = 0; i < C3DB_IDX_CAPACITY; ++i) {
    if (idx_hdr_.slots[i].len == 0) continue;

    char* idx_base_name = nullptr;
    ON_ERR_RETURN(make_idx_base_name(i, idx_base_name));

    indexes_[i] = new (std::nothrow) c3db_index_t();
    if (!indexes_[i]) {
      delete[] idx_base_name;
      return C3DB_GENERIC_ERR;
    }

    c3db_err_t err = indexes_[i]->begin(idx_base_name, storage().is_read_only());
    delete[] idx_base_name;
    if (IS_ERR(err)) {
      delete indexes_[i];
      indexes_[i] = nullptr;
      return err;
    }

    ++idx_count_;
  }
  return C3DB_OK;
}

void c3db_db_t::reset_state() {
  for (size_t i = 0; i < C3DB_IDX_CAPACITY; ++i) {
    if (indexes_[i]) {
      if (indexes_[i]->is_open()) indexes_[i]->end();
      delete indexes_[i];
      indexes_[i] = nullptr;
    }
  }

  delete[] base_file_name_;
  base_file_name_ = nullptr;
  idx_hdr_ = c3db_idx_hdr_t{};
  idx_count_ = 0;
}
