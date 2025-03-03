#include <stdio.h>
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define TAG "SLAVE"
#define LED_GPIO GPIO_NUM_2  // Define el pin GPIO donde está conectado el LED

// Dirección MAC de la ESP32-S3 maestra
uint8_t master_mac[] = {0xF0, 0xF5, 0xBD, 0x54, 0xEB, 0x50};

volatile bool led_state = false;

void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len) {
    char command[10] = {0};
    memcpy(command, data, len);

    ESP_LOGI(TAG, "Comando recibido de %02X:%02X:%02X:%02X:%02X:%02X: %s",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5], command);

    if (strcmp(command, "ENCENDIDO") == 0) {
        led_state = true;
        gpio_set_level(LED_GPIO, 1);  // Encender el LED
        ESP_LOGI(TAG, "LED encendido");
    } else if (strcmp(command, "APAGADO") == 0) {
        led_state = false;
        gpio_set_level(LED_GPIO, 0);  // Apagar el LED
        ESP_LOGI(TAG, "LED apagado");
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

    // Configurar el pin del LED como salida
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);  // Asegurarse de que el LED esté apagado al inicio

    // Registrar peer específico (maestro) una sola vez
    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, master_mac, 6);
    peer_info.channel = 0; // Canal 0: cualquier canal
    peer_info.encrypt = false;

    if (esp_now_add_peer(&peer_info) != ESP_OK) {
        ESP_LOGE(TAG, "Error al agregar peer maestro");
        return;
    }

    ESP_LOGI(TAG, "Esperando comandos...");
}