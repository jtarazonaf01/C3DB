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

#include "c3db_index.h"

#include "c3db_defs.h"
#include "c3db_file.h"
#include "c3db_config.h"
#include "c3db_utils.h"

c3db_index_t::c3db_index_t()
  : idx_(sizeof(c3db_idx_header_t)),
    buckets_(),
    refs_(),
    level_(0),
    split_(0),
    bucket_count_(0),
    read_only_(false),
    bucket_buf_{} {
}

c3db_err_t c3db_index_t::create(const char* base_file_name) {
  if (is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;
  if (!base_file_name || base_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  c3db_err_t err = idx_.create(base_file_name, C3DB_INDEX_EXTENSION);
  if (OK(err)) err = buckets_.create(base_file_name);
  if (OK(err)) err = refs_.create(base_file_name);

  level_ = 0;
  split_ = 0;
  bucket_count_ = 1;
  read_only_ = false;

  if (OK(err)) {
    c3db_idx_header_t hdr = {
      .level = level_,
      .split = split_,
      .bucket_count = bucket_count_
    };

    c3db_id_t hdr_id = C3DB_NULL_ID;
    err = idx_.append(reinterpret_cast<const uint8_t*>(&hdr), hdr_id);
    if (OK(err) && hdr_id != 0) err = C3DB_FILE_CORRUPT_ERR;
  }

  if (IS_ERR(err)) {
    close_files();
    delete_index_files(base_file_name);
    reset_state();
    return err;
  }

  return C3DB_OK;
}

c3db_err_t c3db_index_t::begin(const char* base_file_name, bool read_only) {
  if (is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;
  if (!base_file_name || base_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  c3db_err_t err = idx_.begin(base_file_name, read_only, C3DB_INDEX_EXTENSION);
  if (OK(err) || IS_WNG(err)) {
    c3db_err_t buckets_err = buckets_.begin(base_file_name, read_only);
    if (IS_ERR(buckets_err)) err = buckets_err;
    else if (OK(err)) err = buckets_err;
  }
  if (OK(err) || IS_WNG(err)) {
    c3db_err_t refs_err = refs_.begin(base_file_name, read_only);
    if (IS_ERR(refs_err)) err = refs_err;
    else if (OK(err)) err = refs_err;
  }

  c3db_idx_header_t hdr = {};
  if (OK(err) || IS_WNG(err)) {
    c3db_err_t hdr_err = idx_.select(c3db_id(0, 0), reinterpret_cast<uint8_t*>(&hdr));
    if (IS_ERR(hdr_err)) err = hdr_err;
    else if (OK(err)) err = hdr_err;
  }

  if (OK(err) || IS_WNG(err)) {
    const uint64_t base_bucket_count = hdr.level < 32 ? (1ULL << hdr.level) : 0;
    if (base_bucket_count == 0 ||
        hdr.split >= base_bucket_count ||
        hdr.bucket_count != base_bucket_count + hdr.split) {
      err = C3DB_FILE_CORRUPT_ERR;
    }
    else {
      level_ = hdr.level;
      split_ = hdr.split;
      bucket_count_ = hdr.bucket_count;
      read_only_ = read_only;

      size_t physical_bucket_count = 0;
      c3db_err_t count_err = buckets_.get_canonical_count(physical_bucket_count);
      if (IS_ERR(count_err)) {
        err = count_err;
      }
      else if (physical_bucket_count == bucket_count_) {
        if (OK(err)) err = count_err;
      }
      else if (physical_bucket_count == static_cast<size_t>(bucket_count_) + 1) {
        err = recover_split();
      }
      else {
        err = C3DB_FILE_CORRUPT_ERR;
      }
    }
  }

  if (IS_ERR(err)) {
    close_files();
    reset_state();
    return err;
  }

  return err;
}

c3db_err_t c3db_index_t::end() {
  c3db_err_t err = close_files();
  reset_state();
  return err;
}

bool c3db_index_t::is_open() const {
  return idx_.is_open() && buckets_.is_open() && refs_.is_open();
}

bool c3db_index_t::is_read_only() const {
  return read_only_;
}

c3db_err_t c3db_index_t::index(uint64_t hash, c3db_id_t record_id) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (record_id == C3DB_NULL_ID) return C3DB_INVALID_ARG_ERR;

  c3db_idx_bkt_ref_t bkt_ref = {};
  c3db_id_t first_ref = C3DB_NULL_ID;
  c3db_err_t err = find_entry(hash, bkt_ref, first_ref);

  if (OK(err)) {
    c3db_id_t new_ref = C3DB_NULL_ID;
    ON_ERR_RETURN(refs_.create_new_ref(record_id, first_ref, new_ref));
    ON_ERR_RETURN(read_bucket(bkt_ref, bucket_buf_));
    bucket_buf_.entries[bkt_ref.pos].first_ref = new_ref;
    return write_bucket(bkt_ref, bucket_buf_);
  }
  if (err != C3DB_REC_NOT_FOUND_ERR) return err;

  c3db_id_t new_ref = C3DB_NULL_ID;
  ON_ERR_RETURN(refs_.create_new_ref(record_id, C3DB_NULL_ID, new_ref));

  c3db_bucket_entry_t entry = {
    .hash = hash,
    .first_ref = new_ref
  };

  if (bkt_ref.pos < C3DB_INDEX_BUCKET_CAPACITY) {
    ON_ERR_RETURN(read_bucket(bkt_ref, bucket_buf_));
    bucket_buf_.entries[bkt_ref.pos] = entry;
    ++bucket_buf_.count;
    ON_ERR_RETURN(write_bucket(bkt_ref, bucket_buf_));
  }
  else {
    c3db_bucket_t overflow = {};
    overflow.count = 1;
    overflow.next_overflow = C3DB_NULL_ID;
    overflow.entries[0] = entry;

    c3db_id_t overflow_id = C3DB_NULL_ID;
    if (bkt_ref.canonical) {
      ON_ERR_RETURN(buckets_.link_from_canonical_bkt(static_cast<uint32_t>(bkt_ref.bucket_id), overflow, overflow_id));
    }
    else {
      ON_ERR_RETURN(buckets_.link_from_overflow_bkt(bkt_ref.bucket_id, overflow, overflow_id));
    }
  }

  /*
   * The split policy is local to the canonical bucket. Overflow buckets are a
   * consequence of previous pressure, but they are not used as the alpha metric.
   */
  ON_ERR_RETURN(buckets_.read_canonical(get_canonical_bkt_num(hash), bucket_buf_));
  if (should_split(bucket_buf_)) {
    ON_ERR_RETURN(split(split_, bucket_count_));
  }
  return C3DB_OK;
}

c3db_err_t c3db_index_t::remove(uint64_t hash, c3db_id_t last_ref) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (last_ref == C3DB_NULL_ID) return C3DB_INVALID_ARG_ERR;

  c3db_idx_bkt_ref_t bkt_ref = {};
  c3db_id_t first_ref = C3DB_NULL_ID;
  c3db_err_t err = find_entry(hash, bkt_ref, first_ref);
  if (err == C3DB_REC_NOT_FOUND_ERR) return err;
  if (IS_ERR(err)) return err;

  ON_ERR_RETURN(read_bucket(bkt_ref, bucket_buf_));
  if (bkt_ref.pos >= bucket_buf_.count) return C3DB_FILE_CORRUPT_ERR;

  /*
   * The bucket entry is removed before freeing IRF nodes. A reset after this
   * point may leak the reference chain, but the index will not point to nodes
   * that can later be reused for another hash.
   */
  const uint8_t last_pos = static_cast<uint8_t>(bucket_buf_.count - 1);
  if (bkt_ref.pos != last_pos) {
    bucket_buf_.entries[bkt_ref.pos] = bucket_buf_.entries[last_pos];
  }
  --bucket_buf_.count;
  ON_ERR_RETURN(write_bucket(bkt_ref, bucket_buf_));

  c3db_err_t free_err = refs_.free_full_chain(first_ref, last_ref);
  if (IS_ERR(free_err)) return C3DB_ORPHANED_SPACE_WRN;
  return free_err;
}

c3db_err_t c3db_index_t::find(uint64_t hash, c3db_id_t &record_id, c3db_idx_cursor_t &cursor) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;

  /*
   * find_entry() returns the bucket position as well as the first IRF node.
   * Both are kept in the cursor so lazy cleanup can edit the same chain without
   * repeating the bucket lookup.
   */
  cursor = c3db_idx_cursor_t{};
  c3db_id_t first_ref = C3DB_NULL_ID;
  ON_ERR_RETURN(find_entry(hash, cursor.bkt_entry, first_ref));

  cursor.hash = hash;
  cursor.prev_node = C3DB_NULL_ID;
  cursor.node = first_ref;
  return get_current_ref(record_id, cursor);
}

c3db_err_t c3db_index_t::get_current_ref(c3db_id_t &record_id, c3db_idx_cursor_t &cursor) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (cursor.node == C3DB_NULL_ID) return C3DB_REC_NOT_FOUND_ERR;

  return read_ref_node(cursor.node, record_id, cursor.next_node);
}

