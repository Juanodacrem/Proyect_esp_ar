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
#define BUTTON_GPIO GPIO_NUM_4  // Define el pin GPIO donde está conectado el botón
#define NVS_NAMESPACE "led_state"
#define NVS_KEY "state"
#define BUTTON_POLL_DELAY_MS 50  // Retardo para leer el estado del botón (debouncing)

// Dirección MAC de la ESP32-S3 maestra
uint8_t master_mac[] = {0xF0, 0xF5, 0xBD, 0x54, 0xEB, 0x50};

volatile bool led_state = false;  // Estado del LED
volatile bool last_button_state = false;  // Último estado del botón

// Función para guardar el estado del LED en NVS
void save_led_state(bool state) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) al abrir NVS handle!", esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(nvs_handle, NVS_KEY, state ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) al guardar el estado del LED en NVS!", esp_err_to_name(err));
    }

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

// Función para recuperar el estado del LED desde NVS
bool load_led_state() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) al abrir NVS handle!", esp_err_to_name(err));
        return false;
    }

    uint8_t state = 0;
    err = nvs_get_u8(nvs_handle, NVS_KEY, &state);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Error (%s) al leer el estado del LED desde NVS!", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    return state == 1;
}

// Tarea para monitorear el estado del interruptor
void button_monitor_task(void *arg) {
    // Leer el estado inicial del botón
    last_button_state = gpio_get_level(BUTTON_GPIO);

    while (1) {
        bool current_button_state = gpio_get_level(BUTTON_GPIO);  // Leer el estado actual del botón

        // Si el estado del botón cambia
        if (current_button_state != last_button_state) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_DELAY_MS));  // Esperar para evitar rebotes
            current_button_state = gpio_get_level(BUTTON_GPIO);  // Leer nuevamente para confirmar

            if (current_button_state != last_button_state) {  // Confirmar el cambio de estado
                led_state = !led_state;  // Cambiar el estado del LED (toggle)
                gpio_set_level(LED_GPIO, led_state ? 1 : 0);  // Actualizar el estado del LED
                save_led_state(led_state);  // Guardar el nuevo estado en NVS
                ESP_LOGI(TAG, "Estado del LED cambiado a: %s", led_state ? "ENCENDIDO" : "APAGADO");
                last_button_state = current_button_state;  // Actualizar el último estado del botón
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_DELAY_MS));  // Esperar antes de la próxima lectura
    }
}

void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len) {
    char command[10] = {0};
    memcpy(command, data, len);

    ESP_LOGI(TAG, "Comando recibido de %02X:%02X:%02X:%02X:%02X:%02X: %s",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5], command);

    if (strcmp(command, "DATO") == 0) {
        led_state = !led_state;  // Cambiar el estado del LED (toggle)
        gpio_set_level(LED_GPIO, led_state ? 1 : 0);  // Actualizar el estado del LED
        save_led_state(led_state);  // Guardar el nuevo estado en NVS
        ESP_LOGI(TAG, "Estado del LED cambiado a: %s", led_state ? "ENCENDIDO" : "APAGADO");
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

    // Configurar el pin del botón como entrada con resistencia pull-down
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_pulldown_en(BUTTON_GPIO);  // Habilitar resistencia pull-down
    gpio_pullup_dis(BUTTON_GPIO);  // Deshabilitar resistencia pull-up

    // Cargar el estado del LED desde NVS
    led_state = load_led_state();
    gpio_set_level(LED_GPIO, led_state ? 1 : 0);  // Establecer el estado del LED según lo cargado de NVS

    // Crear una tarea para monitorear el estado del interruptor
    xTaskCreate(button_monitor_task, "button_monitor_task", 2048, NULL, 10, NULL);

    // Registrar peer específico (maestro) una sola vez
    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, master_mac, 6);
    peer_info.channel = 0; // Canal 0: cualquier canal
    peer_info.encrypt = false;

    if (esp_now_add_peer(&peer_info) != ESP_OK) {
        ESP_LOGE(TAG, "Error al agregar peer maestro");
        return;
    }

    ESP_LOGI(TAG, "Esperando comandos o cambios en el estado del botón...");
}