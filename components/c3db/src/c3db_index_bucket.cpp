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

#include "c3db_index_bucket.h"

#include "c3db_config.h"
#include "c3db_defs.h"
#include "c3db_file.h"
#include "c3db_utils.h"

c3db_index_bucket_t::c3db_index_bucket_t()
  : icb_(sizeof(c3db_bucket_t)),
    iob_(sizeof(c3db_bucket_t)),
    read_only_(false) {
}

c3db_err_t c3db_index_bucket_t::create(const char* base_file_name) {
  if (is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;
  if (!base_file_name || base_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  c3db_err_t err = icb_.create(base_file_name, C3DB_INDEX_CANONICAL_BUCKET_EXTENSION);
  if (OK(err)) err = iob_.create(base_file_name, C3DB_INDEX_OVERFLOW_BUCKET_EXTENSION);

  c3db_id_t bucket_id = C3DB_NULL_ID;
  if (OK(err)) {
    /*
     * Linear Hashing needs at least bucket 0: even before growth, every hash
     * must resolve to a valid canonical bucket.
     */
    err = append_canonical_bucket(bucket_id);
    if (OK(err) && bucket_id != 0) err = C3DB_FILE_CORRUPT_ERR;
  }

  if (IS_ERR(err)) {
    close_files();
    delete_index_files(base_file_name);
    return err;
  }

  read_only_ = false;
  return C3DB_OK;
}

c3db_err_t c3db_index_bucket_t::begin(const char* base_file_name, bool read_only) {
  if (is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;
  if (!base_file_name || base_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  c3db_err_t err = icb_.begin(base_file_name, read_only, C3DB_INDEX_CANONICAL_BUCKET_EXTENSION);
  if (OK(err) || IS_WNG(err)) {
    c3db_err_t iob_err = iob_.begin(base_file_name, read_only, C3DB_INDEX_OVERFLOW_BUCKET_EXTENSION);
    if (IS_ERR(iob_err)) err = iob_err;
    else if (OK(err)) err = iob_err;
  }

  size_t canonical_count = 0;
  if (OK(err) || IS_WNG(err)) {
    c3db_err_t count_err = icb_.max_rec_count(canonical_count);
    if (IS_ERR(count_err)) err = count_err;
    else if (OK(err)) err = count_err;
  }

  if ((OK(err) || IS_WNG(err)) && canonical_count == 0) {
    err = C3DB_FILE_CORRUPT_ERR;
  }

  if (IS_ERR(err)) {
    close_files();
    return err;
  }

  read_only_ = read_only;
  return err;
}

c3db_err_t c3db_index_bucket_t::end() {
  c3db_err_t err = close_files();
  read_only_ = false;
  return err;
}

c3db_err_t c3db_index_bucket_t::append_canonical_bucket(c3db_id_t &bucket_id) {
  c3db_bucket_t* bucket = reinterpret_cast<c3db_bucket_t*>(c3db_shared_buffer);
  init_empty_bucket(*bucket);
  return append_canonical_bucket(*bucket, bucket_id);
}

c3db_err_t c3db_index_bucket_t::append_canonical_bucket(
  const c3db_bucket_t &bucket,
  c3db_id_t &bucket_id
) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (bucket.count > C3DB_INDEX_BUCKET_CAPACITY) return C3DB_FILE_CORRUPT_ERR;

  /*
   * Canonical buckets are never reused. Their row number is part of the Linear
   * Hashing address space, so appending is required here instead of insert().
  */
  ON_ERR_RETURN(icb_.append(reinterpret_cast<const uint8_t*>(&bucket), bucket_id));
  /*
   * ICB addresses canonical buckets by row number only. A non-zero cycle would
   * break read_canonical()/write_canonical(), which rebuild ids from bucket_num.
   */
  if ((bucket_id >> 32) != 0) return C3DB_FILE_CORRUPT_ERR;
  return C3DB_OK;
}

c3db_err_t c3db_index_bucket_t::read_canonical(uint32_t bucket_num, c3db_bucket_t &bucket) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;

  /*
   * Canonical buckets are addressed by stable row number. Cycle is always zero
   * because ICB records are not deleted or reused.
   */
  const c3db_id_t bucket_id = static_cast<c3db_id_t>(bucket_num);
  ON_ERR_RETURN(icb_.select(bucket_id, reinterpret_cast<uint8_t*>(&bucket)));
  if (bucket.count > C3DB_INDEX_BUCKET_CAPACITY) return C3DB_FILE_CORRUPT_ERR;
  return C3DB_OK;
}

c3db_err_t c3db_index_bucket_t::write_canonical(uint32_t bucket_num, const c3db_bucket_t &bucket) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (bucket.count > C3DB_INDEX_BUCKET_CAPACITY) return C3DB_FILE_CORRUPT_ERR;

  const c3db_id_t bucket_id = static_cast<c3db_id_t>(bucket_num);
  return icb_.update(bucket_id, reinterpret_cast<const uint8_t*>(&bucket));
}

c3db_err_t c3db_index_bucket_t::create_overflow_bkt(
  const c3db_bucket_t &bucket,
  c3db_id_t &bucket_id
) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (bucket.count > C3DB_INDEX_BUCKET_CAPACITY) return C3DB_FILE_CORRUPT_ERR;

  /*
   * Overflow buckets can be released after splits. insert() lets IOB reuse
   * DBF free-list slots before growing the file.
   */
  return iob_.insert(reinterpret_cast<const uint8_t*>(&bucket), bucket_id);
}

