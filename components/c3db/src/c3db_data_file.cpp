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

#include "c3db_data_file.h"

#include <cstring>
#include <new>

#include "c3db_config.h"
#include "c3db_defs.h"
#include "c3db_utils.h"

c3db_data_file_t::c3db_data_file_t(size_t data_size)
  : c3db_db_file_t(sizeof(c3db_data_ref_t)),
    payload_size_(data_size),
    base_file_name_(nullptr),
    dat_file_() {
}

c3db_data_file_t::~c3db_data_file_t() {
  end();
}

c3db_err_t c3db_data_file_t::create(const char* base_file_name) {
  if (payload_size_ == 0) return C3DB_INVALID_ARG_ERR;

  char* dat_name = nullptr;
  ON_ERR_RETURN(c3db_make_file_name(base_file_name, ".dat", dat_name));

  c3db_err_t err = dat_file_.create(dat_name, 0, 0, payload_size_);
  if (IS_ERR(err)) {
    delete[] dat_name;
    return err;
  }

  err = c3db_db_file_t::create(base_file_name, ".met");
  if (IS_ERR(err)) {
    dat_file_.end();
    c3db_file_t::delete_file(dat_name);
    delete[] dat_name;
    return err;
  }

  delete[] dat_name;
  release_base_file_name();
  err = c3db_make_file_name(base_file_name, "", base_file_name_);
  if (IS_ERR(err)) end();
  return err;
}

c3db_err_t c3db_data_file_t::begin(
  const char* base_file_name,
  bool read_only
) {
  if (payload_size_ == 0) return C3DB_INVALID_ARG_ERR;
  ON_ERR_RETURN(recover_defrag_files(base_file_name));

  char* dat_name = nullptr;
  ON_ERR_RETURN(c3db_make_file_name(base_file_name, ".dat", dat_name));

  c3db_err_t err = dat_file_.begin(dat_name, read_only, 0, 0, payload_size_);
  delete[] dat_name;
  if (IS_ERR(err)) return err;

  err = c3db_db_file_t::begin(base_file_name, read_only, ".met");
  if (IS_ERR(err)) {
    dat_file_.end();
    return err;
  }

  const c3db_err_t open_wrn = IS_WNG(err) ? err : C3DB_OK;
  release_base_file_name();
  err = c3db_make_file_name(base_file_name, "", base_file_name_);
  if (IS_ERR(err)) {
    end();
    return err;
  }
  return IS_WNG(open_wrn) ? open_wrn : err;
}

c3db_err_t c3db_data_file_t::end() {
  c3db_err_t err_met = c3db_db_file_t::end();
  c3db_err_t err_dat = dat_file_.end();
  release_base_file_name();
  if (IS_ERR(err_met)) return err_met;
  if (IS_ERR(err_dat)) return err_dat;
  return KO(err_met) ? err_met : err_dat;
}

c3db_err_t c3db_data_file_t::append(const uint8_t* data, c3db_id_t &id) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!data) return C3DB_INVALID_ARG_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;

  /*
   * The payload is written to .dat before publishing its reference in .met.
   * If reset happens before the DBF append, the orphan .dat row is invisible
   * and can be recovered later only through compaction/rebuild logic.
   */
  size_t dat_row = 0;
  ON_ERR_RETURN(dat_file_.append(data, dat_row));
  if (dat_row >= C3DB_DATA_ROW_NULL) return C3DB_FILE_SIZE_ERR;

  c3db_data_ref_t ref = {};
  ref.active = static_cast<uint32_t>(dat_row);
  ref.active_crc = payload_crc(data);
  ref.spare = C3DB_DATA_ROW_NULL;
  ref.spare_crc = 0;

  return c3db_db_file_t::append(reinterpret_cast<const uint8_t*>(&ref), id);
}

