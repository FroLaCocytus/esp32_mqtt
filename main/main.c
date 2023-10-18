/*************************************************LIB**************************************************/
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "driver/uart.h"

#include "nvs_flash.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

/*************************************************PARAM************************************************/
static const char *TAG_UART = "UART";
static const char *TAG_WIFI = "WIFI";
static const char *TAG_MQTT = "MQTT";

esp_mqtt_client_handle_t mqtt_client;

static const char *TOPIC_PUB = "from_esp32";
static const char *TOPIC_SUB = "from_PC";


#define UART_NUM            (UART_NUM_0)
#define BUF_SIZE            (1024)
#define PATTERN_CHR_NUM     (2)   

static QueueHandle_t uart_rx_queue, uart_tx_queue, mqtt_pub_queue;

/*************************************************WIFI*************************************************/

/* Обработчик событий связанных с WiFi */
static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG_WIFI, "WiFi connecting ...\n");
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG_WIFI, "WiFi connected\n");
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG_WIFI, "WiFi lost connection\n");
        break;
    case IP_EVENT_STA_GOT_IP:
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_WIFI, "Got IP-address: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        break;
    default:
        break;
    }
}

/* Подключениие к WiFi */
void wifi_connection()
{
    /* 1 фаза - Инициализация сетевых компонентов */
    esp_netif_init();                      // Инициализация базового стэка TCP/IP
    esp_event_loop_create_default();       // Создание цикла событий по умолчанию
    esp_netif_create_default_wifi_sta();   // Создание Wi-Fi в режиме STA
    wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_initiation);       // Инициализация Wi-Fi
    /* 2 фаза - Конфигурация Wi-Fi */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    wifi_config_t wifi_configuration = {
        .sta = {
            .ssid = CONFIG_SSID,
            .password = CONFIG_PASSWORD}};
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_configuration);
    /* 3 фаза - Запуск Wi-Fi */
    esp_wifi_start();
    /* 4 фаза - Подключение к Wi-Fi сети */
    esp_wifi_connect();
}

/*************************************************UART*************************************************/

/* Обработчик событий UART */
static void uart_rx_task(void *pvParameters)
{

    uart_event_t event;     // переменная для хранения событий UART
    size_t buffered_size;   // переменная для хранения размера буфера
    uint8_t* dtmp = (uint8_t*) malloc(BUF_SIZE); // выделяем динамическую память хранения данных

    for(;;) {
        /* Ожидаем событие UART */
        if(xQueueReceive(uart_rx_queue, (void * )&event, (TickType_t)portMAX_DELAY)) {
            bzero(dtmp, BUF_SIZE); // заполняем буфер нулями
            switch(event.type) {
                /* Событие обнаружения даты*/
                case UART_DATA:
                    ESP_LOGI(TAG_UART, "Event UART_DATA\n");
                    uart_flush_input(UART_NUM);
                    break;
                /* Событие обнаружения UART паттерна*/
                case UART_PATTERN_DET:
                    ESP_LOGI(TAG_UART, "Event UART_PATTERN_DET");
                    uart_get_buffered_data_len(UART_NUM, &buffered_size);   // получаем длину данных, находящихся в буфере UART Rx
                    int pos = uart_pattern_pop_pos(UART_NUM);               // получаем позицию обнаруженного паттерна
                    ESP_LOGI(TAG_UART, "Info - pos: %d, buffered size: %d", pos, buffered_size);
                    if (pos == -1) {
                        // Если ранее было событие UART_PATTERN_DET, но очередь позиций паттерна переполнена,
                        // поэтому не удалось сохранить позицию (Можно увеличить размер очереди)
                        uart_flush_input(UART_NUM); // очищаем буфер приёма UART
                    } else {
                        uart_read_bytes(UART_NUM, dtmp, pos, pdMS_TO_TICKS(100)); // читаем данные до позиции обнаруженного паттерна
                        ESP_LOGI(TAG_UART, "Read data \"%s\"\n", dtmp);
                        uart_flush_input(UART_NUM);

                        uint8_t* data_to_pub = (uint8_t*)malloc(BUF_SIZE);
                        memcpy(data_to_pub, dtmp, BUF_SIZE); 
                        if (xQueueSend(mqtt_pub_queue, &data_to_pub, (TickType_t)portMAX_DELAY) != pdPASS) {
                            ESP_LOGE(TAG_UART, "Failed to send data to queue\n");
                            free(data_to_pub);
                        }
                    }
                    break;
                /* Другие события */
                default:
                    ESP_LOGI(TAG_UART, "Other UART event (%d)\n", event.type);
                    break;
            }
        }
    }
    free(dtmp); 
    vTaskDelete(NULL);
}

