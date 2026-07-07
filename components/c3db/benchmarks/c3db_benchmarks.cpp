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

#include "c3db_benchmarks.h"

#include <stdio.h>
#include <unistd.h>

#include "esp_timer.h"

#include "c3db_data_file.h"
#include "c3db_db_file.h"
#include "c3db_file.h"
#include "c3db_index.h"

static void print_scaled_3(uint64_t value_x1000) {
  printf("%llu.%03llu",
         (unsigned long long)(value_x1000 / 1000),
         (unsigned long long)(value_x1000 % 1000));
}

static void print_read_result(const char* name, size_t block_size, size_t reads, int64_t elapsed_us) {
  if (elapsed_us <= 0) elapsed_us = 1;

  const uint64_t total_bytes = static_cast<uint64_t>(block_size) * reads;
  const uint64_t ms_per_read_x1000 = static_cast<uint64_t>(elapsed_us) / reads;
  const uint64_t bytes_per_ms_x1000 = (total_bytes * 1000000ULL) / static_cast<uint64_t>(elapsed_us);

  printf("%s block=%u reads=%u elapsed_us=%lld ms/read=",
         name, (unsigned)block_size, (unsigned)reads, (long long)elapsed_us);
  print_scaled_3(ms_per_read_x1000);
  printf(" bytes/ms=");
  print_scaled_3(bytes_per_ms_x1000);
  printf("\n");
}

static void print_write_result(const char* name, size_t block_size, size_t writes, int64_t elapsed_us) {
  if (elapsed_us <= 0) elapsed_us = 1;

  const uint64_t total_bytes = static_cast<uint64_t>(block_size) * writes;
  const uint64_t ms_per_write_x1000 = static_cast<uint64_t>(elapsed_us) / writes;
  const uint64_t bytes_per_ms_x1000 = (total_bytes * 1000000ULL) / static_cast<uint64_t>(elapsed_us);

  printf("%s block=%u writes=%u elapsed_us=%lld ms/write=",
         name, (unsigned)block_size, (unsigned)writes, (long long)elapsed_us);
  print_scaled_3(ms_per_write_x1000);
  printf(" bytes/ms=");
  print_scaled_3(bytes_per_ms_x1000);
  printf("\n");
}

static void print_index_result(const char* name, size_t operations, int64_t elapsed_us) {
  if (elapsed_us <= 0) elapsed_us = 1;

  const uint64_t us_per_op_x1000 =
    (static_cast<uint64_t>(elapsed_us) * 1000ULL) / operations;
  const uint64_t ops_per_s_x1000 =
    (static_cast<uint64_t>(operations) * 1000000000ULL) / static_cast<uint64_t>(elapsed_us);

  printf("%s ops=%u elapsed_us=%lld us/op=",
         name, (unsigned)operations, (long long)elapsed_us);
  print_scaled_3(us_per_op_x1000);
  printf(" ops/s=");
  print_scaled_3(ops_per_s_x1000);
  printf("\n");
}

static bool expect_ok(const char* name, c3db_err_t err) {
  if (err == C3DB_OK) return true;
  printf("%s failed: err=%d\n", name, err);
  return false;
}

static uint32_t prng_next(uint32_t &state) {
  state = (state * 1664525UL) + 1013904223UL;
  return state;
}

static size_t random_aligned_offset(uint32_t &state, size_t file_size, size_t block_size) {
  const size_t max_block = (file_size / block_size) - 1;
  return (prng_next(state) % (max_block + 1)) * block_size;
}

static size_t read_iterations(size_t block_size) {
  if (block_size <= 256) return 100;
  if (block_size == 512) return 50;
  if (block_size == 1024) return 25;
  return 10;
}

static size_t write_iterations(size_t block_size) {
  if (block_size <= 256) return 40;
  if (block_size <= 1024) return 20;
  return 10;
}

static bool create_file(const char* file_name, size_t file_size) {
  static uint8_t buffer[8192] = {};

  FILE* file = fopen(file_name, "wb");
  if (!file) {
    printf("benchmark setup open failed\n");
    return false;
  }

  size_t offset = 0;
  while (offset < file_size) {
    const size_t chunk = (file_size - offset) < sizeof(buffer) ? (file_size - offset) : sizeof(buffer);
    for (size_t i = 0; i < chunk; ++i) {
      buffer[i] = static_cast<uint8_t>((offset + i) & 0xFF);
    }
    if (fwrite(buffer, 1, chunk, file) != chunk) {
      fclose(file);
      printf("benchmark setup write failed\n");
      return false;
    }
    offset += chunk;
  }

  if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
    fclose(file);
    printf("benchmark setup flush failed\n");
    return false;
  }

  fclose(file);
  return true;
}

static bool create_record_file(const char* file_name, size_t rec_size, size_t rec_count, const uint8_t* rec_buffer) {
  remove(file_name);

  FILE* file = fopen(file_name, "wb");
  if (!file) {
    printf("record benchmark setup open failed\n");
    return false;
  }

  /*
   * Build record files directly so read/update benchmarks are compared against
   * the same contiguous physical layout as the low-level baselines.
   */
  for (size_t i = 0; i < rec_count; ++i) {
    if (fwrite(rec_buffer, 1, rec_size, file) != rec_size) {
      fclose(file);
      printf("record benchmark setup write failed\n");
      return false;
    }
  }

  if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
    fclose(file);
    printf("record benchmark setup flush failed\n");
    return false;
  }

  fclose(file);
  return true;
}

