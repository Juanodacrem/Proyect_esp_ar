#include <EEPROM.h>  
#include <esp_now.h>
#include <WiFi.h>

#define LED_GPIO 2  
#define BUTTON_GPIO 4  
#define BUTTON_POLL_DELAY_MS 50  

uint8_t master_mac[] = {0xF0, 0xF5, 0xBD, 0x54, 0xEB, 0x50};

volatile bool led_state = false;  
volatile bool last_button_state = false;  

void save_led_state(bool state) {
    EEPROM.write(0, state ? 1 : 0);
    EEPROM.commit();
}

bool load_led_state() {
    return EEPROM.read(0) == 1;
}

void button_monitor_task(void *arg) {
    last_button_state = digitalRead(BUTTON_GPIO);

    while (1) {
        bool current_button_state = digitalRead(BUTTON_GPIO); 

        if (current_button_state != last_button_state) {
            delay(BUTTON_POLL_DELAY_MS);  
            current_button_state = digitalRead(BUTTON_GPIO);  

            if (current_button_state != last_button_state) {  
                led_state = !led_state;  
                digitalWrite(LED_GPIO, led_state ? HIGH : LOW);  
                save_led_state(led_state);  
                Serial.println(led_state ? "Estado del LED cambiado a: ENCENDIDO" : "Estado del LED cambiado a: APAGADO");
                last_button_state = current_button_state;  
            }
        }

        delay(BUTTON_POLL_DELAY_MS);
    }
}

void espnow_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len) {
    char command[10] = {0};
    memcpy(command, data, len);

    Serial.printf("Comando recibido de %02X:%02X:%02X:%02X:%02X:%02X: %s\n",
                  esp_now_info->src_addr[0], esp_now_info->src_addr[1], esp_now_info->src_addr[2],
                  esp_now_info->src_addr[3], esp_now_info->src_addr[4], esp_now_info->src_addr[5], command);

    if (strcmp(command, "DATO") == 0) {
        led_state = !led_state;  
        digitalWrite(LED_GPIO, led_state ? HIGH : LOW);  
        save_led_state(led_state);  
        Serial.println(led_state ? "Estado del LED cambiado a: ENCENDIDO" : "Estado del LED cambiado a: APAGADO");
    }
}

void setup() {
    Serial.begin(115200);

    EEPROM.begin(512);

    WiFi.mode(WIFI_STA);

    if (esp_now_init() != ESP_OK) {
        Serial.println("Error inicializando ESP-NOW");
        return;
    }

    esp_now_register_recv_cb(espnow_recv_cb);

    pinMode(LED_GPIO, OUTPUT);

    pinMode(BUTTON_GPIO, INPUT_PULLDOWN);

    led_state = load_led_state();
    digitalWrite(LED_GPIO, led_state ? HIGH : LOW);  

    xTaskCreate(button_monitor_task, "button_monitor_task", 2048, NULL, 10, NULL);

    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, master_mac, 6);
    peer_info.channel = 0; 
    peer_info.encrypt = false;

    if (esp_now_add_peer(&peer_info) != ESP_OK) {
        Serial.println("Error al agregar peer maestro");
        return;
    }

    Serial.println("Esperando comandos o cambios en el estado del botón...");
}

void loop() {
}