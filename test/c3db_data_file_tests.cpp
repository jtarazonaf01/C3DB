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
#include "c3db_data_file_tests.h"

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

[[maybe_unused]] static void fill_test_payload(uint8_t* data, uint8_t seed) {
  for (size_t i = 0; i < DBF_TEST_DATA_SIZE; ++i) data[i] = seed + i;
}

[[maybe_unused]] static bool payload_eq(const uint8_t* a, const uint8_t* b) {
  return std::memcmp(a, b, DBF_TEST_DATA_SIZE) == 0;
}

static void fill_data_file_payload(uint8_t* data, uint8_t seed) {
  for (size_t i = 0; i < DATA_FILE_TEST_DATA_SIZE; ++i) {
    data[i] = static_cast<uint8_t>(seed + (i * 3u));
  }
}

static bool data_file_payload_eq(const uint8_t* a, const uint8_t* b) {
  return std::memcmp(a, b, DATA_FILE_TEST_DATA_SIZE) == 0;
}

static void fill_payload(uint8_t* data, size_t size, uint8_t seed) {
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>(seed + (i * 7u) + (i >> 3));
  }
}

static bool payload_eq_size(const uint8_t* a, const uint8_t* b, size_t size) {
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

[[maybe_unused]] static bool read_raw_payload(FILE* file, uint8_t* data) {
  return fread(data, 1, DBF_TEST_DATA_SIZE, file) == DBF_TEST_DATA_SIZE;
}

[[maybe_unused]] static bool corrupt_dbf_free_ctrl(const char* dbf_name) {
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

[[maybe_unused]] static bool corrupt_dbf_slot_crc(const char* dbf_name, uint32_t row, uint8_t slot) {
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
static bool run_c3db_data_file_basic_test() {
  printf("c3db_data_file_t basic test start\n");

  const char* base_name = "/sdcard/datbasic";
  const char* met_name = "/sdcard/datbasic.met";
  const char* dat_name = "/sdcard/datbasic.dat";
  const char* met_dfg_name = "/sdcard/datbasic.mdf";
  const char* dat_dfg_name = "/sdcard/datbasic.ddf";
  const char* met_bak_name = "/sdcard/datbasic.mbk";
  const char* dat_bak_name = "/sdcard/datbasic.dbk";

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);

  uint8_t p0[DATA_FILE_TEST_DATA_SIZE];
  uint8_t p1[DATA_FILE_TEST_DATA_SIZE];
  uint8_t p1_updated[DATA_FILE_TEST_DATA_SIZE];
  uint8_t p2[DATA_FILE_TEST_DATA_SIZE];
  uint8_t out[DATA_FILE_TEST_DATA_SIZE];

  fill_data_file_payload(p0, 0x10);
  fill_data_file_payload(p1, 0x40);
  fill_data_file_payload(p1_updated, 0x41);
  fill_data_file_payload(p2, 0x70);

  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t id2 = C3DB_NULL_ID;

  c3db_data_file_t data_file(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data create", data_file.create(base_name))) return false;
  if (!expect_ok("data append 0", data_file.append(p0, id0))) return false;
  if (!expect_ok("data append 1", data_file.append(p1, id1))) return false;

  if (!expect_ok("data select 0", data_file.select(id0, out))) return false;
  if (!expect_true("data select 0 payload", data_file_payload_eq(out, p0))) return false;

  if (!expect_ok("data update 1", data_file.update(id1, p1_updated))) return false;
  if (!expect_ok("data select updated 1", data_file.select(id1, out))) return false;
  if (!expect_true("data updated payload", data_file_payload_eq(out, p1_updated))) return false;

  if (!expect_ok("data remove 0", data_file.remove(id0))) return false;
  if (!expect_err("data select removed 0", C3DB_REC_NOT_FOUND_ERR, data_file.select(id0, out))) return false;

  /*
   * insert() debe reutilizar la fila .met borrada y publicar una nueva versión
   * lógica; los .dat asociados a esa fila quedan bajo control del nuevo id.
   */
  if (!expect_ok("data insert reuse", data_file.insert(p2, id2))) return false;
  if (!expect_true("data insert reused row", test_row(id2) == test_row(id0))) return false;
  if (!expect_true("data insert advanced cycle", test_cycle(id2) == test_cycle(id0) + 1)) return false;
  if (!expect_ok("data select reused", data_file.select(id2, out))) return false;
  if (!expect_true("data reused payload", data_file_payload_eq(out, p2))) return false;
  if (!expect_ok("data close", data_file.end())) return false;

  /*
   * La reapertura comprueba que .met y .dat quedan publicados de forma
   * persistente, no solo visibles por estado en memoria.
   */
  c3db_data_file_t reopened(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data reopen", reopened.begin(base_name))) return false;
  if (!expect_err("data old id hidden", C3DB_REC_NOT_FOUND_ERR, reopened.select(id0, out))) return false;
  if (!expect_ok("data reopened select 1", reopened.select(id1, out))) return false;
  if (!expect_true("data reopened payload 1", data_file_payload_eq(out, p1_updated))) return false;
  if (!expect_ok("data reopened select 2", reopened.select(id2, out))) return false;
  if (!expect_true("data reopened payload 2", data_file_payload_eq(out, p2))) return false;
  if (!expect_ok("data reopened close", reopened.end())) return false;

  printf("c3db_data_file_t basic test OK\n");
  return true;
}

static bool read_data_file_payload(FILE* file, uint8_t* data) {
  return fread(data, 1, DATA_FILE_TEST_DATA_SIZE, file) == DATA_FILE_TEST_DATA_SIZE;
}

static bool file_size_is(const char* file_name, size_t expected_size) {
  FILE* file = fopen(file_name, "rb");
  if (!file) return false;

  const bool ok = fseek(file, 0, SEEK_END) == 0 &&
                  ftell(file) == static_cast<long>(expected_size);
  fclose(file);
  return ok;
}

static bool file_exists(const char* file_name) {
  FILE* file = fopen(file_name, "rb");
  if (!file) return false;
  fclose(file);
  return true;
}

static bool rename_test_file(const char* old_name, const char* new_name) {
  return rename(old_name, new_name) == 0;
}

static bool create_test_file(const char* file_name, const uint8_t* data, size_t size) {
  FILE* file = fopen(file_name, "wb");
  if (!file) return false;

  const bool ok = size == 0 ||
                  fwrite(data, 1, size, file) == size;
  const bool close_ok = fclose(file) == 0;
  return ok && close_ok;
}

static bool corrupt_file_byte(const char* file_name, size_t offset) {
  FILE* file = fopen(file_name, "r+");
  if (!file) return false;

  uint8_t value = 0;
  const bool read_ok = fseek(file, offset, SEEK_SET) == 0 &&
                       fread(&value, 1, sizeof(value), file) == sizeof(value);
  value ^= 0x5Au;
  const bool write_ok = read_ok &&
                        fseek(file, offset, SEEK_SET) == 0 &&
                        fwrite(&value, 1, sizeof(value), file) == sizeof(value) &&
                        fflush(file) == 0;
  fclose(file);
  return write_ok;
}

static bool run_c3db_data_file_import_export_test() {
  printf("c3db_data_file_t import/export test start\n");

  const char* base_name = "/sdcard/datexp";
  const char* met_name = "/sdcard/datexp.met";
  const char* dat_name = "/sdcard/datexp.dat";
  const char* export_name = "/sdcard/datexp.bin";
  const char* import_base_name = "/sdcard/datimp";
  const char* import_met_name = "/sdcard/datimp.met";
  const char* import_dat_name = "/sdcard/datimp.dat";

  remove(met_name);
  remove(dat_name);
  remove(export_name);
  remove(import_met_name);
  remove(import_dat_name);

  uint8_t p0[DATA_FILE_TEST_DATA_SIZE];
  uint8_t p1[DATA_FILE_TEST_DATA_SIZE];
  uint8_t p1_updated[DATA_FILE_TEST_DATA_SIZE];
  uint8_t p2[DATA_FILE_TEST_DATA_SIZE];
  uint8_t out[DATA_FILE_TEST_DATA_SIZE];

  fill_data_file_payload(p0, 0x20);
  fill_data_file_payload(p1, 0x50);
  fill_data_file_payload(p1_updated, 0x51);
  fill_data_file_payload(p2, 0x80);

  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t id2 = C3DB_NULL_ID;

  c3db_data_file_t data_file(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data exp create", data_file.create(base_name))) return false;
  if (!expect_ok("data exp append 0", data_file.append(p0, id0))) return false;
  if (!expect_ok("data exp append 1", data_file.append(p1, id1))) return false;
  if (!expect_ok("data exp append 2", data_file.append(p2, id2))) return false;
  if (!expect_ok("data exp update 1", data_file.update(id1, p1_updated))) return false;
  if (!expect_ok("data exp remove 0", data_file.remove(id0))) return false;

  size_t rows_exported = 0;
  if (!expect_ok("data export", data_file.export_file(export_name, rows_exported))) return false;
  if (!expect_true("data export count", rows_exported == 2)) return false;
  if (!expect_ok("data exp close", data_file.end())) return false;

  /*
   * El fichero exportado debe contener solo payloads activos. No arrastra ids,
   * ciclos ni huecos de .met; import_file reconstruye metadatos nuevos.
   */
  FILE* exported = fopen(export_name, "rb");
  if (!expect_true("data open export", exported != nullptr)) return false;
  bool export_ok = read_data_file_payload(exported, out) && data_file_payload_eq(out, p1_updated) &&
                   read_data_file_payload(exported, out) && data_file_payload_eq(out, p2);
  fclose(exported);
  if (!expect_true("data export payloads", export_ok)) return false;

  c3db_data_file_t imported(DATA_FILE_TEST_DATA_SIZE);
  c3db_id_t first_id = C3DB_NULL_ID;
  size_t rows_added = 0;
  if (!expect_ok("data import create", imported.create(import_base_name))) return false;
  if (!expect_ok("data import", imported.import_file(export_name, first_id, rows_added))) return false;
  if (!expect_true("data import first id", first_id == make_test_id(0, 0))) return false;
  if (!expect_true("data import rows", rows_added == 2)) return false;
  if (!expect_ok("data import select 0", imported.select(make_test_id(0, 0), out))) return false;
  if (!expect_true("data import payload 0", data_file_payload_eq(out, p1_updated))) return false;
  if (!expect_ok("data import select 1", imported.select(make_test_id(1, 0), out))) return false;
  if (!expect_true("data import payload 1", data_file_payload_eq(out, p2))) return false;
  if (!expect_ok("data import close", imported.end())) return false;

  printf("c3db_data_file_t import/export test OK\n");
  return true;
}

static bool run_c3db_data_file_defrag_test() {
  printf("c3db_data_file_t defrag test start\n");

  const char* base_name = "/sdcard/datdfg";
  const char* met_name = "/sdcard/datdfg.met";
  const char* dat_name = "/sdcard/datdfg.dat";
  const char* met_dfg_name = "/sdcard/datdfg.mdf";
  const char* dat_dfg_name = "/sdcard/datdfg.ddf";
  const char* met_bak_name = "/sdcard/datdfg.mbk";
  const char* dat_bak_name = "/sdcard/datdfg.dbk";

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);

  uint8_t p0[DATA_FILE_TEST_DATA_SIZE];
  uint8_t p1[DATA_FILE_TEST_DATA_SIZE];
  uint8_t p1_updated[DATA_FILE_TEST_DATA_SIZE];
  uint8_t p2[DATA_FILE_TEST_DATA_SIZE];
  uint8_t p3[DATA_FILE_TEST_DATA_SIZE];
  uint8_t out[DATA_FILE_TEST_DATA_SIZE];

  fill_data_file_payload(p0, 0x11);
  fill_data_file_payload(p1, 0x31);
  fill_data_file_payload(p1_updated, 0x32);
  fill_data_file_payload(p2, 0x51);
  fill_data_file_payload(p3, 0x71);

  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;
  c3db_id_t id2 = C3DB_NULL_ID;
  c3db_id_t id3 = C3DB_NULL_ID;

  c3db_data_file_t data_file(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data dfg create", data_file.create(base_name))) return false;
  if (!expect_ok("data dfg append 0", data_file.append(p0, id0))) return false;
  if (!expect_ok("data dfg append 1", data_file.append(p1, id1))) return false;
  if (!expect_ok("data dfg append 2", data_file.append(p2, id2))) return false;
  if (!expect_ok("data dfg update 1", data_file.update(id1, p1_updated))) return false;
  if (!expect_ok("data dfg remove 0", data_file.remove(id0))) return false;
  if (!expect_ok("data dfg insert reuse", data_file.insert(p3, id3))) return false;

  if (!expect_true("data dfg reused row", test_row(id3) == test_row(id0))) return false;
  if (!expect_true("data dfg pre size", file_size_is(dat_name, 4 * DATA_FILE_TEST_DATA_SIZE))) return false;

  /*
   * defrag() compacta .dat, pero no compacta .met: los ids publicados deben
   * seguir funcionando exactamente igual después de reescribir las referencias.
   */
  if (!expect_ok("data defrag", data_file.defrag())) return false;
  if (!expect_true("data dfg compact size", file_size_is(dat_name, 3 * DATA_FILE_TEST_DATA_SIZE))) return false;
  if (!expect_err("data dfg old id hidden", C3DB_REC_NOT_FOUND_ERR, data_file.select(id0, out))) return false;
  if (!expect_ok("data dfg select reused", data_file.select(id3, out))) return false;
  if (!expect_true("data dfg payload reused", data_file_payload_eq(out, p3))) return false;
  if (!expect_ok("data dfg select updated", data_file.select(id1, out))) return false;
  if (!expect_true("data dfg payload updated", data_file_payload_eq(out, p1_updated))) return false;
  if (!expect_ok("data dfg select 2", data_file.select(id2, out))) return false;
  if (!expect_true("data dfg payload 2", data_file_payload_eq(out, p2))) return false;
  if (!expect_ok("data dfg close", data_file.end())) return false;

  c3db_data_file_t reopened(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data dfg reopen", reopened.begin(base_name))) return false;
  if (!expect_ok("data dfg reopened select reused", reopened.select(id3, out))) return false;
  if (!expect_true("data dfg reopened payload reused", data_file_payload_eq(out, p3))) return false;
  if (!expect_ok("data dfg reopened select updated", reopened.select(id1, out))) return false;
  if (!expect_true("data dfg reopened payload updated", data_file_payload_eq(out, p1_updated))) return false;
  if (!expect_ok("data dfg reopened close", reopened.end())) return false;

  printf("c3db_data_file_t defrag test OK\n");
  return true;
}

static bool run_c3db_data_file_crc_fallback_test() {
  printf("c3db_data_file_t CRC fallback test start\n");

  const char* base_name = "/sdcard/datcrc";
  const char* met_name = "/sdcard/datcrc.met";
  const char* dat_name = "/sdcard/datcrc.dat";
  const char* met_dfg_name = "/sdcard/datcrc.mdf";
  const char* dat_dfg_name = "/sdcard/datcrc.ddf";
  const char* met_bak_name = "/sdcard/datcrc.mbk";
  const char* dat_bak_name = "/sdcard/datcrc.dbk";

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);

  uint8_t original[DATA_FILE_TEST_DATA_SIZE];
  uint8_t updated[DATA_FILE_TEST_DATA_SIZE];
  uint8_t out[DATA_FILE_TEST_DATA_SIZE];

  fill_data_file_payload(original, 0x25);
  fill_data_file_payload(updated, 0xA5);

  c3db_id_t id = C3DB_NULL_ID;

  c3db_data_file_t data_file(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data crc create", data_file.create(base_name))) return false;
  if (!expect_ok("data crc append", data_file.append(original, id))) return false;
  if (!expect_ok("data crc update", data_file.update(id, updated))) return false;
  if (!expect_ok("data crc close before corrupt", data_file.end())) return false;

  /*
   * En esta secuencia la fila .dat 0 contiene el payload inicial y la fila 1
   * contiene el payload publicado por update(). Corromper la fila activa debe
   * hacer que select() caiga a spare, no que devuelva bytes dudosos.
   */
  if (!expect_true("data crc corrupt active", corrupt_file_byte(dat_name, DATA_FILE_TEST_DATA_SIZE))) return false;

  c3db_data_file_t reopened(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data crc reopen", reopened.begin(base_name))) return false;
  if (!expect_ok("data crc select fallback", reopened.select(id, out))) return false;
  if (!expect_true("data crc fallback payload", data_file_payload_eq(out, original))) return false;
  if (!expect_ok("data crc close", reopened.end())) return false;

  printf("c3db_data_file_t CRC fallback test OK\n");
  return true;
}

static bool run_c3db_data_file_crc_corrupt_test() {
  printf("c3db_data_file_t CRC corrupt test start\n");

  const char* base_name = "/sdcard/datbad";
  const char* met_name = "/sdcard/datbad.met";
  const char* dat_name = "/sdcard/datbad.dat";
  const char* met_dfg_name = "/sdcard/datbad.mdf";
  const char* dat_dfg_name = "/sdcard/datbad.ddf";
  const char* met_bak_name = "/sdcard/datbad.mbk";
  const char* dat_bak_name = "/sdcard/datbad.dbk";

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);

  uint8_t payload[DATA_FILE_TEST_DATA_SIZE];
  uint8_t out[DATA_FILE_TEST_DATA_SIZE];
  fill_data_file_payload(payload, 0xC0);

  c3db_id_t id = C3DB_NULL_ID;

  c3db_data_file_t data_file(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data bad create", data_file.create(base_name))) return false;
  if (!expect_ok("data bad append", data_file.append(payload, id))) return false;
  if (!expect_ok("data bad close before corrupt", data_file.end())) return false;

  /*
   * Tras un append no hay spare válido. Si el único payload publicado no pasa
   * el CRC, la fila no puede darse por buena.
   */
  if (!expect_true("data bad corrupt active", corrupt_file_byte(dat_name, 0))) return false;

  c3db_data_file_t reopened(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data bad reopen", reopened.begin(base_name))) return false;
  if (!expect_err("data bad select corrupt", C3DB_REC_CORRUPT_ERR, reopened.select(id, out))) return false;
  if (!expect_ok("data bad close", reopened.end())) return false;

  printf("c3db_data_file_t CRC corrupt test OK\n");
  return true;
}

static bool run_c3db_data_file_defrag_recovery_test() {
  printf("c3db_data_file_t defrag recovery test start\n");

  const char* base_name = "/sdcard/datrcv";
  const char* met_name = "/sdcard/datrcv.met";
  const char* dat_name = "/sdcard/datrcv.dat";
  const char* met_dfg_name = "/sdcard/datrcv.mdf";
  const char* dat_dfg_name = "/sdcard/datrcv.ddf";
  const char* met_bak_name = "/sdcard/datrcv.mbk";
  const char* dat_bak_name = "/sdcard/datrcv.dbk";

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);

  uint8_t p0[DATA_FILE_TEST_DATA_SIZE];
  uint8_t p1[DATA_FILE_TEST_DATA_SIZE];
  uint8_t out[DATA_FILE_TEST_DATA_SIZE];

  fill_data_file_payload(p0, 0x18);
  fill_data_file_payload(p1, 0x48);

  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;

  c3db_data_file_t data_file(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data rcv create", data_file.create(base_name))) return false;
  if (!expect_ok("data rcv append 0", data_file.append(p0, id0))) return false;
  if (!expect_ok("data rcv append 1", data_file.append(p1, id1))) return false;
  if (!expect_ok("data rcv close before simulate", data_file.end())) return false;

  /*
   * Simula un reset justo después de que defrag haya movido la pareja activa a
   * backup, pero antes de publicar la pareja compactada. begin() debe restaurar
   * ambos backups como pareja activa antes de abrir la base.
   */
  if (!expect_true("data rcv rename met backup", rename_test_file(met_name, met_bak_name))) return false;
  if (!expect_true("data rcv rename dat backup", rename_test_file(dat_name, dat_bak_name))) return false;

  c3db_data_file_t recovered(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data rcv begin", recovered.begin(base_name))) return false;
  if (!expect_true("data rcv met restored", file_exists(met_name))) return false;
  if (!expect_true("data rcv dat restored", file_exists(dat_name))) return false;
  if (!expect_true("data rcv met backup removed", !file_exists(met_bak_name))) return false;
  if (!expect_true("data rcv dat backup removed", !file_exists(dat_bak_name))) return false;
  if (!expect_ok("data rcv select 0", recovered.select(id0, out))) return false;
  if (!expect_true("data rcv payload 0", data_file_payload_eq(out, p0))) return false;
  if (!expect_ok("data rcv select 1", recovered.select(id1, out))) return false;
  if (!expect_true("data rcv payload 1", data_file_payload_eq(out, p1))) return false;
  if (!expect_ok("data rcv close", recovered.end())) return false;

  printf("c3db_data_file_t defrag recovery test OK\n");
  return true;
}

static bool run_c3db_data_file_defrag_partial_recovery_test() {
  printf("c3db_data_file_t defrag partial recovery test start\n");

  uint8_t payload[DATA_FILE_TEST_DATA_SIZE];
  uint8_t out[DATA_FILE_TEST_DATA_SIZE];
  fill_data_file_payload(payload, 0x58);

  auto prepare_base = [&](const char* base_name, const char* met_name, const char* dat_name,
                          const char* met_dfg_name, const char* dat_dfg_name,
                          const char* met_bak_name, const char* dat_bak_name,
                          c3db_id_t &id) -> bool {
    remove(met_name);
    remove(dat_name);
    remove(met_dfg_name);
    remove(dat_dfg_name);
    remove(met_bak_name);
    remove(dat_bak_name);

    c3db_data_file_t data_file(DATA_FILE_TEST_DATA_SIZE);
    if (!expect_ok("data partial create", data_file.create(base_name))) return false;
    if (!expect_ok("data partial append", data_file.append(payload, id))) return false;
    return expect_ok("data partial close", data_file.end());
  };

  {
    const char* base_name = "/sdcard/datrm";
    const char* met_name = "/sdcard/datrm.met";
    const char* dat_name = "/sdcard/datrm.dat";
    const char* met_dfg_name = "/sdcard/datrm.mdf";
    const char* dat_dfg_name = "/sdcard/datrm.ddf";
    const char* met_bak_name = "/sdcard/datrm.mbk";
    const char* dat_bak_name = "/sdcard/datrm.dbk";
    c3db_id_t id = C3DB_NULL_ID;

    if (!prepare_base(base_name, met_name, dat_name, met_dfg_name, dat_dfg_name, met_bak_name, dat_bak_name, id)) {
      return false;
    }

    /*
     * Simula reset tras mover solo .met a backup. .dat sigue siendo la copia
     * activa, así que begin() debe restaurar únicamente .met desde .mbk.
     */
    if (!expect_true("data partial rename met", rename_test_file(met_name, met_bak_name))) return false;

    c3db_data_file_t recovered(DATA_FILE_TEST_DATA_SIZE);
    if (!expect_ok("data partial met begin", recovered.begin(base_name))) return false;
    if (!expect_true("data partial met restored", file_exists(met_name))) return false;
    if (!expect_true("data partial met backup removed", !file_exists(met_bak_name))) return false;
    if (!expect_ok("data partial met select", recovered.select(id, out))) return false;
    if (!expect_true("data partial met payload", data_file_payload_eq(out, payload))) return false;
    if (!expect_ok("data partial met close", recovered.end())) return false;
  }

  {
    const char* base_name = "/sdcard/datrd";
    const char* met_name = "/sdcard/datrd.met";
    const char* dat_name = "/sdcard/datrd.dat";
    const char* met_dfg_name = "/sdcard/datrd.mdf";
    const char* dat_dfg_name = "/sdcard/datrd.ddf";
    const char* met_bak_name = "/sdcard/datrd.mbk";
    const char* dat_bak_name = "/sdcard/datrd.dbk";
    c3db_id_t id = C3DB_NULL_ID;

    if (!prepare_base(base_name, met_name, dat_name, met_dfg_name, dat_dfg_name, met_bak_name, dat_bak_name, id)) {
      return false;
    }

    /*
     * Simula reset tras mover solo .dat a backup. .met sigue activa y begin()
     * debe restaurar la parte de datos desde .dbk antes de abrir.
     */
    if (!expect_true("data partial rename dat", rename_test_file(dat_name, dat_bak_name))) return false;

    c3db_data_file_t recovered(DATA_FILE_TEST_DATA_SIZE);
    if (!expect_ok("data partial dat begin", recovered.begin(base_name))) return false;
    if (!expect_true("data partial dat restored", file_exists(dat_name))) return false;
    if (!expect_true("data partial dat backup removed", !file_exists(dat_bak_name))) return false;
    if (!expect_ok("data partial dat select", recovered.select(id, out))) return false;
    if (!expect_true("data partial dat payload", data_file_payload_eq(out, payload))) return false;
    if (!expect_ok("data partial dat close", recovered.end())) return false;
  }

  printf("c3db_data_file_t defrag partial recovery test OK\n");
  return true;
}

static bool run_c3db_data_file_defrag_temp_cleanup_test() {
  printf("c3db_data_file_t defrag temp cleanup test start\n");

  const char* base_name = "/sdcard/datcln";
  const char* met_name = "/sdcard/datcln.met";
  const char* dat_name = "/sdcard/datcln.dat";
  const char* met_dfg_name = "/sdcard/datcln.mdf";
  const char* dat_dfg_name = "/sdcard/datcln.ddf";
  const char* met_bak_name = "/sdcard/datcln.mbk";
  const char* dat_bak_name = "/sdcard/datcln.dbk";

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);

  uint8_t payload[DATA_FILE_TEST_DATA_SIZE];
  uint8_t temp_payload[DATA_FILE_TEST_DATA_SIZE];
  uint8_t out[DATA_FILE_TEST_DATA_SIZE];
  fill_data_file_payload(payload, 0x62);
  fill_data_file_payload(temp_payload, 0x92);

  c3db_id_t id = C3DB_NULL_ID;

  c3db_data_file_t data_file(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data cln create", data_file.create(base_name))) return false;
  if (!expect_ok("data cln append", data_file.append(payload, id))) return false;
  if (!expect_ok("data cln close", data_file.end())) return false;

  if (!expect_true("data cln create mdf", create_test_file(met_dfg_name, temp_payload, sizeof(temp_payload)))) return false;
  if (!expect_true("data cln create ddf", create_test_file(dat_dfg_name, temp_payload, sizeof(temp_payload)))) return false;

  /*
   * Si la pareja activa .met/.dat existe completa, los temporales .mdf/.ddf no
   * deben competir con ella. begin() los descarta antes de abrir la base.
   */
  c3db_data_file_t reopened(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data cln begin", reopened.begin(base_name))) return false;
  if (!expect_true("data cln mdf removed", !file_exists(met_dfg_name))) return false;
  if (!expect_true("data cln ddf removed", !file_exists(dat_dfg_name))) return false;
  if (!expect_ok("data cln select", reopened.select(id, out))) return false;
  if (!expect_true("data cln payload", data_file_payload_eq(out, payload))) return false;
  if (!expect_ok("data cln reopened close", reopened.end())) return false;

  printf("c3db_data_file_t defrag temp cleanup test OK\n");
  return true;
}

static bool run_c3db_data_file_read_only_test() {
  printf("c3db_data_file_t read-only test start\n");

  const char* base_name = "/sdcard/datro";
  const char* met_name = "/sdcard/datro.met";
  const char* dat_name = "/sdcard/datro.dat";
  const char* met_dfg_name = "/sdcard/datro.mdf";
  const char* dat_dfg_name = "/sdcard/datro.ddf";
  const char* met_bak_name = "/sdcard/datro.mbk";
  const char* dat_bak_name = "/sdcard/datro.dbk";

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);

  uint8_t payload[DATA_FILE_TEST_DATA_SIZE];
  uint8_t updated[DATA_FILE_TEST_DATA_SIZE];
  uint8_t out[DATA_FILE_TEST_DATA_SIZE];

  fill_data_file_payload(payload, 0x36);
  fill_data_file_payload(updated, 0x66);

  c3db_id_t id = C3DB_NULL_ID;
  c3db_id_t new_id = C3DB_NULL_ID;

  c3db_data_file_t data_file(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data ro create", data_file.create(base_name))) return false;
  if (!expect_ok("data ro append initial", data_file.append(payload, id))) return false;
  if (!expect_ok("data ro close initial", data_file.end())) return false;

  c3db_data_file_t read_only(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data ro begin", read_only.begin(base_name, true))) return false;
  if (!expect_ok("data ro select", read_only.select(id, out))) return false;
  if (!expect_true("data ro payload", data_file_payload_eq(out, payload))) return false;

  /*
   * begin(read_only=true) abre .met y .dat sin permiso de escritura. Las
   * operaciones que publicarían metadatos o modificarían .dat deben fallar
   * antes de tocar disco.
   */
  if (!expect_err("data ro append", C3DB_READ_ONLY_ERR, read_only.append(updated, new_id))) return false;
  if (!expect_err("data ro insert", C3DB_READ_ONLY_ERR, read_only.insert(updated, new_id))) return false;
  if (!expect_err("data ro update", C3DB_READ_ONLY_ERR, read_only.update(id, updated))) return false;
  if (!expect_err("data ro remove", C3DB_READ_ONLY_ERR, read_only.remove(id))) return false;
  if (!expect_err("data ro defrag", C3DB_READ_ONLY_ERR, read_only.defrag())) return false;
  if (!expect_ok("data ro close", read_only.end())) return false;

  c3db_data_file_t reopened(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data ro reopen", reopened.begin(base_name))) return false;
  if (!expect_ok("data ro verify select", reopened.select(id, out))) return false;
  if (!expect_true("data ro verify payload", data_file_payload_eq(out, payload))) return false;
  if (!expect_ok("data ro verify close", reopened.end())) return false;

  printf("c3db_data_file_t read-only test OK\n");
  return true;
}

static bool run_c3db_data_file_large_payload_test() {
  printf("c3db_data_file_t large payload test start\n");

  const char* base_name = "/sdcard/datbig";
  const char* met_name = "/sdcard/datbig.met";
  const char* dat_name = "/sdcard/datbig.dat";
  const char* met_dfg_name = "/sdcard/datbig.mdf";
  const char* dat_dfg_name = "/sdcard/datbig.ddf";
  const char* met_bak_name = "/sdcard/datbig.mbk";
  const char* dat_bak_name = "/sdcard/datbig.dbk";
  const char* export_name = "/sdcard/datbig.bin";
  const char* import_base_name = "/sdcard/datbgi";
  const char* import_met_name = "/sdcard/datbgi.met";
  const char* import_dat_name = "/sdcard/datbgi.dat";

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);
  remove(export_name);
  remove(import_met_name);
  remove(import_dat_name);

  static uint8_t p0[DATA_FILE_LARGE_TEST_SIZE];
  static uint8_t p1[DATA_FILE_LARGE_TEST_SIZE];
  static uint8_t p0_updated[DATA_FILE_LARGE_TEST_SIZE];
  static uint8_t out[DATA_FILE_LARGE_TEST_SIZE];

  fill_payload(p0, DATA_FILE_LARGE_TEST_SIZE, 0x13);
  fill_payload(p1, DATA_FILE_LARGE_TEST_SIZE, 0x43);
  fill_payload(p0_updated, DATA_FILE_LARGE_TEST_SIZE, 0x73);

  c3db_id_t id0 = C3DB_NULL_ID;
  c3db_id_t id1 = C3DB_NULL_ID;

  c3db_data_file_t data_file(DATA_FILE_LARGE_TEST_SIZE);
  if (!expect_ok("data big create", data_file.create(base_name))) return false;
  if (!expect_ok("data big append 0", data_file.append(p0, id0))) return false;
  if (!expect_ok("data big append 1", data_file.append(p1, id1))) return false;
  if (!expect_ok("data big update 0", data_file.update(id0, p0_updated))) return false;
  if (!expect_ok("data big remove 1", data_file.remove(id1))) return false;

  /*
   * Con payload mayor que C3DB_SHARED_BUFFER_SIZE, export y defrag no pueden
   * apoyarse en una fila completa en RAM: deben copiar la fila por chunks.
   */
  if (!expect_ok("data big defrag", data_file.defrag())) return false;
  if (!expect_true("data big compact size", file_size_is(dat_name, DATA_FILE_LARGE_TEST_SIZE))) return false;
  if (!expect_ok("data big select 0", data_file.select(id0, out))) return false;
  if (!expect_true("data big payload 0", payload_eq_size(out, p0_updated, DATA_FILE_LARGE_TEST_SIZE))) return false;
  if (!expect_err("data big removed hidden", C3DB_REC_NOT_FOUND_ERR, data_file.select(id1, out))) return false;

  size_t rows_exported = 0;
  if (!expect_ok("data big export", data_file.export_file(export_name, rows_exported))) return false;
  if (!expect_true("data big export count", rows_exported == 1)) return false;
  if (!expect_true("data big export size", file_size_is(export_name, DATA_FILE_LARGE_TEST_SIZE))) return false;
  if (!expect_ok("data big close", data_file.end())) return false;

  c3db_data_file_t imported(DATA_FILE_LARGE_TEST_SIZE);
  c3db_id_t first_id = C3DB_NULL_ID;
  size_t rows_added = 0;
  if (!expect_ok("data big import create", imported.create(import_base_name))) return false;
  if (!expect_ok("data big import", imported.import_file(export_name, first_id, rows_added))) return false;
  if (!expect_true("data big import first id", first_id == make_test_id(0, 0))) return false;
  if (!expect_true("data big import rows", rows_added == 1)) return false;
  if (!expect_ok("data big import select", imported.select(first_id, out))) return false;
  if (!expect_true("data big import payload", payload_eq_size(out, p0_updated, DATA_FILE_LARGE_TEST_SIZE))) return false;
  if (!expect_ok("data big import close", imported.end())) return false;

  printf("c3db_data_file_t large payload test OK\n");
  return true;
}

static bool run_c3db_data_file_import_edge_test() {
  printf("c3db_data_file_t import edge test start\n");

  const char* empty_file_name = "/sdcard/datemp.bin";
  const char* bad_file_name = "/sdcard/datbad.bin";
  const char* base_name = "/sdcard/datied";
  const char* met_name = "/sdcard/datied.met";
  const char* dat_name = "/sdcard/datied.dat";
  const char* met_dfg_name = "/sdcard/datied.mdf";
  const char* dat_dfg_name = "/sdcard/datied.ddf";
  const char* met_bak_name = "/sdcard/datied.mbk";
  const char* dat_bak_name = "/sdcard/datied.dbk";

  remove(empty_file_name);
  remove(bad_file_name);
  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);

  uint8_t partial[DATA_FILE_TEST_DATA_SIZE / 2];
  fill_payload(partial, sizeof(partial), 0x91);

  if (!expect_true("data import edge create empty file", create_test_file(empty_file_name, nullptr, 0))) return false;
  if (!expect_true("data import edge create bad file", create_test_file(bad_file_name, partial, sizeof(partial)))) return false;

  c3db_data_file_t data_file(DATA_FILE_TEST_DATA_SIZE);
  c3db_id_t first_id = C3DB_NULL_ID;
  size_t rows_added = 123;

  if (!expect_ok("data import edge create", data_file.create(base_name))) return false;
  if (!expect_ok("data import empty", data_file.import_file(empty_file_name, first_id, rows_added))) return false;
  if (!expect_true("data import empty first id", first_id == C3DB_NULL_ID)) return false;
  if (!expect_true("data import empty rows", rows_added == 0)) return false;
  if (!expect_err("data import bad size", C3DB_FILE_SIZE_ERR, data_file.import_file(bad_file_name, first_id, rows_added))) {
    return false;
  }
  if (!expect_ok("data import edge close", data_file.end())) return false;

  printf("c3db_data_file_t import edge test OK\n");
  return true;
}

static bool run_c3db_data_file_invalid_args_test() {
  printf("c3db_data_file_t invalid args test start\n");

  const char* base_name = "/sdcard/datinv";
  const char* met_name = "/sdcard/datinv.met";
  const char* dat_name = "/sdcard/datinv.dat";
  const char* met_dfg_name = "/sdcard/datinv.mdf";
  const char* dat_dfg_name = "/sdcard/datinv.ddf";
  const char* met_bak_name = "/sdcard/datinv.mbk";
  const char* dat_bak_name = "/sdcard/datinv.dbk";

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);

  uint8_t payload[DATA_FILE_TEST_DATA_SIZE];
  uint8_t out[DATA_FILE_TEST_DATA_SIZE];
  fill_data_file_payload(payload, 0x22);

  c3db_id_t id = C3DB_NULL_ID;
  size_t rows = 0;

  c3db_data_file_t zero_size(0);
  if (!expect_err("data inv zero create", C3DB_INVALID_ARG_ERR, zero_size.create(base_name))) return false;
  if (!expect_err("data inv zero begin", C3DB_INVALID_ARG_ERR, zero_size.begin(base_name))) return false;

  c3db_data_file_t unopened(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_err("data inv unopened append", C3DB_FILE_NOT_OPEN_ERR, unopened.append(payload, id))) return false;
  if (!expect_err("data inv unopened select", C3DB_FILE_NOT_OPEN_ERR, unopened.select(id, out))) return false;
  if (!expect_err("data inv unopened update", C3DB_FILE_NOT_OPEN_ERR, unopened.update(id, payload))) return false;
  if (!expect_err("data inv unopened defrag", C3DB_FILE_NOT_OPEN_ERR, unopened.defrag())) return false;

  c3db_data_file_t data_file(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data inv create", data_file.create(base_name))) return false;
  if (!expect_err("data inv append null", C3DB_INVALID_ARG_ERR, data_file.append(nullptr, id))) return false;
  if (!expect_ok("data inv append", data_file.append(payload, id))) return false;
  if (!expect_err("data inv select null", C3DB_INVALID_ARG_ERR, data_file.select(id, nullptr))) return false;
  if (!expect_err("data inv update null", C3DB_INVALID_ARG_ERR, data_file.update(id, nullptr))) return false;
  if (!expect_err("data inv import null file", C3DB_INVALID_ARG_ERR, data_file.import_file(static_cast<FILE*>(nullptr), id, rows))) {
    return false;
  }
  if (!expect_err("data inv import null name", C3DB_FILE_BAD_NAME_ERR, data_file.import_file(static_cast<const char*>(nullptr), id, rows))) {
    return false;
  }
  if (!expect_err("data inv import empty name", C3DB_FILE_BAD_NAME_ERR, data_file.import_file("", id, rows))) return false;
  if (!expect_err("data inv export null file", C3DB_INVALID_ARG_ERR, data_file.export_file(static_cast<FILE*>(nullptr), rows))) {
    return false;
  }
  if (!expect_err("data inv export null name", C3DB_FILE_BAD_NAME_ERR, data_file.export_file(static_cast<const char*>(nullptr), rows))) {
    return false;
  }
  if (!expect_err("data inv export empty name", C3DB_FILE_BAD_NAME_ERR, data_file.export_file("", rows))) return false;
  if (!expect_ok("data inv close", data_file.end())) return false;

  printf("c3db_data_file_t invalid args test OK\n");
  return true;
}

static bool run_c3db_data_file_reuse_stress_test() {
  printf("c3db_data_file_t reuse stress test start\n");

  const char* base_name = "/sdcard/datstr";
  const char* met_name = "/sdcard/datstr.met";
  const char* dat_name = "/sdcard/datstr.dat";
  const char* met_dfg_name = "/sdcard/datstr.mdf";
  const char* dat_dfg_name = "/sdcard/datstr.ddf";
  const char* met_bak_name = "/sdcard/datstr.mbk";
  const char* dat_bak_name = "/sdcard/datstr.dbk";

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);

  static constexpr size_t ROWS = 8;
  uint8_t payloads[ROWS][DATA_FILE_TEST_DATA_SIZE];
  uint8_t inserted[4][DATA_FILE_TEST_DATA_SIZE];
  uint8_t updated[4][DATA_FILE_TEST_DATA_SIZE];
  uint8_t out[DATA_FILE_TEST_DATA_SIZE];
  c3db_id_t ids[ROWS] = {};
  c3db_id_t reused[4] = {};

  for (size_t i = 0; i < ROWS; ++i) fill_data_file_payload(payloads[i], static_cast<uint8_t>(0x10 + (i * 0x10)));
  for (size_t i = 0; i < 4; ++i) {
    fill_data_file_payload(inserted[i], static_cast<uint8_t>(0xA0 + (i * 0x10)));
    fill_data_file_payload(updated[i], static_cast<uint8_t>(0xE0 + (i * 0x04)));
  }

  c3db_data_file_t data_file(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data stress create", data_file.create(base_name))) return false;

  for (size_t i = 0; i < ROWS; ++i) {
    if (!expect_ok("data stress append", data_file.append(payloads[i], ids[i]))) return false;
    if (!expect_true("data stress append row", test_row(ids[i]) == i)) return false;
    if (!expect_true("data stress append cycle", test_cycle(ids[i]) == 0)) return false;
  }

  const size_t removed_rows[4] = {1, 3, 5, 7};
  for (size_t i = 0; i < 4; ++i) {
    if (!expect_ok("data stress remove", data_file.remove(ids[removed_rows[i]]))) return false;
  }

  /*
   * La lista libre es LIFO. Tras borrar 1,3,5,7, las inserciones deben reutilizar
   * 7,5,3,1 y avanzar el ciclo lógico de cada fila.
   */
  const size_t expected_reused_rows[4] = {7, 5, 3, 1};
  for (size_t i = 0; i < 4; ++i) {
    if (!expect_ok("data stress insert", data_file.insert(inserted[i], reused[i]))) return false;
    if (!expect_true("data stress reused row", test_row(reused[i]) == expected_reused_rows[i])) return false;
    if (!expect_true("data stress reused cycle", test_cycle(reused[i]) == 1)) return false;
  }

  for (size_t i = 0; i < 4; ++i) {
    if (!expect_ok("data stress update reused", data_file.update(reused[i], updated[i]))) return false;
  }

  if (!expect_ok("data stress close", data_file.end())) return false;

  c3db_data_file_t reopened(DATA_FILE_TEST_DATA_SIZE);
  if (!expect_ok("data stress reopen", reopened.begin(base_name))) return false;

  for (size_t i = 0; i < 4; ++i) {
    if (!expect_err("data stress old id hidden", C3DB_REC_NOT_FOUND_ERR, reopened.select(ids[removed_rows[i]], out))) {
      return false;
    }
    if (!expect_ok("data stress select reused", reopened.select(reused[i], out))) return false;
    if (!expect_true("data stress reused payload", data_file_payload_eq(out, updated[i]))) return false;
  }

  for (size_t i = 0; i < ROWS; i += 2) {
    if (!expect_ok("data stress select original", reopened.select(ids[i], out))) return false;
    if (!expect_true("data stress original payload", data_file_payload_eq(out, payloads[i]))) return false;
  }

  if (!expect_ok("data stress close reopened", reopened.end())) return false;

  printf("c3db_data_file_t reuse stress test OK\n");
  return true;
}

bool run_c3db_data_file_tests() {
  if (!run_c3db_data_file_basic_test()) return false;
  if (!run_c3db_data_file_import_export_test()) return false;
  if (!run_c3db_data_file_defrag_test()) return false;
  if (!run_c3db_data_file_crc_fallback_test()) return false;
  if (!run_c3db_data_file_crc_corrupt_test()) return false;
  if (!run_c3db_data_file_defrag_recovery_test()) return false;
  if (!run_c3db_data_file_defrag_partial_recovery_test()) return false;
  if (!run_c3db_data_file_defrag_temp_cleanup_test()) return false;
  if (!run_c3db_data_file_read_only_test()) return false;
  if (!run_c3db_data_file_large_payload_test()) return false;
  if (!run_c3db_data_file_import_edge_test()) return false;
  if (!run_c3db_data_file_invalid_args_test()) return false;
  if (!run_c3db_data_file_reuse_stress_test()) return false;
  return true;
}
