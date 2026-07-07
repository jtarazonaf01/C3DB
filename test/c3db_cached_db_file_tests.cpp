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

#include "c3db_cached_db_file_tests.h"

#include <cstring>
#include <stdio.h>

#include "c3db_cached_db_file.h"
#include "c3db_db_file.h"
#include "c3db_types.h"

static constexpr size_t DBF_TEST_DATA_SIZE = 16;
static constexpr size_t CACHED_DBF_TEST_ENTRY_SIZE =
  (3 * sizeof(uint8_t)) +
  sizeof(c3db_id_t) +
  sizeof(c3db_db_rec_t::del_block_t) +
  sizeof(c3db_db_rec_t::data_block_t) +
  DBF_TEST_DATA_SIZE;
static constexpr size_t CACHED_DBF_TEST_MEMORY = 16 * CACHED_DBF_TEST_ENTRY_SIZE;

static uint32_t test_row(c3db_id_t id) {
  return static_cast<uint32_t>(id & 0xFFFFFFFFu);
}

static uint32_t test_cycle(c3db_id_t id) {
  return static_cast<uint32_t>(id >> 32);
}

static void fill_test_payload(uint8_t* data, uint8_t seed) {
  for (size_t i = 0; i < DBF_TEST_DATA_SIZE; ++i) data[i] = seed + i;
}

