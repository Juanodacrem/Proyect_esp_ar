#include <stdio.h>
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "SLAVE"

// Dirección MAC de la ESP32-S3 maestra
uint8_t master_mac[] = {0x68, 0xB6, 0xB3, 0x54, 0xA9, 0x0C};

volatile bool send_data = false;

void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len) {
    char command[10] = {0};
    memcpy(command, data, len);

    ESP_LOGI(TAG, "Comando recibido de %02X:%02X:%02X:%02X:%02X:%02X: %s",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5], command);

    if (strcmp(command, "DELE") == 0) {
        send_data = true;
    } else if (strcmp(command, "NAA") == 0) {
        send_data = false;
    }
}

void send_sensor_data() {
    // Registrar peer específico (maestro) una sola vez
    static bool peer_added = false;
    if (!peer_added) {
        esp_now_peer_info_t peer_info = {};
        memcpy(peer_info.peer_addr, master_mac, 6);
        peer_info.channel = 0; // Canal 0: cualquier canal
        peer_info.encrypt = false;

        if (esp_now_add_peer(&peer_info) != ESP_OK) {
            ESP_LOGE(TAG, "Error al agregar peer maestro");
            return;
        }
        peer_added = true; // Marca como agregado
    }

    while (1) {
        if (send_data) {
            int sensor_value = rand() % 100; // Simulando lectura de sensor
            char message[20];
            snprintf(message, sizeof(message), "Valor: %d", sensor_value);
            ESP_LOGI(TAG, "Enviando datos: %s", message);

            esp_err_t err = esp_now_send(master_mac, (uint8_t *)message, strlen(message));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error al enviar datos: %s", esp_err_to_name(err));
            }
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS); // Delay de 1 segundo
    }
}

void app_main(void) {
    // Inicializar NVS
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Inicializar Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Inicializar ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    // Comenzar tarea para enviar datos
    xTaskCreate(send_sensor_data, "send_sensor_data", 1024*3, NULL, 5, NULL);
}
