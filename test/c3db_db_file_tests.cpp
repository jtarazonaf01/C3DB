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
#include <cstring>
#include <stdio.h>

#include "c3db_config.h"
#include "c3db_data_file.h"
#include "c3db_db_file.h"
#include "c3db_utils.h"
#include "c3db_db_file_tests.h"

static constexpr size_t DBF_TEST_DATA_SIZE = 16;
static constexpr size_t DATA_FILE_TEST_DATA_SIZE = 64;
static constexpr size_t DATA_FILE_LARGE_TEST_SIZE = C3DB_SHARED_BUFFER_SIZE + 512;

static c3db_id_t make_test_id(uint32_t row_id, uint32_t cycle) {
  return (static_cast<c3db_id_t>(cycle) << 32) | row_id;
}

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

[[maybe_unused]] static void fill_data_file_payload(uint8_t* data, uint8_t seed) {
  for (size_t i = 0; i < DATA_FILE_TEST_DATA_SIZE; ++i) {
    data[i] = static_cast<uint8_t>(seed + (i * 3u));
  }
}

[[maybe_unused]] static bool data_file_payload_eq(const uint8_t* a, const uint8_t* b) {
  return std::memcmp(a, b, DATA_FILE_TEST_DATA_SIZE) == 0;
}

[[maybe_unused]] static void fill_payload(uint8_t* data, size_t size, uint8_t seed) {
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>(seed + (i * 7u) + (i >> 3));
  }
}

