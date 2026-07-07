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

#include "c3db_index_tests.h"

#include <stdio.h>

#include "c3db_index.h"

static bool expect_ok(const char* label, c3db_err_t got) {
  if (got == C3DB_OK) return true;
  printf("%s failed: expected=%d got=%d\n", label, C3DB_OK, got);
  return false;
}

static bool expect_true(const char* label, bool value) {
  if (value) return true;
  printf("%s failed\n", label);
  return false;
}

static void remove_index_files(const char* base_name) {
  char name[64];
  snprintf(name, sizeof(name), "%s.idx", base_name);
  remove(name);
  snprintf(name, sizeof(name), "%s.icb", base_name);
  remove(name);
  snprintf(name, sizeof(name), "%s.iob", base_name);
  remove(name);
  snprintf(name, sizeof(name), "%s.irf", base_name);
  remove(name);
}

static bool expect_index_record(
  const char* label,
  c3db_index_t &index,
  uint64_t hash,
  c3db_id_t expected_record
) {
  c3db_id_t record_id = C3DB_NULL_ID;
  c3db_idx_cursor_t cursor = {};
  if (!expect_ok(label, index.find(hash, record_id, cursor))) return false;
  if (!expect_true(label, record_id == expected_record)) return false;
  return expect_true(label, cursor.next_node == C3DB_NULL_ID);
}

static bool get_last_ref(c3db_index_t &index, uint64_t hash, c3db_id_t &last_ref) {
  c3db_id_t record_id = C3DB_NULL_ID;
  c3db_idx_cursor_t cursor = {};
  if (!expect_ok("idx get last find", index.find(hash, record_id, cursor))) return false;

  last_ref = cursor.node;
  while (cursor.next_node != C3DB_NULL_ID) {
    if (!expect_ok("idx get last next", index.find_next(record_id, cursor))) return false;
    last_ref = cursor.node;
  }

  return true;
}

static uint32_t split_threshold_count() {
  uint32_t count = 0;
  while ((count * 100u) <
         (C3DB_INDEX_BUCKET_CAPACITY * C3DB_INDEX_MAX_LOAD_PERCENT)) {
    ++count;
  }
  return count;
}

static bool run_c3db_index_basic_test() {
  printf("c3db_index_t basic test start\n");

  const char* base_name = "/sdcard/idxb";
  remove_index_files(base_name);

  static constexpr uint64_t hash = 0x123456789ABCDEF0ULL;
  static constexpr c3db_id_t record0 = 0x1000;
  static constexpr c3db_id_t record1 = 0x1001;

  c3db_index_t index;
  if (!expect_ok("idx create", index.create(base_name))) return false;
  if (!expect_true("idx open", index.is_open())) return false;

  if (!expect_ok("idx index first", index.index(hash, record0))) return false;

  c3db_id_t record_id = C3DB_NULL_ID;
  c3db_idx_cursor_t cursor = {};
  if (!expect_ok("idx find first", index.find(hash, record_id, cursor))) return false;
  if (!expect_true("idx first record", record_id == record0)) return false;
  if (!expect_true("idx first tail", cursor.next_node == C3DB_NULL_ID)) return false;

  if (!expect_ok("idx index second", index.index(hash, record1))) return false;
  if (!expect_ok("idx find second", index.find(hash, record_id, cursor))) return false;
  if (!expect_true("idx second record at head", record_id == record1)) return false;
  if (!expect_true("idx second has next", cursor.next_node != C3DB_NULL_ID)) return false;

  if (!expect_ok("idx next old", index.find_next(record_id, cursor))) return false;
  if (!expect_true("idx old record after second", record_id == record0)) return false;
  if (!expect_true("idx old tail", cursor.next_node == C3DB_NULL_ID)) return false;
  if (!expect_ok("idx close", index.end())) return false;

  c3db_index_t reopened;
  if (!expect_ok("idx reopen", reopened.begin(base_name))) return false;
  if (!expect_ok("idx reopened find", reopened.find(hash, record_id, cursor))) return false;
  if (!expect_true("idx reopened record", record_id == record1)) return false;
  if (!expect_ok("idx reopened close", reopened.end())) return false;

  printf("c3db_index_t basic test OK\n");
  return true;
}

static bool run_c3db_index_remove_single_test() {
  printf("c3db_index_t remove single test start\n");

  const char* base_name = "/sdcard/idxrm1";
  remove_index_files(base_name);

  static constexpr uint64_t hash_a = 0xA001;
  static constexpr uint64_t hash_b = 0xB001;
  static constexpr c3db_id_t record_a = 0x4001;
  static constexpr c3db_id_t record_b = 0x4002;

  c3db_index_t index;
  if (!expect_ok("idx rm1 create", index.create(base_name))) return false;
  if (!expect_ok("idx rm1 index a", index.index(hash_a, record_a))) return false;
  if (!expect_ok("idx rm1 index b", index.index(hash_b, record_b))) return false;

  c3db_id_t last_ref = C3DB_NULL_ID;
  if (!get_last_ref(index, hash_a, last_ref)) return false;
  if (!expect_ok("idx rm1 remove a", index.remove(hash_a, last_ref))) return false;

  c3db_id_t missing_record_id = C3DB_NULL_ID;
  c3db_idx_cursor_t missing_cursor = {};
  if (!expect_true(
        "idx rm1 a missing",
        index.find(hash_a, missing_record_id, missing_cursor) == C3DB_REC_NOT_FOUND_ERR
      )) return false;
  if (!expect_index_record("idx rm1 b remains", index, hash_b, record_b)) return false;
  if (!expect_ok("idx rm1 close", index.end())) return false;

  printf("c3db_index_t remove single test OK\n");
  return true;
}

