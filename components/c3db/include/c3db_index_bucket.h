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
#include "c3db_types.h"

//! Physical size of one index bucket record.
static constexpr size_t C3DB_INDEX_BUCKET_SIZE = 256;

//! File extension used for index canonical buckets.
static constexpr const char* C3DB_INDEX_CANONICAL_BUCKET_EXTENSION = ".icb";

//! File extension used for index overflow buckets.
static constexpr const char* C3DB_INDEX_OVERFLOW_BUCKET_EXTENSION = ".iob";

/**
 * Persistent entry stored inside one index bucket.
 *
 * Only the 64-bit hash is stored as key material. Collisions are therefore a
 * property accepted by the index design, not resolved by storing original keys.
 */
struct __attribute__((packed)) c3db_bucket_entry_t
{
  uint64_t hash = 0;
  c3db_id_t first_ref = C3DB_NULL_ID;
};

//! Physical metadata stored before bucket entries.
static constexpr size_t C3DB_INDEX_BUCKET_HEADER_SIZE =
  sizeof(uint32_t) + // count
  sizeof(uint32_t) + // depth
  sizeof(c3db_id_t); // next_overflow

//! Maximum number of entries that fit in one fixed-size bucket.
static constexpr size_t C3DB_INDEX_BUCKET_CAPACITY =
  (C3DB_INDEX_BUCKET_SIZE - C3DB_INDEX_BUCKET_HEADER_SIZE) /
  sizeof(c3db_bucket_entry_t);

/**
 * Persistent Linear Hashing bucket.
 *
 * A bucket is the physical record stored in ICB or IOB. ICB buckets are
 * addressed by row number and never deleted; IOB buckets are addressed by full
 * c3db_id_t because they can be reused.
 */
struct __attribute__((packed)) c3db_bucket_t
{
  uint32_t count = 0;
  //! Effective Linear Hashing level for canonical buckets; ignored in overflows.
  uint32_t depth = 0;
  c3db_id_t next_overflow = C3DB_NULL_ID;
  c3db_bucket_entry_t entries[C3DB_INDEX_BUCKET_CAPACITY] = {};
};

static_assert(sizeof(c3db_bucket_t) == C3DB_INDEX_BUCKET_SIZE, "Invalid C3DB index bucket size");

/**
 * Persistent storage manager for canonical and overflow index buckets.
 */
class c3db_index_bucket_t
{
public:
  //! Creates an unopened bucket manager.
  c3db_index_bucket_t();

  //! Creates ICB/IOB files and initializes canonical bucket 0.
  c3db_err_t create(const char* base_file_name);

  //! Opens existing ICB/IOB files.
  c3db_err_t begin(const char* base_file_name, bool read_only = false);

  //! Closes ICB/IOB files.
  c3db_err_t end();

  //! Appends a new canonical bucket at the end of ICB.
  c3db_err_t append_canonical_bucket(const c3db_bucket_t &bucket, c3db_id_t &bucket_id);

  //! Appends an empty canonical bucket at the end of ICB.
  c3db_err_t append_canonical_bucket(c3db_id_t &bucket_id);

  //! Reads a canonical bucket by bucket number.
  c3db_err_t read_canonical(uint32_t bucket_num, c3db_bucket_t &bucket);

  //! Writes an existing canonical bucket by bucket number.
  c3db_err_t write_canonical(uint32_t bucket_num, const c3db_bucket_t &bucket);

  //! Creates or reuses an overflow bucket and stores bucket content in it.
  //!
  //! IOB uses DBF insert semantics, so buckets released by free_overflow_bkt_chain()
  //! can be reused before the overflow file grows.
  c3db_err_t create_overflow_bkt(const c3db_bucket_t &bucket, c3db_id_t &bucket_id);

  //! Reads an overflow bucket by its full persistent id.
  c3db_err_t read_overflow_bkt(c3db_id_t bucket_id, c3db_bucket_t &bucket);

  //! Writes an existing overflow bucket by its full persistent id.
  c3db_err_t write_overflow_bkt(c3db_id_t bucket_id, const c3db_bucket_t &bucket);

  //! Creates an overflow bucket and links it from a canonical bucket with no overflow.
  c3db_err_t link_from_canonical_bkt(uint32_t canonical_bucket_num, const c3db_bucket_t &bucket, c3db_id_t &bucket_id);

  //! Creates an overflow bucket and links it from an overflow bucket with no successor.
  c3db_err_t link_from_overflow_bkt(c3db_id_t tail_bucket_id, const c3db_bucket_t &bucket, c3db_id_t &bucket_id);

  //! Frees a complete overflow bucket chain.
  c3db_err_t free_overflow_bkt_chain(c3db_id_t first_bucket_id);

  //! Returns the physical number of canonical buckets stored in ICB.
  c3db_err_t get_canonical_count(size_t &count) const;

  //! Returns true when both bucket files are open.
  bool is_open() const;

  //! Returns true when the bucket manager is opened read-only.
  bool is_read_only() const;

private:
  void init_empty_bucket(c3db_bucket_t &bucket) const;
  c3db_err_t close_files();
  c3db_err_t delete_index_files(const char* base_file_name) const;

  c3db_db_file_t icb_;
  c3db_db_file_t iob_;
  bool read_only_;
};
