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
#include <errno.h>

#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include "esp_log.h"
#include "esp_log_buffer.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/debug.h"

#define STACK_SIZE 1024*10
#include <semaphore.h>
#include <strings.h>
#include <arpa/inet.h>

#include "led.h"
#include "barrier_usb.h"

// FOR USB
extern const tusb_desc_device_t descriptor_dev_default;
extern const uint8_t descriptor_fs_cfg_default[];

#include "esp_log.h"
#include "esp_log_buffer.h"

#define CONNECTION_MAX_RETRY 10
// Wifi
#define DEFAULT_SCAN_LIST_SIZE 20
static wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE];
static uint16_t ap_count = DEFAULT_SCAN_LIST_SIZE;
static sem_t scan_sem;
static bool scan_done = false;
wifi_creds creds = {NULL, NULL};
enum wifi_status wifi_current_status = wifi_disconnected;
esp_netif_t *interface;
esp_netif_ip_info_t ip_interface;
remote_conn remote_barrier;
int barrier_socket = -1;

void set_wifi_creds_from_buffer(char* buf) {
	unsigned int len;
	char * buf2;
	if (creds.ssid != NULL)
		free(creds.ssid);
	if (creds.psk != NULL)
		free(creds.psk);
	len = strlen(buf);
	creds.ssid = malloc(len + 1);
	strncpy(creds.ssid, buf, len + 1);
	buf2 = buf + len + 1;
	len = strlen(buf2);
	creds.psk = malloc(len + 1);
	strncpy(creds.psk, buf2, len + 1);
}

void display_wifi_list(void) {
	char *module="display_wifi_list";
	for(unsigned int i=0; i<ap_count; i++) {
		ESP_LOGI(module, "Found %s (%02x:%02x:%02x:%02x:%02x)",ap_info[i].ssid, ap_info[i].bssid[0],
				ap_info[i].bssid[1], ap_info[i].bssid[2], ap_info[i].bssid[3],
				ap_info[i].bssid[4], ap_info[i].bssid[5]);
	}
}

esp_err_t start_wifi_scan() {
	char *module = "start_wifi_scan";
	esp_err_t ret;

	if (sem_trywait(&scan_sem) != 0) {
		ESP_LOGW(module, "A scan is already ongoing");
		return -1;
	}

	ret = esp_wifi_scan_start(NULL, false);
	ESP_ERROR_CHECK_WITHOUT_ABORT(ret);
	return ret;
}

esp_err_t get_buffer_from_scan_result(uint8_t **buffer, unsigned int *size) {
	unsigned int initial_offset , offset;
	unsigned int i;
	scan_result sr;
	char * module = "get_buffer_from_scan_result";

	if (! scan_done) {
		ESP_LOGE(module, "No scan has been done");
		return -2;
	}
	ESP_LOGD(module, "Number of ap: %d", ap_count);
       	initial_offset = offset = sizeof(int) + ap_count * sizeof(access_point);
	*size = offset;
	ESP_LOGD(module, "Initial offset: %d", initial_offset);
	for (i=0; i<ap_count; i++)
		*size += strlen((char*)ap_info[i].ssid) +1;

	ESP_LOGD(module, "Requested size of the buffer: %d", *size);
	*buffer = malloc(*size);
	if (*buffer == NULL) {
		ESP_LOGE(module, "Malloc failed, errno=%d", errno);
		return -3;
	}
	((scan_result*)*buffer)->result = ap_count;
	sr.access_point_tab = (access_point *)(*buffer + sizeof(int));
	for (i=0; i < ap_count; i++) {
		ESP_LOGD(module, "Wifi %s", ap_info[i].ssid);
		ESP_LOG_BUFFER_HEX_LEVEL(module, ap_info[i].bssid, 6, ESP_LOG_DEBUG);
		memcpy(sr.access_point_tab[i].bssid, ap_info[i].bssid, 6);
		sr.access_point_tab[i].ssid_offset = offset - initial_offset;
		strncpy((char*)*buffer + offset, (char*)ap_info[i].ssid, *size - offset);
		offset += strlen((char*)ap_info[i].ssid) + 1;
	}
	scan_done = false;
	ESP_LOG_BUFFER_HEX_LEVEL(module, *buffer, *size, ESP_LOG_DEBUG);
	return ESP_OK;
}	

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    char *module ="event_handler";
    static unsigned int s_retry_num = 0;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
	ESP_LOGD(module, "Wifi started in station mode");
        esp_wifi_connect(); 
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < CONNECTION_MAX_RETRY) {
	    wifi_current_status = wifi_connecting;
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(module, "retry to connect to the AP");
        } else {
	    wifi_current_status = wifi_disconnected;
        }
        ESP_LOGI(module,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
	ip_event_got_ip_t *ip_event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(module, "got ip:" IPSTR, IP2STR(&ip_event->ip_info.ip));
        s_retry_num = 0;
	ip_interface = ip_event->ip_info;
	wifi_current_status = ip_configured;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
	    ESP_LOGD(module, "Wifi is connected in station mode");
	    wifi_current_status = wifi_connected;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        wifi_event_sta_scan_done_t  *event = (wifi_event_sta_scan_done_t*) event_data;
        ESP_LOGI(module, "scan finished (status = %d, count = %d)", event->status, event->number);
	sem_post(&scan_sem);
	ap_count = event->number;
	if (event->status != 0) {
		ESP_LOGE(module, "Error in the scan");
		scan_done = false;
		return;
	}
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_records(&ap_count, ap_info));
	scan_done = true;
    }
}

