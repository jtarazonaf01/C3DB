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

#include "c3db_db_tests.h"

#include <cstring>
#include <new>
#include <stdio.h>

#include "c3db_cached_db.h"
#include "c3db_data_db.h"
#include "c3db_types.h"

static constexpr size_t DB_TEST_DATA_SIZE = 16;
static constexpr size_t DB_TEST_CACHE_MEMORY = 4096;
static constexpr size_t DB_TEST_KEY_OFFSET = 0;
static constexpr size_t DB_TEST_KEY_LEN = 1;
static constexpr size_t DB_TEST_TAG_OFFSET = 1;

static void remove_db_files(const char* base_name) {
  char name[80];
  snprintf(name, sizeof(name), "%s.dbf", base_name);
  remove(name);
  snprintf(name, sizeof(name), "%s.met", base_name);
  remove(name);
  snprintf(name, sizeof(name), "%s.dat", base_name);
  remove(name);
  snprintf(name, sizeof(name), "%s.db", base_name);
  remove(name);

  for (size_t i = 0; i < C3DB_IDX_CAPACITY; ++i) {
    snprintf(name, sizeof(name), "%s_i%u.idx", base_name, static_cast<unsigned>(i));
    remove(name);
    snprintf(name, sizeof(name), "%s_i%u.icb", base_name, static_cast<unsigned>(i));
    remove(name);
    snprintf(name, sizeof(name), "%s_i%u.iob", base_name, static_cast<unsigned>(i));
    remove(name);
    snprintf(name, sizeof(name), "%s_i%u.irf", base_name, static_cast<unsigned>(i));
    remove(name);
  }
}

static void fill_record(uint8_t* data, uint8_t key, uint8_t tag) {
  std::memset(data, 0, DB_TEST_DATA_SIZE);
  data[DB_TEST_KEY_OFFSET] = key;
  data[DB_TEST_TAG_OFFSET] = tag;
  for (size_t i = 2; i < DB_TEST_DATA_SIZE; ++i) data[i] = tag + static_cast<uint8_t>(i);
}

static bool expect_ok(const char* label, c3db_err_t got) {
  if (got == C3DB_OK) return true;
  printf("%s failed: expected=%d got=%d\n", label, C3DB_OK, got);
  return false;
}

static bool expect_err(const char* label, c3db_err_t expected, c3db_err_t got) {
  if (got == expected) return true;
  printf("%s failed: expected=%d got=%d\n", label, expected, got);
  return false;
}

static bool expect_true(const char* label, bool value) {
  if (value) return true;
  printf("%s failed\n", label);
  return false;
}

static bool indexed_select_tags(
  c3db_db_t &db,
  size_t idx_num,
  uint8_t key,
  uint8_t* tags,
  size_t max_tags,
  size_t &count
) {
  count = 0;
  uint8_t out[DB_TEST_DATA_SIZE] = {};
  c3db_idx_cursor_t cursor = {};

  while (true) {
    const c3db_err_t err = db.select(idx_num, &key, out, cursor);
    if (err == C3DB_REC_NOT_FOUND_ERR) return true;
    if (!expect_ok("db indexed select", err)) return false;
    if (!expect_true("db indexed select count", count < max_tags)) return false;
    tags[count++] = out[DB_TEST_TAG_OFFSET];
  }
}

static bool has_tag(const uint8_t* tags, size_t count, uint8_t tag) {
  for (size_t i = 0; i < count; ++i) {
    if (tags[i] == tag) return true;
  }
  return false;
}

