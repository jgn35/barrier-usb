/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "led.h"

static spi_device_handle_t led;

color current_color = { 0xff, 0, 0, 0xff};

void set_led_color(uint8_t brightness, uint8_t r, uint8_t g, uint8_t b) {
	char *module = "set_led_color";
	esp_err_t ret;
	uint8_t data[12] = { 0x00, 0x00, 0x00, 0x00, 0xe0 | (brightness & 0x1f), b, g, r, 0xff, 0xff, 0xff, 0xff};
	
	spi_transaction_t tr;
	memset(&tr, 0, sizeof(tr));
	tr.length=12*8;
	tr.tx_buffer=data;
	tr.rx_buffer=NULL;
	ret=spi_device_polling_transmit(led, &tr);
	ESP_LOGD(module, "result %d (OK = %d, INVALID_ARG = %d, TIMEOUT = %d, NO_MEM = %d, INVALID_STATE=%d\n",
			ret, ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM, ESP_ERR_INVALID_STATE);
	ESP_ERROR_CHECK(ret);
}

void init_led() {
	const spi_bus_config_t config = {
		.mosi_io_num = LED_DI_PIN,
		.sclk_io_num = LED_CI_PIN,
		.miso_io_num = -1,
		.data1_io_num = -1,
		.quadwp_io_num = -1,
		.data2_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 12
	};
	char *module = "init_led";
	ESP_ERROR_CHECK(spi_bus_initialize(SPI_LED, &config, SPI_DMA_CH_AUTO));
        ESP_LOGD(module, "spi_bus_initialized\n");
	const spi_device_interface_config_t dev_conf = {
		.command_bits = 0,
		.address_bits = 0,
		.dummy_bits = 0,
		.mode = 0,
		.clock_speed_hz = 1000 * 1000,
		.spics_io_num = -1,
		.queue_size = 2,
		.duty_cycle_pos = 0
	};
	ESP_ERROR_CHECK(spi_bus_add_device(SPI_LED, &dev_conf, &led));
	ESP_LOGD(module, "spi_device added: %p\n", led);
}

void colorTask(void *param) {
	while(true) {
		set_led_color(current_color.transparency, current_color.red, current_color.green, current_color.blue);
		vTaskDelay(500 / portTICK_PERIOD_MS);
		set_led_color(0xff, 0, 0, 0);
		vTaskDelay(500 / portTICK_PERIOD_MS);
	}
}
