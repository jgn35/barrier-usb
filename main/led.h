/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#define LED_DI_PIN 40
#define LED_CI_PIN 39

#define SPI_LED SPI2_HOST

typedef struct {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
	uint8_t transparency;} color;

extern color current_color;

void init_led();
void set_led_color(uint8_t brightness, uint8_t r, uint8_t g, uint8_t b);
void colorTask(void *param);
