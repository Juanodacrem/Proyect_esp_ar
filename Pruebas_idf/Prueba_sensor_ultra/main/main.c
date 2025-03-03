#include <stdio.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Definición de pines (ajusta según tu conexión)
#define TRIG_GPIO 41  // GPIO12 para el Trigger
#define ECHO_GPIO 40  // GPIO11 para el Echo

// Constantes para el cálculo de la distancia
#define SOUND_SPEED 0.034  // Velocidad del sonido en cm/μs
#define TIMEOUT_US 100000  // Tiempo de espera máximo para el eco (en microsegundos)

// Etiqueta para el logging
static const char *TAG = "Ultrasonic Sensor";

// Función para inicializar los pines
void ultrasonic_sensor_init() {
    // Configurar el pin del Trigger como salida
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << TRIG_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // Configurar el pin del Echo como entrada
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << ECHO_GPIO);
    gpio_config(&io_conf);
}

// Función para medir la distancia
float ultrasonic_sensor_measure_distance() {
    // Enviar un pulso al Trigger
    gpio_set_level(TRIG_GPIO, 0);
    esp_rom_delay_us(5);  // Esperar 5 μs
    gpio_set_level(TRIG_GPIO, 1);
    esp_rom_delay_us(10);  // Esperar 10 μs
    gpio_set_level(TRIG_GPIO, 0);

    // Medir el tiempo que el Echo está en alto
    int64_t start_time = esp_timer_get_time();
    while (gpio_get_level(ECHO_GPIO) == 0 && (esp_timer_get_time() - start_time) < TIMEOUT_US);

    start_time = esp_timer_get_time();
    while (gpio_get_level(ECHO_GPIO) == 1 && (esp_timer_get_time() - start_time) < TIMEOUT_US);
    int64_t end_time = esp_timer_get_time();

    // Calcular la duración del pulso en microsegundos
    int64_t pulse_duration = end_time - start_time;

    // Calcular la distancia en centímetros
    float distance = (pulse_duration * SOUND_SPEED) / 2;
    return distance;
}

void app_main() {
    // Inicializar el sensor ultrasónico
    ultrasonic_sensor_init();

    while (1) {
        // Medir la distancia
        float distance = ultrasonic_sensor_measure_distance();

        // Imprimir la distancia en el monitor serial
        ESP_LOGI(TAG, "Distance: %.2f cm", distance);

        // Esperar 100 ms antes de la siguiente medición
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}