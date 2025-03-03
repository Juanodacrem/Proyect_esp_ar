#include <stdio.h>
#include "esp_system.h"
#include "esp_mac.h"

void app_main(void) {
    uint8_t mac[6]; // Array para almacenar la dirección MAC (6 bytes)

    // Obtener la dirección MAC de la interfaz Wi-Fi Station
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    // Mostrar la dirección MAC en la consola
    printf("Dirección MAC para ESP-NOW: %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