//--------------------------------------------------------------------+
// BOS Descriptor
//--------------------------------------------------------------------+
/*
https://developers.google.com/web/fundamentals/native-hardware/build-for-webusb/
(Section Microsoft OS compatibility descriptors)
*/

#define BOS_TOTAL_LEN      (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

#define MS_OS_20_DESC_LEN  0x1E
#define VENDOR_REQUEST_MICROSOFT 2
//
// BOS Descriptor is required for windows to link to winusb driver
uint8_t const desc_bos[] =
{
  // total length, number of device caps
  TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),

  // Microsoft OS 2.0 descriptor
  TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT)
};

uint8_t const * tud_descriptor_bos_cb(void)
{
	char * module="tinyusb_bos_cb";
	ESP_LOGI(module, "In the bos descriptor");
	current_color.red = 0;
	current_color.green = 0xff;
	current_color.blue = 0;
  return desc_bos;
}


uint8_t const desc_ms_os_20[] =
{
  // Set header: length, type, windows version, total length
  U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

  // MS OS 2.0 Compatible ID descriptor: length, type, compatible ID, sub compatible ID
  U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // sub-compatible
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect size");

static uint8_t recv_buffer[200];
void recvTask(void *param);
void tlsRecvTask(void *param);
mbedtls_ssl_context tls;
mbedtls_ssl_config tls_conf;
mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context ctr_drbg;


// Invoked when a control transfer occurred on an interface of this class
// Driver response accordingly to the request and the transfer stage (setup/data/ack)
// return false to stall control endpoint (e.g unsupported request)
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
  char *module = "tud_vendor_control_xfer_cb";
  esp_err_t ret;
  struct sockaddr_in dest;

  ESP_LOGD(module, "Port: %d, Stage: %d, brequest: %x", rhport, stage, request->bRequest);
  // nothing to with ACK stage
  if (stage == CONTROL_STAGE_ACK) {
    if (request ->bRequest == VENDOR_REQUEST_WIFI_CONNECT) 
	    ESP_LOG_BUFFER_HEX_LEVEL(module, recv_buffer, 20, ESP_LOG_DEBUG);
    return true;
  }

  // DATA  stage
  if (stage == CONTROL_STAGE_DATA) {
    if (request ->bRequest == VENDOR_REQUEST_WIFI_CONNECT) {
	    ESP_LOGI(module, "Receiving wificreds");
	    wifi_current_status = wifi_connecting;
	    ESP_LOG_BUFFER_HEX_LEVEL(module, recv_buffer, 20, ESP_LOG_DEBUG);
	    set_wifi_creds_from_buffer((char *)recv_buffer);
	    wifi_config_t wifi_config = { 0 };
	    strncpy((char*)wifi_config.sta.ssid, creds.ssid, 32);
	    strncpy((char*)wifi_config.sta.password, creds.psk, 64);
	    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop() );
	    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA) );
	    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
	    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start() );
    } else if(request->bRequest == VENDOR_REQUEST_TCP_SET_REMOTE) {
	    ESP_LOGI(module, "receiving remote barrier address");
	    memcpy(&remote_barrier, &recv_buffer, sizeof(remote_barrier));
	    ESP_LOGD(module, "Setting remote ip %s and port %d", inet_ntop(AF_INET, (void*)&remote_barrier.ip, (char*)recv_buffer, 200),
			    ntohs(remote_barrier.port));
    }
    return true;
  }
  switch (request->bmRequestType_bit.type) {
    case TUSB_REQ_TYPE_VENDOR:
      ESP_LOGD(module, "Vendor bRequest %d", request->bRequest);
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

	case VENDOR_REQUEST_WIFI_SCAN:
          ESP_LOGI(module, "Received a wifi scan request");
	  ret = start_wifi_scan();
          ESP_LOGD(module, "Result of start of scan: %d", ret);
          return tud_control_xfer(rhport, request, (void*)&ret, sizeof(ret));

	case VENDOR_REQUEST_WIFI_GET_NET:
          ESP_LOGI(module, "Received a wifi scan result request");
	  int result = -1;
	  int value_sem;
	  uint8_t *buffer;
	  unsigned int size_buffer;
	  ESP_ERROR_CHECK(sem_getvalue(&scan_sem, &value_sem));
	  if (value_sem == 0) {
		ESP_LOGW(module, "Scan is already ongoing");
                return tud_control_xfer(rhport, request, (void*)&result, sizeof(result));
	  } 
	  result = get_buffer_from_scan_result(&buffer, &size_buffer);
	  if (result != 0) {
		ESP_LOGE(module, "Error from getting the scan result buffer: %d", result);
                return tud_control_xfer(rhport, request, (void*)&result, sizeof(result));
	  } 
          return tud_control_xfer(rhport, request, (void*)buffer, size_buffer);

	case VENDOR_REQUEST_WIFI_CONNECT:
	case VENDOR_REQUEST_TCP_SET_REMOTE:
            return tud_control_xfer(rhport, request, (void*)recv_buffer, sizeof(recv_buffer));

	case VENDOR_REQUEST_WIFI_GET_STATUS:
          ESP_LOGI(module, "Sending network status");
	  ESP_LOGD(module, "status : %d", wifi_current_status);
          return tud_control_xfer(rhport, request, (void*)&wifi_current_status, sizeof(wifi_current_status));

	case VENDOR_REQUEST_TCP_CONNECT:
          ESP_LOGI(module, "Establing connection to remote IP");
	  if (barrier_socket >= 0) {
		  ESP_LOGI(module, "Clearing previous socket");
		  close(barrier_socket);
	  }
	  barrier_socket = socket(AF_INET, SOCK_STREAM, 0);
	  if (barrier_socket < 0){
		  ESP_LOGE(module, "Fail to create socket: %d", barrier_socket);
		  return tud_control_xfer(rhport, request, (void*)&errno, sizeof(errno));
	  }
	  dest.sin_family = AF_INET;
	  dest.sin_port = remote_barrier.port;
	  dest.sin_addr.s_addr = remote_barrier.ip;;
	  ret = connect(barrier_socket, (void*)&dest, sizeof(dest));
	  if (ret < 0) {
		  ESP_LOGE(module, "Connection failed");
		  ret = -1 * errno;
		  return tud_control_xfer(rhport, request, (void*)&ret, sizeof(ret));
	  }
	  ESP_LOGI(module, "TCP Connected");
	  if (remote_barrier.use_tls) {
		  mbedtls_ssl_session_reset(&tls);
		  mbedtls_ssl_set_bio(&tls, (void*)barrier_socket, (mbedtls_ssl_send_t*) write, 
				  (mbedtls_ssl_recv_t*) read, NULL);
		  ret = mbedtls_ssl_handshake(&tls);
		  if (ret != 0) {
			  ESP_LOGE(module, "Handshake failed : %d", ret);
          		  return tud_control_xfer(rhport, request, (void*)&ret, sizeof(ret));
		  }

	  }
	  if (remote_barrier.use_tls) {
		  xTaskCreate(tlsRecvTask, "tls_recv", STACK_SIZE,
			 NULL, tskIDLE_PRIORITY, NULL);
	  } else {
		  xTaskCreate(recvTask, "recv", STACK_SIZE,
			 NULL, tskIDLE_PRIORITY, NULL);
	  }

          return tud_control_xfer(rhport, request, (void*)&ret, sizeof(ret));

	case VENDOR_REQUEST_WIFI_STOP:
          ESP_LOGI(module, "Stopping wifi");
	  ret = esp_wifi_stop();
          return tud_control_xfer(rhport, request, (void*)&ret, sizeof(ret));
	case VENDOR_REQUEST_TCP_DISCONNECT:
	  ESP_LOGI(module, "Disconnecting the TCP connection");
	  if (barrier_socket >= 0) {
		  if (remote_barrier.use_tls){
			  mbedtls_ssl_close_notify(&tls);
			  ESP_LOGI(module, "Freeing TLS  session context");
			  mbedtls_ssl_session_reset(&tls);
		  }
		  shutdown(barrier_socket, SHUT_RDWR);
		  close(barrier_socket);
		  barrier_socket = -1;
	  } else {
		  ESP_LOGW(module, "Socket already closed");
	  }
	  tud_control_status(rhport, request);
        default: 
	  break;
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
  char *module = "tud_vendor_rx_cb";

  ESP_LOGD(module, "Size: %d", bufsize);
  //ESP_LOG_BUFFER_HEX_LEVEL(module, buffer, bufsize, ESP_LOG_DEBUG);

  if (barrier_socket < 0) {
	  ESP_LOGE(module, "No connection established");
  } else {
	  if(remote_barrier.use_tls) {
		  if (mbedtls_ssl_write(&tls, buffer, bufsize)  < 0) {
			  ESP_LOGE(module, "Error while sending data : %d", errno);
			  close(barrier_socket);
			  barrier_socket = -1;
		  }
	  } else {
		  if (send(barrier_socket, buffer, bufsize, 0) == -1) {
			  ESP_LOGE(module, "Error while sending data : %d", errno);
			  close(barrier_socket);
			  barrier_socket = -1;
		  }
	  }
  }
  // if using RX buffered is enabled, we need to flush the buffer to make room for new data
  #if CFG_TUD_VENDOR_RX_BUFSIZE > 0
  tud_vendor_read_flush();
  #endif
}

