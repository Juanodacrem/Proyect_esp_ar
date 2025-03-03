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

// Direcciones MAC específicas para cada placa
const uint8_t mac_addresses[MENU_SIZE][6] = {
    {0x68, 0xB6, 0xB3, 0x54, 0xB4, 0xC4}, // MAC de Placa_1
    {0x68, 0xB6, 0xB3, 0x52, 0xF4, 0x90}, // MAC de Placa_2
    {0x68, 0xB6, 0xB3, 0x54, 0xA9, 0x0C}, // MAC de Placa_3
    {0x48, 0xBF, 0x6B, 0xA9, 0x87, 0x65}  // MAC de Placa_4
};

typedef struct {
    char text[32];
} oled_message_t;

OLEDDisplay_t *oled = NULL;
QueueHandle_t oled_queue;

esp_err_t configure_button(void);

void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status);
void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len);
void send_command_to_mac(const char *command, int menu_index);
void oled_task(void *arg);
void send_message_to_oled(const char *message);
void next_menu_item();
void button_task(void *arg);

void app_main(void) {
    // Inicialización
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));  // Registrar callback de recepción

    oled_queue = xQueueCreate(10, sizeof(oled_message_t));
    if (oled_queue == NULL) {
        ESP_LOGE(TAG, "OLED queue not created");
        return;
    }

    if (configure_button() == ESP_OK) {
        xTaskCreate(oled_task, "oled_task", STACKMEMORY * 3, NULL, 5, NULL);
        xTaskCreate(button_task, "button_task", STACKMEMORY * 3, NULL, 5, NULL);
    } else {
        ESP_LOGE(TAG, "Tasks not created");
    }

    send_command_to_mac("STOP", current_menu_index);
    send_message_to_oled(menu_items[current_menu_index]);

    // Envío de comandos según el menú seleccionado
    while (1) {
        ESP_LOGI(TAG, "Menú actual: %s", menu_items[current_menu_index]);
        //send_message_to_oled(menu_items[current_menu_index]);  // Mostrar menú actual
        //send_message_to_oled(" ");  // Mostrar menú actual

        // Enviar comandos START y STOP a la MAC seleccionada
        //send_command_to_mac("START", current_menu_index);
        //vTaskDelay(5000 / portTICK_PERIOD_MS);

        //send_command_to_mac("STOP", current_menu_index);
        //vTaskDelay(5000 / portTICK_PERIOD_MS);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    ESP_LOGI(TAG, "Mensaje enviado a %02X:%02X:%02X:%02X:%02X:%02X, Estado: %s",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5],
             (status == ESP_NOW_SEND_SUCCESS) ? "Éxito" : "Fallo");

    // Mostrar el nombre de la placa asociada al comando enviado
    const char *selected_board = menu_items[current_menu_index]; // Obtenemos el nombre de la placa
    char message[32];
    snprintf(message, sizeof(message), "%s", selected_board);

    //send_message_to_oled(message);  // Mostrar mensaje en la OLED
}

void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len) {
    if (len > 0) {
        char received_message[32];
        snprintf(received_message, sizeof(received_message), "%s", data);

        send_message_to_oled(received_message);  // Mostrar mensaje en OLED solo si estamos en el menú de datos
    }
}

void send_command_to_mac(const char *command, int menu_index) {
    const uint8_t *mac = mac_addresses[menu_index];
    ESP_LOGI(TAG, "Enviando comando: %s a MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             command, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, mac, 6);
    peer_info.channel = 0; // Canal por defecto
    peer_info.encrypt = false;

    if (esp_now_add_peer(&peer_info) == ESP_OK) {
        ESP_ERROR_CHECK(esp_now_send(mac, (uint8_t *)command, strlen(command)));
        ESP_ERROR_CHECK(esp_now_del_peer(mac));
    } else {
        ESP_LOGE(TAG, "No se pudo agregar el peer con MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
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

void button_task(void *arg) {
    uint32_t press_start_time = 0;
    bool button_held = false;
    static bool in_data_view = false;  // Indica si estamos viendo los datos de una placa

    while (true) {
        if (gpio_get_level(BUTTON_GPIO) == 0) {  // Botón presionado
            if (!button_held) {  // Primera detección del botón presionado
                press_start_time = esp_timer_get_time() / 1000;  // Guardar tiempo en ms
                button_held = true;
            }
        } else if (button_held) {  // Botón liberado después de presionarlo
            uint32_t press_duration = (esp_timer_get_time() / 1000) - press_start_time;

            if (!in_data_view && press_duration >= 500) {  
                // Pulsación de 1 segundo -> Entra a ver los datos de la placa
                ESP_LOGI(TAG, "Pulsación larga (1s): Entrando a datos de la placa.");
                in_data_view = true;
                send_command_to_mac("START", current_menu_index);  
            } 
            else if (in_data_view && press_duration >= 1000) {  
                // Pulsación de 2 segundos -> Salir del menú de datos y volver a la selección de placas
                ESP_LOGI(TAG, "Pulsación larga (2s): Saliendo de datos de la placa.");
                in_data_view = false;
                send_command_to_mac("STOP", current_menu_index);
                send_message_to_oled(menu_items[current_menu_index]);  // Mostrar la placa actual
            } 
            else if (!in_data_view && press_duration < 1000) {  
                // Pulsación corta -> Cambiar de placa solo si NO estamos viendo datos
                ESP_LOGI(TAG, "Pulsación corta: Cambiando de placa...");
                send_command_to_mac("STOP", current_menu_index);
                next_menu_item();
                send_message_to_oled(menu_items[current_menu_index]);
            }

            button_held = false;  // Resetear estado
        }

        vTaskDelay(pdMS_TO_TICKS(10));  
    }
}

esp_err_t configure_button(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    return gpio_config(&io_conf);
}