c3db_err_t c3db_index_bucket_t::read_overflow_bkt(c3db_id_t bucket_id, c3db_bucket_t &bucket) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (bucket_id == C3DB_NULL_ID) return C3DB_INVALID_ARG_ERR;

  ON_ERR_RETURN(iob_.select(bucket_id, reinterpret_cast<uint8_t*>(&bucket)));
  if (bucket.count > C3DB_INDEX_BUCKET_CAPACITY) return C3DB_FILE_CORRUPT_ERR;
  return C3DB_OK;
}

c3db_err_t c3db_index_bucket_t::write_overflow_bkt(
  c3db_id_t bucket_id,
  const c3db_bucket_t &bucket
) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (bucket_id == C3DB_NULL_ID) return C3DB_INVALID_ARG_ERR;
  if (bucket.count > C3DB_INDEX_BUCKET_CAPACITY) return C3DB_FILE_CORRUPT_ERR;

  return iob_.update(bucket_id, reinterpret_cast<const uint8_t*>(&bucket));
}

c3db_err_t c3db_index_bucket_t::link_from_canonical_bkt(
  uint32_t canonical_bucket_num,
  const c3db_bucket_t &bucket,
  c3db_id_t &bucket_id
) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (bucket.count > C3DB_INDEX_BUCKET_CAPACITY) return C3DB_FILE_CORRUPT_ERR;
  if (bucket.next_overflow != C3DB_NULL_ID) return C3DB_INVALID_ARG_ERR;

  c3db_bucket_t tail = {};
  ON_ERR_RETURN(read_canonical(canonical_bucket_num, tail));
  if (tail.next_overflow != C3DB_NULL_ID) return C3DB_INVALID_ARG_ERR;

  c3db_err_t err = create_overflow_bkt(bucket, bucket_id);
  if (IS_ERR(err)) return err;
  tail.next_overflow = bucket_id;

  /*
   * Linking is a two-step operation: the new IOB record must exist before the
   * previous bucket can point to it. If the second write fails, try to release
   * the newly created bucket so the file does not keep an unreachable overflow.
   */
  c3db_err_t link_err = write_canonical(canonical_bucket_num, tail);
  if (IS_ERR(link_err)) {
    iob_.remove(bucket_id);
    bucket_id = C3DB_NULL_ID;
    return link_err;
  }
  return err;
}

c3db_err_t c3db_index_bucket_t::link_from_overflow_bkt(
  c3db_id_t tail_bucket_id,
  const c3db_bucket_t &bucket,
  c3db_id_t &bucket_id
) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (tail_bucket_id == C3DB_NULL_ID) return C3DB_INVALID_ARG_ERR;
  if (bucket.count > C3DB_INDEX_BUCKET_CAPACITY) return C3DB_FILE_CORRUPT_ERR;
  if (bucket.next_overflow != C3DB_NULL_ID) return C3DB_INVALID_ARG_ERR;

  c3db_bucket_t tail = {};
  ON_ERR_RETURN(read_overflow_bkt(tail_bucket_id, tail));
  if (tail.next_overflow != C3DB_NULL_ID) return C3DB_INVALID_ARG_ERR;

  c3db_err_t err = create_overflow_bkt(bucket, bucket_id);
  if (IS_ERR(err)) return err;
  tail.next_overflow = bucket_id;

  c3db_err_t link_err = write_overflow_bkt(tail_bucket_id, tail);
  if (IS_ERR(link_err)) {
    iob_.remove(bucket_id);
    bucket_id = C3DB_NULL_ID;
    return link_err;
  }
  return err;
}

c3db_err_t c3db_index_bucket_t::free_overflow_bkt_chain(c3db_id_t first_bucket_id) {
  if (first_bucket_id == C3DB_NULL_ID) return C3DB_OK;
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;

  c3db_id_t current_id = first_bucket_id;
  while (current_id != C3DB_NULL_ID) {
    c3db_bucket_t* bucket = reinterpret_cast<c3db_bucket_t*>(c3db_shared_buffer);
    ON_ERR_RETURN(read_overflow_bkt(current_id, *bucket));

    /*
     * next_overflow must be read before remove(): after removal, the full id
     * can become stale if DBF later reuses the physical row.
     */
    const c3db_id_t next_id = bucket->next_overflow;
    ON_ERR_RETURN(iob_.remove(current_id));
    current_id = next_id;
  }

  return C3DB_OK;
}

c3db_err_t c3db_index_bucket_t::get_canonical_count(size_t &count) const {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  return icb_.max_rec_count(count);
}

bool c3db_index_bucket_t::is_open() const {
  return icb_.is_open() && iob_.is_open();
}

bool c3db_index_bucket_t::is_read_only() const {
  return read_only_;
}

void c3db_index_bucket_t::init_empty_bucket(c3db_bucket_t &bucket) const {
  bucket = c3db_bucket_t{};
}

c3db_err_t c3db_index_bucket_t::close_files() {
  c3db_err_t icb_err = icb_.end();
  c3db_err_t iob_err = iob_.end();

  if (IS_ERR(icb_err)) return icb_err;
  if (IS_ERR(iob_err)) return iob_err;
  return KO(icb_err) ? icb_err : iob_err;
}

c3db_err_t c3db_index_bucket_t::delete_index_files(const char* base_file_name) const {
  char* icb_name = nullptr;
  char* iob_name = nullptr;

  c3db_err_t err = c3db_make_file_name(base_file_name, C3DB_INDEX_CANONICAL_BUCKET_EXTENSION, icb_name);
  if (OK(err)) err = c3db_make_file_name(base_file_name, C3DB_INDEX_OVERFLOW_BUCKET_EXTENSION, iob_name);

  if (OK(err) && c3db_file_t::exists(icb_name)) err = c3db_file_t::delete_file(icb_name);
  if (OK(err) && c3db_file_t::exists(iob_name)) err = c3db_file_t::delete_file(iob_name);

  delete[] icb_name;
  delete[] iob_name;
  return err;
}