void recvTask(void *param) {
	char module[] = "recvTask";
	uint8_t buffer[CONFIG_TINYUSB_VENDOR_TX_BUFSIZE];
	int len;
	while(true) {
		len = recv(barrier_socket, buffer, CONFIG_TINYUSB_VENDOR_TX_BUFSIZE, 0);
		if (len == -1) {
			ESP_LOGE(module, "Recv failed: %d", errno);
			shutdown(barrier_socket, SHUT_RDWR);
			close(barrier_socket);
			barrier_socket = -1;
			vTaskDelete(NULL);
		} else {
		ESP_LOGD(module, "len: %d", len);
		//ESP_LOG_BUFFER_HEX_LEVEL(module, buffer, len, ESP_LOG_DEBUG);
		while (tud_vendor_write_available()<len){
			ESP_LOGD(module, "Not ready, waiting...");
        		vTaskDelay(1 / portTICK_PERIOD_MS);
		}

		tud_vendor_write(buffer, len);
		tud_vendor_write_flush();
		}
	}
}

void tlsRecvTask(void *param) {
	char module[] = "tlsRecvTask";
	uint8_t buffer[CONFIG_TINYUSB_VENDOR_TX_BUFSIZE];
	int len;
	while(true) {
		len = mbedtls_ssl_read(&tls, buffer, CONFIG_TINYUSB_VENDOR_TX_BUFSIZE);
		if (len < 0) {
			ESP_LOGE(module, "tlsRecv failed: %d", len);
			mbedtls_ssl_close_notify(&tls);
			shutdown(barrier_socket, SHUT_RDWR);
			close(barrier_socket);
			barrier_socket = -1;
			vTaskDelete(NULL);
		} else {
		  ESP_LOGD(module, "len: %d", len);
		  //ESP_LOG_BUFFER_HEX_LEVEL(module, buffer, len, ESP_LOG_DEBUG);
		while (tud_vendor_write_available()<len){
			ESP_LOGD(module, "Not ready, waiting...");
        		vTaskDelay(1 / portTICK_PERIOD_MS);
		}
		tud_vendor_write(buffer, len);
		tud_vendor_write_flush();
		}
	}
}

