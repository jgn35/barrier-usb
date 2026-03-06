// gcc -o test main.c -lusb-1.0 `pkg-config gnutls --cflags --libs` -lpthread -lconfig
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>
#include <string.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <gnutls/gnutls.h>
#include <pthread.h>
#include <libconfig.h>
#include "barrier_usb.h"

#define VENDOR_ID 0x303a
#define PRODUCT_ID 0x5678

#define WIFI_CONFIG_FILE "wifi.conf"
#define WIFI_INTERFACE "wlan0"

#define IP_TXT_MAX_LENGTH 16

#define FILE_CERTIFICATE "cert.pem"
#define FILE_KEY "key.pem"

char **wifi_ssids = NULL;
char **wifi_keys = NULL;
size_t wifi_list_length = 0;

int create_server(char * if_name, remote_conn *port) {
	struct ifaddrs *ifap = NULL, *ifa = NULL;
	struct sockaddr_in local_if;
	bool found_ip = false;
	char txt_ip[IP_TXT_MAX_LENGTH];
	int soc, len;

	if (getifaddrs(&ifap) != 0) {
		perror("Error getting info about interfaces");
	       return -1;
	}	       
	for(ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		if(!ifa->ifa_addr) {
			continue;
		}
		if (strcmp(ifa->ifa_name, if_name) != 0) {
			continue;
		}
		if (ifa->ifa_addr->sa_family != AF_INET) {
			continue;
		}
		memcpy(&local_if, ifa->ifa_addr, sizeof(struct sockaddr_in));
		found_ip = true;
		break;
	}
	local_if.sin_port = 0;
	freeifaddrs(ifap);
	if (! found_ip) {
		printf("Interface %S not found\n", if_name);
		return -2;
	}
	printf("Found ip %s for interface %s\n", inet_ntop(AF_INET, &local_if.sin_addr, txt_ip, IP_TXT_MAX_LENGTH),
			if_name);
	soc = socket(AF_INET, SOCK_STREAM, 0);
	if (soc == -1 ) {
		perror("Creating server socket");
		return -3;
	}
	if (bind(soc, (struct sockaddr*)&local_if, sizeof(local_if)) != 0) {
		perror("Binding the socket");
		return -4;
	}
	len = sizeof(local_if);
	if (getsockname(soc, (struct sockaddr*)&local_if, &len) != 0) {
		perror("Retrieving port number");
		return -5;
	}
	printf("Port allocated %d\n", ntohs(local_if.sin_port));
	port->ip = local_if.sin_addr.s_addr;
	port->port = local_if.sin_port;

	if (listen(soc, 5) != 0) {
		perror("Listening socket");
		return -6;
	}
	return soc;
}

void get_request_status_cb(struct libusb_transfer *tr){
	printf("Status of control request for function %d: %d\n", tr->user_data, tr->status);
}

struct tls_accept_args {
	int socket_server;
	gnutls_session_t *tls;
	int *client_socket;
};

void *tls_accept(struct tls_accept_args *args) {
	struct sockaddr_in remote_ip;
	int len = sizeof(remote_ip);
	int client_socket, ret;
	printf("Waiting for the TLS connection\n");
	*(args->client_socket) = client_socket = accept(args->socket_server, (struct sockaddr*)&remote_ip, &len) ;
	if (client_socket < 0) {
		perror("Failed to get the client socket");
		printf("Retrying...\n");
		if (client_socket = accept(args->socket_server, (struct sockaddr*)&remote_ip, &len) < 0) {
			perror("Failed again");
			return (void*)EXIT_FAILURE;
		}
	}
	printf("Connection received(%d)\n", client_socket);

	// We initiate the TLS handshake
	gnutls_transport_set_int(*(args->tls), client_socket);
	ret = gnutls_handshake(*(args->tls));
	if (ret < 0) {
		printf("Failed to do handshake : %d\n", ret);
		return (void*)EXIT_FAILURE;
	}
	return (void*)EXIT_SUCCESS;

}