c3db_err_t c3db_data_file_t::insert(const uint8_t* data, c3db_id_t &id) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!data) return C3DB_INVALID_ARG_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;

  if (first_free_ == C3DB_NULL_REF) return append(data, id);

  c3db_data_ref_t old_ref = {};
  old_ref.active = C3DB_DATA_ROW_NULL;
  old_ref.active_crc = 0;
  old_ref.spare = C3DB_DATA_ROW_NULL;
  old_ref.spare_crc = 0;

  /*
   * A deleted DBF row may still own .dat rows from its previous lifetime.
   * We reuse the row references, but no previous payload can be considered a
   * valid backup for the new logical record, so spare_crc is reset to zero.
   */
  c3db_err_t err = read_record(first_free_);
  if (OK(err)) err = rec_->get_payload(reinterpret_cast<uint8_t*>(&old_ref));
  if (err != C3DB_OK && err != C3DB_REC_CORRUPT_ERR && err != C3DB_REC_NOT_FOUND_ERR) return err;

  /*
   * Reusing a deleted .met row also tries to reuse its previous .dat rows.
   * Only one becomes active for the new logical record; the other is kept as
   * spare capacity but marked invalid until a future update writes it.
   */
  uint32_t active = old_ref.active != C3DB_DATA_ROW_NULL ? old_ref.active : old_ref.spare;
  uint32_t spare = C3DB_DATA_ROW_NULL;
  if (active == old_ref.active) {
    spare = old_ref.spare;
  } else {
    spare = old_ref.active;
  }

  ON_ERR_RETURN(write_data_row(active, data));

  c3db_data_ref_t ref = {};
  ref.active = active;
  ref.active_crc = payload_crc(data);
  ref.spare = spare != C3DB_DATA_ROW_NULL ? spare : C3DB_DATA_ROW_NULL;
  ref.spare_crc = 0;

  return c3db_db_file_t::insert(reinterpret_cast<const uint8_t*>(&ref), id);
}

c3db_err_t c3db_data_file_t::select(c3db_id_t id, uint8_t* data) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!data) return C3DB_INVALID_ARG_ERR;

  c3db_data_ref_t ref = {};
  ON_ERR_RETURN(c3db_db_file_t::select(id, reinterpret_cast<uint8_t*>(&ref)));
  return read_data_row(ref, data);
}

c3db_err_t c3db_data_file_t::update(c3db_id_t id, const uint8_t* data) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!data) return C3DB_INVALID_ARG_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;

  c3db_data_ref_t ref = {};
  ON_ERR_RETURN(c3db_db_file_t::select(id, reinterpret_cast<uint8_t*>(&ref)));

  /*
   * The new payload is written to the non-visible row first. Only after that
   * succeeds do we publish the swapped references in DBF. This mirrors DBF's
   * own two-slot policy at the large-payload layer.
   */
  uint32_t target = ref.spare;
  ON_ERR_RETURN(write_data_row(target, data));

  c3db_data_ref_t new_ref = {};
  new_ref.active = target;
  new_ref.active_crc = payload_crc(data);
  new_ref.spare = ref.active;
  new_ref.spare_crc = ref.active_crc;

  return c3db_db_file_t::update(id, reinterpret_cast<const uint8_t*>(&new_ref));
}

c3db_err_t c3db_data_file_t::remove(c3db_id_t id) {
  return c3db_db_file_t::remove(id);
}

c3db_err_t c3db_data_file_t::import_file(FILE* source_file, c3db_id_t &first_id, size_t &rows_added) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!source_file) return C3DB_INVALID_ARG_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;

  size_t first_dat_row = 0;
  ON_ERR_RETURN(dat_file_.import_file(source_file, first_dat_row, rows_added));
  return publish_imported_rows(first_dat_row, rows_added, first_id);
}

c3db_err_t c3db_data_file_t::import_file(
  const char* source_file_name,
  c3db_id_t &first_id,
  size_t &rows_added
) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!source_file_name || source_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;

  size_t first_dat_row = 0;
  ON_ERR_RETURN(dat_file_.import_file(source_file_name, first_dat_row, rows_added));
  return publish_imported_rows(first_dat_row, rows_added, first_id);
}

c3db_err_t c3db_data_file_t::export_file(FILE* target_file, size_t &rows_exported) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!target_file) return C3DB_INVALID_ARG_ERR;

  c3db_file_t target;
  ON_ERR_RETURN(target.begin(target_file, false));
  return export_to(target, rows_exported);
}