static bool payload_eq(const uint8_t* a, const uint8_t* b) {
  return std::memcmp(a, b, DBF_TEST_DATA_SIZE) == 0;
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

static bool run_c3db_cached_db_file_basic_test() {
  printf("c3db_cached_db_file_t basic test start\n");

  const char* base_name = "/sdcard/ccbasic";
  const char* dbf_name = "/sdcard/ccbasic.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p2[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t id2 = C3DB_NULL_ID;

  fill_test_payload(p0, 0x10);
  fill_test_payload(p1, 0x30);
  fill_test_payload(p2, 0x50);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached create", db.create(base_name))) return false;
  if (!expect_ok("cached append 0", db.append(p0, id0))) return false;
  if (!expect_ok("cached append 1", db.append(p1, id1))) return false;

  if (!expect_ok("cached select pending 0", db.select(id0, out))) return false;
  if (!expect_true("cached pending payload 0", payload_eq(out, p0))) return false;

  if (!expect_ok("cached commit", db.commit())) return false;
  if (!expect_ok("cached append 2", db.append(p2, id2))) return false;
  if (!expect_ok("cached select persisted 1", db.select(id1, out))) return false;
  if (!expect_true("cached persisted payload 1", payload_eq(out, p1))) return false;
  if (!expect_ok("cached close", db.end())) return false;

  c3db_db_file_t reopened(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached reopen plain dbf", reopened.begin(base_name))) return false;
  if (!expect_ok("cached reopened select 0", reopened.select(id0, out))) return false;
  if (!expect_true("cached reopened payload 0", payload_eq(out, p0))) return false;
  if (!expect_ok("cached reopened select 2", reopened.select(id2, out))) return false;
  if (!expect_true("cached reopened payload 2", payload_eq(out, p2))) return false;
  if (!expect_ok("cached reopened close", reopened.end())) return false;

  printf("c3db_cached_db_file_t basic test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_update_remove_insert_test() {
  printf("c3db_cached_db_file_t update/remove/insert test start\n");

  const char* base_name = "/sdcard/ccmut";
  const char* dbf_name = "/sdcard/ccmut.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p0_updated[DBF_TEST_DATA_SIZE];
  uint8_t p2[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t id2 = C3DB_NULL_ID;

  fill_test_payload(p0, 0x11);
  fill_test_payload(p1, 0x22);
  fill_test_payload(p0_updated, 0x33);
  fill_test_payload(p2, 0x44);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached mut create", db.create(base_name, C3DB_SEQUENTIAL_ACCESS_MODE))) return false;
  if (!expect_ok("cached mut append 0", db.append(p0, id0))) return false;
  if (!expect_ok("cached mut append 1", db.append(p1, id1))) return false;
  if (!expect_ok("cached mut commit", db.commit())) return false;

  /*
   * These reads populate the sequential cache. The following mutations verify
   * that cached entries remain coherent when DBF state changes underneath.
   */
  if (!expect_ok("cached mut select 0", db.select(id0, out))) return false;
  if (!expect_ok("cached mut select 1", db.select(id1, out))) return false;

  if (!expect_ok("cached mut update 0", db.update(id0, p0_updated))) return false;
  if (!expect_ok("cached mut select updated 0", db.select(id0, out))) return false;
  if (!expect_true("cached mut updated payload", payload_eq(out, p0_updated))) return false;

  if (!expect_ok("cached mut remove 1", db.remove(id1))) return false;
  if (!expect_err("cached mut removed hidden", C3DB_REC_NOT_FOUND_ERR, db.select(id1, out))) return false;

  if (!expect_ok("cached mut insert reuse", db.insert(p2, id2))) return false;
  if (!expect_true("cached mut reused row", test_row(id2) == test_row(id1))) return false;
  if (!expect_true("cached mut advanced cycle", test_cycle(id2) == test_cycle(id1) + 1)) return false;
  if (!expect_ok("cached mut select reused", db.select(id2, out))) return false;
  if (!expect_true("cached mut reused payload", payload_eq(out, p2))) return false;
  if (!expect_ok("cached mut close", db.end())) return false;

  printf("c3db_cached_db_file_t update/remove/insert test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_mode_test() {
  printf("c3db_cached_db_file_t mode test start\n");

  const char* base_name = "/sdcard/ccmode";
  const char* dbf_name = "/sdcard/ccmode.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;

  fill_test_payload(p0, 0x61);
  fill_test_payload(p1, 0x71);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached mode create", db.create(base_name, C3DB_BULK_INSERT_MODE))) return false;
  if (!expect_true("cached mode initial", db.mode() == C3DB_BULK_INSERT_MODE)) return false;
  if (!expect_ok("cached mode append 0", db.append(p0, id0))) return false;

  /*
   * mode(new_mode) repartitions cache memory, so it must commit pending
   * appends first. Reopening with plain DBF verifies that the commit happened.
   */
  if (!expect_ok("cached mode change", db.mode(C3DB_SEQUENTIAL_ACCESS_MODE))) return false;
  if (!expect_true("cached mode changed", db.mode() == C3DB_SEQUENTIAL_ACCESS_MODE)) return false;
  if (!expect_ok("cached mode append 1", db.append(p1, id1))) return false;
  if (!expect_ok("cached mode close", db.end())) return false;

  c3db_db_file_t reopened(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached mode reopen", reopened.begin(base_name))) return false;
  if (!expect_ok("cached mode select 0", reopened.select(id0, out))) return false;
  if (!expect_true("cached mode payload 0", payload_eq(out, p0))) return false;
  if (!expect_ok("cached mode select 1", reopened.select(id1, out))) return false;
  if (!expect_true("cached mode payload 1", payload_eq(out, p1))) return false;
  if (!expect_ok("cached mode reopened close", reopened.end())) return false;

  printf("c3db_cached_db_file_t mode test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_sequential_window_test() {
  printf("c3db_cached_db_file_t sequential window test start\n");

  const char* base_name = "/sdcard/ccseq";
  const char* dbf_name = "/sdcard/ccseq.dbf";

  remove(dbf_name);

  uint8_t payloads[6][DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t ids[6];

  for (size_t i = 0; i < 6; ++i) {
    fill_test_payload(payloads[i], static_cast<uint8_t>(0x10 + (i * 0x10)));
    ids[i] = C3DB_NULL_ID;
  }

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached seq create", db.create(base_name, C3DB_SEQUENTIAL_ACCESS_MODE))) return false;

  for (size_t i = 0; i < 6; ++i) {
    if (!expect_ok("cached seq append", db.append(payloads[i], ids[i]))) return false;
  }

  if (!expect_ok("cached seq commit", db.commit())) return false;

  /*
   * The first select loads a sequential window. The following contiguous
   * reads should be served coherently from that window or from disk with the
   * same externally visible result.
   */
  for (size_t i = 0; i < 6; ++i) {
    if (!expect_ok("cached seq select", db.select(ids[i], out))) return false;
    if (!expect_true("cached seq payload", payload_eq(out, payloads[i]))) return false;
  }

  if (!expect_ok("cached seq close", db.end())) return false;

  printf("c3db_cached_db_file_t sequential window test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_history_test() {
  printf("c3db_cached_db_file_t history test start\n");

  const char* base_name = "/sdcard/cchist";
  const char* dbf_name = "/sdcard/cchist.dbf";

  remove(dbf_name);

  uint8_t payloads[12][DBF_TEST_DATA_SIZE];
  uint8_t updated[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t ids[12];

  for (size_t i = 0; i < 12; ++i) {
    fill_test_payload(payloads[i], static_cast<uint8_t>(0x20 + i));
    ids[i] = C3DB_NULL_ID;
  }
  fill_test_payload(updated, 0xA0);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached hist create", db.create(base_name, C3DB_BALANCED_MODE))) return false;

  for (size_t i = 0; i < 12; ++i) {
    if (!expect_ok("cached hist append", db.append(payloads[i], ids[i]))) return false;
  }

  if (!expect_ok("cached hist commit", db.commit())) return false;

  /*
   * Accessing the first window and then forcing a different sequential window
   * exercises the migration of recently accessed valid records into history.
   */
  if (!expect_ok("cached hist select 0", db.select(ids[0], out))) return false;
  if (!expect_ok("cached hist select 1", db.select(ids[1], out))) return false;
  if (!expect_ok("cached hist select 10", db.select(ids[10], out))) return false;
  if (!expect_true("cached hist payload 10", payload_eq(out, payloads[10]))) return false;

  if (!expect_ok("cached hist revisit 0", db.select(ids[0], out))) return false;
  if (!expect_true("cached hist payload 0", payload_eq(out, payloads[0]))) return false;

  if (!expect_ok("cached hist update 0", db.update(ids[0], updated))) return false;
  if (!expect_ok("cached hist select updated 0", db.select(ids[0], out))) return false;
  if (!expect_true("cached hist updated payload", payload_eq(out, updated))) return false;

  if (!expect_ok("cached hist remove 1", db.remove(ids[1]))) return false;
  if (!expect_err("cached hist removed hidden", C3DB_REC_NOT_FOUND_ERR, db.select(ids[1], out))) return false;
  if (!expect_ok("cached hist close", db.end())) return false;

  printf("c3db_cached_db_file_t history test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_memory_limit_test() {
  printf("c3db_cached_db_file_t memory limit test start\n");

  const char* base_name = "/sdcard/ccmem";
  const char* dbf_name = "/sdcard/ccmem.dbf";

  remove(dbf_name);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY / 8);
  if (!expect_err("cached memory create", C3DB_INVALID_ARG_ERR, db.create(base_name))) return false;

  printf("c3db_cached_db_file_t memory limit test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_persisted_deleted_test() {
  printf("c3db_cached_db_file_t persisted deleted test start\n");

  const char* base_name = "/sdcard/ccdel";
  const char* dbf_name = "/sdcard/ccdel.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p2[DBF_TEST_DATA_SIZE];
  uint8_t reused[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t id2 = C3DB_NULL_ID;
  c3db_id_t reused_id = C3DB_NULL_ID;

  fill_test_payload(p0, 0x12);
  fill_test_payload(p1, 0x24);
  fill_test_payload(p2, 0x36);
  fill_test_payload(reused, 0x48);

  c3db_db_file_t plain(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached del plain create", plain.create(base_name))) return false;
  if (!expect_ok("cached del plain append 0", plain.append(p0, id0))) return false;
  if (!expect_ok("cached del plain append 1", plain.append(p1, id1))) return false;
  if (!expect_ok("cached del plain append 2", plain.append(p2, id2))) return false;
  if (!expect_ok("cached del plain remove 1", plain.remove(id1))) return false;
  if (!expect_ok("cached del plain close", plain.end())) return false;

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached del begin", db.begin(base_name, false, C3DB_SEQUENTIAL_ACCESS_MODE))) return false;

  /*
   * The deleted row is loaded from the persisted DBF state. It may be cached
   * as deleted, but externally it must remain indistinguishable from DBF:
   * selecting that old identifier returns not found.
   */
  if (!expect_err("cached del select deleted", C3DB_REC_NOT_FOUND_ERR, db.select(id1, out))) return false;
  if (!expect_ok("cached del select neighbour", db.select(id2, out))) return false;
  if (!expect_true("cached del neighbour payload", payload_eq(out, p2))) return false;

  if (!expect_ok("cached del insert reuse", db.insert(reused, reused_id))) return false;
  if (!expect_true("cached del reused row", test_row(reused_id) == test_row(id1))) return false;
  if (!expect_true("cached del advanced cycle", test_cycle(reused_id) == test_cycle(id1) + 1)) return false;
  if (!expect_err("cached del old id hidden", C3DB_REC_NOT_FOUND_ERR, db.select(id1, out))) return false;
  if (!expect_ok("cached del select reused", db.select(reused_id, out))) return false;
  if (!expect_true("cached del reused payload", payload_eq(out, reused))) return false;
  if (!expect_ok("cached del close", db.end())) return false;

  printf("c3db_cached_db_file_t persisted deleted test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_pending_count_test() {
  printf("c3db_cached_db_file_t pending count test start\n");

  const char* base_name = "/sdcard/cccnt";
  const char* dbf_name = "/sdcard/cccnt.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p2[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t id2 = C3DB_NULL_ID;
  size_t count = 0;

  fill_test_payload(p0, 0x81);
  fill_test_payload(p1, 0x91);
  fill_test_payload(p2, 0xA1);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached count create", db.create(base_name, C3DB_BULK_INSERT_MODE))) return false;
  if (!expect_ok("cached count append 0", db.append(p0, id0))) return false;
  if (!expect_ok("cached count append 1", db.append(p1, id1))) return false;
  if (!expect_ok("cached count pending max", db.max_rec_count(count))) return false;
  if (!expect_true("cached count pending value", count == 2)) return false;

  if (!expect_ok("cached count commit", db.commit())) return false;
  if (!expect_ok("cached count remove 0", db.remove(id0))) return false;
  if (!expect_ok("cached count after remove max", db.max_rec_count(count))) return false;
  if (!expect_true("cached count after remove value", count == 1)) return false;

  if (!expect_ok("cached count append 2", db.append(p2, id2))) return false;
  if (!expect_ok("cached count mixed max", db.max_rec_count(count))) return false;
  if (!expect_true("cached count mixed value", count == 2)) return false;
  if (!expect_ok("cached count close", db.end())) return false;

  printf("c3db_cached_db_file_t pending count test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_export_commits_test() {
  printf("c3db_cached_db_file_t export commits test start\n");

  const char* base_name = "/sdcard/ccexp";
  const char* dbf_name = "/sdcard/ccexp.dbf";
  const char* export_name = "/sdcard/ccexp.raw";

  remove(dbf_name);
  remove(export_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  size_t rows_exported = 0;

  fill_test_payload(p0, 0xB0);
  fill_test_payload(p1, 0xC0);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached exp create", db.create(base_name, C3DB_BULK_INSERT_MODE))) return false;
  if (!expect_ok("cached exp append 0", db.append(p0, id0))) return false;
  if (!expect_ok("cached exp append 1", db.append(p1, id1))) return false;

  if (!expect_ok("cached exp export", db.export_file(export_name, rows_exported))) return false;
  if (!expect_true("cached exp rows", rows_exported == 2)) return false;

  FILE* exported = fopen(export_name, "rb");
  if (!expect_true("cached exp open raw", exported != nullptr)) return false;

  bool ok = fread(out, 1, DBF_TEST_DATA_SIZE, exported) == DBF_TEST_DATA_SIZE &&
            payload_eq(out, p0) &&
            fread(out, 1, DBF_TEST_DATA_SIZE, exported) == DBF_TEST_DATA_SIZE &&
            payload_eq(out, p1);
  fclose(exported);
  if (!expect_true("cached exp raw payloads", ok)) return false;

  if (!expect_ok("cached exp close", db.end())) return false;

  printf("c3db_cached_db_file_t export commits test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_import_commits_test() {
  printf("c3db_cached_db_file_t import commits test start\n");

  const char* base_name = "/sdcard/ccimp";
  const char* dbf_name = "/sdcard/ccimp.dbf";
  const char* import_name = "/sdcard/ccimp.raw";

  remove(dbf_name);
  remove(import_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p2[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t first_id = C3DB_NULL_ID;
  size_t rows_added = 0;

  fill_test_payload(p0, 0xD0);
  fill_test_payload(p1, 0xE0);
  fill_test_payload(p2, 0xF0);

  FILE* imported = fopen(import_name, "wb");
  if (!expect_true("cached imp create raw", imported != nullptr)) return false;

  bool raw_ok = fwrite(p1, 1, DBF_TEST_DATA_SIZE, imported) == DBF_TEST_DATA_SIZE &&
                fwrite(p2, 1, DBF_TEST_DATA_SIZE, imported) == DBF_TEST_DATA_SIZE;
  fclose(imported);
  if (!expect_true("cached imp write raw", raw_ok)) return false;

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached imp create", db.create(base_name, C3DB_BULK_INSERT_MODE))) return false;
  if (!expect_ok("cached imp append pending", db.append(p0, id0))) return false;

  /*
   * Import delegates to DBF after committing pending append-cache records, so
   * imported rows must be placed after the already pending logical append.
   */
  if (!expect_ok("cached imp import", db.import_file(import_name, first_id, rows_added))) return false;
  if (!expect_true("cached imp rows added", rows_added == 2)) return false;
  if (!expect_true("cached imp first row", test_row(first_id) == 1)) return false;

  if (!expect_ok("cached imp select pending", db.select(id0, out))) return false;
  if (!expect_true("cached imp pending payload", payload_eq(out, p0))) return false;
  if (!expect_ok("cached imp select first import", db.select(first_id, out))) return false;
  if (!expect_true("cached imp first payload", payload_eq(out, p1))) return false;
  if (!expect_ok("cached imp select second import", db.select(first_id + 1, out))) return false;
  if (!expect_true("cached imp second payload", payload_eq(out, p2))) return false;
  if (!expect_ok("cached imp close", db.end())) return false;

  printf("c3db_cached_db_file_t import commits test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_write_cache_overflow_test() {
  printf("c3db_cached_db_file_t write cache overflow test start\n");

  const char* base_name = "/sdcard/ccwov";
  const char* dbf_name = "/sdcard/ccwov.dbf";

  remove(dbf_name);

  uint8_t payloads[20][DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t ids[20];

  for (size_t i = 0; i < 20; ++i) {
    fill_test_payload(payloads[i], static_cast<uint8_t>(0x20 + i));
    ids[i] = C3DB_NULL_ID;
  }

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached wov create", db.create(base_name, C3DB_BULK_INSERT_MODE))) return false;

  /*
   * Appending more rows than the write cache can hold forces an internal
   * commit and starts a new pending block. The observable contract is that all
   * generated ids remain readable and contiguous.
   */
  for (size_t i = 0; i < 20; ++i) {
    if (!expect_ok("cached wov append", db.append(payloads[i], ids[i]))) return false;
    if (!expect_true("cached wov row id", test_row(ids[i]) == i)) return false;
  }

  for (size_t i = 0; i < 20; ++i) {
    if (!expect_ok("cached wov select", db.select(ids[i], out))) return false;
    if (!expect_true("cached wov payload", payload_eq(out, payloads[i]))) return false;
  }

  if (!expect_ok("cached wov close", db.end())) return false;

  c3db_db_file_t reopened(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached wov reopen", reopened.begin(base_name))) return false;
  for (size_t i = 0; i < 20; ++i) {
    if (!expect_ok("cached wov reopened select", reopened.select(ids[i], out))) return false;
    if (!expect_true("cached wov reopened payload", payload_eq(out, payloads[i]))) return false;
  }
  if (!expect_ok("cached wov reopened close", reopened.end())) return false;

  printf("c3db_cached_db_file_t write cache overflow test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_read_only_test() {
  printf("c3db_cached_db_file_t read only test start\n");

  const char* base_name = "/sdcard/ccro";
  const char* dbf_name = "/sdcard/ccro.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t updated[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t new_id = C3DB_NULL_ID;

  fill_test_payload(p0, 0x42);
  fill_test_payload(updated, 0x52);

  c3db_db_file_t plain(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached ro plain create", plain.create(base_name))) return false;
  if (!expect_ok("cached ro plain append", plain.append(p0, id0))) return false;
  if (!expect_ok("cached ro plain close", plain.end())) return false;

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached ro begin", db.begin(base_name, true, C3DB_BALANCED_MODE))) return false;
  if (!expect_ok("cached ro select", db.select(id0, out))) return false;
  if (!expect_true("cached ro payload", payload_eq(out, p0))) return false;
  if (!expect_err("cached ro append", C3DB_READ_ONLY_ERR, db.append(updated, new_id))) return false;
  if (!expect_err("cached ro insert", C3DB_READ_ONLY_ERR, db.insert(updated, new_id))) return false;
  if (!expect_err("cached ro update", C3DB_READ_ONLY_ERR, db.update(id0, updated))) return false;
  if (!expect_err("cached ro remove", C3DB_READ_ONLY_ERR, db.remove(id0))) return false;
  if (!expect_ok("cached ro close", db.end())) return false;

  printf("c3db_cached_db_file_t read only test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_pending_mutation_test() {
  printf("c3db_cached_db_file_t pending mutation test start\n");

  const char* base_name = "/sdcard/ccpmut";
  const char* dbf_name = "/sdcard/ccpmut.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p1_updated[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;

  fill_test_payload(p0, 0x13);
  fill_test_payload(p1, 0x23);
  fill_test_payload(p1_updated, 0x33);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached pmut create", db.create(base_name, C3DB_BULK_INSERT_MODE))) return false;
  if (!expect_ok("cached pmut append 0", db.append(p0, id0))) return false;
  if (!expect_ok("cached pmut append 1", db.append(p1, id1))) return false;

  /*
   * update/remove cannot modify the compact write cache in isolation because
   * DBF consistency metadata lives on disk. They first commit pending appends
   * and then delegate the mutation to the DBF layer.
   */
  if (!expect_ok("cached pmut update pending", db.update(id1, p1_updated))) return false;
  if (!expect_ok("cached pmut remove pending", db.remove(id0))) return false;
  if (!expect_err("cached pmut removed hidden", C3DB_REC_NOT_FOUND_ERR, db.select(id0, out))) return false;
  if (!expect_ok("cached pmut select updated", db.select(id1, out))) return false;
  if (!expect_true("cached pmut updated payload", payload_eq(out, p1_updated))) return false;
  if (!expect_ok("cached pmut close", db.end())) return false;

  c3db_db_file_t reopened(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached pmut reopen", reopened.begin(base_name))) return false;
  if (!expect_err("cached pmut reopened removed", C3DB_REC_NOT_FOUND_ERR, reopened.select(id0, out))) return false;
  if (!expect_ok("cached pmut reopened updated", reopened.select(id1, out))) return false;
  if (!expect_true("cached pmut reopened payload", payload_eq(out, p1_updated))) return false;
  if (!expect_ok("cached pmut reopened close", reopened.end())) return false;

  printf("c3db_cached_db_file_t pending mutation test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_minimum_cache_test() {
  printf("c3db_cached_db_file_t minimum cache test start\n");

  const char* base_name = "/sdcard/ccmin";
  const char* dbf_name = "/sdcard/ccmin.dbf";
  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;

  fill_test_payload(p0, 0x64);
  fill_test_payload(p1, 0x74);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, 3 * CACHED_DBF_TEST_ENTRY_SIZE);
  if (!expect_ok("cached min create", db.create(base_name, C3DB_BALANCED_MODE))) return false;

  /*
   * With the minimum cache size there is exactly one entry per cache area.
   * This exercises the boundary case where every cache algorithm must still
   * make progress without relying on spare capacity.
   */
  if (!expect_ok("cached min append 0", db.append(p0, id0))) return false;
  if (!expect_ok("cached min append 1", db.append(p1, id1))) return false;
  if (!expect_ok("cached min select 0", db.select(id0, out))) return false;
  if (!expect_true("cached min payload 0", payload_eq(out, p0))) return false;
  if (!expect_ok("cached min select 1", db.select(id1, out))) return false;
  if (!expect_true("cached min payload 1", payload_eq(out, p1))) return false;
  if (!expect_ok("cached min close", db.end())) return false;

  printf("c3db_cached_db_file_t minimum cache test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_reuse_object_test() {
  printf("c3db_cached_db_file_t reuse object test start\n");

  const char* base_a = "/sdcard/ccra";
  const char* dbf_a = "/sdcard/ccra.dbf";
  const char* base_b = "/sdcard/ccrb";
  const char* dbf_b = "/sdcard/ccrb.dbf";

  remove(dbf_a);
  remove(dbf_b);

  uint8_t pa[DBF_TEST_DATA_SIZE];
  uint8_t pb[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id_a = C3DB_NULL_ID;
  c3db_id_t id_b = C3DB_NULL_ID;

  fill_test_payload(pa, 0x84);
  fill_test_payload(pb, 0x94);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached reuse create a", db.create(base_a))) return false;
  if (!expect_ok("cached reuse append a", db.append(pa, id_a))) return false;
  if (!expect_ok("cached reuse close a", db.end())) return false;

  /*
   * end() must release cache ownership and reset transient state so the same
   * wrapper can safely be opened again for a different physical file.
   */
  if (!expect_ok("cached reuse create b", db.create(base_b))) return false;
  if (!expect_ok("cached reuse append b", db.append(pb, id_b))) return false;
  if (!expect_ok("cached reuse close b", db.end())) return false;

  c3db_db_file_t reopened(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached reuse reopen a", reopened.begin(base_a))) return false;
  if (!expect_ok("cached reuse select a", reopened.select(id_a, out))) return false;
  if (!expect_true("cached reuse payload a", payload_eq(out, pa))) return false;
  if (!expect_ok("cached reuse close reopened a", reopened.end())) return false;

  if (!expect_ok("cached reuse reopen b", reopened.begin(base_b))) return false;
  if (!expect_ok("cached reuse select b", reopened.select(id_b, out))) return false;
  if (!expect_true("cached reuse payload b", payload_eq(out, pb))) return false;
  if (!expect_ok("cached reuse close reopened b", reopened.end())) return false;

  printf("c3db_cached_db_file_t reuse object test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_api_errors_test() {
  printf("c3db_cached_db_file_t api errors test start\n");

  const char* base_name = "/sdcard/ccapi";
  const char* dbf_name = "/sdcard/ccapi.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;

  fill_test_payload(p0, 0xA4);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_err("cached api mode unopened", C3DB_FILE_NOT_OPEN_ERR, db.mode(C3DB_BULK_INSERT_MODE))) return false;
  if (!expect_err("cached api select unopened", C3DB_FILE_NOT_OPEN_ERR, db.select(id0, out))) return false;
  if (!expect_err("cached api append unopened", C3DB_FILE_NOT_OPEN_ERR, db.append(p0, id0))) return false;

  if (!expect_ok("cached api create", db.create(base_name))) return false;
  if (!expect_err("cached api create open", C3DB_FILE_ALREADY_OPEN_ERR, db.create(base_name))) return false;
  if (!expect_err("cached api append null", C3DB_INVALID_ARG_ERR, db.append(nullptr, id0))) return false;
  if (!expect_err("cached api insert null", C3DB_INVALID_ARG_ERR, db.insert(nullptr, id0))) return false;
  if (!expect_ok("cached api append", db.append(p0, id0))) return false;
  if (!expect_err("cached api select null", C3DB_INVALID_ARG_ERR, db.select(id0, nullptr))) return false;
  if (!expect_err("cached api update null", C3DB_INVALID_ARG_ERR, db.update(id0, nullptr))) return false;
  if (!expect_ok("cached api close", db.end())) return false;
  if (!expect_err("cached api close select", C3DB_FILE_NOT_OPEN_ERR, db.select(id0, out))) return false;

  printf("c3db_cached_db_file_t api errors test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_append_existing_test() {
  printf("c3db_cached_db_file_t append existing test start\n");

  const char* base_name = "/sdcard/ccext";
  const char* dbf_name = "/sdcard/ccext.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p2[DBF_TEST_DATA_SIZE];
  uint8_t p3[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t id2 = C3DB_NULL_ID;
  c3db_id_t id3 = C3DB_NULL_ID;

  fill_test_payload(p0, 0x15);
  fill_test_payload(p1, 0x25);
  fill_test_payload(p2, 0x35);
  fill_test_payload(p3, 0x45);

  c3db_db_file_t plain(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached ext plain create", plain.create(base_name))) return false;
  if (!expect_ok("cached ext plain append 0", plain.append(p0, id0))) return false;
  if (!expect_ok("cached ext plain append 1", plain.append(p1, id1))) return false;
  if (!expect_ok("cached ext plain close", plain.end())) return false;

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached ext begin", db.begin(base_name, false, C3DB_BULK_INSERT_MODE))) return false;

  /*
   * When appending to an existing DBF, the pending write-cache block starts at
   * the current physical record count, not at zero.
   */
  if (!expect_ok("cached ext append 2", db.append(p2, id2))) return false;
  if (!expect_ok("cached ext append 3", db.append(p3, id3))) return false;
  if (!expect_true("cached ext row 2", test_row(id2) == 2)) return false;
  if (!expect_true("cached ext row 3", test_row(id3) == 3)) return false;

  if (!expect_ok("cached ext select old 0", db.select(id0, out))) return false;
  if (!expect_true("cached ext payload old 0", payload_eq(out, p0))) return false;
  if (!expect_ok("cached ext select pending 2", db.select(id2, out))) return false;
  if (!expect_true("cached ext payload pending 2", payload_eq(out, p2))) return false;
  if (!expect_ok("cached ext close", db.end())) return false;

  c3db_db_file_t reopened(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached ext reopen", reopened.begin(base_name))) return false;
  if (!expect_ok("cached ext reopened old 1", reopened.select(id1, out))) return false;
  if (!expect_true("cached ext reopened payload 1", payload_eq(out, p1))) return false;
  if (!expect_ok("cached ext reopened new 3", reopened.select(id3, out))) return false;
  if (!expect_true("cached ext reopened payload 3", payload_eq(out, p3))) return false;
  if (!expect_ok("cached ext reopened close", reopened.end())) return false;

  printf("c3db_cached_db_file_t append existing test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_pending_insert_reuse_test() {
  printf("c3db_cached_db_file_t pending insert reuse test start\n");

  const char* base_name = "/sdcard/ccpins";
  const char* dbf_name = "/sdcard/ccpins.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t appended[DBF_TEST_DATA_SIZE];
  uint8_t inserted[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t append_id = C3DB_NULL_ID;
  c3db_id_t insert_id = C3DB_NULL_ID;

  fill_test_payload(p0, 0x16);
  fill_test_payload(p1, 0x26);
  fill_test_payload(appended, 0x36);
  fill_test_payload(inserted, 0x46);

  c3db_db_file_t plain(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached pins plain create", plain.create(base_name))) return false;
  if (!expect_ok("cached pins plain append 0", plain.append(p0, id0))) return false;
  if (!expect_ok("cached pins plain append 1", plain.append(p1, id1))) return false;
  if (!expect_ok("cached pins plain remove 0", plain.remove(id0))) return false;
  if (!expect_ok("cached pins plain close", plain.end())) return false;

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached pins begin", db.begin(base_name, false, C3DB_BULK_INSERT_MODE))) return false;
  if (!expect_ok("cached pins append pending", db.append(appended, append_id))) return false;

  /*
   * insert() commits the pending append first, then delegates to DBF. The
   * append must keep the physical end row while insert reuses the deleted row.
   */
  if (!expect_ok("cached pins insert reuse", db.insert(inserted, insert_id))) return false;
  if (!expect_true("cached pins append row", test_row(append_id) == 2)) return false;
  if (!expect_true("cached pins insert row", test_row(insert_id) == test_row(id0))) return false;
  if (!expect_true("cached pins insert cycle", test_cycle(insert_id) == test_cycle(id0) + 1)) return false;

  if (!expect_err("cached pins old hidden", C3DB_REC_NOT_FOUND_ERR, db.select(id0, out))) return false;
  if (!expect_ok("cached pins select append", db.select(append_id, out))) return false;
  if (!expect_true("cached pins append payload", payload_eq(out, appended))) return false;
  if (!expect_ok("cached pins select insert", db.select(insert_id, out))) return false;
  if (!expect_true("cached pins insert payload", payload_eq(out, inserted))) return false;
  if (!expect_ok("cached pins close", db.end())) return false;

  printf("c3db_cached_db_file_t pending insert reuse test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_history_reuse_test() {
  printf("c3db_cached_db_file_t history reuse test start\n");

  const char* base_name = "/sdcard/cchreu";
  const char* dbf_name = "/sdcard/cchreu.dbf";

  remove(dbf_name);

  uint8_t payloads[12][DBF_TEST_DATA_SIZE];
  uint8_t reused[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t ids[12];
  c3db_id_t reused_id = C3DB_NULL_ID;

  for (size_t i = 0; i < 12; ++i) {
    fill_test_payload(payloads[i], static_cast<uint8_t>(0x30 + i));
    ids[i] = C3DB_NULL_ID;
  }
  fill_test_payload(reused, 0xB5);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached hreu create", db.create(base_name, C3DB_BALANCED_MODE))) return false;

  for (size_t i = 0; i < 12; ++i) {
    if (!expect_ok("cached hreu append", db.append(payloads[i], ids[i]))) return false;
  }
  if (!expect_ok("cached hreu commit", db.commit())) return false;

  /*
   * These reads place id0 in the historical cache. remove() must invalidate
   * that historical copy, and insert() must not be shadowed by the old id.
   */
  if (!expect_ok("cached hreu select 0", db.select(ids[0], out))) return false;
  if (!expect_ok("cached hreu force history", db.select(ids[10], out))) return false;
  if (!expect_ok("cached hreu revisit 0", db.select(ids[0], out))) return false;
  if (!expect_true("cached hreu payload 0", payload_eq(out, payloads[0]))) return false;

  if (!expect_ok("cached hreu remove 0", db.remove(ids[0]))) return false;
  if (!expect_err("cached hreu old hidden", C3DB_REC_NOT_FOUND_ERR, db.select(ids[0], out))) return false;
  if (!expect_ok("cached hreu insert reuse", db.insert(reused, reused_id))) return false;
  if (!expect_true("cached hreu reused row", test_row(reused_id) == test_row(ids[0]))) return false;
  if (!expect_true("cached hreu reused cycle", test_cycle(reused_id) == test_cycle(ids[0]) + 1)) return false;
  if (!expect_ok("cached hreu select reused", db.select(reused_id, out))) return false;
  if (!expect_true("cached hreu reused payload", payload_eq(out, reused))) return false;
  if (!expect_ok("cached hreu close", db.end())) return false;

  printf("c3db_cached_db_file_t history reuse test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_open_failure_recovery_test() {
  printf("c3db_cached_db_file_t open failure recovery test start\n");

  const char* missing_base = "/sdcard/ccmiss";
  const char* missing_dbf = "/sdcard/ccmiss.dbf";
  const char* existing_base = "/sdcard/ccexist";
  const char* existing_dbf = "/sdcard/ccexist.dbf";

  remove(missing_dbf);
  remove(existing_dbf);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;

  fill_test_payload(p0, 0x57);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_err("cached fail missing begin", C3DB_FILE_OPEN_ERR, db.begin(missing_base))) return false;

  /*
   * begin() allocates cache memory before delegating to DBF. If opening fails,
   * that transient cache ownership must be released so the same object remains
   * usable.
   */
  if (!expect_ok("cached fail create after begin", db.create(missing_base))) return false;
  if (!expect_ok("cached fail append after begin", db.append(p0, id0))) return false;
  if (!expect_ok("cached fail close after begin", db.end())) return false;

  c3db_db_file_t plain(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached fail plain create", plain.create(existing_base))) return false;
  if (!expect_ok("cached fail plain close", plain.end())) return false;

  if (!expect_err("cached fail duplicate create", C3DB_FILE_ALREADY_EXISTS_ERR, db.create(existing_base))) return false;

  /*
   * create() has the same ownership pattern: cache allocation must be undone if
   * the physical file cannot be created.
   */
  if (!expect_ok("cached fail begin existing", db.begin(existing_base))) return false;
  if (!expect_ok("cached fail append existing", db.append(p0, id0))) return false;
  if (!expect_ok("cached fail close existing", db.end())) return false;

  if (!expect_ok("cached fail reopen missing", plain.begin(missing_base))) return false;
  if (!expect_ok("cached fail select missing", plain.select(id0, out))) return false;
  if (!expect_true("cached fail payload missing", payload_eq(out, p0))) return false;
  if (!expect_ok("cached fail close missing", plain.end())) return false;

  printf("c3db_cached_db_file_t open failure recovery test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_mode_repartition_test() {
  printf("c3db_cached_db_file_t mode repartition test start\n");

  const char* base_name = "/sdcard/ccrept";
  const char* dbf_name = "/sdcard/ccrept.dbf";

  remove(dbf_name);

  uint8_t payloads[10][DBF_TEST_DATA_SIZE];
  uint8_t pending[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t ids[10];
  c3db_id_t pending_id = C3DB_NULL_ID;
  size_t count = 0;

  for (size_t i = 0; i < 10; ++i) {
    fill_test_payload(payloads[i], static_cast<uint8_t>(0x40 + i));
    ids[i] = C3DB_NULL_ID;
  }
  fill_test_payload(pending, 0xC7);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached rept create", db.create(base_name, C3DB_BALANCED_MODE))) return false;

  for (size_t i = 0; i < 10; ++i) {
    if (!expect_ok("cached rept append", db.append(payloads[i], ids[i]))) return false;
  }
  if (!expect_ok("cached rept commit", db.commit())) return false;

  /*
   * These reads populate sequential/history state. Repartitioning must discard
   * transient cache contents safely while keeping the DBF-visible data intact.
   */
  if (!expect_ok("cached rept select 0", db.select(ids[0], out))) return false;
  if (!expect_ok("cached rept select 8", db.select(ids[8], out))) return false;
  if (!expect_ok("cached rept select 0 again", db.select(ids[0], out))) return false;

  if (!expect_ok("cached rept append pending", db.append(pending, pending_id))) return false;
  if (!expect_ok("cached rept mode bulk", db.mode(C3DB_BULK_INSERT_MODE))) return false;
  if (!expect_true("cached rept mode bulk value", db.mode() == C3DB_BULK_INSERT_MODE)) return false;
  if (!expect_ok("cached rept mode seq", db.mode(C3DB_SEQUENTIAL_ACCESS_MODE))) return false;
  if (!expect_true("cached rept mode seq value", db.mode() == C3DB_SEQUENTIAL_ACCESS_MODE)) return false;

  if (!expect_ok("cached rept max count", db.max_rec_count(count))) return false;
  if (!expect_true("cached rept count value", count == 11)) return false;
  if (!expect_ok("cached rept select old", db.select(ids[0], out))) return false;
  if (!expect_true("cached rept old payload", payload_eq(out, payloads[0]))) return false;
  if (!expect_ok("cached rept select pending", db.select(pending_id, out))) return false;
  if (!expect_true("cached rept pending payload", payload_eq(out, pending))) return false;
  if (!expect_ok("cached rept close", db.end())) return false;

  printf("c3db_cached_db_file_t mode repartition test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_file_pointer_io_test() {
  printf("c3db_cached_db_file_t FILE io test start\n");

  const char* export_base = "/sdcard/ccfexp";
  const char* export_dbf = "/sdcard/ccfexp.dbf";
  const char* export_raw = "/sdcard/ccfexp.raw";
  const char* import_base = "/sdcard/ccfimp";
  const char* import_dbf = "/sdcard/ccfimp.dbf";
  const char* import_raw = "/sdcard/ccfimp.raw";

  remove(export_dbf);
  remove(export_raw);
  remove(import_dbf);
  remove(import_raw);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p2[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t first_id = C3DB_NULL_ID;
  size_t rows = 0;

  fill_test_payload(p0, 0x18);
  fill_test_payload(p1, 0x28);
  fill_test_payload(p2, 0x38);

  c3db_cached_db_file_t db_export(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached fio export create", db_export.create(export_base, C3DB_BULK_INSERT_MODE))) return false;
  if (!expect_ok("cached fio export append", db_export.append(p0, id0))) return false;

  FILE* target = fopen(export_raw, "wb");
  if (!expect_true("cached fio export open raw", target != nullptr)) return false;
  c3db_err_t export_err = db_export.export_file(target, rows);
  fclose(target);
  if (!expect_ok("cached fio export file", export_err)) return false;
  if (!expect_true("cached fio export rows", rows == 1)) return false;
  if (!expect_ok("cached fio export close", db_export.end())) return false;

  FILE* exported = fopen(export_raw, "rb");
  if (!expect_true("cached fio export read raw", exported != nullptr)) return false;
  bool export_ok = fread(out, 1, DBF_TEST_DATA_SIZE, exported) == DBF_TEST_DATA_SIZE && payload_eq(out, p0);
  fclose(exported);
  if (!expect_true("cached fio export payload", export_ok)) return false;

  FILE* source = fopen(import_raw, "wb");
  if (!expect_true("cached fio import create raw", source != nullptr)) return false;
  bool raw_ok = fwrite(p1, 1, DBF_TEST_DATA_SIZE, source) == DBF_TEST_DATA_SIZE &&
                fwrite(p2, 1, DBF_TEST_DATA_SIZE, source) == DBF_TEST_DATA_SIZE;
  fclose(source);
  if (!expect_true("cached fio import write raw", raw_ok)) return false;

  source = fopen(import_raw, "rb");
  if (!expect_true("cached fio import open raw", source != nullptr)) return false;

  c3db_cached_db_file_t db_import(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached fio import create", db_import.create(import_base, C3DB_BULK_INSERT_MODE))) {
    fclose(source);
    return false;
  }
  c3db_err_t import_err = db_import.import_file(source, first_id, rows);
  fclose(source);
  if (!expect_ok("cached fio import file", import_err)) return false;
  if (!expect_true("cached fio import rows", rows == 2)) return false;
  if (!expect_ok("cached fio import select 0", db_import.select(first_id, out))) return false;
  if (!expect_true("cached fio import payload 0", payload_eq(out, p1))) return false;
  if (!expect_ok("cached fio import select 1", db_import.select(first_id + 1, out))) return false;
  if (!expect_true("cached fio import payload 1", payload_eq(out, p2))) return false;
  if (!expect_ok("cached fio import close", db_import.end())) return false;

  printf("c3db_cached_db_file_t FILE io test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_corrupt_record_test() {
  printf("c3db_cached_db_file_t corrupt record test start\n");

  const char* base_name = "/sdcard/cccor";
  const char* dbf_name = "/sdcard/cccor.dbf";
  static constexpr size_t DBF_HEADER_SIZE = 41;
  const size_t physical_size = 12 + (2 * (12 + DBF_TEST_DATA_SIZE));

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p2[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t id2 = C3DB_NULL_ID;

  fill_test_payload(p0, 0x19);
  fill_test_payload(p1, 0x29);
  fill_test_payload(p2, 0x39);

  c3db_db_file_t plain(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached corrupt plain create", plain.create(base_name))) return false;
  if (!expect_ok("cached corrupt plain append 0", plain.append(p0, id0))) return false;
  if (!expect_ok("cached corrupt plain append 1", plain.append(p1, id1))) return false;
  if (!expect_ok("cached corrupt plain append 2", plain.append(p2, id2))) return false;
  if (!expect_ok("cached corrupt plain close", plain.end())) return false;

  FILE* file = fopen(dbf_name, "r+b");
  if (!expect_true("cached corrupt open raw", file != nullptr)) return false;

  /*
   * Row 1 initially has only DATA_BLOCK[0] valid. Flipping one payload byte
   * invalidates that slot CRC and leaves no valid free/data group, so the row
   * becomes corrupt while neighbouring rows remain readable.
   */
  const long corrupt_offset = static_cast<long>(DBF_HEADER_SIZE + physical_size + 12 + 12);
  uint8_t byte = 0;
  bool corrupt_ok = fseek(file, corrupt_offset, SEEK_SET) == 0 &&
                    fread(&byte, 1, 1, file) == 1 &&
                    fseek(file, corrupt_offset, SEEK_SET) == 0;
  byte ^= 0x5Au;
  corrupt_ok = corrupt_ok && fwrite(&byte, 1, 1, file) == 1 && fflush(file) == 0;
  fclose(file);
  if (!expect_true("cached corrupt write raw", corrupt_ok)) return false;

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached corrupt begin", db.begin(base_name, false, C3DB_SEQUENTIAL_ACCESS_MODE))) return false;
  if (!expect_ok("cached corrupt select neighbour 0", db.select(id0, out))) return false;
  if (!expect_true("cached corrupt payload 0", payload_eq(out, p0))) return false;
  if (!expect_err("cached corrupt hidden", C3DB_REC_NOT_FOUND_ERR, db.select(id1, out))) return false;
  if (!expect_ok("cached corrupt select neighbour 2", db.select(id2, out))) return false;
  if (!expect_true("cached corrupt payload 2", payload_eq(out, p2))) return false;
  if (!expect_ok("cached corrupt close", db.end())) return false;

  printf("c3db_cached_db_file_t corrupt record test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_begin_file_pointer_test() {
  printf("c3db_cached_db_file_t begin FILE test start\n");

  const char* base_name = "/sdcard/ccbf";
  const char* dbf_name = "/sdcard/ccbf.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;

  fill_test_payload(p0, 0x1A);
  fill_test_payload(p1, 0x2A);

  c3db_db_file_t plain(DBF_TEST_DATA_SIZE);
  if (!expect_ok("cached bfile plain create", plain.create(base_name))) return false;
  if (!expect_ok("cached bfile plain append", plain.append(p0, id0))) return false;
  if (!expect_ok("cached bfile plain close", plain.end())) return false;

  FILE* file = fopen(dbf_name, "r+b");
  if (!expect_true("cached bfile open raw", file != nullptr)) return false;

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  c3db_err_t begin_err = db.begin(file, false, C3DB_BULK_INSERT_MODE);
  if (!expect_ok("cached bfile begin", begin_err)) {
    fclose(file);
    return false;
  }

  /*
   * The FILE* begin path must initialize cache memory exactly like the named
   * begin path. Otherwise appending would touch an unallocated write cache.
   */
  if (!expect_ok("cached bfile select old", db.select(id0, out))) {
    fclose(file);
    return false;
  }
  if (!expect_true("cached bfile old payload", payload_eq(out, p0))) {
    fclose(file);
    return false;
  }
  if (!expect_ok("cached bfile append", db.append(p1, id1))) {
    fclose(file);
    return false;
  }
  if (!expect_true("cached bfile append row", test_row(id1) == 1)) {
    fclose(file);
    return false;
  }
  if (!expect_ok("cached bfile close", db.end())) {
    fclose(file);
    return false;
  }
  fclose(file);

  if (!expect_ok("cached bfile reopen", plain.begin(base_name))) return false;
  if (!expect_ok("cached bfile reopened select 0", plain.select(id0, out))) return false;
  if (!expect_true("cached bfile reopened payload 0", payload_eq(out, p0))) return false;
  if (!expect_ok("cached bfile reopened select 1", plain.select(id1, out))) return false;
  if (!expect_true("cached bfile reopened payload 1", payload_eq(out, p1))) return false;
  if (!expect_ok("cached bfile reopened close", plain.end())) return false;

  printf("c3db_cached_db_file_t begin FILE test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_missing_id_test() {
  printf("c3db_cached_db_file_t missing id test start\n");

  const char* base_name = "/sdcard/ccmissid";
  const char* dbf_name = "/sdcard/ccmissid.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;

  fill_test_payload(p0, 0x1B);

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached missid create", db.create(base_name))) return false;
  if (!expect_ok("cached missid append", db.append(p0, id0))) return false;
  if (!expect_ok("cached missid commit", db.commit())) return false;

  /*
   * The cached layer must preserve DBF semantics: a row outside the persisted
   * range is a missing logical record, not a low-level EOF leak.
   */
  if (!expect_err("cached missid future row", C3DB_REC_NOT_FOUND_ERR, db.select(id0 + 10, out))) return false;
  if (!expect_ok("cached missid valid row", db.select(id0, out))) return false;
  if (!expect_true("cached missid valid payload", payload_eq(out, p0))) return false;
  if (!expect_ok("cached missid close", db.end())) return false;

  printf("c3db_cached_db_file_t missing id test OK\n");
  return true;
}

static bool run_c3db_cached_db_file_import_clears_read_cache_test() {
  printf("c3db_cached_db_file_t import clears read cache test start\n");

  const char* base_name = "/sdcard/cciclr";
  const char* dbf_name = "/sdcard/cciclr.dbf";
  const char* import_name = "/sdcard/cciclr.raw";

  remove(dbf_name);
  remove(import_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p2[DBF_TEST_DATA_SIZE];
  uint8_t p3[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t first_import_id = C3DB_NULL_ID;
  size_t rows_added = 0;

  fill_test_payload(p0, 0x1C);
  fill_test_payload(p1, 0x2C);
  fill_test_payload(p2, 0x3C);
  fill_test_payload(p3, 0x4C);

  FILE* imported = fopen(import_name, "wb");
  if (!expect_true("cached iclr create raw", imported != nullptr)) return false;
  bool raw_ok = fwrite(p2, 1, DBF_TEST_DATA_SIZE, imported) == DBF_TEST_DATA_SIZE &&
                fwrite(p3, 1, DBF_TEST_DATA_SIZE, imported) == DBF_TEST_DATA_SIZE;
  fclose(imported);
  if (!expect_true("cached iclr write raw", raw_ok)) return false;

  c3db_cached_db_file_t db(DBF_TEST_DATA_SIZE, CACHED_DBF_TEST_MEMORY);
  if (!expect_ok("cached iclr create", db.create(base_name, C3DB_SEQUENTIAL_ACCESS_MODE))) return false;
  if (!expect_ok("cached iclr append 0", db.append(p0, id0))) return false;
  if (!expect_ok("cached iclr append 1", db.append(p1, id1))) return false;
  if (!expect_ok("cached iclr commit", db.commit())) return false;

  /*
   * Populate the read caches before importing. import_file() appends physical
   * rows through DBF, so cached read windows must be cleared afterwards.
   */
  if (!expect_ok("cached iclr select 0", db.select(id0, out))) return false;
  if (!expect_ok("cached iclr import", db.import_file(import_name, first_import_id, rows_added))) return false;
  if (!expect_true("cached iclr rows added", rows_added == 2)) return false;
  if (!expect_true("cached iclr first row", test_row(first_import_id) == 2)) return false;

  if (!expect_ok("cached iclr select imported 0", db.select(first_import_id, out))) return false;
  if (!expect_true("cached iclr imported payload 0", payload_eq(out, p2))) return false;
  if (!expect_ok("cached iclr select imported 1", db.select(first_import_id + 1, out))) return false;
  if (!expect_true("cached iclr imported payload 1", payload_eq(out, p3))) return false;
  if (!expect_ok("cached iclr select old 1", db.select(id1, out))) return false;
  if (!expect_true("cached iclr old payload 1", payload_eq(out, p1))) return false;
  if (!expect_ok("cached iclr close", db.end())) return false;

  printf("c3db_cached_db_file_t import clears read cache test OK\n");
  return true;
}

bool run_c3db_cached_db_file_tests() {
  if (!run_c3db_cached_db_file_basic_test()) return false;
  if (!run_c3db_cached_db_file_update_remove_insert_test()) return false;
  if (!run_c3db_cached_db_file_mode_test()) return false;
  if (!run_c3db_cached_db_file_sequential_window_test()) return false;
  if (!run_c3db_cached_db_file_history_test()) return false;
  if (!run_c3db_cached_db_file_memory_limit_test()) return false;
  if (!run_c3db_cached_db_file_persisted_deleted_test()) return false;
  if (!run_c3db_cached_db_file_pending_count_test()) return false;
  if (!run_c3db_cached_db_file_export_commits_test()) return false;
  if (!run_c3db_cached_db_file_import_commits_test()) return false;
  if (!run_c3db_cached_db_file_write_cache_overflow_test()) return false;
  if (!run_c3db_cached_db_file_read_only_test()) return false;
  if (!run_c3db_cached_db_file_pending_mutation_test()) return false;
  if (!run_c3db_cached_db_file_minimum_cache_test()) return false;
  if (!run_c3db_cached_db_file_reuse_object_test()) return false;
  if (!run_c3db_cached_db_file_api_errors_test()) return false;
  if (!run_c3db_cached_db_file_append_existing_test()) return false;
  if (!run_c3db_cached_db_file_pending_insert_reuse_test()) return false;
  if (!run_c3db_cached_db_file_history_reuse_test()) return false;
  if (!run_c3db_cached_db_file_open_failure_recovery_test()) return false;
  if (!run_c3db_cached_db_file_mode_repartition_test()) return false;
  if (!run_c3db_cached_db_file_file_pointer_io_test()) return false;
  if (!run_c3db_cached_db_file_corrupt_record_test()) return false;
  if (!run_c3db_cached_db_file_begin_file_pointer_test()) return false;
  if (!run_c3db_cached_db_file_missing_id_test()) return false;
  if (!run_c3db_cached_db_file_import_clears_read_cache_test()) return false;
  return true;
}