static bool run_c3db_cached_db_index_test() {
  printf("c3db_cached_db_t index test start\n");

  const char* base_name = "/sdcard/hcdb";
  remove_db_files(base_name);

  uint8_t rec_a[DB_TEST_DATA_SIZE];
  uint8_t rec_b[DB_TEST_DATA_SIZE];
  uint8_t rec_c[DB_TEST_DATA_SIZE];
  uint8_t rec_a_updated[DB_TEST_DATA_SIZE];
  uint8_t rec_d[DB_TEST_DATA_SIZE];
  uint8_t rec_d_updated[DB_TEST_DATA_SIZE];
  fill_record(rec_a, 0x10, 0xA0);
  fill_record(rec_b, 0x10, 0xB0);
  fill_record(rec_c, 0x20, 0xC0);
  fill_record(rec_a_updated, 0x30, 0xA1);
  fill_record(rec_d, 0x40, 0xD0);
  fill_record(rec_d_updated, 0x41, 0xD1);

  c3db_cached_db_t* db = new (std::nothrow) c3db_cached_db_t(DB_TEST_DATA_SIZE, DB_TEST_CACHE_MEMORY);
  if (!expect_true("cached db alloc", db != nullptr)) return false;
  if (!expect_ok("cached db create", db->create(base_name, C3DB_SEQUENTIAL_ACCESS_MODE))) return false;
  printf("cached db created\n");

  size_t idx_num = C3DB_IDX_CAPACITY;
  if (!expect_ok("cached db create idx", db->create_idx(DB_TEST_KEY_OFFSET, DB_TEST_KEY_LEN, idx_num))) return false;
  printf("cached db index created: %u\n", static_cast<unsigned>(idx_num));

  c3db_id_t id_a = C3DB_NULL_ID;
  c3db_id_t id_b = C3DB_NULL_ID;
  c3db_id_t id_c = C3DB_NULL_ID;
  c3db_id_t id_d = C3DB_NULL_ID;
  if (!expect_ok("cached db insert a", db->insert(rec_a, id_a))) return false;
  if (!expect_ok("cached db insert b", db->insert(rec_b, id_b))) return false;
  if (!expect_ok("cached db insert c", db->insert(rec_c, id_c))) return false;
  if (!expect_ok("cached db insert d", db->insert(rec_d, id_d))) return false;
  printf("cached db records inserted: a=%llu b=%llu c=%llu\n",
         static_cast<unsigned long long>(id_a),
         static_cast<unsigned long long>(id_b),
         static_cast<unsigned long long>(id_c));

  uint8_t tags[4] = {};
  size_t count = 0;
  if (!indexed_select_tags(*db, idx_num, 0x10, tags, 4, count)) return false;
  printf("cached db key 10 count=%u\n", static_cast<unsigned>(count));
  if (!expect_true("cached db key 10 count", count == 2)) return false;
  if (!expect_true("cached db key 10 tag a", has_tag(tags, count, 0xA0))) return false;
  if (!expect_true("cached db key 10 tag b", has_tag(tags, count, 0xB0))) return false;
  printf("cached db initial indexed select OK\n");

  if (!expect_ok("cached db update a", db->update(id_a, rec_a_updated))) return false;
  if (!indexed_select_tags(*db, idx_num, 0x10, tags, 4, count)) return false;
  printf("cached db key 10 after update count=%u\n", static_cast<unsigned>(count));
  if (!expect_true("cached db key 10 after update count", count == 1)) return false;
  if (!expect_true("cached db key 10 after update tag b", has_tag(tags, count, 0xB0))) return false;
  printf("cached db update lazy select OK\n");

  if (!indexed_select_tags(*db, idx_num, 0x30, tags, 4, count)) return false;
  printf("cached db key 30 count=%u\n", static_cast<unsigned>(count));
  if (!expect_true("cached db key 30 count", count == 1)) return false;
  if (!expect_true("cached db key 30 tag a1", has_tag(tags, count, 0xA1))) return false;
  printf("cached db updated key select OK\n");

  if (!expect_ok("cached db update d", db->update(id_d, rec_d_updated))) return false;
  size_t removed_count = 0;
  const uint8_t old_key_40 = 0x40;
  if (!expect_ok("cached db remove stale key 40", db->remove(idx_num, &old_key_40, removed_count))) return false;
  if (!expect_true("cached db stale remove count", removed_count == 0)) return false;
  if (!indexed_select_tags(*db, idx_num, 0x41, tags, 4, count)) return false;
  if (!expect_true("cached db updated d still indexed", has_tag(tags, count, 0xD1))) return false;
  printf("cached db stale indexed remove OK\n");

  if (!expect_ok("cached db remove b", db->remove(id_b))) return false;
  if (!indexed_select_tags(*db, idx_num, 0x10, tags, 4, count)) return false;
  printf("cached db key 10 after remove count=%u\n", static_cast<unsigned>(count));
  if (!expect_true("cached db key 10 removed count", count == 0)) return false;
  printf("cached db id remove lazy select OK\n");

  const uint8_t key_20 = 0x20;
  if (!expect_ok("cached db remove key 20", db->remove(idx_num, &key_20, removed_count))) return false;
  if (!expect_true("cached db removed key 20 count", removed_count == 1)) return false;
  printf("cached db indexed remove OK\n");

  if (!expect_ok("cached db close", db->end())) return false;
  delete db;
  db = nullptr;

  c3db_cached_db_t* reopened = new (std::nothrow) c3db_cached_db_t(DB_TEST_DATA_SIZE, DB_TEST_CACHE_MEMORY);
  if (!expect_true("cached db reopen alloc", reopened != nullptr)) return false;
  if (!expect_ok("cached db reopen", reopened->begin(base_name, false, C3DB_SEQUENTIAL_ACCESS_MODE))) return false;
  if (!indexed_select_tags(*reopened, idx_num, 0x30, tags, 4, count)) return false;
  if (!expect_true("cached db reopened key 30 count", count == 1)) return false;
  if (!expect_true("cached db reopened key 30 tag a1", has_tag(tags, count, 0xA1))) return false;
  if (!expect_ok("cached db reopened close", reopened->end())) return false;
  delete reopened;

  printf("c3db_cached_db_t index test OK\n");
  return true;
}