static bool run_c3db_index_remove_chain_test() {
  printf("c3db_index_t remove chain test start\n");

  const char* base_name = "/sdcard/idxrmc";
  remove_index_files(base_name);

  static constexpr uint64_t hash = 0xC001;
  static constexpr c3db_id_t record0 = 0x5001;
  static constexpr c3db_id_t record1 = 0x5002;
  static constexpr c3db_id_t record2 = 0x5003;

  c3db_index_t index;
  if (!expect_ok("idx rmc create", index.create(base_name))) return false;
  if (!expect_ok("idx rmc index 0", index.index(hash, record0))) return false;
  if (!expect_ok("idx rmc index 1", index.index(hash, record1))) return false;
  if (!expect_ok("idx rmc index 2", index.index(hash, record2))) return false;

  c3db_id_t last_ref = C3DB_NULL_ID;
  if (!get_last_ref(index, hash, last_ref)) return false;
  if (!expect_ok("idx rmc remove", index.remove(hash, last_ref))) return false;

  c3db_id_t missing_record_id = C3DB_NULL_ID;
  c3db_idx_cursor_t missing_cursor = {};
  if (!expect_true(
        "idx rmc missing",
        index.find(hash, missing_record_id, missing_cursor) == C3DB_REC_NOT_FOUND_ERR
      )) return false;
  if (!expect_ok("idx rmc close", index.end())) return false;

  printf("c3db_index_t remove chain test OK\n");
  return true;
}

static bool run_c3db_index_split_test() {
  printf("c3db_index_t split test start\n");

  const char* base_name = "/sdcard/idxsplit";
  remove_index_files(base_name);

  const uint32_t count = split_threshold_count();
  static constexpr c3db_id_t record_base = 0x2000;

  c3db_index_t index;
  if (!expect_ok("idx split create", index.create(base_name))) return false;

  for (uint32_t i = 0; i < count; ++i) {
    if (!expect_ok("idx split index", index.index(i, record_base + i))) return false;
  }

  for (uint32_t i = 0; i < count; ++i) {
    if (!expect_index_record("idx split find", index, i, record_base + i)) return false;
  }

  if (!expect_ok("idx split close", index.end())) return false;

  c3db_index_t reopened;
  if (!expect_ok("idx split reopen", reopened.begin(base_name))) return false;
  for (uint32_t i = 0; i < count; ++i) {
    if (!expect_index_record("idx split reopened find", reopened, i, record_base + i)) return false;
  }
  if (!expect_ok("idx split reopened close", reopened.end())) return false;

  printf("c3db_index_t split test OK\n");
  return true;
}

static bool run_c3db_index_skewed_overflow_split_test() {
  printf("c3db_index_t skewed overflow split test start\n");

  const char* base_name = "/sdcard/idxskew";
  remove_index_files(base_name);

  static constexpr uint32_t count = C3DB_INDEX_BUCKET_CAPACITY + 8;
  static constexpr c3db_id_t record_base = 0x3000;

  c3db_index_t index;
  if (!expect_ok("idx skew create", index.create(base_name))) return false;

  /*
   * Multiples of 8 keep falling into bucket 0 through several early splits.
   * This exercises split reconstruction while that bucket already has overflow.
   */
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t hash = static_cast<uint64_t>(i) * 8u;
    if (!expect_ok("idx skew index", index.index(hash, record_base + i))) return false;
  }

  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t hash = static_cast<uint64_t>(i) * 8u;
    if (!expect_index_record("idx skew find", index, hash, record_base + i)) return false;
  }

  if (!expect_ok("idx skew close", index.end())) return false;

  c3db_index_t reopened;
  if (!expect_ok("idx skew reopen", reopened.begin(base_name))) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t hash = static_cast<uint64_t>(i) * 8u;
    if (!expect_index_record("idx skew reopened find", reopened, hash, record_base + i)) return false;
  }
  if (!expect_ok("idx skew reopened close", reopened.end())) return false;

  printf("c3db_index_t skewed overflow split test OK\n");
  return true;
}

bool run_c3db_index_tests() {
  if (!run_c3db_index_basic_test()) return false;
  if (!run_c3db_index_remove_single_test()) return false;
  if (!run_c3db_index_remove_chain_test()) return false;
  if (!run_c3db_index_split_test()) return false;
  if (!run_c3db_index_skewed_overflow_split_test()) return false;
  return true;
}