c3db_err_t c3db_index_t::find_next(c3db_id_t &record_id, c3db_idx_cursor_t &cursor) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (cursor.next_node == C3DB_NULL_ID) return C3DB_REC_NOT_FOUND_ERR;

  cursor.prev_node = cursor.node;
  cursor.node = cursor.next_node;
  return get_current_ref(record_id, cursor);
}

c3db_err_t c3db_index_t::remove_current_ref(c3db_idx_cursor_t &cursor) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (cursor.node == C3DB_NULL_ID) return C3DB_INVALID_ARG_ERR;

  c3db_id_t next_ref = C3DB_NULL_ID;

  if (cursor.prev_node == C3DB_NULL_ID) {
    ON_ERR_RETURN(read_bucket(cursor.bkt_entry, bucket_buf_));
    if (cursor.bkt_entry.pos >= bucket_buf_.count) return C3DB_FILE_CORRUPT_ERR;

    /*
     * The bucket is the parent of the first IRF node. It is updated before
     * freeing the node, so a reset cannot leave the visible chain pointing to
     * a reference that may later be reused.
     */
    if (cursor.next_node == C3DB_NULL_ID) {
      const uint8_t last_pos = static_cast<uint8_t>(bucket_buf_.count - 1);
      if (cursor.bkt_entry.pos != last_pos) {
        bucket_buf_.entries[cursor.bkt_entry.pos] = bucket_buf_.entries[last_pos];
      }
      --bucket_buf_.count;
    }
    else {
      bucket_buf_.entries[cursor.bkt_entry.pos].first_ref = cursor.next_node;
    }

    ON_ERR_RETURN(write_bucket(cursor.bkt_entry, bucket_buf_));
    ON_ERR_RETURN(refs_.free_ref(cursor.node, next_ref));
  }
  else {
    /*
     * Non-head nodes can be detached only through their previous node. IRF
     * performs the relink and releases the removed node for later reuse.
     */
    ON_ERR_RETURN(refs_.free_ref(cursor.prev_node, cursor.node, next_ref));
  }

  cursor.node = next_ref;
  cursor.next_node = C3DB_NULL_ID;

  if (cursor.node != C3DB_NULL_ID) {
    c3db_id_t ignored_record_id = C3DB_NULL_ID;
    ON_ERR_RETURN(get_current_ref(ignored_record_id, cursor));
  }

  return C3DB_OK;
}