void parse_config_file(config_t *wifi_config) {
	config_setting_t *wifi_list, *current, *child;
	unsigned int i;

	current = config_root_setting(wifi_config);
	wifi_list = config_setting_get_member(current, "wifi_list");

	wifi_list_length = config_setting_length(wifi_list);
	wifi_ssids = (char**) malloc(sizeof(char*) * wifi_list_length);
	wifi_keys = (char**) malloc(sizeof(char*) * wifi_list_length);

	for(i=0; i < wifi_list_length; i++) {
		current = config_setting_get_elem(wifi_list, i);
		config_setting_lookup_string(current, "ssid", (const char **)&wifi_ssids[i]);
		config_setting_lookup_string(current, "psk", (const char **)&wifi_keys[i]);
	}
	
}
int main(int argc, char **argv, char **env){
	libusb_context *usb_ctx;
	libusb_device_handle *usb_dev_h;
	libusb_device *usb_devi = NULL;
	libusb_device **usb_dev_list;
	struct libusb_transfer tr = {0};

	int socket_server;
	
	size_t num_devices;
	unsigned int i;

	remote_conn test_conn;

	config_t wifi_config;

	config_init(&wifi_config);
	if (config_read_file(&wifi_config, WIFI_CONFIG_FILE) == CONFIG_FALSE) {
		printf("Failed to read config file %s line %d\n", WIFI_CONFIG_FILE, config_error_line(&wifi_config));
		return EXIT_FAILURE;
	}
	printf("Parsing config file");
	parse_config_file(&wifi_config);


	libusb_init_context(&usb_ctx, NULL, 0);
	num_devices = libusb_get_device_list(usb_ctx, &usb_dev_list);
	printf("Found %d USB devices\n", num_devices);
	for (i=0; i < num_devices; i++){
		libusb_device_handle *usb_dev_i;
		struct libusb_device_descriptor dev_desc;
		char product_str[200];
		int size=0;
		if (libusb_open(usb_dev_list[i], &usb_dev_i) != 0)
			continue;
		if (libusb_get_device_descriptor(usb_dev_list[i], &dev_desc) == 0){
			size = libusb_get_string_descriptor_ascii(usb_dev_i, dev_desc.iManufacturer, product_str, 200);
			if (size > 0)
				printf("\t%04x:%04x %s\n", dev_desc.idVendor, dev_desc.idProduct, product_str);
			else
				printf("\t%04x:%04x UNKNOWN\n", dev_desc.idVendor, dev_desc.idProduct);
		}
		if (dev_desc.idVendor == VENDOR_ID && dev_desc.idProduct == PRODUCT_ID)
			usb_devi = usb_dev_list[i];
		libusb_close(usb_dev_i);
	}

	if (usb_devi == NULL) {
		printf("Device not found\n");
		return EXIT_FAILURE;
	}
	printf("Device found\n");
	if (libusb_open(usb_devi, &usb_dev_h) != 0){
		printf("Fail to open the device\n");
		return EXIT_FAILURE;
	}
	libusb_free_device_list(usb_dev_list, 1);

	socket_server = create_server(WIFI_INTERFACE, &test_conn);
	unsigned char *buffer = malloc(20);
	printf("Preparing control request for Wifi scan\n");
	//libusb_fill_control_setup(buffer, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT,
	//				VENDOR_REQUEST_WIFI_SCAN, 0, 0, 0);
	//libusb_fill_control_transfer(&tr, usb_dev_h, buffer, get_request_status_cb, (void *)VENDOR_REQUEST_WIFI_SCAN, 1000);
	printf("Sending control request for Wifi scan\n");
	//if(libusb_submit_transfer(&tr) != 0) {
	//	printf("Fail to submit transfer\n");
	//	return EXIT_FAILURE;
	//}
	int status, btransfered;
	btransfered = libusb_control_transfer(usb_dev_h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN, 
				VENDOR_REQUEST_WIFI_SCAN, 0, 2, (unsigned char*)&status, sizeof(status), 2000);
        if (btransfered	< 0) {
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}
	if (status != 0) {
		printf("Failed to launch a wifi scan\n");
		return EXIT_FAILURE;
	}
	btransfered = libusb_control_transfer(usb_dev_h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN, 
				VENDOR_REQUEST_WIFI_SCAN, 0, 0, (unsigned char*)&status, sizeof(status), 2000);
       if (btransfered	< 0) {
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}
	if (status == 0) {
		printf("Strange, it successed instead of failing\n");
	}

	// We try to get the result of the scan
	bool success = false;
	scan_result sr;
	uint8_t usb_buffer[2000];
	while (!success) {
		btransfered = libusb_control_transfer(usb_dev_h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN, 
					VENDOR_REQUEST_WIFI_GET_NET, 0, 0, (unsigned char*)usb_buffer, 2000, 1000);
	       if (btransfered	< 0) {
			printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
			return EXIT_FAILURE;
		}
	       if (btransfered == 4){
		       switch (*(int*)usb_buffer) {
			 	case -1: 
					printf("Scan still ongoing...\n");
					break;
				case -2:
					printf("No scan result now\n");
					success = true;
					break;
				case -3:
					printf("Error in malloc\n");
					success = true;
		       }
		       sleep(1);
	       } else {
		       success = true;
	       }
	}
	sr.result = *(int *)usb_buffer;
	if (sr.result < 0) {
		printf("Error in scan\n");
		return EXIT_FAILURE;
	}
	printf("Found %d networks\n", sr.result);
	printf("sizeof(access_point) = %d\n", sizeof(access_point));
	sr.access_point_tab = (access_point*)(usb_buffer + 4);
	sr.ssid_name = (char *)(usb_buffer + 4 + sr.result * sizeof(access_point));

	char *ssid, *key;
	printf("Scan results:\n");
	for(i=0; i< sr.result; i++) {
		printf("ssid ofset = %d\n", sr.access_point_tab[i].ssid_offset);
		printf("\t[%02x:%02x:%02x:%02x:%02x:%02x] %s\n", sr.access_point_tab[i].bssid[0], sr.access_point_tab[i].bssid[1], 
				sr.access_point_tab[i].bssid[2], sr.access_point_tab[i].bssid[3], sr.access_point_tab[i].bssid[4], 
				sr.access_point_tab[i].bssid[5], sr.ssid_name + sr.access_point_tab[i].ssid_offset);
		for(unsigned int j=0; j < wifi_list_length; j ++) {
			if (strcmp(wifi_ssids[j], sr.ssid_name+sr.access_point_tab[i].ssid_offset) == 0) {
				ssid = wifi_ssids[j];
				key = wifi_keys[j];
				printf("Found known network %s\n", ssid);
			}
		}
	}

	printf("Starting connection to wifi\n");
	// We search the known wifi network
	#define WIFI_LENGTH (strlen(ssid) + strlen(key) +2)
	char *buffer_creds = malloc(WIFI_LENGTH);
	strncpy(buffer_creds, ssid, WIFI_LENGTH);
	buffer_creds[strlen(ssid)] = 0;
	strncpy(buffer_creds + strlen(ssid) + 1, key, WIFI_LENGTH - strlen(ssid) - 1);
	buffer_creds[WIFI_LENGTH - 1] = 0;

	btransfered = libusb_control_transfer(usb_dev_h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT, 
				VENDOR_REQUEST_WIFI_CONNECT, 0, 0, buffer_creds, WIFI_LENGTH, 2000);
        if (btransfered	< 0) {
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}
	// We get the status of the wifi connection
	enum wifi_status wstatus = wifi_connecting;
	while ((wstatus != wifi_disconnected) && (wstatus != ip_configured)) {
		btransfered = libusb_control_transfer(usb_dev_h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN, 
					VENDOR_REQUEST_WIFI_GET_STATUS, 0, 0, (unsigned char*)&wstatus, sizeof(enum wifi_status), 2000);
		if (btransfered	< 0) {
			printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
			return EXIT_FAILURE;
		}
		printf(".");
		fflush(NULL);
		sleep(1);
	}
	if (wstatus == wifi_disconnected ) {
		printf("Connection failed\n");
		return EXIT_FAILURE;
	}
	printf("Connected\n");

	//
	// We set the remote Barrier USB (here this program)
	printf("Setting TCP info\n");
	test_conn.use_tls = false;
	btransfered = libusb_control_transfer(usb_dev_h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT, 
				VENDOR_REQUEST_TCP_SET_REMOTE, 0, 0, (unsigned char*)&test_conn, sizeof(test_conn), 2000);
        if (btransfered	< 0) {
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}

	// We connect to the configured port
	printf("Starting connection\n");
	btransfered = libusb_control_transfer(usb_dev_h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN, 
				VENDOR_REQUEST_TCP_CONNECT, 0, 0, (unsigned char*)&status, sizeof(status), 2000);
        if (btransfered	< 0) {
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}
	printf("Status of the connection  :%d\n", status);

	struct sockaddr_in remote_ip;
	int len = sizeof(remote_ip);
	int client_socket;

	printf("Waiting for the connection\n");
	client_socket = accept(socket_server, (struct sockaddr*)&remote_ip, &len) ;
	if (client_socket < 0) {
		perror("Failed to get the client socket");
		printf("Retrying...\n");
		if (client_socket = accept(socket_server, (struct sockaddr*)&remote_ip, &len) < 0) {
			perror("Failed again");
			return EXIT_FAILURE;
		}
	}
	printf("Connection received(%d)\n", client_socket);

	// Sending Hello to the USB device
	const char hello[] = "hello";
	const char test[] = "test";
	printf("Sending %s to USB\n", hello);

	if (btransfered = libusb_bulk_transfer(usb_dev_h, 0x1, hello, sizeof(hello), &len, 2000) != 0){
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}
	if (recv(client_socket, usb_buffer, sizeof(usb_buffer), 0)  == -1) {
		perror("Reading socket");
		return EXIT_FAILURE;
	}
	printf("Received %s from socket\n", usb_buffer);

	printf("Sending %s to network\n", test);
	if (send(client_socket, test, sizeof(test), 0)  == -1) {
		perror("sending socket");
		return EXIT_FAILURE;
	}

	if (btransfered = libusb_bulk_transfer(usb_dev_h, 0x81, usb_buffer, sizeof(usb_buffer), &len, 2000) != 0){
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}
	printf("Received %s from USB\n", usb_buffer);

	// Transmitting a big buffer
	uint8_t bigbuffer[5000];
	uint8_t second_buffer[5000];
	unsigned int count;
	int rf = open("/dev/random", O_RDONLY);
	read(rf, bigbuffer, sizeof(bigbuffer));
	close(rf);

	printf("Sending a buffer of size %d to USB\n", sizeof(bigbuffer));

	if (btransfered = libusb_bulk_transfer(usb_dev_h, 0x1, bigbuffer, sizeof(bigbuffer), &len, 2000) != 0){
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}
	printf("We send %d byte over USB\n", len);
	len = 0;
	while (len != 5000) {
		if ((count=recv(client_socket, second_buffer + len, sizeof(second_buffer) - len, 0))  == -1) {
			perror("Reading socket");
			return EXIT_FAILURE;
		}
		len += count;
	}
	printf("We read %d bytes from network\n", len);
	if (memcmp(bigbuffer, second_buffer, sizeof(bigbuffer)) != 0) {
		printf("The readen bytes are not the same as the one writen\n");
		return EXIT_FAILURE;
	}
	printf("It matches ! USB -> socket\n");

	rf = open("/dev/random", O_RDONLY);
	read(rf, bigbuffer, sizeof(bigbuffer));
	close(rf);

	printf("Sending a buffer of size %d to network\n", sizeof(bigbuffer));
	if (send(client_socket, bigbuffer, sizeof(bigbuffer), 0)  == -1) {
		perror("sending socket");
		return EXIT_FAILURE;
	}

	count = 0;
	while (count != 5000) {
		if (btransfered = libusb_bulk_transfer(usb_dev_h, 0x81, second_buffer+count, sizeof(second_buffer)-count, &len, 2000) != 0){
			printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
			return EXIT_FAILURE;
		}
		count+=len;
	}
	printf("We read %d bytes from USB\n", count);
	if (memcmp(bigbuffer, second_buffer, sizeof(bigbuffer)) != 0) {
		printf("The readen bytes are not the same as the one writen\n");
		return EXIT_FAILURE;
	}
	printf("It matches ! socket -> USB\n");

	printf("Disconnecting USB socket\n");
	btransfered = libusb_control_transfer(usb_dev_h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT, 
				VENDOR_REQUEST_TCP_DISCONNECT, 0, 0, NULL, 0, 2000);
        if (btransfered	< 0) {
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}

	sleep(1);
	printf("Sending %s to network\n", test);
	if ((len=send(client_socket, test, sizeof(test), 0))  != -1) {
		printf("It should failed and it's not the case... %d\n", len);
		//return EXIT_FAILURE;
	} else {
		printf("It failed as expected\n");
	}


	// We set the remote Barrier USB (here this program)
	printf("Setting TCP info with ssl\n");
	test_conn.use_tls = true;
	btransfered = libusb_control_transfer(usb_dev_h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT, 
				VENDOR_REQUEST_TCP_SET_REMOTE, 0, 0, (unsigned char*)&test_conn, sizeof(test_conn), 2000);
        if (btransfered	< 0) {
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}

	unsigned int ret;
	// Preparing TLS communication on our side
	if (gnutls_global_init() < 0) {
		printf("Failed to initialize GnuTLS\n");
		return EXIT_FAILURE;
	}
	gnutls_session_t tls;
	gnutls_certificate_credentials_t cert;
	ret = gnutls_init(&tls, GNUTLS_SERVER);
	if (ret < 0) {
		printf("Failed to initialize TLS session\n");
		return EXIT_FAILURE;
	}
	ret = gnutls_certificate_allocate_credentials(&cert);
	if (ret < 0) {
		printf("Failed to initialize certificate allocation\n");
		return EXIT_FAILURE;
	}
	ret = gnutls_certificate_set_x509_key_file2(cert, FILE_CERTIFICATE, FILE_KEY, GNUTLS_X509_FMT_PEM, NULL, 0);
	if (ret < 0) {
		printf("Failed to load certificate\n");
		return EXIT_FAILURE;
	}
	ret = gnutls_credentials_set(tls, GNUTLS_CRD_CERTIFICATE, (void*)cert);
	if (ret < 0) {
		printf("Failed to associate certificate with session\n");
		return EXIT_FAILURE;
	}
	
	// Apply priority in ciphers
	gnutls_priority_t tls_p;

	ret = gnutls_priority_init2 (&tls_p, "NORMAL", NULL, 0);
	if (ret < 0) {
		printf("Failed to set priority in ciphers\n");
		return EXIT_FAILURE;
	}
	ret = gnutls_priority_set(tls, tls_p);
	if (ret < 0) {
		printf("Failed to set priority in ciphers (2)\n");
		return EXIT_FAILURE;
	}

	pthread_t taccept;
	struct tls_accept_args args;
	args.socket_server = socket_server;
	args.tls=&tls;
	args.client_socket = &client_socket;

	pthread_create(&taccept, NULL, (void * (*)(void *))&tls_accept, &args);

	// We connect to the configured port
	printf("Starting connection with tls\n");
	btransfered = libusb_control_transfer(usb_dev_h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN, 
				VENDOR_REQUEST_TCP_CONNECT, 0, 0, (unsigned char*)&status, sizeof(status), 2000);
        if (btransfered	< 0) {
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}
	printf("Status of the connection  :%d\n", status);
	pthread_join(taccept, (void**)&ret);

	printf("Sending secure %s to USB\n", hello);

	if (btransfered = libusb_bulk_transfer(usb_dev_h, 0x1, hello, sizeof(hello), &len, 2000) != 0){
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}
	if ((ret = gnutls_record_recv(tls, usb_buffer, sizeof(usb_buffer)))  < 0) {
		perror("Reading socket");
		return EXIT_FAILURE;
	}
	printf("Received %s from TLS socket (%d)\n", usb_buffer, ret);

	printf("Sending secure %s to network\n", test);
	if (gnutls_record_send(tls, test, sizeof(test))  == -1) {
		perror("sending socket");
		return EXIT_FAILURE;
	}

	if (btransfered = libusb_bulk_transfer(usb_dev_h, 0x81, usb_buffer, sizeof(usb_buffer), &len, 2000) != 0){
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}
	printf("Received secure %s from USB\n", usb_buffer);

	// Transmitting a big buffer over secure channel
	rf = open("/dev/random", O_RDONLY);
	read(rf, bigbuffer, sizeof(bigbuffer));
	close(rf);

	printf("Sending a buffer securely of size %d to USB\n", sizeof(bigbuffer));

	if (btransfered = libusb_bulk_transfer(usb_dev_h, 0x1, bigbuffer, sizeof(bigbuffer), &len, 2000) != 0){
		printf("Fail to submit transfer: %d %s\n", btransfered, libusb_strerror(btransfered));
		return EXIT_FAILURE;
	}
	printf("We send %d byte over USB\n", len);
	len = 0;
	while (len != 5000) {
		if ((count=gnutls_record_recv(tls, second_buffer + len, sizeof(second_buffer) - len))  == -1) {
			perror("Reading TLS socket");
			return EXIT_FAILURE;
		}
		len += count;
	}
	printf("We read %d bytes from network\n", len);
	if (memcmp(bigbuffer, second_buffer, sizeof(bigbuffer)) != 0) {
		printf("The readen bytes are not the same as the one writen\n");
		return EXIT_FAILURE;
	}
	printf("It matches ! USB -> TLS socket\n");

	rf = open("/dev/random", O_RDONLY);
	read(rf, bigbuffer, sizeof(bigbuffer));
	close(rf);

	printf("Sending securely  a buffer of size %d to network\n", sizeof(bigbuffer));
	if (gnutls_record_send(tls, bigbuffer, sizeof(bigbuffer))  == -1) {
		perror("sending TLS socket");
		return EXIT_FAILURE;
	}

	count = 0;
	while (count != 5000) {
		if (btransfered = libusb_bulk_transfer(usb_dev_h, 0x81, second_buffer+count, sizeof(second_buffer)-count, &len, 2000) != 0){
			printf("Fail to submit transfer after %d bytes(len=%d): %d %s\n", count, len, btransfered, libusb_strerror(btransfered));
			return EXIT_FAILURE;
		}
		count+=len;
	}
	printf("We read %d bytes from USB\n", count);
	if (memcmp(bigbuffer, second_buffer, sizeof(bigbuffer)) != 0) {
		printf("The readen bytes are not the same as the one writen\n");
		return EXIT_FAILURE;
	}
	printf("It matches ! TLS socket -> USB\n");

	ret = gnutls_bye(tls, GNUTLS_SHUT_RDWR);
	if (ret < 0) {
		printf("Failed to close securly the connection\n");
		return EXIT_FAILURE;
	}

	gnutls_deinit(tls);


	gnutls_priority_deinit(tls_p);
	gnutls_certificate_free_credentials(cert);
	gnutls_global_deinit();

	libusb_close(usb_dev_h);
	return EXIT_SUCCESS;
}