void app_main(void)
{
    char *module = "app_main";
    int ret;
    char *buffer;

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    // USB
    ESP_LOGI(module, "Starting main app...");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    
    static tusb_desc_device_t my_dev;
    memcpy(&my_dev, &descriptor_dev_default, sizeof(tusb_desc_device_t));
    tusb_cfg.descriptor.device = &my_dev;
    my_dev.bcdUSB= 0x210;
    tusb_cfg.descriptor.full_speed_config = descriptor_fs_cfg_default;
    ESP_LOGD(module, "Value of configuration descriptor: %d, fs: %d, hs: %d", tusb_cfg.descriptor.device,
		    tusb_cfg.descriptor.full_speed_config, tusb_cfg.descriptor.high_speed_config);
    ESP_LOGD(module, "Value of configuration device usb version: %d", tusb_cfg.descriptor.device->bcdUSB);
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    // WIFI 
    ESP_ERROR_CHECK(esp_netif_init());
    interface = esp_netif_create_default_wifi_sta();
    ESP_LOGI(module, "Starting wifi configuration...");
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    wifi.nvs_enable = false;
    ESP_ERROR_CHECK(sem_init(&scan_sem, 0, 1));
    ESP_ERROR_CHECK(esp_wifi_init(&wifi));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));   //It's default
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(module, "Starting task configuration...");
    init_led();

    //xTaskCreate(colorTask, "color", STACK_SIZE,