c3db_err_t c3db_index_t::read_ref_node(c3db_id_t ref_id, c3db_id_t &record_id, c3db_id_t &next_ref) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;

  c3db_ref_node_t node = {};
  ON_ERR_RETURN(refs_.read_ref(ref_id, node));

  record_id = node.record_id;
  next_ref = node.next_ref;
  return C3DB_OK;
}

c3db_err_t c3db_index_t::save_hdr() {
  c3db_idx_header_t hdr = {
    .level = level_,
    .split = split_,
    .bucket_count = bucket_count_
  };

  return idx_.update(c3db_id(0, 0), reinterpret_cast<const uint8_t*>(&hdr));
}

c3db_err_t c3db_index_t::read_bucket(const c3db_idx_bkt_ref_t &bkt_ref, c3db_bucket_t &bucket) {
  if (bkt_ref.canonical) {
    return buckets_.read_canonical(static_cast<uint32_t>(bkt_ref.bucket_id), bucket);
  }

  return buckets_.read_overflow_bkt(bkt_ref.bucket_id, bucket);
}

c3db_err_t c3db_index_t::write_bucket(const c3db_idx_bkt_ref_t &bkt_ref, const c3db_bucket_t &bucket) {
  if (bkt_ref.canonical) {
    return buckets_.write_canonical(static_cast<uint32_t>(bkt_ref.bucket_id), bucket);
  }

  return buckets_.write_overflow_bkt(bkt_ref.bucket_id, bucket);
}

