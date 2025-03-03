#include <stdio.h>
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "math.h"
#include "driver/adc.h"

#define TAG "SLAVE"

// Dirección MAC de la ESP32-S3 maestra
uint8_t master_mac[] = {0xF0, 0xF5, 0xBD, 0x54, 0xEB, 0x50};

// Pines del sensor ultrasónico
#define TRIG_GPIO 41
#define ECHO_GPIO 40
#define SOUND_SPEED 0.034   // Velocidad del sonido en cm/μs
#define TIMEOUT_US 100000   // Tiempo máximo de espera para el eco (μs)

// Valores por defecto del tinaco
#define DEFAULT_H_TOTAL_CM 120    // Altura total por defecto del tinaco en cm
#define DEFAULT_DIAMETRO_CM 97    // Diámetro por defecto del tinaco en cm

// Variables globales para almacenar las dimensiones del tinaco
int g_tank_height = DEFAULT_H_TOTAL_CM;
int g_tank_diameter = DEFAULT_DIAMETRO_CM;

volatile bool send_data = false;

#define NVS_NAMESPACE "storage"

// Prototipos de funciones
float calcular_litros_restantes(float distancia_sensor_cm);
void ultrasonic_sensor_init(void);
float ultrasonic_sensor_measure_distance(void);
void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len);
void send_sensor_data(void);
esp_err_t save_tank_dimensions(int diameter, int height);
esp_err_t load_tank_dimensions(void);

//
// Inicialización del sensor ultrasónico
//
void ultrasonic_sensor_init(void) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << TRIG_GPIO);
    gpio_config(&io_conf);

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << ECHO_GPIO);
    gpio_config(&io_conf);
}

//
// Medir la distancia utilizando el sensor ultrasónico
//
float ultrasonic_sensor_measure_distance(void) {
    gpio_set_level(TRIG_GPIO, 0);
    esp_rom_delay_us(5);
    gpio_set_level(TRIG_GPIO, 1);
    esp_rom_delay_us(10);
    gpio_set_level(TRIG_GPIO, 0);

    int64_t start_time = esp_timer_get_time();
    while (gpio_get_level(ECHO_GPIO) == 0 && (esp_timer_get_time() - start_time) < TIMEOUT_US);

    start_time = esp_timer_get_time();
    while (gpio_get_level(ECHO_GPIO) == 1 && (esp_timer_get_time() - start_time) < TIMEOUT_US);
    int64_t end_time = esp_timer_get_time();

    int64_t pulse_duration = end_time - start_time;
    return (pulse_duration * SOUND_SPEED) / 2;
}

//
// Callback que se ejecuta cuando se recibe un mensaje vía ESP‑NOW
//
void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len) {
    char command[50] = {0};
    int copy_len = len < sizeof(command) - 1 ? len : sizeof(command) - 1;
    memcpy(command, data, copy_len);
    command[copy_len] = '\0';

    ESP_LOGI(TAG, "Comando recibido de %02X:%02X:%02X:%02X:%02X:%02X: %s",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5], command);

    // Si se recibe el comando para actualizar dimensiones, formato: "TANK:diametro,altura"
    if (strncmp(command, "TANK:", 5) == 0) {
        int new_diameter, new_height;
        if (sscanf(command + 5, "%d,%d", &new_diameter, &new_height) == 2) {
            g_tank_diameter = new_diameter;
            g_tank_height = new_height;
            if (save_tank_dimensions(new_diameter, new_height) == ESP_OK) {
                ESP_LOGI(TAG, "Dimensiones del tinaco actualizadas: diametro=%d cm, altura=%d cm",
                         new_diameter, new_height);
                // --- Opción 1: Enviar respuesta inmediata con los datos actualizados ---
                char resp_message[50];
                snprintf(resp_message, sizeof(resp_message), "TANK:%d,%d", g_tank_diameter, g_tank_height);
                esp_err_t err = esp_now_send(master_mac, (uint8_t *)resp_message, strlen(resp_message));
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Error enviando respuesta TANK: %s", esp_err_to_name(err));
                }
            } else {
                ESP_LOGE(TAG, "Error al guardar las dimensiones del tinaco");
            }
        } else {
            ESP_LOGE(TAG, "Formato invalido para comando TANK. Use: TANK:diametro,altura");
        }
        return;
    }

    // Otros comandos: "START" para comenzar a enviar datos o "STOP" para detenerlos
    if (strcmp(command, "START") == 0) {
        send_data = true;
    } else if (strcmp(command, "STOP") == 0) {
        send_data = false;
    }
}

