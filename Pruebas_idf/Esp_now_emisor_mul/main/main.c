#include <stdio.h>
#include "string.h"
#include "esp_log.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "OLEDDisplay.h"

#define TAG "MASTER"

#define I2C_MASTER_SCL_IO GPIO_NUM_4
#define I2C_MASTER_SDA_IO GPIO_NUM_5
#define OLED_DIRECTION 0x78
#define BUTTON_GPIO GPIO_NUM_0
#define MENU_SIZE 4
#define STACKMEMORY 1024

uint8_t current_menu_index = 0;

const char *menu_items[MENU_SIZE] = {
    "Placa_1",
    "Placa_2",
    "Placa_3",
    "Placa_4"
};

typedef struct {
    char text[32];
} oled_message_t;

OLEDDisplay_t *oled = NULL;
QueueHandle_t oled_queue;

uint8_t slave_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // MAC de la ESP32 esclava
//uint8_t slave_mac[] = {0x68, 0xB6, 0xB3, 0x54, 0xB4, 0xC4}; // MAC de la ESP32 esclava

esp_err_t configure_button(void);

void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status);
void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len);
void send_command(const char *command);
void oled_task(void *arg);
void send_message_to_oled(const char *message);
void next_menu_item();
void botton_task(void *arg);

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
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));  // Registrar el callback de recepción

    // Agregar peer
    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, slave_mac, 6);
    peer_info.channel = 0; // Canal 0: cualquier canal
    peer_info.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));

    //  Create queue for data message and check if is created
    oled_queue = xQueueCreate(10, sizeof(oled_message_t));
    if (oled_queue == NULL) {
        ESP_LOGE(TAG, "OLED queue dont create");
        return;
    }

    if (configure_button() == ESP_OK)
    {
        xTaskCreate(oled_task, "oled_task", STACKMEMORY * 3, NULL, 5, NULL);
        xTaskCreate(botton_task, "botton_task", STACKMEMORY * 3, NULL, 5, NULL);
    }
    else{
        ESP_LOGE(TAG, "TASK dont create");
    }

    // Control de envío
    while (1) {
            switch (current_menu_index)
            {
            case 0:
                ESP_LOGI(TAG, "Enviando comando para INICIAR el envío de datos");
                send_command("START");
                vTaskDelay(5000 / portTICK_PERIOD_MS);

                //ESP_LOGI(TAG, "Enviando comando para DETENER el envío de datos");
                //send_command("STOP");
                //vTaskDelay(5000 / portTICK_PERIOD_MS);
                break;

            case 1:
                ESP_LOGI(TAG, "Enviando comando para INICIAR el envío de datos");
                send_command("DELE");
                vTaskDelay(5000 / portTICK_PERIOD_MS);

                ESP_LOGI(TAG, "Enviando comando para DETENER el envío de datos");
                send_command("NAA");
                vTaskDelay(5000 / portTICK_PERIOD_MS);
                break;
            
            default:
                vTaskDelay(pdMS_TO_TICKS(100));  // Ceder tiempo si no hay tareas activas
                break;
            }
    }
}

void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    ESP_LOGI(TAG, "Mensaje enviado a %02X:%02X:%02X:%02X:%02X:%02X, Estado: %s",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5],
             (status == ESP_NOW_SEND_SUCCESS) ? "Éxito" : "Fallo");
}

void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len) {
    // Convertir los datos recibidos en una cadena y mostrarla
    oled_message_t msg_data = {0};
    memcpy(msg_data.text, data, len > 31 ? 31 : len); // Copiar mensaje recibido
    msg_data.text[len] = '\0'; // Asegura que el mensaje esté terminado en null

    if (xQueueSend(oled_queue, &msg_data, portMAX_DELAY) != pdPASS) {
        ESP_LOGE(TAG, "No se pudo enviar el mensaje a la cola OLED");
        ESP_LOGI(TAG, "Mensaje recibido: %s", msg_data.text);
    }

    /*ESP_LOGI(TAG, "Datos recibidos de %02X:%02X:%02X:%02X:%02X:%02X: %s",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5], msg_data);
             */
}

void send_command(const char *command) {
    ESP_LOGI(TAG, "Enviando comando: %s", command);
    ESP_ERROR_CHECK(esp_now_send(slave_mac, (uint8_t *)command, strlen(command)));
}

esp_err_t configure_button(void) {       

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    return ESP_OK;
}

void oled_task(void *arg) {
    oled = OLEDDisplay_init(I2C_NUM_1, OLED_DIRECTION, I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    OLEDDisplay_setTextAlignment(oled, TEXT_ALIGN_CENTER);
    OLEDDisplay_setFont(oled, ArialMT_Plain_10);

    OLEDDisplay_clear(oled);
    OLEDDisplay_drawString(oled, 64, 25, "Iniciando...");
    OLEDDisplay_display(oled);

    oled_message_t msg_data;

    while (1) {
        if (xQueueReceive(oled_queue, &msg_data, portMAX_DELAY) == pdPASS) {
            OLEDDisplay_clear(oled);
            OLEDDisplay_drawString(oled, 64, 25, msg_data.text);
            OLEDDisplay_display(oled);
        }
    }
}

void send_message_to_oled(const char *message) {
    oled_message_t msg;
    snprintf(msg.text, sizeof(msg.text), "%s", message);
    xQueueSend(oled_queue, &msg, portMAX_DELAY);
}

void next_menu_item() {
    current_menu_index = (current_menu_index + 1) % MENU_SIZE;
    ESP_LOGI(TAG, "Menú cambiado a: %s", menu_items[current_menu_index]);
}

void botton_task(void *arg) {
    uint32_t last_press_time = 0;
    while (true) {
        if (gpio_get_level(BUTTON_GPIO) == 0) { // Nivel bajo indica botón presionado
            uint32_t current_time = esp_timer_get_time() / 1000; // Tiempo en ms
            if (current_time - last_press_time > 200) { // Ignorar presiones dentro de 200ms
                next_menu_item();
                last_press_time = current_time; // Actualiza el tiempo de última presión
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // Reducido para menor impacto en el sistema
    }
}