uint32_t c3db_index_t::get_canonical_bkt_num(uint64_t hash) const {
  if (level_ >= 31) return 0;
  const uint32_t base_count = 1u << level_;
  uint32_t bucket_num = static_cast<uint32_t>(hash % base_count);
  if (bucket_num < split_) bucket_num = static_cast<uint32_t>(hash % (base_count << 1));
  return bucket_num;
}

uint32_t c3db_index_t::get_split_bkt_num(uint64_t hash) const {
  if (level_ >= 31) return 0;
  return static_cast<uint32_t>(hash % (1u << (level_ + 1)));
}

bool c3db_index_t::should_split(const c3db_bucket_t &canonical_bucket) const {
  return static_cast<uint64_t>(canonical_bucket.count) * 100 >=
         static_cast<uint64_t>(C3DB_INDEX_BUCKET_CAPACITY) * C3DB_INDEX_MAX_LOAD_PERCENT;
}

uint8_t c3db_index_t::get_tail_bkt_count(uint32_t entry_count) const {
  if (entry_count == 0) return 0;

  const uint8_t tail_count = static_cast<uint8_t>(entry_count % C3DB_INDEX_BUCKET_CAPACITY);
  return tail_count == 0 ? C3DB_INDEX_BUCKET_CAPACITY : tail_count;
}

c3db_err_t c3db_index_t::persist_overflow_bkt_split(
  c3db_bucket_t &bucket,
  c3db_id_t &last_bkt_id
) {
  /*
   * Split reconstruction links overflow buckets in reverse build order. Entry
   * order is not significant for hash lookup, and this avoids predicting ids or
   * rewriting already persisted overflow buckets.
   */
  bucket.next_overflow = last_bkt_id;

  c3db_id_t bucket_id = C3DB_NULL_ID;
  ON_ERR_RETURN(buckets_.create_overflow_bkt(bucket, bucket_id));

  last_bkt_id = bucket_id;
  bucket.count = 0;
  bucket.depth = 0;
  bucket.next_overflow = C3DB_NULL_ID;
  return C3DB_OK;
}

