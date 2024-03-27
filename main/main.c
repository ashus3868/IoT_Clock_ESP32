/**
 * @file app_main.c
 * @brief Example application for the TM1637 LED segment display
 */

#include <stdio.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <sys/time.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include "esp_event.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "esp_netif_sntp.h"
#include "lwip/ip_addr.h"
#include "esp_sntp.h"

#include "sdkconfig.h"
#include "tm1637.h"

#define TAG "app"

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 48
#endif

const gpio_num_t LED_CLK = CONFIG_TM1637_CLK_PIN;
const gpio_num_t LED_DTA = CONFIG_TM1637_DIO_PIN;

void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Notification of a time synchronization event");
}

static void print_servers(void)
{
    ESP_LOGI(TAG, "List of configured NTP servers:");

    for (uint8_t i = 0; i < SNTP_MAX_SERVERS; ++i){
        if (esp_sntp_getservername(i)){
            ESP_LOGI(TAG, "server %d: %s", i, esp_sntp_getservername(i));
        } else {
            // we have either IPv4 or IPv6 address, let's print it
            char buff[INET6_ADDRSTRLEN];
            ip_addr_t const *ip = esp_sntp_getserver(i);
            if (ipaddr_ntoa_r(ip, buff, INET6_ADDRSTRLEN) != NULL)
                ESP_LOGI(TAG, "server %d: %s", i, buff);
        }
    }
}

static void obtain_time(void)
{
    ESP_ERROR_CHECK( nvs_flash_init() );
    ESP_ERROR_CHECK( esp_netif_init());
    ESP_ERROR_CHECK( esp_event_loop_create_default() );

#if LWIP_DHCP_GET_NTP_SRV
    /**
     * NTP server address could be acquired via DHCP,
     * see following menuconfig options:
     * 'LWIP_DHCP_GET_NTP_SRV' - enable STNP over DHCP
     * 'LWIP_SNTP_DEBUG' - enable debugging messages
     *
     * NOTE: This call should be made BEFORE esp acquires IP address from DHCP,
     * otherwise NTP option would be rejected by default.
     */
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_SNTP_TIME_SERVER);
    config.start = false;                       // start SNTP service explicitly (after connecting)
    config.server_from_dhcp = true;             // accept NTP offers from DHCP server, if any (need to enable *before* connecting)
    config.renew_servers_after_new_IP = true;   // let esp-netif update configured SNTP server(s) after receiving DHCP lease
    config.index_of_first_server = 1;           // updates from server num 1, leaving server 0 (from DHCP) intact
    // configure the event on which we renew servers
#ifdef CONFIG_EXAMPLE_CONNECT_WIFI
    config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
#else
    config.ip_event_to_renew = IP_EVENT_ETH_GOT_IP;
#endif
    config.sync_cb = time_sync_notification_cb; // only if we need the notification function
    esp_netif_sntp_init(&config);

#endif /* LWIP_DHCP_GET_NTP_SRV */

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

#if LWIP_DHCP_GET_NTP_SRV
    ESP_LOGI(TAG, "Starting SNTP");
    esp_netif_sntp_start();
#if LWIP_IPV6 && SNTP_MAX_SERVERS > 2
    /* This demonstrates using IPv6 address as an additional SNTP server
     * (statically assigned IPv6 address is also possible)
     */
    ip_addr_t ip6;
    if (ipaddr_aton("2a01:3f7::1", &ip6)) {    // ipv6 ntp source "ntp.netnod.se"
        esp_sntp_setserver(2, &ip6);
    }
#endif  /* LWIP_IPV6 */

#else
    ESP_LOGI(TAG, "Initializing and starting SNTP");
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    /* This demonstrates configuring more than one server
     */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
                               ESP_SNTP_SERVER_LIST(CONFIG_SNTP_TIME_SERVER, "pool.ntp.org" ) );
#else
    /*
     * This is the basic default config with one server and starting the service
     */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_SNTP_TIME_SERVER);
#endif
    config.sync_cb = time_sync_notification_cb;     // Note: This is only needed if we want
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    config.smooth_sync = true;
#endif

    esp_netif_sntp_init(&config);
#endif

    print_servers();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 15;
    while (esp_netif_sntp_sync_wait(2000 / portTICK_PERIOD_MS) == ESP_ERR_TIMEOUT && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
    }
    time(&now);
    localtime_r(&now, &timeinfo);

    ESP_ERROR_CHECK( example_disconnect() );
    esp_netif_sntp_deinit();
}