//
// Función que envía los datos del sensor junto con las dimensiones actualizadas
// (Opción 2: envío periódico que incluye el prefijo TANK y todos los datos)
//
void send_sensor_data(void) {
    static bool peer_added = false;
    if (!peer_added) {
        esp_now_peer_info_t peer_info = {};
        memcpy(peer_info.peer_addr, master_mac, 6);
        peer_info.channel = 0;
        peer_info.encrypt = false;

        if (esp_now_add_peer(&peer_info) != ESP_OK) {
            ESP_LOGE(TAG, "Error al agregar peer maestro");
            return;
        }
        peer_added = true;
    }

    while (1) {
        if (send_data) {
            float distance = ultrasonic_sensor_measure_distance();
            float volumen_litros = calcular_litros_restantes(distance);
            // Se evita enviar datos si la distancia es demasiado corta (posible error de medición)
            if (distance > 20) {
                char message[100];
                // Se envía un mensaje con el siguiente formato:
                // "TANK:diametro,altura,volumen"  
                // Ejemplo: "TANK:97,120,350.50"
                snprintf(message, sizeof(message), "Litros:%.2f L",
                          volumen_litros);
                ESP_LOGI(TAG, "Enviando datos: %s", message);

                esp_err_t err = esp_now_send(master_mac, (uint8_t *)message, strlen(message));
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Error al enviar datos: %s", esp_err_to_name(err));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

//
// Guarda en NVS las dimensiones del tinaco
//
esp_err_t save_tank_dimensions(int diameter, int height) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error al abrir NVS para guardar dimensiones");
        return err;
    }
    err = nvs_set_i32(handle, "diameter", diameter);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error al guardar diametro");
    }
    err = nvs_set_i32(handle, "height", height);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error al guardar altura");
    }
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error al hacer commit en NVS");
    }
    nvs_close(handle);
    return err;
}

//
// Carga desde NVS las dimensiones del tinaco. Si no existen, se usan los valores por defecto.
//
esp_err_t load_tank_dimensions(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error al abrir NVS para cargar dimensiones");
        return err;
    }
    int32_t diameter = 0, height = 0;
    bool need_commit = false;

    err = nvs_get_i32(handle, "diameter", &diameter);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        diameter = DEFAULT_DIAMETRO_CM;
        need_commit = true;
        ESP_LOGI(TAG, "Diametro no encontrado en NVS, usando valor por defecto: %d", DEFAULT_DIAMETRO_CM);
        nvs_set_i32(handle, "diameter", diameter);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error al leer diametro de NVS");
    }

    err = nvs_get_i32(handle, "height", &height);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        height = DEFAULT_H_TOTAL_CM;
        need_commit = true;
        ESP_LOGI(TAG, "Altura no encontrada en NVS, usando valor por defecto: %d", DEFAULT_H_TOTAL_CM);
        nvs_set_i32(handle, "height", height);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error al leer altura de NVS");
    }

    if (need_commit) {
        nvs_commit(handle);
    }
    nvs_close(handle);

    g_tank_diameter = diameter;
    g_tank_height = height;
    ESP_LOGI(TAG, "Dimensiones del tinaco cargadas: diametro = %d cm, altura = %d cm", g_tank_diameter, g_tank_height);
    return ESP_OK;
}

//
// Calcula los litros restantes a partir de la distancia medida por el sensor
//
float calcular_litros_restantes(float distancia_sensor_cm) {
    float radio_cm = g_tank_diameter / 2.0; // Radio del tinaco en cm
    float altura_agua_cm = g_tank_height - distancia_sensor_cm; // Altura del agua

    if (altura_agua_cm < 0) { // Si la distancia del sensor es mayor a la altura del tinaco
        return 0.0;
    }

    float volumen_cm3 = M_PI * pow(radio_cm, 2) * altura_agua_cm; // Volumen en cm³
    return volumen_cm3 / 1000.0; // Convertir cm³ a litros
}

//
// Función principal
//
void app_main(void) {
    // Inicializar NVS
    ESP_ERROR_CHECK(nvs_flash_init());
    // Cargar dimensiones del tinaco desde NVS (o usar los valores por defecto)
    load_tank_dimensions();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Inicializar Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Inicializar ESP‑NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    // Inicializar sensor ultrasónico
    ultrasonic_sensor_init();

    // Crear tarea para enviar los datos del sensor (incluyendo los datos del tinaco)
    xTaskCreate(send_sensor_data, "send_sensor_data", 1024 * 3, NULL, 5, NULL);
}
