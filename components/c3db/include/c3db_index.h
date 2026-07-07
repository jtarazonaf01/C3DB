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

#include "c3db_db_file.h"
#include "c3db_index_bucket.h"
#include "c3db_index_ref.h"
#include "c3db_types.h"

//! File extension used for high-level index metadata.
static constexpr const char* C3DB_INDEX_EXTENSION = ".idx";

//! Physical size of one index metadata record.
static constexpr size_t C3DB_INDEX_HEADER_SIZE = sizeof(uint32_t) * 3;

//! Local bucket load threshold that triggers an incremental Linear Hashing split.
static constexpr uint32_t C3DB_INDEX_MAX_LOAD_PERCENT = 90;

/**
 * Persistent metadata stored in record 0 of the IDX file.
 *
 * bucket_count is the commit marker for split recovery: if ICB contains exactly
 * one more canonical bucket than this value, begin() must complete the pending
 * split before the index can be used.
 */
struct __attribute__((packed)) c3db_idx_header_t
{
  uint32_t level = 0;
  uint32_t split = 0;
  uint32_t bucket_count = 0;
};

static_assert(sizeof(c3db_idx_header_t) == C3DB_INDEX_HEADER_SIZE, "Invalid C3DB index header size");

/**
 * Physical location of an index entry inside ICB or IOB.
 */
struct c3db_idx_bkt_ref_t
{
  bool canonical = true;
  c3db_id_t bucket_id = C3DB_NULL_ID;
  uint8_t pos = C3DB_INDEX_BUCKET_CAPACITY;
};

/**
 * Cursor used to continue or clean a reference-list traversal.
 *
 * The cursor deliberately stores the bucket entry and neighbouring IRF nodes.
 * This lets higher layers skip or remove stale references without repeating
 * the hash lookup that found the chain.
 */
struct c3db_idx_cursor_t
{
  //! Hash associated with the current indexed search.
  uint64_t hash = 0;
  //! Physical bucket entry containing the head of the reference chain.
  c3db_idx_bkt_ref_t bkt_entry = {};
  //! Previous IRF node, or C3DB_NULL_ID when node is the chain head.
  c3db_id_t prev_node = C3DB_NULL_ID;
  //! Current IRF node.
  c3db_id_t node = C3DB_NULL_ID;
  //! Next IRF node.
  c3db_id_t next_node = C3DB_NULL_ID;
};

/**
 * High-level persistent hash index based on Linear Hashing.
 */
class c3db_index_t
{
public:
  //! Creates an unopened high-level index manager.
  c3db_index_t();

  //! Creates IDX/ICB/IOB/IRF files and initializes Linear Hashing state.
  c3db_err_t create(const char* base_file_name);

  //! Opens an existing high-level index.
  c3db_err_t begin(const char* base_file_name, bool read_only = false);

  //! Closes all index files and clears transient state.
  c3db_err_t end();

  //! Returns true when all index files are open.
  bool is_open() const;

  //! Returns true when the index is opened read-only.
  bool is_read_only() const;

  //! Adds a record id to the reference list associated with a hash.
  c3db_err_t index(uint64_t hash, c3db_id_t record_id);

  //! Removes the index entry for a hash and releases its IRF chain.
  //!
  //! Data records are not removed here: the caller is expected to traverse the
  //! chain, remove the indexed records it owns, and pass the last IRF node id.
  c3db_err_t remove(uint64_t hash, c3db_id_t last_ref);

  //! Finds the first record id associated with a hash and initializes a cursor.
  c3db_err_t find(uint64_t hash, c3db_id_t &record_id, c3db_idx_cursor_t &cursor);

  //! Reads the record id referenced by the current cursor node.
  c3db_err_t get_current_ref(c3db_id_t &record_id, c3db_idx_cursor_t &cursor);

  //! Advances an initialized cursor and returns the next record id.
  c3db_err_t find_next(c3db_id_t &record_id, c3db_idx_cursor_t &cursor);

  //! Removes the current cursor node and advances cursor.node to next_node.
  c3db_err_t remove_current_ref(c3db_idx_cursor_t &cursor);

  //! Deletes all physical files associated with one index base name.
  static c3db_err_t delete_index_files(const char* base_file_name);

private:
  c3db_err_t read_ref_node(c3db_id_t ref_id, c3db_id_t &record_id, c3db_id_t &next_ref);
  c3db_err_t save_hdr();
  c3db_err_t read_bucket(const c3db_idx_bkt_ref_t &bkt_ref, c3db_bucket_t &bucket);
  c3db_err_t write_bucket(const c3db_idx_bkt_ref_t &bkt_ref, const c3db_bucket_t &bucket);
  uint32_t get_canonical_bkt_num(uint64_t hash) const;
  uint32_t get_split_bkt_num(uint64_t hash) const;
  bool should_split(const c3db_bucket_t &canonical_bucket) const;
  uint8_t get_tail_bkt_count(uint32_t entry_count) const;
  c3db_err_t persist_overflow_bkt_split(c3db_bucket_t &bucket, c3db_id_t &last_bkt_id);
  c3db_err_t count_split_entries(
    uint32_t old_bucket_num,
    uint32_t new_bucket_num,
    uint32_t &old_count,
    uint32_t &new_count
  );
  c3db_err_t split(uint32_t old_bucket_num, uint32_t new_bucket_num);
  c3db_err_t recover_split();
  c3db_err_t find_entry(uint64_t hash, c3db_idx_bkt_ref_t &bkt_ref, c3db_id_t &first_ref);
  c3db_err_t close_files();
  void reset_state();

  c3db_db_file_t idx_;
  c3db_index_bucket_t buckets_;
  c3db_index_ref_t refs_;

  uint32_t level_;
  uint32_t split_;
  uint32_t bucket_count_;
  bool read_only_;

  c3db_bucket_t bucket_buf_;
};