void lcd_tm1637_task(void * arg)
{
	tm1637_led_t * lcd = tm1637_init(LED_CLK, LED_DTA);

	
	time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    // strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    // ESP_LOGI(TAG, "The current date/time in New Delhi, India is: %s", strftime_buf);
	printf("%d \n",timeinfo.tm_year);
	if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
        // update 'now' variable with current time
        time(&now);
    }
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
	else {
		// add 500 ms error to the current system time.
		// Only to demonstrate a work of adjusting method!
		{
			ESP_LOGI(TAG, "Add a error for test adjtime");
			struct timeval tv_now;
			gettimeofday(&tv_now, NULL);
			int64_t cpu_time = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
			int64_t error_time = cpu_time + 500 * 1000L;
			struct timeval tv_error = { .tv_sec = error_time / 1000000L, .tv_usec = error_time % 1000000L };
			settimeofday(&tv_error, NULL);
		}

		ESP_LOGI(TAG, "Time was set, now just adjusting it. Use SMOOTH SYNC method.");
		obtain_time();
		// update 'now' variable with current time
		time(&now);
	}
#endif

	char strftime_buf[64];

	setenv("TZ", "IST-5:30", 1);
    tzset();
    localtime_r(&now, &timeinfo);
	strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time in New Delhi, India is: %s", strftime_buf);

	printf("Value of now: %lu\n", (unsigned long)&now);
	while (true)
	{	
		
		// printf("Value of now: %lu\n", (unsigned long)&now);
		// Test segment control
		// uint8_t seg_data[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20};
		// for (uint8_t x=0; x<32; ++x)
		// {
		// 	uint8_t v_seg_data = seg_data[x%6];
		// 	tm1637_set_segment_raw(lcd, 0, v_seg_data);
		// 	tm1637_set_segment_raw(lcd, 1, v_seg_data);
		// 	tm1637_set_segment_raw(lcd, 2, v_seg_data);
		// 	tm1637_set_segment_raw(lcd, 3, v_seg_data);
		// 	vTaskDelay(100 / portTICK_PERIOD_MS);
		// }

		// // Test brightness
		// for (int x=0; x<7; x++) {
		// 	tm1637_set_brightness(lcd, x);
		// 	tm1637_set_number(lcd, 1234);
		// 	vTaskDelay(300 / portTICK_PERIOD_MS);
		// }

		// for (uint8_t x=0; x<3; ++x)
		// {
			// Set random system time
			struct timeval tm_test = {now - 3600-5400, 0};
			settimeofday(&tm_test, NULL);

			// Get current system time
			// time_t now = 0;
			// struct tm timeinfo = { 0 };
			// time(&now);
			// localtime_r(&now, &timeinfo);
			int time_number = 100 * timeinfo.tm_hour + timeinfo.tm_min;

			// Display time with blinking dots
			for (int z=0; z<2; ++z) {
				tm1637_set_number_lead_dot(lcd, time_number, true, z%2 ? 0xFF : 0x00);
				vTaskDelay(500 / portTICK_PERIOD_MS);
			}
		// }

		// // Test display numbers
		// for (int x=0; x<16; ++x) {
		// 	bool show_dot = x%2; // Show dot every 2nd cycle
		// 	tm1637_set_segment_number(lcd, 0, x, show_dot);
		// 	tm1637_set_segment_number(lcd, 1, x, show_dot); // On my display "dot" (clock symbol ":") connected only here
		// 	tm1637_set_segment_number(lcd, 2, x, show_dot);
		// 	tm1637_set_segment_number(lcd, 3, x, show_dot);
		// 	vTaskDelay(100 / portTICK_PERIOD_MS);
		// }


		// // uint8_t v_seg_data = seg_data[x%6];
		// tm1637_set_segment_raw(lcd, 0, 0x77);
		// tm1637_set_segment_raw(lcd, 1, 0x6D);
		// tm1637_set_segment_raw(lcd, 2, 0x74);
		// tm1637_set_segment_raw(lcd, 3, 0x3E);
		// vTaskDelay(200 / portTICK_PERIOD_MS);
		
		++now;
		localtime_r(&now, &timeinfo);
	}
}

void app_main()
{
	xTaskCreate(&lcd_tm1637_task, "lcd_tm1637_task", 4096, NULL, 5, NULL);
}