static bool run_c3db_data_db_index_test() {
  printf("c3db_data_db_t index test start\n");

  const char* base_name = "/sdcard/hddb";
  remove_db_files(base_name);

  uint8_t rec_a[DB_TEST_DATA_SIZE];
  uint8_t rec_b[DB_TEST_DATA_SIZE];
  fill_record(rec_a, 0x41, 0xD1);
  fill_record(rec_b, 0x42, 0xD2);

  c3db_data_db_t* db = new (std::nothrow) c3db_data_db_t(DB_TEST_DATA_SIZE);
  if (!expect_true("data db alloc", db != nullptr)) return false;
  if (!expect_ok("data db create", db->create(base_name))) return false;

  c3db_id_t id_a = C3DB_NULL_ID;
  c3db_id_t id_b = C3DB_NULL_ID;
  if (!expect_ok("data db insert a before idx", db->insert(rec_a, id_a))) return false;
  if (!expect_ok("data db insert b before idx", db->insert(rec_b, id_b))) return false;

  size_t idx_num = C3DB_IDX_CAPACITY;
  if (!expect_ok("data db create idx content", db->create_idx(DB_TEST_KEY_OFFSET, DB_TEST_KEY_LEN, idx_num, true))) {
    return false;
  }

  uint8_t tags[2] = {};
  size_t count = 0;
  if (!indexed_select_tags(*db, idx_num, 0x41, tags, 2, count)) return false;
  if (!expect_true("data db key 41 count", count == 1)) return false;
  if (!expect_true("data db key 41 tag", has_tag(tags, count, 0xD1))) return false;

  if (!expect_ok("data db delete idx", db->delete_idx(idx_num))) return false;
  c3db_idx_cursor_t cursor = {};
  uint8_t out[DB_TEST_DATA_SIZE] = {};
  if (!expect_err("data db select deleted idx", C3DB_REC_NOT_FOUND_ERR,
                  db->select(idx_num, rec_a + DB_TEST_KEY_OFFSET, out, cursor))) {
    return false;
  }

  if (!expect_ok("data db close", db->end())) return false;
  delete db;

  printf("c3db_data_db_t index test OK\n");
  return true;
}

bool run_c3db_db_tests() {
  if (!run_c3db_cached_db_index_test()) return false;
  if (!run_c3db_data_db_index_test()) return false;
  return true;
}
