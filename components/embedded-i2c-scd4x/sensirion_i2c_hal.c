/*
 * Copyright (c) 2018, Sensirion AG
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Sensirion AG nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "sensirion_i2c_hal.h"
#include "sensirion_common.h"
#include "sensirion_config.h"
#include "sensirion_i2c.h"

#include "sensirion_i2c_hal_esp_idf.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

/*
 * INSTRUCTIONS
 * ============
 *
 * Implement all functions where they are marked as IMPLEMENT.
 * Follow the function specification in the comments.
 */

/**
 * Select the current i2c bus by index.
 * All following i2c operations will be directed at that bus.
 *
 * THE IMPLEMENTATION IS OPTIONAL ON SINGLE-BUS SETUPS (all sensors on the same
 * bus)
 *
 * @param bus_idx   Bus index to select
 * @returns         0 on success, an error code otherwise
 */
int16_t sensirion_i2c_hal_select_bus(uint8_t bus_idx) {
    (void)bus_idx;
    return NO_ERROR;
}

/**
 * Initialize all hard- and software components that are needed for the I2C
 * communication.
 */
void sensirion_i2c_hal_init(void) {
    // No-op: bus handle is provided by the application via
    // sensirion_i2c_hal_set_bus_handle().
}

/**
 * Release all resources initialized by sensirion_i2c_hal_init().
 */
void sensirion_i2c_hal_free(void) {
    // Best-effort: remove cached device handle.
    sensirion_i2c_hal_set_bus_handle(NULL);
}

/**
 * Sleep for a given number of microseconds. The function should delay the
 * execution for at least the given time, but may also sleep longer.
 *
 * Despite the unit, a <10 millisecond precision is sufficient.
 *
 * @param useconds the sleep time in microseconds
 */
void sensirion_i2c_hal_sleep_usec(uint32_t useconds) {
    if (useconds == 0) {
        return;
    }
    // Keep sub-20ms sleeps in a busy wait to avoid FreeRTOS tick granularity.
    if (useconds < 20000) {
        esp_rom_delay_us(useconds);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS((useconds + 999) / 1000));
}

// ----- ESP-IDF I2C master implementation -----

static const char *TAG = "sensirion_i2c_hal";

static i2c_master_bus_handle_t g_bus_handle = NULL;
static i2c_master_dev_handle_t g_dev_handle = NULL;
static uint8_t g_dev_addr = 0;

static esp_err_t ensure_device(uint8_t address) {
    if (g_bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_dev_handle != NULL && g_dev_addr == address) {
        return ESP_OK;
    }

    if (g_dev_handle != NULL) {
        (void)i2c_master_bus_rm_device(g_dev_handle);
        g_dev_handle = NULL;
        g_dev_addr = 0;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };

    esp_err_t err = i2c_master_bus_add_device(g_bus_handle, &dev_cfg, &g_dev_handle);
    if (err != ESP_OK) {
        g_dev_handle = NULL;
        return err;
    }
    g_dev_addr = address;
    return ESP_OK;
}

void sensirion_i2c_hal_set_bus_handle(i2c_master_bus_handle_t bus_handle) {
    // If bus changes (or cleared), drop cached device handle.
    if (g_dev_handle != NULL) {
        (void)i2c_master_bus_rm_device(g_dev_handle);
        g_dev_handle = NULL;
        g_dev_addr = 0;
    }
    g_bus_handle = bus_handle;
}

int8_t sensirion_i2c_hal_read(uint8_t address, uint8_t *data, uint8_t count) {
    if (data == NULL || count == 0) {
        return I2C_BUS_ERROR;
    }
    const esp_err_t err = ensure_device(address);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c ensure device failed addr=0x%02x: %s", (unsigned)address, esp_err_to_name(err));
        return I2C_BUS_ERROR;
    }

    const esp_err_t r = i2c_master_receive(g_dev_handle, data, (size_t)count, 1000);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "i2c read failed addr=0x%02x: %s", (unsigned)address, esp_err_to_name(r));
        return I2C_BUS_ERROR;
    }
    return NO_ERROR;
}

int8_t sensirion_i2c_hal_write(uint8_t address, const uint8_t *data, uint8_t count) {
    if (data == NULL || count == 0) {
        return I2C_BUS_ERROR;
    }
    const esp_err_t err = ensure_device(address);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c ensure device failed addr=0x%02x: %s", (unsigned)address, esp_err_to_name(err));
        return I2C_BUS_ERROR;
    }

    const esp_err_t r = i2c_master_transmit(g_dev_handle, data, (size_t)count, 1000);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "i2c write failed addr=0x%02x: %s", (unsigned)address, esp_err_to_name(r));
        return I2C_BUS_ERROR;
    }
    return NO_ERROR;
}