c3db_err_t c3db_index_t::count_split_entries(
  uint32_t old_bucket_num,
  uint32_t new_bucket_num,
  uint32_t &old_count,
  uint32_t &new_count
) {
  old_count = 0;
  new_count = 0;

  c3db_idx_bkt_ref_t bkt_ref = {
    .canonical = true,
    .bucket_id = old_bucket_num,
    .pos = 0
  };

  while (true) {
    ON_ERR_RETURN(read_bucket(bkt_ref, bucket_buf_));

    for (uint8_t pos = 0; pos < bucket_buf_.count; ++pos) {
      const uint32_t dst_bucket_num = get_split_bkt_num(bucket_buf_.entries[pos].hash);
      if (dst_bucket_num == old_bucket_num) {
        ++old_count;
      }
      else if (dst_bucket_num == new_bucket_num) {
        ++new_count;
      }
      else {
        return C3DB_FILE_CORRUPT_ERR;
      }
    }

    if (bucket_buf_.next_overflow == C3DB_NULL_ID) return C3DB_OK;

    bkt_ref.canonical = false;
    bkt_ref.bucket_id = bucket_buf_.next_overflow;
  }
}

c3db_err_t c3db_index_t::split(uint32_t old_bucket_num, uint32_t new_bucket_num) {
  static_assert(
    C3DB_SHARED_BUFFER_SIZE >= (2 * C3DB_INDEX_BUCKET_SIZE),
    "C3DB split needs shared buffer space for two index buckets"
  );

  ON_ERR_RETURN(buckets_.read_canonical(old_bucket_num, bucket_buf_));
  if (bucket_buf_.depth != level_) return C3DB_FILE_CORRUPT_ERR;
  const c3db_id_t old_first_overflow = bucket_buf_.next_overflow;

  uint32_t old_count = 0;
  uint32_t new_count = 0;
  ON_ERR_RETURN(count_split_entries(old_bucket_num, new_bucket_num, old_count, new_count));

  /*
   * Split needs two output buckets while bucket_buf_ keeps reading the old
   * chain. They live in the shared buffer to keep the task stack bounded during
   * worst-case overflow reconstruction.
   */
  c3db_bucket_t* old_out = reinterpret_cast<c3db_bucket_t*>(c3db_shared_buffer);
  c3db_bucket_t* new_out = reinterpret_cast<c3db_bucket_t*>(c3db_shared_buffer + C3DB_INDEX_BUCKET_SIZE);
  old_out->count = 0;
  old_out->depth = 0;
  old_out->next_overflow = C3DB_NULL_ID;
  new_out->count = 0;
  new_out->depth = 0;
  new_out->next_overflow = C3DB_NULL_ID;

  c3db_id_t old_last_bkt_id = C3DB_NULL_ID;
  c3db_id_t new_last_bkt_id = C3DB_NULL_ID;

  uint32_t old_remaining = old_count;
  uint32_t new_remaining = new_count;
  uint8_t old_target_count = get_tail_bkt_count(old_count);
  uint8_t new_target_count = get_tail_bkt_count(new_count);

  c3db_idx_bkt_ref_t bkt_ref = {
    .canonical = true,
    .bucket_id = old_bucket_num,
    .pos = 0
  };

  while (true) {
    ON_ERR_RETURN(read_bucket(bkt_ref, bucket_buf_));

    for (uint8_t pos = 0; pos < bucket_buf_.count; ++pos) {
      const c3db_bucket_entry_t entry = bucket_buf_.entries[pos];
      const uint32_t dst_bucket_num = get_split_bkt_num(entry.hash);

      if (dst_bucket_num == old_bucket_num) {
        old_out->entries[old_out->count] = entry;
        ++old_out->count;
        --old_remaining;

        if (old_out->count == old_target_count && old_remaining > 0) {
          ON_ERR_RETURN(persist_overflow_bkt_split(*old_out, old_last_bkt_id));
          old_target_count = old_remaining < C3DB_INDEX_BUCKET_CAPACITY ?
            static_cast<uint8_t>(old_remaining) :
            static_cast<uint8_t>(C3DB_INDEX_BUCKET_CAPACITY);
        }
      }
      else if (dst_bucket_num == new_bucket_num) {
        new_out->entries[new_out->count] = entry;
        ++new_out->count;
        --new_remaining;

        if (new_out->count == new_target_count && new_remaining > 0) {
          ON_ERR_RETURN(persist_overflow_bkt_split(*new_out, new_last_bkt_id));
          new_target_count = new_remaining < C3DB_INDEX_BUCKET_CAPACITY ?
            static_cast<uint8_t>(new_remaining) :
            static_cast<uint8_t>(C3DB_INDEX_BUCKET_CAPACITY);
        }
      }
      else {
        return C3DB_FILE_CORRUPT_ERR;
      }
    }

    if (bucket_buf_.next_overflow == C3DB_NULL_ID) break;

    bkt_ref.canonical = false;
    bkt_ref.bucket_id = bucket_buf_.next_overflow;
  }

  old_out->next_overflow = old_last_bkt_id;
  new_out->next_overflow = new_last_bkt_id;
  old_out->depth = level_ + 1;
  new_out->depth = level_ + 1;

  /*
   * The new canonical is written before the old one. Until the IDX header is
   * updated, bucket_count_ still points to the pre-split state and recovery can
   * detect that there is exactly one uncommitted canonical bucket in ICB.
   */
  size_t canonical_count = 0;
  ON_ERR_RETURN(buckets_.get_canonical_count(canonical_count));
  if (canonical_count == new_bucket_num) {
    c3db_id_t new_bucket_id = C3DB_NULL_ID;
    ON_ERR_RETURN(buckets_.append_canonical_bucket(*new_out, new_bucket_id));
    if (new_bucket_id != new_bucket_num) return C3DB_FILE_CORRUPT_ERR;
  }
  else if (canonical_count == static_cast<size_t>(new_bucket_num) + 1) {
    ON_ERR_RETURN(buckets_.write_canonical(new_bucket_num, *new_out));
  }
  else {
    return C3DB_FILE_CORRUPT_ERR;
  }

  ON_ERR_RETURN(buckets_.write_canonical(old_bucket_num, *old_out));

  ++bucket_count_;
  ++split_;

  const uint32_t base_bucket_count = 1u << level_;
  if (split_ == base_bucket_count) {
    split_ = 0;
    ++level_;
  }

  ON_ERR_RETURN(save_hdr());

  c3db_err_t free_err = buckets_.free_overflow_bkt_chain(old_first_overflow);
  if (IS_ERR(free_err)) return C3DB_ORPHANED_SPACE_WRN;
  return free_err;
}