[[maybe_unused]] static bool payload_eq_size(const uint8_t* a, const uint8_t* b, size_t size) {
  return std::memcmp(a, b, size) == 0;
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

static bool read_raw_payload(FILE* file, uint8_t* data) {
  return fread(data, 1, DBF_TEST_DATA_SIZE, file) == DBF_TEST_DATA_SIZE;
}

static bool corrupt_dbf_free_ctrl(const char* dbf_name) {
  FILE* file = fopen(dbf_name, "r+");
  if (!file) return false;

  constexpr size_t magic_size = 4;
  constexpr size_t version_size = sizeof(uint32_t);
  constexpr size_t repair_size = sizeof(uint8_t);
  constexpr size_t ctrl_free_size = 4 * sizeof(uint32_t);
  constexpr size_t ctrl_free_offset = magic_size + version_size + repair_size;

  uint8_t zeros[2 * ctrl_free_size] = {};
  const bool ok = fseek(file, ctrl_free_offset, SEEK_SET) == 0 &&
                  fwrite(zeros, 1, sizeof(zeros), file) == sizeof(zeros) &&
                  fflush(file) == 0;
  fclose(file);
  return ok;
}

static size_t dbf_test_header_size() {
  constexpr size_t magic_size = 4;
  constexpr size_t version_size = sizeof(uint32_t);
  constexpr size_t repair_size = sizeof(uint8_t);
  constexpr size_t ctrl_free_size = 4 * sizeof(uint32_t);
  return magic_size + version_size + repair_size + (2 * ctrl_free_size);
}

static size_t dbf_test_physical_rec_size() {
  constexpr size_t delete_block_size = 3 * sizeof(uint32_t);
  constexpr size_t data_block_size = 3 * sizeof(uint32_t) + DBF_TEST_DATA_SIZE;
  return delete_block_size + (2 * data_block_size);
}

static size_t dbf_test_slot_offset(uint8_t slot) {
  constexpr size_t delete_block_size = 3 * sizeof(uint32_t);
  constexpr size_t data_block_size = 3 * sizeof(uint32_t) + DBF_TEST_DATA_SIZE;
  return delete_block_size + (slot * data_block_size);
}

static bool corrupt_dbf_slot_crc(const char* dbf_name, uint32_t row, uint8_t slot) {
  FILE* file = fopen(dbf_name, "r+");
  if (!file) return false;

  const size_t offset = dbf_test_header_size() +
                        (row * dbf_test_physical_rec_size()) +
                        dbf_test_slot_offset(slot);
  uint32_t bad_crc = 0;
  const bool ok = fseek(file, offset, SEEK_SET) == 0 &&
                  fwrite(&bad_crc, 1, sizeof(bad_crc), file) == sizeof(bad_crc) &&
                  fflush(file) == 0;
  fclose(file);
  return ok;
}
static bool run_c3db_db_file_basic_test() {
  printf("c3db_db_file_t basic test start\n");

  const char* base_name = "/sdcard/dbfbasic";
  const char* dbf_name = "/sdcard/dbfbasic.dbf";
  const char* export_name = "/sdcard/dbfexp.bin";
  const char* import_base_name = "/sdcard/dbfimp";
  const char* import_dbf_name = "/sdcard/dbfimp.dbf";

  remove(dbf_name);
  remove(export_name);
  remove(import_dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p2[DBF_TEST_DATA_SIZE];
  uint8_t p2_updated[DBF_TEST_DATA_SIZE];
  uint8_t p3[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];

  fill_test_payload(p0, 0x10);
  fill_test_payload(p1, 0x20);
  fill_test_payload(p2, 0x30);
  fill_test_payload(p2_updated, 0x31);
  fill_test_payload(p3, 0x40);

  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t id2 = C3DB_NULL_ID;
  c3db_id_t id3 = C3DB_NULL_ID;

  c3db_db_file_t db(DBF_TEST_DATA_SIZE);
  if (!expect_ok("dbf create", db.create(base_name))) return false;
  if (!expect_ok("dbf append 0", db.append(p0, id0))) return false;
  if (!expect_ok("dbf append 1", db.append(p1, id1))) return false;
  if (!expect_ok("dbf append 2", db.append(p2, id2))) return false;

  if (!expect_ok("dbf select 0", db.select(id0, out))) return false;
  if (!expect_true("dbf select 0 payload", payload_eq(out, p0))) return false;

  if (!expect_ok("dbf update 2", db.update(id2, p2_updated))) return false;
  if (!expect_ok("dbf select updated 2", db.select(id2, out))) return false;
  if (!expect_true("dbf updated payload", payload_eq(out, p2_updated))) return false;

  if (!expect_ok("dbf remove 1", db.remove(id1))) return false;
  if (!expect_err("dbf select removed 1", C3DB_REC_NOT_FOUND_ERR, db.select(id1, out))) return false;

  if (!expect_ok("dbf insert reuse", db.insert(p3, id3))) return false;
  if (!expect_true("dbf insert reused row", test_row(id3) == test_row(id1))) return false;
  if (!expect_true("dbf insert advanced cycle", test_cycle(id3) == test_cycle(id1) + 1)) return false;
  if (!expect_ok("dbf select reused", db.select(id3, out))) return false;
  if (!expect_true("dbf reused payload", payload_eq(out, p3))) return false;

  size_t max_count = 0;
  if (!expect_ok("dbf max_rec_count", db.max_rec_count(max_count))) return false;
  if (!expect_true("dbf max_rec_count value", max_count == 3)) return false;

  size_t rows_exported = 0;
  if (!expect_ok("dbf export", db.export_file(export_name, rows_exported))) return false;
  if (!expect_true("dbf export count", rows_exported == 3)) return false;
  if (!expect_ok("dbf close", db.end())) return false;

  FILE* exported = fopen(export_name, "rb");
  if (!expect_true("dbf open export", exported != nullptr)) return false;
  bool export_ok = read_raw_payload(exported, out) && payload_eq(out, p0) &&
                   read_raw_payload(exported, out) && payload_eq(out, p3) &&
                   read_raw_payload(exported, out) && payload_eq(out, p2_updated);
  fclose(exported);
  if (!expect_true("dbf export payloads", export_ok)) return false;

  c3db_db_file_t imported(DBF_TEST_DATA_SIZE);
  c3db_id_t first_id = C3DB_NULL_ID;
  size_t rows_added = 0;
  if (!expect_ok("dbf import create", imported.create(import_base_name))) return false;
  if (!expect_ok("dbf import", imported.import_file(export_name, first_id, rows_added))) return false;
  if (!expect_true("dbf import first id", first_id == make_test_id(0, 0))) return false;
  if (!expect_true("dbf import rows", rows_added == 3)) return false;
  if (!expect_ok("dbf import select 0", imported.select(make_test_id(0, 0), out))) return false;
  if (!expect_true("dbf import payload 0", payload_eq(out, p0))) return false;
  if (!expect_ok("dbf import select 1", imported.select(make_test_id(1, 0), out))) return false;
  if (!expect_true("dbf import payload 1", payload_eq(out, p3))) return false;
  if (!expect_ok("dbf import select 2", imported.select(make_test_id(2, 0), out))) return false;
  if (!expect_true("dbf import payload 2", payload_eq(out, p2_updated))) return false;
  if (!expect_ok("dbf import close", imported.end())) return false;

  c3db_db_file_t repair_db(DBF_TEST_DATA_SIZE);
  if (!expect_ok("dbf repair", repair_db.repair_free_list(base_name))) return false;
  printf("c3db_db_file_t basic test OK\n");
  return true;
}

static bool run_c3db_db_file_edge_test() {
  printf("c3db_db_file_t edge test start\n");

  const char* base_name = "/sdcard/dbfedge";
  const char* dbf_name = "/sdcard/dbfedge.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p2[DBF_TEST_DATA_SIZE];
  uint8_t p3[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];

  fill_test_payload(p0, 0x50);
  fill_test_payload(p1, 0x60);
  fill_test_payload(p2, 0x70);
  fill_test_payload(p3, 0x80);

  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t id2 = C3DB_NULL_ID;
  c3db_id_t id3 = C3DB_NULL_ID;

  c3db_db_file_t db(DBF_TEST_DATA_SIZE);
  if (!expect_ok("edge create", db.create(base_name))) return false;
  if (!expect_ok("edge append 0", db.append(p0, id0))) return false;
  if (!expect_ok("edge append 1", db.append(p1, id1))) return false;
  if (!expect_ok("edge append 2", db.append(p2, id2))) return false;

  if (!expect_ok("edge remove 1", db.remove(id1))) return false;
  if (!expect_err("edge double remove", C3DB_REC_NOT_FOUND_ERR, db.remove(id1))) return false;
  if (!expect_err("edge update removed", C3DB_REC_NOT_FOUND_ERR, db.update(id1, p3))) return false;

  if (!expect_ok("edge insert reuse", db.insert(p3, id3))) return false;
  if (!expect_true("edge reused row", test_row(id3) == test_row(id1))) return false;
  if (!expect_true("edge reused cycle", test_cycle(id3) == test_cycle(id1) + 1)) return false;
  if (!expect_err("edge old id invalid", C3DB_REC_NOT_FOUND_ERR, db.select(id1, out))) return false;
  if (!expect_ok("edge new id valid", db.select(id3, out))) return false;
  if (!expect_true("edge new id payload", payload_eq(out, p3))) return false;

  size_t max_count = 0;
  if (!expect_ok("edge max_rec_count before close", db.max_rec_count(max_count))) return false;
  if (!expect_true("edge max_rec_count before close value", max_count == 3)) return false;
  if (!expect_ok("edge close", db.end())) return false;

  c3db_db_file_t reopened(DBF_TEST_DATA_SIZE);
  if (!expect_ok("edge reopen", reopened.begin(base_name))) return false;
  if (!expect_ok("edge reopened select 0", reopened.select(id0, out))) return false;
  if (!expect_true("edge reopened payload 0", payload_eq(out, p0))) return false;
  if (!expect_ok("edge reopened select 2", reopened.select(id2, out))) return false;
  if (!expect_true("edge reopened payload 2", payload_eq(out, p2))) return false;
  if (!expect_ok("edge reopened select 3", reopened.select(id3, out))) return false;
  if (!expect_true("edge reopened payload 3", payload_eq(out, p3))) return false;

  if (!expect_ok("edge reopened max_rec_count", reopened.max_rec_count(max_count))) return false;
  if (!expect_true("edge reopened max_rec_count value", max_count == 3)) return false;
  if (!expect_ok("edge reopened close", reopened.end())) return false;

  c3db_db_file_t repair_db(DBF_TEST_DATA_SIZE);
  if (!expect_ok("edge repair", repair_db.repair_free_list(base_name))) return false;

  printf("c3db_db_file_t edge test OK\n");
  return true;
}

static bool run_c3db_db_file_free_list_repair_test() {
  printf("c3db_db_file_t free-list repair test start\n");

  const char* base_name = "/sdcard/dbfrep";
  const char* dbf_name = "/sdcard/dbfrep.dbf";

  remove(dbf_name);

  uint8_t payloads[5][DBF_TEST_DATA_SIZE];
  uint8_t inserted[2][DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t ids[5] = {};
  c3db_id_t reused0 = C3DB_NULL_ID;
  c3db_id_t reused1 = C3DB_NULL_ID;

  for (uint8_t i = 0; i < 5; ++i) fill_test_payload(payloads[i], 0x90 + (i * 0x10));
  fill_test_payload(inserted[0], 0xE0);
  fill_test_payload(inserted[1], 0xF0);

  c3db_db_file_t db(DBF_TEST_DATA_SIZE);
  if (!expect_ok("repair create", db.create(base_name))) return false;

  for (size_t i = 0; i < 5; ++i) {
    char label[] = "repair append 0";
    label[14] = static_cast<char>('0' + i);
    if (!expect_ok(label, db.append(payloads[i], ids[i]))) return false;
  }

  if (!expect_ok("repair remove 1", db.remove(ids[1]))) return false;
  if (!expect_ok("repair remove 3", db.remove(ids[3]))) return false;

  size_t max_count = 0;
  if (!expect_ok("repair max count after removes", db.max_rec_count(max_count))) return false;
  if (!expect_true("repair max count after removes value", max_count == 3)) return false;

  if (!expect_ok("repair close before repair", db.end())) return false;

  c3db_db_file_t repair_db(DBF_TEST_DATA_SIZE);
  if (!expect_ok("repair rebuild", repair_db.repair_free_list(base_name))) return false;

  c3db_db_file_t reopened(DBF_TEST_DATA_SIZE);
  if (!expect_ok("repair reopen", reopened.begin(base_name))) return false;
  if (!expect_true("repair flag cleared", !reopened.free_list_repair_needed())) return false;
  if (!expect_ok("repair max count after repair", reopened.max_rec_count(max_count))) return false;
  if (!expect_true("repair max count after repair value", max_count == 3)) return false;

  if (!expect_ok("repair insert reused 0", reopened.insert(inserted[0], reused0))) return false;
  if (!expect_ok("repair insert reused 1", reopened.insert(inserted[1], reused1))) return false;
  if (!expect_true("repair reused row 0", test_row(reused0) == test_row(ids[1]) || test_row(reused0) == test_row(ids[3]))) return false;
  if (!expect_true("repair reused row 1", test_row(reused1) == test_row(ids[1]) || test_row(reused1) == test_row(ids[3]))) return false;
  if (!expect_true("repair reused rows differ", test_row(reused0) != test_row(reused1))) return false;
  if (!expect_true("repair reused cycle 0", test_cycle(reused0) == 1)) return false;
  if (!expect_true("repair reused cycle 1", test_cycle(reused1) == 1)) return false;

  if (!expect_ok("repair select reused 0", reopened.select(reused0, out))) return false;
  if (!expect_true("repair payload reused 0", payload_eq(out, inserted[0]))) return false;
  if (!expect_ok("repair select reused 1", reopened.select(reused1, out))) return false;
  if (!expect_true("repair payload reused 1", payload_eq(out, inserted[1]))) return false;

  if (!expect_ok("repair max count full", reopened.max_rec_count(max_count))) return false;
  if (!expect_true("repair max count full value", max_count == 5)) return false;
  if (!expect_ok("repair final close", reopened.end())) return false;

  printf("c3db_db_file_t free-list repair test OK\n");
  return true;
}

static bool run_c3db_db_file_repair_warning_test() {
  printf("c3db_db_file_t repair warning test start\n");

  const char* base_name = "/sdcard/dbfwrn";
  const char* dbf_name = "/sdcard/dbfwrn.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t p2[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t id2 = C3DB_NULL_ID;

  fill_test_payload(p0, 0x11);
  fill_test_payload(p1, 0x22);
  fill_test_payload(p2, 0x33);

  c3db_db_file_t db(DBF_TEST_DATA_SIZE);
  if (!expect_ok("warning create", db.create(base_name))) return false;
  if (!expect_ok("warning append 0", db.append(p0, id0))) return false;
  if (!expect_ok("warning append 1", db.append(p1, id1))) return false;
  if (!expect_ok("warning append 2", db.append(p2, id2))) return false;
  if (!expect_ok("warning remove 1", db.remove(id1))) return false;
  if (!expect_ok("warning close before corrupt", db.end())) return false;

  if (!expect_true("warning corrupt ctrl", corrupt_dbf_free_ctrl(dbf_name))) return false;

  c3db_db_file_t reopened(DBF_TEST_DATA_SIZE);
  if (!expect_err("warning begin", C3DB_FREE_LIST_REPAIR_WRN, reopened.begin(base_name))) return false;
  if (!expect_true("warning flag", reopened.free_list_repair_needed())) return false;

  size_t max_count = 0;
  if (!expect_err("warning max_rec_count", C3DB_FREE_LIST_REPAIR_WRN, reopened.max_rec_count(max_count))) return false;
  if (!expect_true("warning max_rec_count value", max_count == 3)) return false;
  if (!expect_ok("warning select 0", reopened.select(id0, out))) return false;
  if (!expect_true("warning payload 0", payload_eq(out, p0))) return false;
  if (!expect_ok("warning select 2", reopened.select(id2, out))) return false;
  if (!expect_true("warning payload 2", payload_eq(out, p2))) return false;
  if (!expect_err("warning removed still hidden", C3DB_REC_NOT_FOUND_ERR, reopened.select(id1, out))) return false;
  if (!expect_ok("warning close", reopened.end())) return false;

  c3db_db_file_t repair_db(DBF_TEST_DATA_SIZE);
  if (!expect_ok("warning repair", repair_db.repair_free_list(base_name))) return false;

  c3db_db_file_t repaired(DBF_TEST_DATA_SIZE);
  if (!expect_ok("warning repaired begin", repaired.begin(base_name))) return false;
  if (!expect_true("warning repaired flag", !repaired.free_list_repair_needed())) return false;
  if (!expect_ok("warning repaired max_rec_count", repaired.max_rec_count(max_count))) return false;
  if (!expect_true("warning repaired max_rec_count value", max_count == 2)) return false;
  if (!expect_ok("warning repaired close", repaired.end())) return false;

  printf("c3db_db_file_t repair warning test OK\n");
  return true;
}

static bool run_c3db_db_file_record_recovery_test() {
  printf("c3db_db_file_t record recovery test start\n");

  const char* base_name = "/sdcard/dbfrec";
  const char* dbf_name = "/sdcard/dbfrec.dbf";

  remove(dbf_name);

  uint8_t p0[DBF_TEST_DATA_SIZE];
  uint8_t p0_updated[DBF_TEST_DATA_SIZE];
  uint8_t p1[DBF_TEST_DATA_SIZE];
  uint8_t out[DBF_TEST_DATA_SIZE];
  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;

  fill_test_payload(p0, 0x12);
  fill_test_payload(p0_updated, 0x13);
  fill_test_payload(p1, 0x24);

  c3db_db_file_t db(DBF_TEST_DATA_SIZE);
  if (!expect_ok("record create", db.create(base_name))) return false;
  if (!expect_ok("record append 0", db.append(p0, id0))) return false;
  if (!expect_ok("record append 1", db.append(p1, id1))) return false;
  if (!expect_ok("record update 0", db.update(id0, p0_updated))) return false;
  if (!expect_ok("record close before corrupt active", db.end())) return false;

  if (!expect_true("record corrupt active slot", corrupt_dbf_slot_crc(dbf_name, test_row(id0), 1))) return false;

  c3db_db_file_t reopened(DBF_TEST_DATA_SIZE);
  if (!expect_ok("record reopen after active corrupt", reopened.begin(base_name))) return false;
  if (!expect_ok("record select fallback slot", reopened.select(id0, out))) return false;
  if (!expect_true("record fallback payload", payload_eq(out, p0))) return false;
  if (!expect_ok("record close fallback", reopened.end())) return false;

  c3db_db_file_t db2(DBF_TEST_DATA_SIZE);
  if (!expect_ok("record reopen for delete", db2.begin(base_name))) return false;
  if (!expect_ok("record remove 1", db2.remove(id1))) return false;
  if (!expect_ok("record close before corrupt deleted", db2.end())) return false;

  if (!expect_true("record corrupt deleted slot", corrupt_dbf_slot_crc(dbf_name, test_row(id1), 0))) return false;

  c3db_db_file_t reopened2(DBF_TEST_DATA_SIZE);
  if (!expect_ok("record reopen after deleted slot corrupt", reopened2.begin(base_name))) return false;
  if (!expect_err("record deleted remains hidden", C3DB_REC_NOT_FOUND_ERR, reopened2.select(id1, out))) return false;
  if (!expect_ok("record close final", reopened2.end())) return false;

  printf("c3db_db_file_t record recovery test OK\n");
  return true;
}

bool run_c3db_db_file_tests() {
  if (!run_c3db_db_file_basic_test()) return false;
  if (!run_c3db_db_file_edge_test()) return false;
  if (!run_c3db_db_file_free_list_repair_test()) return false;
  if (!run_c3db_db_file_repair_warning_test()) return false;
  if (!run_c3db_db_file_record_recovery_test()) return false;
  return true;
}
