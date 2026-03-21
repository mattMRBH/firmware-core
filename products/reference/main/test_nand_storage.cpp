#include "test_nand_storage.h"

#include <stdio.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_log.h"

#include "board_config.h"
#include "spi_nand_storage.h"

static constexpr const char *TAG = "test_nand";

static constexpr const char *TEST_FILE_PATH  = "/nand/log.bin";
static constexpr const char *TEST_MOUNT_PATH = "/nand";
static constexpr uint8_t     TEST_PATTERN[4] = {0xDE, 0xAD, 0xBE, 0xEF};

void run_test_nand_storage() {
  ESP_LOGI(TAG, "--- NAND storage test start ---");

  // Skip test gracefully if SPI NAND pins are not configured for this board.
  if (PIN_NAND_CS == GPIO_NUM_NC || PIN_NAND_MOSI == GPIO_NUM_NC ||
      PIN_NAND_MISO == GPIO_NUM_NC || PIN_NAND_CLK == GPIO_NUM_NC) {
    ESP_LOGW(TAG, "NAND pins not configured in board_config.h — skipping");
    return;
  }

  // Initialise the SPI bus. The driver will add its own device handle.
  spi_bus_config_t bus_cfg = {};
  bus_cfg.mosi_io_num      = static_cast<int>(PIN_NAND_MOSI);
  bus_cfg.miso_io_num      = static_cast<int>(PIN_NAND_MISO);
  bus_cfg.sclk_io_num      = static_cast<int>(PIN_NAND_CLK);
  bus_cfg.quadwp_io_num    = -1;
  bus_cfg.quadhd_io_num    = -1;
  bus_cfg.max_transfer_sz  = NAND_SPI_MAX_TRANSFER_SZ;

  esp_err_t err = spi_bus_initialize(NAND_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return;
  }

  SpiNandStorage nand({
      .spi_host               = NAND_SPI_HOST,
      .cs_pin                 = PIN_NAND_CS,
      .clock_speed_hz         = NAND_SPI_CLOCK_HZ,
      .mount_path             = TEST_MOUNT_PATH,
      .format_if_mount_failed = true,
  });

  if (!nand.init()) {
    ESP_LOGE(TAG, "NAND init failed");
    spi_bus_free(NAND_SPI_HOST);
    return;
  }
  ESP_LOGI(TAG, "NAND mounted at %s", nand.mount_path());

  // Write test pattern.
  FILE *f = fopen(TEST_FILE_PATH, "wb");
  if (!f) {
    ESP_LOGE(TAG, "fopen write failed");
    nand.deinit();
    spi_bus_free(NAND_SPI_HOST);
    return;
  }
  fwrite(TEST_PATTERN, 1, sizeof(TEST_PATTERN), f);
  fclose(f);
  ESP_LOGI(TAG, "wrote %u bytes to %s", static_cast<unsigned>(sizeof(TEST_PATTERN)),
           TEST_FILE_PATH);

  // Read back and verify.
  uint8_t buf[sizeof(TEST_PATTERN)] = {};
  f                                 = fopen(TEST_FILE_PATH, "rb");
  if (!f) {
    ESP_LOGE(TAG, "fopen read failed");
    nand.deinit();
    spi_bus_free(NAND_SPI_HOST);
    return;
  }
  const size_t n = fread(buf, 1, sizeof(buf), f);
  fclose(f);

  const bool ok = (n == sizeof(TEST_PATTERN)) &&
                  (memcmp(buf, TEST_PATTERN, sizeof(TEST_PATTERN)) == 0);
  ESP_LOGI(TAG, "read-back: %s (read %u bytes)", ok ? "PASS" : "FAIL",
           static_cast<unsigned>(n));

  nand.deinit();
  spi_bus_free(NAND_SPI_HOST);

  ESP_LOGI(TAG, "--- NAND storage test done ---");
}
