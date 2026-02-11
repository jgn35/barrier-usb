/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
//#include "vendor_device.h"

#include "driver/spi_common.h"
#include "driver/spi_master.h"

#define STACK_SIZE 1024*10

#include "led.h"

void otherTask(void* param) {
	unsigned int i=0;
	while(true) {
		printf("Executing for %d time...\n", i);
		vTaskDelay(1000 / portTICK_PERIOD_MS);
		i++;
	}

}

//--------------------------------------------------------------------+
// BOS Descriptor
//--------------------------------------------------------------------+

/* Microsoft OS 2.0 registry property descriptor
Per MS requirements https://msdn.microsoft.com/en-us/library/windows/hardware/hh450799(v=vs.85).aspx
device should create DeviceInterfaceGUIDs. It can be done by driver and
in case of real PnP solution device should expose MS "Microsoft OS 2.0
registry property descriptor". Such descriptor can insert any record
into Windows registry per device/configuration/interface. In our case it
will insert "DeviceInterfaceGUIDs" multistring property.

GUID is freshly generated and should be OK to use.

https://developers.google.com/web/fundamentals/native-hardware/build-for-webusb/
(Section Microsoft OS compatibility descriptors)
*/

#define BOS_TOTAL_LEN      (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

#define MS_OS_20_DESC_LEN  0xB2
#define VENDOR_REQUEST_MICROSOFT 2
#define ITF_NUM_VENDOR 2

// BOS Descriptor is required for webUSB
uint8_t const desc_bos[] =
{
  // total length, number of device caps
  TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),

  // Microsoft OS 2.0 descriptor
  TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT)
};

uint8_t const * tud_descriptor_bos_cb(void)
{
	printf("In the bos descriptorr\n");
	current_color.red = 0;
	current_color.green = 0xff;
	current_color.blue = 0;
  return desc_bos;
}


uint8_t const desc_ms_os_20[] =
{
  // Set header: length, type, windows version, total length
  U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

  // Configuration subset header: length, type, configuration index, reserved, configuration total length
  U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN-0x0A),

  // Function Subset header: length, type, first interface, reserved, subset length
  U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_NUM_VENDOR, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN-0x0A-0x08),

  // MS OS 2.0 Compatible ID descriptor: length, type, compatible ID, sub compatible ID
  U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // sub-compatible

  // MS OS 2.0 Registry property descriptor: length, type
  U16_TO_U8S_LE(MS_OS_20_DESC_LEN-0x0A-0x08-0x08-0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
  U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), // wPropertyDataType, wPropertyNameLength and PropertyName "DeviceInterfaceGUIDs\0" in UTF-16
  'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
  'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
  U16_TO_U8S_LE(0x0050), // wPropertyDataLength
	//bPropertyData: “{975F44D9-0D08-43FD-8B3E-127CA8AFFF9D}”.
  '{', 0x00, '9', 0x00, 'd', 0x00, 'b', 0x00, 'b', 0x00, '2', 0x00, 'a', 0x00, 'e', 0x00, '4', 0x00, '-', 0x00, '9', 0x00, 'f', 0x00, 
  '1', 0x00, 'a', 0x00, '-', 0x00, '4', 0x00, '9', 0x00, 'e', 0x00, 'e', 0x00, '-', 0x00, 'b', 0x00, '4', 0x00, 'b', 0x00, '9', 0x00, 
  '-', 0x00, '4', 0x00, '0', 0x00, '4', 0x00, '4', 0x00, '2', 0x00, 'f', 0x00, '7', 0x00, '5', 0x00, 'd', 0x00, '2', 0x00, '9', 0x00, 
  'c', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect size");

//--------------------------------------------------------------------+
// WebUSB use vendor class
//--------------------------------------------------------------------+

// Invoked when a control transfer occurred on an interface of this class
// Driver response accordingly to the request and the transfer stage (setup/data/ack)
// return false to stall control endpoint (e.g unsupported request)
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
  // nothing to with DATA & ACK stage
  if (stage != CONTROL_STAGE_SETUP) {
    return true;
  }

  switch (request->bmRequestType_bit.type) {
    case TUSB_REQ_TYPE_VENDOR:
      switch (request->bRequest) {
        case VENDOR_REQUEST_MICROSOFT:
          if (request->wIndex == 7) {
            // Get Microsoft OS 2.0 compatible descriptor
            uint16_t total_len;
            memcpy(&total_len, desc_ms_os_20 + 8, 2);

            return tud_control_xfer(rhport, request, (void*)(uintptr_t)desc_ms_os_20, total_len);
          } else {
            return false;
          }

        default: break;
      }
      break;

    case TUSB_REQ_TYPE_CLASS:
      if (request->bRequest == 0x22) {
	      /*
        // Webserial simulate the CDC_REQUEST_SET_CONTROL_LINE_STATE (0x22) to connect and disconnect.
        web_serial_connected = (request->wValue != 0);

          tud_vendor_write_str("\r\nWebUSB interface connected\r\n");
          tud_vendor_write_flush();
        } else {
          tud_vendor_write_clear(); // anything left in the buffer is now thrown out
          blink_interval_ms = BLINK_MOUNTED;
        }

        // response with status OK
        return tud_control_status(rhport, request);*/
      }
      break;

    default: break;
  }

  // stall unknown request
  return false;
}

void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize) {
  (void)idx;
  (void)buffer;
  (void)bufsize;
/*
  while (tud_vendor_available()) {
    uint8_t        buf[64];
    const uint32_t count = tud_vendor_read(buf, sizeof(buf));
    echo_all(buf, count);
  }*/
}
void app_main(void)
{

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    tinyusb_config_cdcacm_t acm_cfg = { 0 }; // the configuration uses default values
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));

    ESP_ERROR_CHECK(tinyusb_console_init(TINYUSB_CDC_ACM_0)); // log to usb

    init_led();
    xTaskCreate(otherTask, "loop1", STACK_SIZE,
		 NULL, tskIDLE_PRIORITY, NULL);

    xTaskCreate(colorTask, "color", STACK_SIZE,
		 NULL, tskIDLE_PRIORITY, NULL);

    printf("Hello world!\n");

    while(1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    };
}
