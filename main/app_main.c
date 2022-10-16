/* MQTT (over TCP) Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"

#define RELAY_GPIO CONFIG_RELAY_GPIO
#define MOTION_GPIO CONFIG_MOTION_GPIO
#define GPIO_INPUT_PIN_SEL (1ULL<<MOTION_GPIO)
#define ESP_INTR_FLAG_DEFAULT 0
#define MOTION_LED_GPIO CONFIG_MOTION_LED_GPIO

static QueueHandle_t gpio_evt_queue = NULL;

static const char *TAG = "MQTT_EXAMPLE";

static esp_mqtt_client_handle_t global_mqtt_client = NULL;

static void configure_led(void)
{
    ESP_LOGI(TAG, "Example configured to actuate the LED on motion!");
    gpio_reset_pin(MOTION_LED_GPIO);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(MOTION_LED_GPIO, GPIO_MODE_OUTPUT);
}

static void blink_motion_led(void)
{
    gpio_set_level(MOTION_LED_GPIO, 0);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    gpio_set_level(MOTION_LED_GPIO, 1);
    vTaskDelay(500 / portTICK_PERIOD_MS);
    gpio_set_level(MOTION_LED_GPIO, 0);
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
//	uint32_t gpio_num = (uint32_t) arg;
	esp_mqtt_client_handle_t mqtt_client = (esp_mqtt_client_handle_t) arg;
	ESP_ERROR_CHECK(gpio_intr_disable(MOTION_GPIO));
	if (gpio_get_level(MOTION_GPIO) == 1)
	{
		xQueueSendFromISR(gpio_evt_queue, &mqtt_client, NULL);
	}
	ESP_ERROR_CHECK(gpio_intr_enable(MOTION_GPIO));
}

static void gpio_task_example(void* arg)
{
    int msg_id;
    esp_mqtt_client_handle_t mqtt_client;
	for(;;) {
		if(xQueueReceive(gpio_evt_queue, &mqtt_client, portMAX_DELAY)) {
			printf("GPIO[%d] intr, val: %d\n", MOTION_GPIO, gpio_get_level(MOTION_GPIO));
			msg_id = esp_mqtt_client_publish(mqtt_client, "/garage/motion", "motion", 0, 0, 0);
			ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
			blink_motion_led();
		}
	}
}




static void configure_relay(void)
{
    ESP_LOGI(TAG, "Example configured to actuate the GPIO Relay!");
    gpio_reset_pin(RELAY_GPIO);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
}



static void actuate_relay(void)
{
    /* Set the GPIO level to actuate the relay, with specific delay.*/
    gpio_set_level(RELAY_GPIO, 0);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    gpio_set_level(RELAY_GPIO, 1);
    vTaskDelay(500 / portTICK_PERIOD_MS);
    gpio_set_level(RELAY_GPIO, 0);

}


static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

        msg_id = esp_mqtt_client_subscribe(global_mqtt_client, "/garage/door", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
//        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
//        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        if ((*event->data == '1') || (*event->data == '0'))
        {
        	printf("Toggling garage door. \r\n");
        	actuate_relay();
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
//        .uri = CONFIG_BROKER_URL,
		.host = CONFIG_HOST,
    };
#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    global_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(global_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(global_mqtt_client);
}


void app_main(void)
{
	//zero-initialize the config structure.
	gpio_config_t io_conf = {};
	//interrupt of rising edge
	io_conf.intr_type = GPIO_INTR_POSEDGE;
	//bit mask of the pins, use GPIO4/5 here
	io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
	//set as input mode
	io_conf.mode = GPIO_MODE_INPUT;
	//enable pull-up mode
	io_conf.pull_up_en = 1;
	gpio_config(&io_conf);

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task


    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);


	configure_relay();
	configure_led();

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    mqtt_app_start();
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(MOTION_GPIO, gpio_isr_handler, (void*) global_mqtt_client);
    xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);

}