//
//		 NULL, tskIDLE_PRIORITY, NULL);

    ESP_LOGI(module, "Initialisation of TLS configuration...");
    mbedtls_ctr_drbg_init( &ctr_drbg );

    mbedtls_entropy_init( &entropy );
    const char pers[] = "Barrier USB";
    if( ( ret = mbedtls_ctr_drbg_seed( &ctr_drbg, mbedtls_entropy_func, &entropy,
                           (const unsigned char*)pers,
                           strlen( pers ) ) ) != 0 )
    {
	buffer = (char*) malloc(200);
	mbedtls_strerror(ret, buffer, 200);
	ESP_LOGE(module, "Failed to initialized RNG(%d): %s", ret, buffer);
	free(buffer);
    }
    mbedtls_ssl_config_init(&tls_conf);
    ESP_ERROR_CHECK(mbedtls_ssl_config_defaults(&tls_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT));
    mbedtls_ssl_conf_authmode( &tls_conf, MBEDTLS_SSL_VERIFY_NONE );
    mbedtls_ssl_conf_rng( &tls_conf, mbedtls_ctr_drbg_random, &ctr_drbg );
    mbedtls_ssl_init(&tls);
    ret=mbedtls_ssl_setup(&tls, &tls_conf);
    if (ret != 0) {
	buffer = (char*) malloc(200);
	mbedtls_strerror(ret, buffer, 200);
	ESP_LOGE(module, "Failed to initialized tls context (%d): %s", ret, buffer);
	free(buffer);
    }


    ESP_LOGI(module, "Enf of initialisation");

    while(1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    };
}
