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

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "c3db_cached_db.h"

static bool mount_sdcard() {
  static sdmmc_card_t* card = nullptr;
  static bool mounted = false;
  if (mounted) return true;

  constexpr gpio_num_t sd_mosi = (gpio_num_t)15;
  constexpr gpio_num_t sd_miso = (gpio_num_t)2;
  constexpr gpio_num_t sd_sclk = (gpio_num_t)14;
  constexpr gpio_num_t sd_cs = (gpio_num_t)13;

  /*
   * SDSPI is very sensitive to floating lines. Enabling pull-ups and a stronger
   * drive capability fixed severe read/write slowdowns during benchmark work.
   */
  gpio_set_pull_mode(sd_miso, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(sd_mosi, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(sd_cs, GPIO_PULLUP_ONLY);
  gpio_set_drive_capability(sd_mosi, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability(sd_sclk, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability(sd_cs, GPIO_DRIVE_CAP_3);

  const char mount_point[] = "/sdcard";
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 16,
    .allocation_unit_size = 16 * 1024
  };

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.max_freq_khz = 20000;

  spi_bus_config_t bus_cfg = {
    .mosi_io_num = sd_mosi,
    .miso_io_num = sd_miso,
    .sclk_io_num = sd_sclk,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 4000
  };

  esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK) {
    printf("SPI init failed: %d\n", ret);
    return false;
  }

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = sd_cs;
  slot_config.host_id = SPI2_HOST;

  ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
  if (ret != ESP_OK) {
    printf("SD mount failed: %d\n", ret);
    return false;
  }

  mounted = true;
  printf("SD mounted\n");
  sdmmc_card_print_info(stdout, card);
  return true;
}

static bool expect_ok(const char* label, c3db_err_t err) {
  if (err == C3DB_OK) return true;
  printf("%s failed: err=%d\n", label, err);
  return false;
}

static void delete_demo_files(const char* base_name) {
  char name[80];
  snprintf(name, sizeof(name), "%s.dbf", base_name);
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

static void run_c3db_demo() {
  constexpr size_t record_size = 16;
  const char* base_name = "/sdcard/c3dmd";

  printf("\n========================================\n");
  printf(" C3DB embedded database demo\n");
  printf("========================================\n");
  printf("Storage: microSD\n");
  printf("Database: cached fixed-record DB\n");
  printf("Record size: %u bytes\n", static_cast<unsigned>(record_size));
  printf("Base file: %s\n\n", base_name);

  printf("[1/6] Cleaning previous demo files...\n");
  delete_demo_files(base_name);

  printf("[2/6] Creating database...\n");
  c3db_cached_db_t db(record_size, 4096);
  if (!expect_ok("demo create", db.create(base_name, C3DB_SEQUENTIAL_ACCESS_MODE))) return;

  printf("[3/6] Creating index on byte 0...\n");
  size_t idx_num = C3DB_IDX_CAPACITY;
  if (!expect_ok("demo create idx", db.create_idx(0, 1, idx_num))) {
    db.end();
    return;
  }
  printf("      Index number: %u\n", static_cast<unsigned>(idx_num));

  uint8_t record[record_size] = {};
  record[0] = 0x42;
  record[1] = 0xA5;
  for (size_t i = 2; i < record_size; ++i) record[i] = static_cast<uint8_t>(i);

  printf("[4/6] Inserting one record...\n");
  c3db_id_t id = C3DB_NULL_ID;
  if (!expect_ok("demo insert", db.insert(record, id))) {
    db.end();
    return;
  }
  printf("      Inserted id: %llu\n", static_cast<unsigned long long>(id));

  printf("[5/6] Reading record by id...\n");
  uint8_t selected[record_size] = {};
  if (!expect_ok("demo select id", db.select(id, selected))) {
    db.end();
    return;
  }
  printf("      key=0x%02X value=0x%02X\n", selected[0], selected[1]);

  printf("[6/6] Reading record through index...\n");
  uint8_t key = 0x42;
  c3db_idx_cursor_t cursor = {};
  memset(selected, 0, sizeof(selected));
  if (!expect_ok("demo select idx", db.select(idx_num, &key, selected, cursor))) {
    db.end();
    return;
  }
  printf("      found id=%llu key=0x%02X value=0x%02X\n",
         static_cast<unsigned long long>(id),
         selected[0],
         selected[1]);

  if (!expect_ok("demo close", db.end())) return;
  printf("\nC3DB demo completed successfully\n");
  printf("========================================\n");
}

extern "C" void app_main(void)
{
  if (!mount_sdcard()) return;
  run_c3db_demo();
}