bool run_c3db_low_level_random_read_benchmark() {
  const char* file_name = "/sdcard/c3dbrdb.bin";
  constexpr size_t file_size = 512 * 1024;
  constexpr size_t max_block_size = 8192;
  const size_t block_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t read_buffer[max_block_size] = {};

  printf("low-level random read baseline start\n");
  printf("file_size=%u bytes\n", (unsigned)file_size);
  printf("metrics: ms/read and bytes/ms\n");

  remove(file_name);
  if (!create_file(file_name, file_size)) return false;

  for (size_t s = 0; s < sizeof(block_sizes) / sizeof(block_sizes[0]); ++s) {
    const size_t block_size = block_sizes[s];
    const size_t iterations = read_iterations(block_size);
    uint32_t rng = 0xC3DB0001UL + static_cast<uint32_t>(block_size);

    FILE* file = fopen(file_name, "rb");
    if (!file) {
      printf("random read baseline open failed\n");
      return false;
    }

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      const size_t offset = random_aligned_offset(rng, file_size, block_size);
      if (fseek(file, static_cast<long>(offset), SEEK_SET) != 0 ||
          fread(read_buffer, 1, block_size, file) != block_size) {
        fclose(file);
        printf("random read baseline failed\n");
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    fclose(file);

    print_read_result("low random fseek+fread", block_size, iterations, elapsed_us);
  }

  remove(file_name);
  printf("low-level random read baseline OK\n");
  return true;
}

bool run_c3db_low_level_extend_write_benchmark() {
  const char* file_name = "/sdcard/c3dbweb.bin";
  constexpr size_t max_block_size = 8192;
  const size_t block_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t write_buffer[max_block_size] = {};

  printf("low-level extend write baseline start\n");
  printf("metrics: ms/write and bytes/ms\n");

  for (size_t i = 0; i < max_block_size; ++i) write_buffer[i] = static_cast<uint8_t>(i & 0xFF);

  for (size_t s = 0; s < sizeof(block_sizes) / sizeof(block_sizes[0]); ++s) {
    const size_t block_size = block_sizes[s];
    const size_t iterations = write_iterations(block_size);

    remove(file_name);
    FILE* file = fopen(file_name, "wb+");
    if (!file) {
      printf("extend write baseline open failed\n");
      return false;
    }

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      if (fwrite(write_buffer, 1, block_size, file) != block_size ||
          fflush(file) != 0 ||
          fsync(fileno(file)) != 0) {
        fclose(file);
        printf("extend write baseline failed\n");
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    fclose(file);

    print_write_result("low extend fwrite+flush", block_size, iterations, elapsed_us);
  }

  remove(file_name);
  printf("low-level extend write baseline OK\n");
  return true;
}

bool run_c3db_low_level_overwrite_benchmark() {
  const char* file_name = "/sdcard/c3dbwob.bin";
  constexpr size_t file_size = 512 * 1024;
  constexpr size_t max_block_size = 8192;
  const size_t block_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t write_buffer[max_block_size] = {};

  printf("low-level overwrite baseline start\n");
  printf("file_size=%u bytes\n", (unsigned)file_size);
  printf("metrics: ms/write and bytes/ms\n");

  for (size_t i = 0; i < max_block_size; ++i) write_buffer[i] = static_cast<uint8_t>((i * 3) & 0xFF);

  remove(file_name);
  if (!create_file(file_name, file_size)) return false;

  for (size_t s = 0; s < sizeof(block_sizes) / sizeof(block_sizes[0]); ++s) {
    const size_t block_size = block_sizes[s];
    const size_t iterations = write_iterations(block_size);
    uint32_t rng = 0xC3DB1001UL + static_cast<uint32_t>(block_size);

    FILE* file = fopen(file_name, "rb+");
    if (!file) {
      printf("overwrite baseline open failed\n");
      return false;
    }

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      const size_t offset = random_aligned_offset(rng, file_size, block_size);
      if (fseek(file, static_cast<long>(offset), SEEK_SET) != 0 ||
          fwrite(write_buffer, 1, block_size, file) != block_size ||
          fflush(file) != 0 ||
          fsync(fileno(file)) != 0) {
        fclose(file);
        printf("overwrite baseline failed\n");
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    fclose(file);

    print_write_result("low overwrite fseek+fwrite+flush", block_size, iterations, elapsed_us);
  }

  remove(file_name);
  printf("low-level overwrite baseline OK\n");
  return true;
}

bool run_c3db_file_select_benchmark() {
  const char* file_name = "/sdcard/c3dbsel.bin";
  constexpr size_t file_size = 512 * 1024;
  constexpr size_t max_rec_size = 8192;
  const size_t rec_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t rec_buffer[max_rec_size] = {};

  printf("c3db_file_t select performance test start\n");
  printf("file_size=%u bytes\n", (unsigned)file_size);
  printf("metrics: ms/select and bytes/ms\n");

  for (size_t i = 0; i < max_rec_size; ++i) rec_buffer[i] = static_cast<uint8_t>((i * 7) & 0xFF);

  for (size_t s = 0; s < sizeof(rec_sizes) / sizeof(rec_sizes[0]); ++s) {
    const size_t rec_size = rec_sizes[s];
    const size_t rec_count = file_size / rec_size;
    const size_t iterations = read_iterations(rec_size);
    uint32_t rng = 0xC3DB2001UL + static_cast<uint32_t>(rec_size);

    if (!create_record_file(file_name, rec_size, rec_count, rec_buffer)) return false;

    c3db_file_t db;
    if (!expect_ok("select perf begin", db.begin(file_name, true, 0, 0, rec_size))) return false;

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      const size_t row_id = prng_next(rng) % rec_count;
      if (!expect_ok("select perf select", db.select(row_id, rec_buffer))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    db.end();

    print_read_result("c3db select random", rec_size, iterations, elapsed_us);
  }

  remove(file_name);
  printf("c3db_file_t select performance test OK\n");
  return true;
}

bool run_c3db_file_append_benchmark() {
  const char* file_name = "/sdcard/c3dbapp.bin";
  constexpr size_t max_rec_size = 8192;
  const size_t rec_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t rec_buffer[max_rec_size] = {};

  printf("c3db_file_t append performance test start\n");
  printf("metrics: ms/append and bytes/ms\n");

  for (size_t i = 0; i < max_rec_size; ++i) rec_buffer[i] = static_cast<uint8_t>((i * 5) & 0xFF);

  for (size_t s = 0; s < sizeof(rec_sizes) / sizeof(rec_sizes[0]); ++s) {
    const size_t rec_size = rec_sizes[s];
    const size_t iterations = write_iterations(rec_size);

    remove(file_name);
    c3db_file_t db;
    if (!expect_ok("append perf create", db.create(file_name, 0, 0, rec_size))) return false;

    size_t row_id = 0;
    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      if (!expect_ok("append perf append", db.append(rec_buffer, row_id))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (!expect_ok("append perf close", db.end())) return false;

    print_write_result("c3db append", rec_size, iterations, elapsed_us);
  }

  remove(file_name);
  printf("c3db_file_t append performance test OK\n");
  return true;
}

bool run_c3db_file_update_benchmark() {
  const char* file_name = "/sdcard/c3dbupd.bin";
  constexpr size_t file_size = 512 * 1024;
  constexpr size_t max_rec_size = 8192;
  const size_t rec_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t rec_buffer[max_rec_size] = {};

  printf("c3db_file_t update performance test start\n");
  printf("file_size=%u bytes\n", (unsigned)file_size);
  printf("metrics: ms/update and bytes/ms\n");

  for (size_t i = 0; i < max_rec_size; ++i) rec_buffer[i] = static_cast<uint8_t>((i * 11) & 0xFF);

  for (size_t s = 0; s < sizeof(rec_sizes) / sizeof(rec_sizes[0]); ++s) {
    const size_t rec_size = rec_sizes[s];
    const size_t rec_count = file_size / rec_size;
    const size_t iterations = write_iterations(rec_size);
    uint32_t rng = 0xC3DB3001UL + static_cast<uint32_t>(rec_size);

    if (!create_record_file(file_name, rec_size, rec_count, rec_buffer)) return false;

    c3db_file_t db;
    if (!expect_ok("update perf begin", db.begin(file_name, false, 0, 0, rec_size))) return false;

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      const size_t row_id = prng_next(rng) % rec_count;
      if (!expect_ok("update perf update", db.update(row_id, rec_buffer, true))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (!expect_ok("update perf close", db.end())) return false;

    print_write_result("c3db update random", rec_size, iterations, elapsed_us);
  }

  remove(file_name);
  printf("c3db_file_t update performance test OK\n");
  return true;
}

bool run_c3db_db_file_select_benchmark() {
  const char* base_name = "/sdcard/dbfsel";
  const char* file_name = "/sdcard/dbfsel.dbf";
  constexpr size_t logical_file_size = 512 * 1024;
  constexpr size_t max_data_size = 8192;
  const size_t data_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t data_buffer[max_data_size] = {};

  printf("c3db_db_file_t select performance test start\n");
  printf("logical_file_size=%u bytes\n", (unsigned)logical_file_size);
  printf("physical_record_size=36+(2*logical_block)\n");
  printf("metrics: ms/select and logical bytes/ms\n");

  for (size_t i = 0; i < max_data_size; ++i) data_buffer[i] = static_cast<uint8_t>((i * 13) & 0xFF);

  for (size_t s = 0; s < sizeof(data_sizes) / sizeof(data_sizes[0]); ++s) {
    const size_t data_size = data_sizes[s];
    const size_t rec_count = logical_file_size / data_size;
    const size_t iterations = read_iterations(data_size);
    const size_t physical_size = 36 + (2 * data_size);
    uint32_t rng = 0xC3DB4001UL + static_cast<uint32_t>(data_size);

    remove(file_name);
    c3db_db_file_t db(data_size);
    if (!expect_ok("dbf select perf create", db.create(base_name))) return false;

    c3db_id_t id = C3DB_NULL_ID;
    for (size_t row = 0; row < rec_count; ++row) {
      data_buffer[0] = static_cast<uint8_t>(row & 0xFF);
      if (!expect_ok("dbf select perf append", db.append(data_buffer, id))) {
        db.end();
        return false;
      }
    }

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      const uint32_t row_id = static_cast<uint32_t>(prng_next(rng) % rec_count);
      if (!expect_ok("dbf select perf select", db.select(static_cast<c3db_id_t>(row_id), data_buffer))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    db.end();

    printf("dbf select physical_record=%u ", (unsigned)physical_size);
    print_read_result("dbf select random", data_size, iterations, elapsed_us);
  }

  remove(file_name);
  printf("c3db_db_file_t select performance test OK\n");
  return true;
}

bool run_c3db_data_file_select_benchmark() {
  const char* base_name = "/sdcard/datbsel";
  const char* met_name = "/sdcard/datbsel.met";
  const char* dat_name = "/sdcard/datbsel.dat";
  const char* met_dfg_name = "/sdcard/datbsel.mdf";
  const char* dat_dfg_name = "/sdcard/datbsel.ddf";
  const char* met_bak_name = "/sdcard/datbsel.mbk";
  const char* dat_bak_name = "/sdcard/datbsel.dbk";
  constexpr size_t logical_file_size = 512 * 1024;
  constexpr size_t max_data_size = 8192;
  const size_t data_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t data_buffer[max_data_size] = {};

  printf("c3db_data_file_t select performance test start\n");
  printf("logical_file_size=%u bytes\n", (unsigned)logical_file_size);
  printf("metadata_record_size=68 data_record_size=logical_block\n");
  printf("metrics: ms/select and logical bytes/ms\n");

  for (size_t i = 0; i < max_data_size; ++i) data_buffer[i] = static_cast<uint8_t>((i * 13) & 0xFF);

  for (size_t s = 0; s < sizeof(data_sizes) / sizeof(data_sizes[0]); ++s) {
    const size_t data_size = data_sizes[s];
    const size_t rec_count = logical_file_size / data_size;
    const size_t iterations = read_iterations(data_size);
    constexpr size_t metadata_size = 68;
    uint32_t rng = 0xC3DB4001UL + static_cast<uint32_t>(data_size);

    remove(met_name);
    remove(dat_name);
    remove(met_dfg_name);
    remove(dat_dfg_name);
    remove(met_bak_name);
    remove(dat_bak_name);

    c3db_data_file_t db(data_size);
    if (!expect_ok("data select perf create", db.create(base_name))) return false;

    c3db_id_t id = C3DB_NULL_ID;
    for (size_t row = 0; row < rec_count; ++row) {
      data_buffer[0] = static_cast<uint8_t>(row & 0xFF);
      if (!expect_ok("data select perf append", db.append(data_buffer, id))) {
        db.end();
        return false;
      }
    }

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      const uint32_t row_id = static_cast<uint32_t>(prng_next(rng) % rec_count);
      if (!expect_ok("data select perf select", db.select(static_cast<c3db_id_t>(row_id), data_buffer))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    db.end();

    printf("data select metadata_record=%u ", (unsigned)metadata_size);
    print_read_result("data select random", data_size, iterations, elapsed_us);
  }

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);
  printf("c3db_data_file_t select performance test OK\n");
  return true;
}

bool run_c3db_db_file_append_benchmark() {
  const char* base_name = "/sdcard/dbfapp";
  const char* file_name = "/sdcard/dbfapp.dbf";
  constexpr size_t max_data_size = 8192;
  const size_t data_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t data_buffer[max_data_size] = {};

  printf("c3db_db_file_t append performance test start\n");
  printf("physical_record_size=36+(2*logical_block)\n");
  printf("metrics: ms/append and logical bytes/ms\n");

  for (size_t i = 0; i < max_data_size; ++i) data_buffer[i] = static_cast<uint8_t>((i * 17) & 0xFF);

  for (size_t s = 0; s < sizeof(data_sizes) / sizeof(data_sizes[0]); ++s) {
    const size_t data_size = data_sizes[s];
    const size_t iterations = write_iterations(data_size);
    const size_t physical_size = 36 + (2 * data_size);

    remove(file_name);
    c3db_db_file_t db(data_size);
    if (!expect_ok("dbf append perf create", db.create(base_name))) return false;

    c3db_id_t id = C3DB_NULL_ID;
    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      data_buffer[0] = static_cast<uint8_t>(i & 0xFF);
      if (!expect_ok("dbf append perf append", db.append(data_buffer, id))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (!expect_ok("dbf append perf close", db.end())) return false;

    printf("dbf append physical_record=%u ", (unsigned)physical_size);
    print_write_result("dbf append", data_size, iterations, elapsed_us);
  }

  remove(file_name);
  printf("c3db_db_file_t append performance test OK\n");
  return true;
}

bool run_c3db_data_file_append_benchmark() {
  const char* base_name = "/sdcard/datbapp";
  const char* met_name = "/sdcard/datbapp.met";
  const char* dat_name = "/sdcard/datbapp.dat";
  const char* met_dfg_name = "/sdcard/datbapp.mdf";
  const char* dat_dfg_name = "/sdcard/datbapp.ddf";
  const char* met_bak_name = "/sdcard/datbapp.mbk";
  const char* dat_bak_name = "/sdcard/datbapp.dbk";
  constexpr size_t max_data_size = 8192;
  const size_t data_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t data_buffer[max_data_size] = {};

  printf("c3db_data_file_t append performance test start\n");
  printf("metadata_record_size=68 data_record_size=logical_block\n");
  printf("metrics: ms/append and logical bytes/ms\n");

  for (size_t i = 0; i < max_data_size; ++i) data_buffer[i] = static_cast<uint8_t>((i * 17) & 0xFF);

  for (size_t s = 0; s < sizeof(data_sizes) / sizeof(data_sizes[0]); ++s) {
    const size_t data_size = data_sizes[s];
    const size_t iterations = write_iterations(data_size);
    constexpr size_t metadata_size = 68;

    remove(met_name);
    remove(dat_name);
    remove(met_dfg_name);
    remove(dat_dfg_name);
    remove(met_bak_name);
    remove(dat_bak_name);

    c3db_data_file_t db(data_size);
    if (!expect_ok("data append perf create", db.create(base_name))) return false;

    c3db_id_t id = C3DB_NULL_ID;
    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      data_buffer[0] = static_cast<uint8_t>(i & 0xFF);
      if (!expect_ok("data append perf append", db.append(data_buffer, id))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (!expect_ok("data append perf close", db.end())) return false;

    printf("data append metadata_record=%u ", (unsigned)metadata_size);
    print_write_result("data append", data_size, iterations, elapsed_us);
  }

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);
  printf("c3db_data_file_t append performance test OK\n");
  return true;
}

bool run_c3db_data_file_update_benchmark() {
  const char* base_name = "/sdcard/datbupd";
  const char* met_name = "/sdcard/datbupd.met";
  const char* dat_name = "/sdcard/datbupd.dat";
  const char* met_dfg_name = "/sdcard/datbupd.mdf";
  const char* dat_dfg_name = "/sdcard/datbupd.ddf";
  const char* met_bak_name = "/sdcard/datbupd.mbk";
  const char* dat_bak_name = "/sdcard/datbupd.dbk";
  constexpr size_t logical_file_size = 512 * 1024;
  constexpr size_t max_data_size = 8192;
  const size_t data_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t data_buffer[max_data_size] = {};

  printf("c3db_data_file_t update performance test start\n");
  printf("logical_file_size=%u bytes\n", (unsigned)logical_file_size);
  printf("metadata_record_size=68 data_record_size=logical_block\n");
  printf("metrics: ms/update and logical bytes/ms\n");

  for (size_t i = 0; i < max_data_size; ++i) data_buffer[i] = static_cast<uint8_t>((i * 19) & 0xFF);

  for (size_t s = 0; s < sizeof(data_sizes) / sizeof(data_sizes[0]); ++s) {
    const size_t data_size = data_sizes[s];
    const size_t rec_count = logical_file_size / data_size;
    const size_t iterations = write_iterations(data_size);
    constexpr size_t metadata_size = 68;
    uint32_t rng = 0xC3DB5001UL + static_cast<uint32_t>(data_size);

    remove(met_name);
    remove(dat_name);
    remove(met_dfg_name);
    remove(dat_dfg_name);
    remove(met_bak_name);
    remove(dat_bak_name);

    c3db_data_file_t db(data_size);
    if (!expect_ok("data update perf create", db.create(base_name))) return false;

    c3db_id_t id = C3DB_NULL_ID;
    for (size_t row = 0; row < rec_count; ++row) {
      data_buffer[0] = static_cast<uint8_t>(row & 0xFF);
      if (!expect_ok("data update perf append", db.append(data_buffer, id))) {
        db.end();
        return false;
      }
    }

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      const uint32_t row_id = static_cast<uint32_t>(prng_next(rng) % rec_count);
      data_buffer[0] = static_cast<uint8_t>(i & 0xFF);
      if (!expect_ok("data update perf update", db.update(static_cast<c3db_id_t>(row_id), data_buffer))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (!expect_ok("data update perf close", db.end())) return false;

    printf("data update metadata_record=%u ", (unsigned)metadata_size);
    print_write_result("data update random", data_size, iterations, elapsed_us);
  }

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);
  printf("c3db_data_file_t update performance test OK\n");
  return true;
}

bool run_c3db_db_file_update_benchmark() {
  const char* base_name = "/sdcard/dbfupd";
  const char* file_name = "/sdcard/dbfupd.dbf";
  constexpr size_t logical_file_size = 512 * 1024;
  constexpr size_t max_data_size = 8192;
  const size_t data_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t data_buffer[max_data_size] = {};

  printf("c3db_db_file_t update performance test start\n");
  printf("logical_file_size=%u bytes\n", (unsigned)logical_file_size);
  printf("physical_record_size=36+(2*logical_block)\n");
  printf("metrics: ms/update and logical bytes/ms\n");

  for (size_t i = 0; i < max_data_size; ++i) data_buffer[i] = static_cast<uint8_t>((i * 19) & 0xFF);

  for (size_t s = 0; s < sizeof(data_sizes) / sizeof(data_sizes[0]); ++s) {
    const size_t data_size = data_sizes[s];
    const size_t rec_count = logical_file_size / data_size;
    const size_t iterations = write_iterations(data_size);
    const size_t physical_size = 36 + (2 * data_size);
    uint32_t rng = 0xC3DB5001UL + static_cast<uint32_t>(data_size);

    remove(file_name);
    c3db_db_file_t db(data_size);
    if (!expect_ok("dbf update perf create", db.create(base_name))) return false;

    c3db_id_t id = C3DB_NULL_ID;
    for (size_t row = 0; row < rec_count; ++row) {
      data_buffer[0] = static_cast<uint8_t>(row & 0xFF);
      if (!expect_ok("dbf update perf append", db.append(data_buffer, id))) {
        db.end();
        return false;
      }
    }

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      const uint32_t row_id = static_cast<uint32_t>(prng_next(rng) % rec_count);
      data_buffer[0] = static_cast<uint8_t>(i & 0xFF);
      if (!expect_ok("dbf update perf update", db.update(static_cast<c3db_id_t>(row_id), data_buffer))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (!expect_ok("dbf update perf close", db.end())) return false;

    printf("dbf update physical_record=%u ", (unsigned)physical_size);
    print_write_result("dbf update random", data_size, iterations, elapsed_us);
  }

  remove(file_name);
  printf("c3db_db_file_t update performance test OK\n");
  return true;
}

bool run_c3db_db_file_remove_benchmark() {
  const char* base_name = "/sdcard/dbfdel";
  const char* file_name = "/sdcard/dbfdel.dbf";
  constexpr size_t logical_file_size = 512 * 1024;
  constexpr size_t max_data_size = 8192;
  const size_t data_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t data_buffer[max_data_size] = {};

  printf("c3db_db_file_t remove performance test start\n");
  printf("logical_file_size=%u bytes\n", (unsigned)logical_file_size);
  printf("physical_record_size=36+(2*logical_block)\n");
  printf("metrics: ms/remove and logical bytes/ms\n");

  for (size_t i = 0; i < max_data_size; ++i) data_buffer[i] = static_cast<uint8_t>((i * 23) & 0xFF);

  for (size_t s = 0; s < sizeof(data_sizes) / sizeof(data_sizes[0]); ++s) {
    const size_t data_size = data_sizes[s];
    const size_t rec_count = logical_file_size / data_size;
    const size_t iterations = write_iterations(data_size);
    const size_t physical_size = 36 + (2 * data_size);
    uint32_t rng = 0xC3DB6001UL + static_cast<uint32_t>(data_size);

    remove(file_name);
    c3db_db_file_t db(data_size);
    if (!expect_ok("dbf remove perf create", db.create(base_name))) return false;

    c3db_id_t id = C3DB_NULL_ID;
    for (size_t row = 0; row < rec_count; ++row) {
      data_buffer[0] = static_cast<uint8_t>(row & 0xFF);
      if (!expect_ok("dbf remove perf append", db.append(data_buffer, id))) {
        db.end();
        return false;
      }
    }

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      const uint32_t row_id = static_cast<uint32_t>(prng_next(rng) % rec_count);
      if (!expect_ok("dbf remove perf remove", db.remove(static_cast<c3db_id_t>(row_id)))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (!expect_ok("dbf remove perf close", db.end())) return false;

    printf("dbf remove physical_record=%u ", (unsigned)physical_size);
    print_write_result("dbf remove random", data_size, iterations, elapsed_us);
  }

  remove(file_name);
  printf("c3db_db_file_t remove performance test OK\n");
  return true;
}

bool run_c3db_data_file_remove_benchmark() {
  const char* base_name = "/sdcard/datbdel";
  const char* met_name = "/sdcard/datbdel.met";
  const char* dat_name = "/sdcard/datbdel.dat";
  const char* met_dfg_name = "/sdcard/datbdel.mdf";
  const char* dat_dfg_name = "/sdcard/datbdel.ddf";
  const char* met_bak_name = "/sdcard/datbdel.mbk";
  const char* dat_bak_name = "/sdcard/datbdel.dbk";
  constexpr size_t logical_file_size = 512 * 1024;
  constexpr size_t max_data_size = 8192;
  const size_t data_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t data_buffer[max_data_size] = {};

  printf("c3db_data_file_t remove performance test start\n");
  printf("logical_file_size=%u bytes\n", (unsigned)logical_file_size);
  printf("metadata_record_size=68 data_record_size=logical_block\n");
  printf("metrics: ms/remove and logical bytes/ms\n");

  for (size_t i = 0; i < max_data_size; ++i) data_buffer[i] = static_cast<uint8_t>((i * 23) & 0xFF);

  for (size_t s = 0; s < sizeof(data_sizes) / sizeof(data_sizes[0]); ++s) {
    const size_t data_size = data_sizes[s];
    const size_t rec_count = logical_file_size / data_size;
    const size_t iterations = write_iterations(data_size);
    constexpr size_t metadata_size = 68;
    uint32_t rng = 0xC3DB6001UL + static_cast<uint32_t>(data_size);

    remove(met_name);
    remove(dat_name);
    remove(met_dfg_name);
    remove(dat_dfg_name);
    remove(met_bak_name);
    remove(dat_bak_name);

    c3db_data_file_t db(data_size);
    if (!expect_ok("data remove perf create", db.create(base_name))) return false;

    c3db_id_t id = C3DB_NULL_ID;
    for (size_t row = 0; row < rec_count; ++row) {
      data_buffer[0] = static_cast<uint8_t>(row & 0xFF);
      if (!expect_ok("data remove perf append", db.append(data_buffer, id))) {
        db.end();
        return false;
      }
    }

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      const uint32_t row_id = static_cast<uint32_t>(prng_next(rng) % rec_count);
      if (!expect_ok("data remove perf remove", db.remove(static_cast<c3db_id_t>(row_id)))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (!expect_ok("data remove perf close", db.end())) return false;

    printf("data remove metadata_record=%u ", (unsigned)metadata_size);
    print_write_result("data remove random", data_size, iterations, elapsed_us);
  }

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);
  printf("c3db_data_file_t remove performance test OK\n");
  return true;
}

bool run_c3db_db_file_insert_reuse_benchmark() {
  const char* base_name = "/sdcard/dbfins";
  const char* file_name = "/sdcard/dbfins.dbf";
  constexpr size_t logical_file_size = 512 * 1024;
  constexpr size_t max_data_size = 8192;
  const size_t data_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t data_buffer[max_data_size] = {};

  printf("c3db_db_file_t insert-reuse performance test start\n");
  printf("logical_file_size=%u bytes\n", (unsigned)logical_file_size);
  printf("physical_record_size=36+(2*logical_block)\n");
  printf("metrics: ms/insert and logical bytes/ms\n");

  for (size_t i = 0; i < max_data_size; ++i) data_buffer[i] = static_cast<uint8_t>((i * 29) & 0xFF);

  for (size_t s = 0; s < sizeof(data_sizes) / sizeof(data_sizes[0]); ++s) {
    const size_t data_size = data_sizes[s];
    const size_t rec_count = logical_file_size / data_size;
    const size_t iterations = write_iterations(data_size);
    const size_t physical_size = 36 + (2 * data_size);

    remove(file_name);
    c3db_db_file_t db(data_size);
    if (!expect_ok("dbf insert perf create", db.create(base_name))) return false;

    c3db_id_t id = C3DB_NULL_ID;
    for (size_t row = 0; row < rec_count; ++row) {
      data_buffer[0] = static_cast<uint8_t>(row & 0xFF);
      if (!expect_ok("dbf insert perf append", db.append(data_buffer, id))) {
        db.end();
        return false;
      }
    }

    /*
     * Free-list preparation is outside the measurement. Rows are deleted in a
     * known range so every measured insert reuses a deleted physical record.
     */
    for (size_t row = 0; row < iterations; ++row) {
      if (!expect_ok("dbf insert perf remove setup", db.remove(static_cast<c3db_id_t>(row)))) {
        db.end();
        return false;
      }
    }

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      data_buffer[0] = static_cast<uint8_t>(i & 0xFF);
      if (!expect_ok("dbf insert perf insert", db.insert(data_buffer, id))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (!expect_ok("dbf insert perf close", db.end())) return false;

    printf("dbf insert-reuse physical_record=%u ", (unsigned)physical_size);
    print_write_result("dbf insert-reuse", data_size, iterations, elapsed_us);
  }

  remove(file_name);
  printf("c3db_db_file_t insert-reuse performance test OK\n");
  return true;
}

bool run_c3db_data_file_insert_reuse_benchmark() {
  const char* base_name = "/sdcard/datbins";
  const char* met_name = "/sdcard/datbins.met";
  const char* dat_name = "/sdcard/datbins.dat";
  const char* met_dfg_name = "/sdcard/datbins.mdf";
  const char* dat_dfg_name = "/sdcard/datbins.ddf";
  const char* met_bak_name = "/sdcard/datbins.mbk";
  const char* dat_bak_name = "/sdcard/datbins.dbk";
  constexpr size_t logical_file_size = 512 * 1024;
  constexpr size_t max_data_size = 8192;
  const size_t data_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint8_t data_buffer[max_data_size] = {};

  printf("c3db_data_file_t insert-reuse performance test start\n");
  printf("logical_file_size=%u bytes\n", (unsigned)logical_file_size);
  printf("metadata_record_size=68 data_record_size=logical_block\n");
  printf("metrics: ms/insert and logical bytes/ms\n");

  for (size_t i = 0; i < max_data_size; ++i) data_buffer[i] = static_cast<uint8_t>((i * 29) & 0xFF);

  for (size_t s = 0; s < sizeof(data_sizes) / sizeof(data_sizes[0]); ++s) {
    const size_t data_size = data_sizes[s];
    const size_t rec_count = logical_file_size / data_size;
    const size_t iterations = write_iterations(data_size);
    constexpr size_t metadata_size = 68;

    remove(met_name);
    remove(dat_name);
    remove(met_dfg_name);
    remove(dat_dfg_name);
    remove(met_bak_name);
    remove(dat_bak_name);

    c3db_data_file_t db(data_size);
    if (!expect_ok("data insert perf create", db.create(base_name))) return false;

    c3db_id_t id = C3DB_NULL_ID;
    for (size_t row = 0; row < rec_count; ++row) {
      data_buffer[0] = static_cast<uint8_t>(row & 0xFF);
      if (!expect_ok("data insert perf append", db.append(data_buffer, id))) {
        db.end();
        return false;
      }
    }

    /*
     * Free-list preparation is outside the measurement. Rows are deleted in a
     * known range so every measured insert reuses an existing .met row and,
     * when available, an associated .dat row.
     */
    for (size_t row = 0; row < iterations; ++row) {
      if (!expect_ok("data insert perf remove setup", db.remove(static_cast<c3db_id_t>(row)))) {
        db.end();
        return false;
      }
    }

    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < iterations; ++i) {
      data_buffer[0] = static_cast<uint8_t>(i & 0xFF);
      if (!expect_ok("data insert perf insert", db.insert(data_buffer, id))) {
        db.end();
        return false;
      }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (!expect_ok("data insert perf close", db.end())) return false;

    printf("data insert-reuse metadata_record=%u ", (unsigned)metadata_size);
    print_write_result("data insert-reuse", data_size, iterations, elapsed_us);
  }

  remove(met_name);
  remove(dat_name);
  remove(met_dfg_name);
  remove(dat_dfg_name);
  remove(met_bak_name);
  remove(dat_bak_name);
  printf("c3db_data_file_t insert-reuse performance test OK\n");
  return true;
}

bool run_c3db_index_new_hash_benchmark() {
  const char* base_name = "/sdcard/idxnew";
  constexpr size_t iterations = 128;

  printf("c3db_index_t new-hash benchmark start\n");
  printf("metrics: us/index and index ops/s\n");

  c3db_index_t::delete_index_files(base_name);

  c3db_index_t index;
  if (!expect_ok("idx new create", index.create(base_name))) return false;

  /*
   * Each measured operation inserts a hash that is not yet present. The index
   * is allowed to grow normally, including splits, because that is the real
   * cost paid while building an index from new keys.
   */
  const int64_t start_us = esp_timer_get_time();
  for (size_t i = 0; i < iterations; ++i) {
    const uint64_t hash = 0xC3DB700000000000ULL + static_cast<uint64_t>(i);
    const c3db_id_t record_id = static_cast<c3db_id_t>(i + 1);
    if (!expect_ok("idx new index", index.index(hash, record_id))) {
      index.end();
      return false;
    }
  }
  const int64_t elapsed_us = esp_timer_get_time() - start_us;

  if (!expect_ok("idx new close", index.end())) return false;
  c3db_index_t::delete_index_files(base_name);

  print_index_result("idx index new hash", iterations, elapsed_us);
  printf("c3db_index_t new-hash benchmark OK\n");
  return true;
}

bool run_c3db_index_existing_ref_benchmark() {
  const char* base_name = "/sdcard/idxref";
  constexpr size_t hash_count = 32;
  constexpr size_t iterations = 128;
  uint64_t hashes[hash_count] = {};

  printf("c3db_index_t existing-ref benchmark start\n");
  printf("hashes=%u\n", (unsigned)hash_count);
  printf("metrics: us/index and index ops/s\n");

  c3db_index_t::delete_index_files(base_name);

  c3db_index_t index;
  if (!expect_ok("idx ref create", index.create(base_name))) return false;

  /*
   * Setup creates the hash entries before timing starts. The measured loop only
   * adds new IRF nodes to hashes that already exist, which isolates the
   * duplicate-key/reference-list path from first-entry creation.
   */
  for (size_t i = 0; i < hash_count; ++i) {
    hashes[i] = 0xC3DB710000000000ULL + static_cast<uint64_t>(i);
    if (!expect_ok("idx ref setup", index.index(hashes[i], static_cast<c3db_id_t>(i + 1)))) {
      index.end();
      return false;
    }
  }

  const int64_t start_us = esp_timer_get_time();
  for (size_t i = 0; i < iterations; ++i) {
    const uint64_t hash = hashes[i % hash_count];
    const c3db_id_t record_id = static_cast<c3db_id_t>(hash_count + i + 1);
    if (!expect_ok("idx ref index", index.index(hash, record_id))) {
      index.end();
      return false;
    }
  }
  const int64_t elapsed_us = esp_timer_get_time() - start_us;

  if (!expect_ok("idx ref close", index.end())) return false;
  c3db_index_t::delete_index_files(base_name);

  print_index_result("idx index existing hash ref", iterations, elapsed_us);
  printf("c3db_index_t existing-ref benchmark OK\n");
  return true;
}

bool run_c3db_index_find_first_benchmark() {
  const char* base_name = "/sdcard/idxfind";
  constexpr size_t hash_count = 32;
  constexpr size_t refs_per_hash = 4;
  constexpr size_t iterations = 256;
  uint64_t hashes[hash_count] = {};

  printf("c3db_index_t find-first benchmark start\n");
  printf("hashes=%u refs/hash=%u\n", (unsigned)hash_count, (unsigned)refs_per_hash);
  printf("metrics: us/find and find ops/s\n");

  c3db_index_t::delete_index_files(base_name);

  c3db_index_t index;
  if (!expect_ok("idx find create", index.create(base_name))) return false;

  /*
   * Finds are measured over several prebuilt hashes instead of one repeated
   * hash. This avoids reducing the benchmark to a single hot bucket/ref record
   * and better represents indexed lookups in normal use.
   */
  for (size_t i = 0; i < hash_count; ++i) {
    hashes[i] = 0xC3DB720000000000ULL + static_cast<uint64_t>(i);
    for (size_t r = 0; r < refs_per_hash; ++r) {
      const c3db_id_t record_id = static_cast<c3db_id_t>((i * refs_per_hash) + r + 1);
      if (!expect_ok("idx find setup", index.index(hashes[i], record_id))) {
        index.end();
        return false;
      }
    }
  }

  uint32_t rng = 0xC3DB7201UL;
  volatile c3db_id_t sink = 0;

  const int64_t start_us = esp_timer_get_time();
  for (size_t i = 0; i < iterations; ++i) {
    const uint64_t hash = hashes[prng_next(rng) % hash_count];
    c3db_id_t record_id = C3DB_NULL_ID;
    c3db_idx_cursor_t cursor = {};
    if (!expect_ok("idx find", index.find(hash, record_id, cursor))) {
      index.end();
      return false;
    }
    sink ^= record_id;
  }
  const int64_t elapsed_us = esp_timer_get_time() - start_us;

  if (!expect_ok("idx find close", index.end())) return false;
  c3db_index_t::delete_index_files(base_name);

  print_index_result("idx find first ref", iterations, elapsed_us);
  printf("idx find sink=%llu\n", (unsigned long long)sink);
  printf("c3db_index_t find-first benchmark OK\n");
  return true;
}
