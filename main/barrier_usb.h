#ifndef _BARRIER_USB_H
#define _BARRIER_USB_H

#include <sys/socket.h>
#include <netinet/in.h>

#define VENDOR_REQUEST_WIFI_SCAN 0x10
#define VENDOR_REQUEST_WIFI_GET_NET 0x11
#define VENDOR_REQUEST_WIFI_CONNECT 0x12
#define VENDOR_REQUEST_WIFI_GET_STATUS 0x13
#define VENDOR_REQUEST_WIFI_STOP 0x14
#define VENDOR_REQUEST_TCP_SET_REMOTE 0x20
#define VENDOR_REQUEST_TCP_CONNECT 0x21
#define VENDOR_REQUEST_TCP_GET_STATUS 0x22
#define VENDOR_REQUEST_TCP_DISCONNECT 0x23

typedef struct __attribute__((packed)) {
	uint8_t bssid[6];
	uint16_t ssid_offset;
} access_point ;
typedef struct {
	int result;
	access_point *access_point_tab;
	char *ssid_name;
} scan_result;
typedef struct {
	char* ssid;
	char* psk;
} wifi_creds;
enum wifi_status {
	wifi_disconnected,
	wifi_connecting,
	wifi_connected,
	ip_configured,
};
typedef struct {
	in_addr_t ip;
	in_port_t port;
	bool use_tls;
} remote_conn;
typedef struct {
	uint16_t size;
	enum wifi_status status;
	in_addr_t ip;
	char ssid[];
} usb_wifi_status;
#endif