c3db_err_t c3db_index_t::recover_split() {
  if (read_only_) return C3DB_READ_ONLY_ERR;

  const uint32_t old_bucket_num = split_;
  const uint32_t new_bucket_num = bucket_count_;

  c3db_bucket_t old_bucket = {};
  ON_ERR_RETURN(buckets_.read_canonical(old_bucket_num, old_bucket));

  if (old_bucket.depth == level_) {
    /*
     * The reset happened before the old canonical bucket was rewritten. The
     * original chain is still reachable, so the normal split can be replayed
     * and will update the already-created new canonical bucket if it exists.
     */
    return split(old_bucket_num, new_bucket_num);
  }

  if (old_bucket.depth != level_ + 1) return C3DB_FILE_CORRUPT_ERR;

  c3db_bucket_t new_bucket = {};
  ON_ERR_RETURN(buckets_.read_canonical(new_bucket_num, new_bucket));
  if (new_bucket.depth != level_ + 1) return C3DB_FILE_CORRUPT_ERR;

  /*
   * Both canonical buckets were already rewritten before the reset. At this
   * point recovery only publishes the IDX header. Old overflow buckets may
   * remain orphaned because the old canonical no longer points to them.
   */
  ++bucket_count_;
  ++split_;

  const uint32_t base_bucket_count = 1u << level_;
  if (split_ == base_bucket_count) {
    split_ = 0;
    ++level_;
  }

  return save_hdr();
}