/* Задача отправки по UART */
static void uart_tx_task(void *pvParameters)
{
    uint8_t* data_to_send;
    for(;;) {
        if(xQueueReceive(uart_tx_queue, &data_to_send, (TickType_t)portMAX_DELAY)) {
            size_t data_len = strlen((char*)data_to_send);
            uart_write_bytes(UART_NUM, (char*)data_to_send, data_len);
            free(data_to_send);
        }
    }
    vTaskDelete(NULL);
}

/* Инициализация UART */
void init_uart(void) 
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,                    // Скорость передачи данных
        .data_bits = UART_DATA_8_BITS,          // Количество бит данных в каждом байте
        .parity = UART_PARITY_DISABLE,          // Отключение бита четности
        .stop_bits = UART_STOP_BITS_1,          // Один стоп-бит
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,  // Отключение аппаратного управления потоком
        .source_clk = UART_SCLK_DEFAULT,        // Использование стандартной частоты тактового сигнала
    };

    /* Устанавливаем драйвер UART */
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 20, &uart_rx_queue, 0);

    /* Конфигурируем UART */
    uart_param_config(UART_NUM, &uart_config);

    /* Устанавливаем пины для UART (дефолтные для связи по USB) */
    uart_set_pin(UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    /* Устанавливаем функцию обнаружения паттерна UART */
    uart_enable_pattern_det_baud_intr(UART_NUM, '+', PATTERN_CHR_NUM, 9, 0, 0);
    /* Выделяем память для записи позиций обнаруженных шаблонов в буфере UART. */
    uart_pattern_queue_reset(UART_NUM, 20);

    
}

/*************************************************MQTT*************************************************/

/* Обработчик событий MQTT */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG_MQTT, "MQTT_EVENT_CONNECTED");
        esp_mqtt_client_subscribe(mqtt_client, TOPIC_SUB, 0); 
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG_MQTT, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG_MQTT, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG_MQTT, "MQTT_EVENT_DATA");
        
        uint8_t *mqtt_data = (uint8_t *)malloc(event->data_len + 1);
        memcpy(mqtt_data, event->data, event->data_len);
        mqtt_data[event->data_len] = '\0';  
        if (xQueueSend(uart_tx_queue, &mqtt_data, (TickType_t)portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG_MQTT, "Failed to send data to the queue");
            free(mqtt_data); 
        }
        break;
    default:
        ESP_LOGI(TAG_MQTT, "Other MQTT event (%d)", event->event_id);
        break;
    }
}

/* Задача публикации в топике MQTT */
static void mqtt_pub_task(void *pvParameters)
{
    uint8_t* data_to_pub;
    for(;;){
        if(xQueueReceive(mqtt_pub_queue, &data_to_pub, (TickType_t)portMAX_DELAY)){
            size_t data_len = strlen((char*)data_to_pub);
            esp_mqtt_client_publish(mqtt_client, TOPIC_PUB, (char*)data_to_pub, data_len, 1, 0);
            free(data_to_pub);
        }
    }
    vTaskDelete(NULL);
}


/* Инициализация MQTT */
static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_URL,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}


/*************************************************MAIN*************************************************/

void app_main(void)
{

    nvs_flash_init();   //инициализация флэш-памяти
    init_uart();        //инициализация UART

    wifi_connection();  //подключение к WiFi
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    mqtt_app_start();

    uart_tx_queue = xQueueCreate(20, sizeof(uint8_t*));
    mqtt_pub_queue = xQueueCreate(20, sizeof(uint8_t*));

    /* Создаем задачу для обработки событий UART из ISR (Interrupt Service Routine) */
    xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 1, NULL);
    xTaskCreate(mqtt_pub_task, "mqtt_pub_task", 4096, NULL, 1, NULL);
    xTaskCreate(uart_tx_task, "uart_tx_task", 4096, NULL, 1, NULL);
    
}