c3db_err_t c3db_data_file_t::export_file(const char* target_file_name, size_t &rows_exported) {
  if (!target_file_name || target_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  c3db_file_t target;
  ON_ERR_RETURN(target.create(target_file_name));
  c3db_err_t err = export_to(target, rows_exported);
  c3db_err_t close_err = target.end();
  if (IS_ERR(err)) return err;
  if (IS_ERR(close_err)) return close_err;
  return KO(err) ? err : close_err;
}

c3db_err_t c3db_data_file_t::defrag() {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (is_read_only()) return C3DB_READ_ONLY_ERR;
  if (!base_file_name_) return C3DB_INVALID_ARG_ERR;

  char* base_name = nullptr;
  char* met_name = nullptr;
  char* dat_name = nullptr;
  char* met_dfg_name = nullptr;
  char* dat_dfg_name = nullptr;
  char* met_bak_name = nullptr;
  char* dat_bak_name = nullptr;

  c3db_err_t err = c3db_make_file_name(base_file_name_, "", base_name);
  if (OK(err)) err = c3db_make_file_name(base_name, ".met", met_name);
  if (OK(err)) err = c3db_make_file_name(base_name, ".dat", dat_name);
  if (OK(err)) err = c3db_make_file_name(base_name, ".mdf", met_dfg_name);
  if (OK(err)) err = c3db_make_file_name(base_name, ".ddf", dat_dfg_name);
  if (OK(err)) err = c3db_make_file_name(base_name, ".mbk", met_bak_name);
  if (OK(err)) err = c3db_make_file_name(base_name, ".dbk", dat_bak_name);

  auto cleanup_names = [&]() {
    delete[] base_name;
    delete[] met_name;
    delete[] dat_name;
    delete[] met_dfg_name;
    delete[] dat_dfg_name;
    delete[] met_bak_name;
    delete[] dat_bak_name;
  };

  if (IS_ERR(err)) {
    cleanup_names();
    return err;
  }

  auto copy_data_row = [&](uint32_t row_ref, c3db_file_t &target_file, uint32_t &new_row_ref) -> c3db_err_t {
    if (row_ref == C3DB_DATA_ROW_NULL) return C3DB_REC_CORRUPT_ERR;

    const size_t new_row = target_file.rec_count();
    if (new_row >= C3DB_DATA_ROW_NULL) return C3DB_FILE_SIZE_ERR;

    size_t remaining = payload_size_;
    size_t offset = 0;
    const size_t target_base = new_row * payload_size_;

    while (remaining > 0) {
      const size_t chunk_len = remaining < C3DB_SHARED_BUFFER_SIZE ? remaining : C3DB_SHARED_BUFFER_SIZE;
      ON_ERR_RETURN(dat_file_.read_rec(row_ref, offset, c3db_shared_buffer, chunk_len));
      ON_ERR_RETURN(target_file.write(target_base + offset, c3db_shared_buffer, chunk_len));

      offset += chunk_len;
      remaining -= chunk_len;
    }

    new_row_ref = static_cast<uint32_t>(new_row);
    return C3DB_OK;
  };

  err = delete_if_exists(met_bak_name);
  if (OK(err)) err = delete_if_exists(dat_bak_name);
  if (OK(err)) err = delete_if_exists(met_dfg_name);
  if (OK(err)) err = delete_if_exists(dat_dfg_name);
  if (IS_ERR(err)) {
    cleanup_names();
    return err;
  }

  /*
   * .met row ids are externally visible through c3db_id_t, so defrag cannot
   * compact or reorder .met. We copy the file and rewrite only row references.
   * On ESP/FatFS the same file may not be open twice, so the metadata layer is
   * closed briefly before copying .met and reopened before scanning records.
   * Temporary file names use 8.3 extensions because LFN may be disabled.
   */
  err = c3db_db_file_t::end();
  if (OK(err)) err = c3db_file_t::copy_file(met_name, met_dfg_name);
  c3db_err_t reopen_source_err = c3db_db_file_t::begin(base_name, false, ".met");
  if (IS_ERR(err)) {
    cleanup_names();
    return err;
  }
  if (IS_ERR(reopen_source_err)) {
    cleanup_names();
    return reopen_source_err;
  }

  if (OK(err)) {
    c3db_file_t dat_dfg;
    err = dat_dfg.create(dat_dfg_name, 0, 0, payload_size_);

    c3db_db_file_t met_dfg(sizeof(c3db_data_ref_t));
    if (OK(err)) {
      err = met_dfg.begin(base_name, false, ".mdf");
    }

    const size_t met_rows = file_.rec_count();

    for (size_t row = 0; OK(err) && row < met_rows; ++row) {
      c3db_data_ref_t old_ref = {};
      err = read_record(row);
      if (OK(err) && !rec_->is_active()) continue;
      if (OK(err)) {
        err = rec_->get_payload(reinterpret_cast<uint8_t*>(&old_ref));
      }

      if (OK(err)) {
        uint32_t source_row_ref = C3DB_DATA_ROW_NULL;
        uint32_t source_crc = 0;
        uint32_t new_row_ref = C3DB_DATA_ROW_NULL;
        err = get_row_ref(old_ref, source_row_ref, source_crc);
        if (OK(err)) err = copy_data_row(source_row_ref, dat_dfg, new_row_ref);
        if (OK(err)) {
          c3db_data_ref_t new_ref = {};
          new_ref.active = new_row_ref;
          new_ref.active_crc = source_crc;
          new_ref.spare = C3DB_DATA_ROW_NULL;
          new_ref.spare_crc = 0;
          const c3db_id_t id = c3db_id(static_cast<uint32_t>(row), rec_->cycle());
          /*
           * The temporary .met copy starts as a copy of .met, so its inactive slot may still
           * point into the old .dat. Updating twice through DBF overwrites both
           * data slots while preserving the row id and generation cycle.
           */
          err = met_dfg.update(id, reinterpret_cast<const uint8_t*>(&new_ref));
          if (OK(err)) err = met_dfg.update(id, reinterpret_cast<const uint8_t*>(&new_ref));
        }
      }
    }

    c3db_err_t met_close_err = met_dfg.end();
    c3db_err_t dat_close_err = dat_dfg.end();
    if (OK(err)) {
      if (IS_ERR(met_close_err)) err = met_close_err;
      else if (IS_ERR(dat_close_err)) err = dat_close_err;
      else err = KO(met_close_err) ? met_close_err : dat_close_err;
    }
  }

  if (IS_ERR(err)) {
    delete_if_exists(met_dfg_name);
    delete_if_exists(dat_dfg_name);
    cleanup_names();
    return err;
  }

  /*
   * Replacement uses .bak as the last known-good pair. If reset happens during
   * this sequence, begin() restores a complete active pair before opening.
   */
  err = end();
  if (OK(err)) err = c3db_file_t::rename_file(met_name, met_bak_name);
  if (OK(err)) err = c3db_file_t::rename_file(dat_name, dat_bak_name);
  if (OK(err)) err = c3db_file_t::rename_file(met_dfg_name, met_name);
  if (OK(err)) err = c3db_file_t::rename_file(dat_dfg_name, dat_name);

  if (OK(err)) {
    err = delete_if_exists(met_bak_name);
    if (OK(err)) err = delete_if_exists(dat_bak_name);
  } else {
    recover_defrag_files(base_name);
  }

  c3db_err_t reopen_err = begin(base_name, false);
  cleanup_names();
  if (IS_ERR(err)) return err;
  if (IS_ERR(reopen_err)) return reopen_err;
  return KO(err) ? err : reopen_err;
}

c3db_err_t c3db_data_file_t::publish_imported_rows(
  size_t first_dat_row,
  size_t rows_added,
  c3db_id_t &first_id
) {
  first_id = C3DB_NULL_ID;
  if (rows_added == 0) return C3DB_OK;
  if (first_dat_row > UINT32_MAX || rows_added > (static_cast<size_t>(UINT32_MAX) - first_dat_row)) {
    return C3DB_FILE_SIZE_ERR;
  }

  const size_t physical_size = rec_size();
  if (physical_size == 0 || physical_size > C3DB_SHARED_BUFFER_SIZE) return C3DB_FILE_SIZE_ERR;

  const size_t rows_per_chunk = C3DB_SHARED_BUFFER_SIZE / physical_size;
  size_t rows_done = 0;
  size_t first_met_row = 0;

  /*
   * The large payloads are already in .dat. Here we build only the .met DBF
   * records in memory and extend the metadata file by chunks, avoiding one
   * physical DBF append per imported row.
   */
  while (rows_done < rows_added) {
    const size_t rows_now = (rows_added - rows_done) < rows_per_chunk
      ? (rows_added - rows_done)
      : rows_per_chunk;

    for (size_t i = 0; i < rows_now; ++i) {
      const uint32_t row_ref = static_cast<uint32_t>(first_dat_row + rows_done + i);
      if (row_ref == C3DB_DATA_ROW_NULL) return C3DB_FILE_SIZE_ERR;

      c3db_data_ref_t ref = {};
      ref.active = row_ref;
      ref.active_crc = 0;
      ref.spare = C3DB_DATA_ROW_NULL;
      ref.spare_crc = 0;
      ON_ERR_RETURN(payload_crc(row_ref, ref.active_crc));
      c3db_db_rec_t rec(c3db_shared_buffer + (i * physical_size), data_size_);
      ON_ERR_RETURN(rec.initialize(reinterpret_cast<const uint8_t*>(&ref)));
    }

    size_t chunk_first_row = 0;
    size_t chunk_rows_added = 0;
    ON_ERR_RETURN(file_.extend(c3db_shared_buffer, rows_now * physical_size, chunk_first_row, chunk_rows_added));
    if (chunk_rows_added != rows_now) return C3DB_FILE_CORRUPT_ERR;
    if (rows_done == 0) first_met_row = chunk_first_row;
    if (chunk_first_row != first_met_row + rows_done) return C3DB_FILE_CORRUPT_ERR;

    rows_done += rows_now;
  }

  first_id = c3db_id(static_cast<uint32_t>(first_met_row), 0);
  return C3DB_OK;
}

c3db_err_t c3db_data_file_t::export_to(c3db_file_t &target_file, size_t &rows_exported) {
  rows_exported = 0;
  const size_t met_rows = file_.rec_count();
  const bool batch_payloads = payload_size_ > 0 && payload_size_ <= C3DB_SHARED_BUFFER_SIZE;
  const size_t rows_per_batch = batch_payloads ? C3DB_SHARED_BUFFER_SIZE / payload_size_ : 0;
  size_t rows_in_batch = 0;
  c3db_data_ref_t ref = {};
  auto flush_batch = [&]() -> c3db_err_t {
    if (rows_in_batch == 0) return C3DB_OK;
    c3db_err_t err = target_file.write(c3db_shared_buffer, rows_in_batch * payload_size_);
    rows_in_batch = 0;
    return err;
  };

  /*
   * Export walks .met because it defines which .dat rows are logically active.
   * Small payloads are batched in c3db_shared_buffer; large payloads are copied
   * row-by-row in chunks because a full row may not fit in RAM.
   */
  for (size_t row = 0; row < met_rows; ++row) {
    c3db_err_t err = read_record(row);
    if (IS_ERR(err)) return err;
    if (!rec_->is_active()) continue;
    ON_ERR_RETURN(rec_->get_payload(reinterpret_cast<uint8_t*>(&ref)));

    if (batch_payloads) {
      if (rows_in_batch == rows_per_batch) ON_ERR_RETURN(flush_batch());
      ON_ERR_RETURN(read_data_row(ref, c3db_shared_buffer + (rows_in_batch * payload_size_)));
      ++rows_in_batch;
    } else {
      uint32_t row_ref = C3DB_DATA_ROW_NULL;
      uint32_t crc = 0;
      ON_ERR_RETURN(get_row_ref(ref, row_ref, crc));
      ON_ERR_RETURN(export_data_row(row_ref, target_file));
    }

    ++rows_exported;
  }

  ON_ERR_RETURN(flush_batch());
  return target_file.flush();
}

c3db_err_t c3db_data_file_t::export_data_row(uint32_t row_ref, c3db_file_t &target_file) {
  if (row_ref == C3DB_DATA_ROW_NULL) return C3DB_REC_CORRUPT_ERR;

  size_t remaining = payload_size_;
  size_t offset = 0;

  /*
   * Export is also used by future defragmentation. Copying by chunks avoids
   * requiring a RAM buffer as large as one payload.
   */
  while (remaining > 0) {
    const size_t chunk_len = remaining < C3DB_SHARED_BUFFER_SIZE ? remaining : C3DB_SHARED_BUFFER_SIZE;
    ON_ERR_RETURN(dat_file_.read_rec(row_ref, offset, c3db_shared_buffer, chunk_len));
    ON_ERR_RETURN(target_file.write(c3db_shared_buffer, chunk_len));

    offset += chunk_len;
    remaining -= chunk_len;
  }

  return C3DB_OK;
}

size_t c3db_data_file_t::payload_size() const {
  return payload_size_;
}

void c3db_data_file_t::release_base_file_name() {
  delete[] base_file_name_;
  base_file_name_ = nullptr;
}

c3db_err_t c3db_data_file_t::delete_if_exists(const char* file_name) const {
  if (!c3db_file_t::exists(file_name)) return C3DB_OK;
  return c3db_file_t::delete_file(file_name);
}

c3db_err_t c3db_data_file_t::recover_defrag_files(const char* base_file_name) const {
  char* met_name = nullptr;
  char* dat_name = nullptr;
  char* met_dfg_name = nullptr;
  char* dat_dfg_name = nullptr;
  char* met_bak_name = nullptr;
  char* dat_bak_name = nullptr;

  c3db_err_t err = c3db_make_file_name(base_file_name, ".met", met_name);
  if (OK(err)) err = c3db_make_file_name(base_file_name, ".dat", dat_name);
  if (OK(err)) err = c3db_make_file_name(base_file_name, ".mdf", met_dfg_name);
  if (OK(err)) err = c3db_make_file_name(base_file_name, ".ddf", dat_dfg_name);
  if (OK(err)) err = c3db_make_file_name(base_file_name, ".mbk", met_bak_name);
  if (OK(err)) err = c3db_make_file_name(base_file_name, ".dbk", dat_bak_name);

  auto cleanup_names = [&]() {
    delete[] met_name;
    delete[] dat_name;
    delete[] met_dfg_name;
    delete[] dat_dfg_name;
    delete[] met_bak_name;
    delete[] dat_bak_name;
  };

  if (IS_ERR(err)) {
    cleanup_names();
    return err;
  }

  const bool met_exists = c3db_file_t::exists(met_name);
  const bool dat_exists = c3db_file_t::exists(dat_name);
  const bool met_bak_exists = c3db_file_t::exists(met_bak_name);
  const bool dat_bak_exists = c3db_file_t::exists(dat_bak_name);

  /*
   * Active pair wins when both files are present. Otherwise we restore the old
   * pair when a reset happened after one or both active files were renamed.
   */
  if (met_exists && dat_exists) {
    err = delete_if_exists(met_dfg_name);
    if (OK(err)) err = delete_if_exists(dat_dfg_name);
    if (OK(err)) err = delete_if_exists(met_bak_name);
    if (OK(err)) err = delete_if_exists(dat_bak_name);
  } else if (met_bak_exists && dat_bak_exists) {
    err = delete_if_exists(met_name);
    if (OK(err)) err = delete_if_exists(dat_name);
    if (OK(err)) err = c3db_file_t::rename_file(met_bak_name, met_name);
    if (OK(err)) err = c3db_file_t::rename_file(dat_bak_name, dat_name);
    if (OK(err)) err = delete_if_exists(met_dfg_name);
    if (OK(err)) err = delete_if_exists(dat_dfg_name);
  } else if (!met_exists && met_bak_exists && dat_exists) {
    err = c3db_file_t::rename_file(met_bak_name, met_name);
    if (OK(err)) err = delete_if_exists(met_dfg_name);
    if (OK(err)) err = delete_if_exists(dat_dfg_name);
  } else if (!dat_exists && dat_bak_exists && met_exists) {
    err = c3db_file_t::rename_file(dat_bak_name, dat_name);
    if (OK(err)) err = delete_if_exists(met_dfg_name);
    if (OK(err)) err = delete_if_exists(dat_dfg_name);
  } else {
    err = delete_if_exists(met_dfg_name);
    if (OK(err)) err = delete_if_exists(dat_dfg_name);
  }

  cleanup_names();
  return err;
}

uint32_t c3db_data_file_t::payload_crc(const uint8_t* data) const {
  if (!data) return 0;

  uint32_t crc = C3DB_CRC32_INIT;
  const size_t sample = C3DB_DATA_CRC_SAMPLE_SIZE;

  /*
   * C3DB data files trade full-payload validation for a bounded consistency
   * sample. The DBF metadata is already protected; sampling both payload edges
   * catches the expected interrupted-write cases without making CRC cost grow
   * with large records.
   */
  if (payload_size_ <= (2 * sample)) {
    crc = c3db_crc32_update(crc, data, payload_size_);
  } else {
    crc = c3db_crc32_update(crc, data, sample);
    crc = c3db_crc32_update(crc, data + payload_size_ - sample, sample);
  }

  crc = c3db_crc32_finish(crc);
  return crc == 0 ? 1 : crc;
}

c3db_err_t c3db_data_file_t::payload_crc(uint32_t row_ref, uint32_t &crc) {
  crc = 0;
  if (row_ref == C3DB_DATA_ROW_NULL) return C3DB_REC_CORRUPT_ERR;

  uint32_t running = C3DB_CRC32_INIT;
  const size_t sample = C3DB_DATA_CRC_SAMPLE_SIZE;

  if (payload_size_ <= (2 * sample)) {
    size_t remaining = payload_size_;
    size_t offset = 0;
    while (remaining > 0) {
      const size_t chunk_len = remaining < C3DB_SHARED_BUFFER_SIZE ? remaining : C3DB_SHARED_BUFFER_SIZE;
      ON_ERR_RETURN(dat_file_.read_rec(row_ref, offset, c3db_shared_buffer, chunk_len));
      running = c3db_crc32_update(running, c3db_shared_buffer, chunk_len);
      offset += chunk_len;
      remaining -= chunk_len;
    }
  } else {
    ON_ERR_RETURN(dat_file_.read_rec(row_ref, 0, c3db_shared_buffer, sample));
    running = c3db_crc32_update(running, c3db_shared_buffer, sample);

    ON_ERR_RETURN(dat_file_.read_rec(row_ref, payload_size_ - sample, c3db_shared_buffer, sample));
    running = c3db_crc32_update(running, c3db_shared_buffer, sample);
  }

  crc = c3db_crc32_finish(running);
  if (crc == 0) crc = 1;
  return C3DB_OK;
}

c3db_err_t c3db_data_file_t::write_data_row(uint32_t &row_ref, const uint8_t* data) {
  if (!data) return C3DB_INVALID_ARG_ERR;

  if (row_ref == C3DB_DATA_ROW_NULL) {
    size_t row = 0;
    ON_ERR_RETURN(dat_file_.append(data, row));
    if (row >= C3DB_DATA_ROW_NULL) return C3DB_FILE_SIZE_ERR;
    row_ref = static_cast<uint32_t>(row);
    return C3DB_OK;
  }

  return dat_file_.update(row_ref, data);
}

c3db_err_t c3db_data_file_t::read_data_row(const c3db_data_ref_t &ref, uint8_t* data) {
  if (!data) return C3DB_INVALID_ARG_ERR;

  auto read_and_validate = [&](uint32_t row_ref, uint32_t crc) -> c3db_err_t {
    if (crc == 0 || row_ref == C3DB_DATA_ROW_NULL) return C3DB_REC_CORRUPT_ERR;

    ON_ERR_RETURN(dat_file_.select(row_ref, data));
    return payload_crc(data) == crc ? C3DB_OK : C3DB_REC_CORRUPT_ERR;
  };

  c3db_err_t err = read_and_validate(ref.active, ref.active_crc);
  if (OK(err)) return C3DB_OK;

  /*
   * Reads follow DBF's recovery philosophy: return the newest valid value that
   * can be proven. Metadata is not repaired here because a read must not create
   * extra writes on microSD.
   */
  if (ref.spare_crc != 0) {
    c3db_err_t spare_err = read_and_validate(ref.spare, ref.spare_crc);
    if (OK(spare_err)) return C3DB_OK;
    if (IS_ERR(spare_err) && spare_err != C3DB_REC_CORRUPT_ERR) return spare_err;
  }

  return err;
}

c3db_err_t c3db_data_file_t::get_row_ref(
  const c3db_data_ref_t &ref,
  uint32_t &row_ref,
  uint32_t &crc
) {
  row_ref = C3DB_DATA_ROW_NULL;
  crc = 0;

  uint32_t calculated_crc = 0;
  c3db_err_t err = payload_crc(ref.active, calculated_crc);
  if (OK(err) && calculated_crc == ref.active_crc) {
    row_ref = ref.active;
    crc = ref.active_crc;
    return C3DB_OK;
  }

  /*
   * Defrag/export only need the valid row reference, not the payload itself.
   * The active row is preferred; spare is used only when active cannot be
   * proven by the stored CRC sample.
   */
  if (ref.spare_crc != 0) {
    calculated_crc = 0;
    err = payload_crc(ref.spare, calculated_crc);
    if (OK(err) && calculated_crc == ref.spare_crc) {
      row_ref = ref.spare;
      crc = ref.spare_crc;
      return C3DB_OK;
    }
  }

  return C3DB_REC_CORRUPT_ERR;
}