c3db_err_t c3db_index_t::find_entry(uint64_t hash, c3db_idx_bkt_ref_t &bkt_ref, c3db_id_t &first_ref) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;

  /*
   * The helper returns a physical bucket reference instead of the bucket data.
   * c3db_db_file_t keeps the last record read in its internal cache, so a later
   * read_bucket()/write_bucket() for the same ICB/IOB record normally avoids a
   * second disk read while keeping buffer ownership explicit in the caller.
   */
  bkt_ref.canonical = true;
  bkt_ref.bucket_id = get_canonical_bkt_num(hash);
  bkt_ref.pos = C3DB_INDEX_BUCKET_CAPACITY;
  first_ref = C3DB_NULL_ID;

  while (true) {
    ON_ERR_RETURN(read_bucket(bkt_ref, bucket_buf_));

    for (uint8_t pos = 0; pos < bucket_buf_.count; ++pos) {
      if (bucket_buf_.entries[pos].hash == hash) {
        bkt_ref.pos = pos;
        first_ref = bucket_buf_.entries[pos].first_ref;
        return C3DB_OK;
      }
    }

    if (bucket_buf_.count < C3DB_INDEX_BUCKET_CAPACITY) {
      bkt_ref.pos = static_cast<uint8_t>(bucket_buf_.count);
      return C3DB_REC_NOT_FOUND_ERR;
    }

    if (bucket_buf_.next_overflow == C3DB_NULL_ID) {
      bkt_ref.pos = C3DB_INDEX_BUCKET_CAPACITY;
      return C3DB_REC_NOT_FOUND_ERR;
    }

    bkt_ref.canonical = false;
    bkt_ref.bucket_id = bucket_buf_.next_overflow;
    bkt_ref.pos = C3DB_INDEX_BUCKET_CAPACITY;
  }
}

c3db_err_t c3db_index_t::close_files() {
  c3db_err_t idx_err = idx_.end();
  c3db_err_t buckets_err = buckets_.end();
  c3db_err_t refs_err = refs_.end();

  if (IS_ERR(idx_err)) return idx_err;
  if (IS_ERR(buckets_err)) return buckets_err;
  if (IS_ERR(refs_err)) return refs_err;
  if (KO(idx_err)) return idx_err;
  if (KO(buckets_err)) return buckets_err;
  return refs_err;
}

void c3db_index_t::reset_state() {
  level_ = 0;
  split_ = 0;
  bucket_count_ = 0;
  read_only_ = false;
  bucket_buf_.count = 0;
  bucket_buf_.depth = 0;
  bucket_buf_.next_overflow = C3DB_NULL_ID;
}

c3db_err_t c3db_index_t::delete_index_files(const char* base_file_name) {
  char* idx_name = nullptr;
  char* icb_name = nullptr;
  char* iob_name = nullptr;
  char* irf_name = nullptr;

  c3db_err_t err = c3db_make_file_name(base_file_name, C3DB_INDEX_EXTENSION, idx_name);
  if (OK(err)) err = c3db_make_file_name(base_file_name, C3DB_INDEX_CANONICAL_BUCKET_EXTENSION, icb_name);
  if (OK(err)) err = c3db_make_file_name(base_file_name, C3DB_INDEX_OVERFLOW_BUCKET_EXTENSION, iob_name);
  if (OK(err)) err = c3db_make_file_name(base_file_name, C3DB_INDEX_REF_EXTENSION, irf_name);

  if (OK(err) && c3db_file_t::exists(idx_name)) err = c3db_file_t::delete_file(idx_name);
  if (OK(err) && c3db_file_t::exists(icb_name)) err = c3db_file_t::delete_file(icb_name);
  if (OK(err) && c3db_file_t::exists(iob_name)) err = c3db_file_t::delete_file(iob_name);
  if (OK(err) && c3db_file_t::exists(irf_name)) err = c3db_file_t::delete_file(irf_name);

  delete[] idx_name;
  delete[] icb_name;
  delete[] iob_name;
  delete[] irf_name;
  return err;
}
