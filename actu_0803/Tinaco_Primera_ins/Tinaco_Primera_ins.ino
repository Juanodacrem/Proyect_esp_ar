#include <esp_now.h>
#include <WiFi.h>
#include <math.h>
#include <EEPROM.h>

#define TAG "SLAVE"

uint8_t master_mac[] = {0xF0, 0xF5, 0xBD, 0x54, 0xEB, 0x50};

#define TRIG_GPIO 41
#define ECHO_GPIO 40
#define SOUND_SPEED 0.034   
#define TIMEOUT_US 100000   

#define DEFAULT_H_TOTAL_CM 120    
#define DEFAULT_DIAMETRO_CM 97    

int g_tank_height = DEFAULT_H_TOTAL_CM;
int g_tank_diameter = DEFAULT_DIAMETRO_CM;

volatile bool send_data = false;

#define ADC_PIN_BATTERY 1  
#define ADC_PIN_CHANNEL2 2 
int adcValueBattery = 0;   
int adcValueChannel2 = 0;  
float voltageBattery = 0; 
float voltageChannel2 = 0; 
int porcentaje = 0;       
unsigned long lastUpdateTime = 0; 
bool batteryAlertActive = false;

#define PULSE_GPIO 21
#define BATTERY_ALERT_GPIO 10

float last_volume = 0.0;
int start_count = 0;

bool dimensions_updated = false;

float calcular_litros_restantes(float distancia_sensor_cm);
void ultrasonic_sensor_init(void);
float ultrasonic_sensor_measure_distance(void);
void espnow_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);
void send_sensor_data(void *parameter);
void updateBatteryPercentage();

void ultrasonic_sensor_init(void) {
    pinMode(TRIG_GPIO, OUTPUT);
    pinMode(ECHO_GPIO, INPUT);
}

float ultrasonic_sensor_measure_distance(void) {
    digitalWrite(TRIG_GPIO, LOW);
    delayMicroseconds(5);
    digitalWrite(TRIG_GPIO, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_GPIO, LOW);

    long start_time = micros();
    while (digitalRead(ECHO_GPIO) == LOW && (micros() - start_time) < TIMEOUT_US);

    start_time = micros();
    while (digitalRead(ECHO_GPIO) == HIGH && (micros() - start_time) < TIMEOUT_US);
    long end_time = micros();

    long pulse_duration = end_time - start_time;
    return (pulse_duration * SOUND_SPEED) / 2;
}

void espnow_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len) {
    char command[50] = {0};
    int copy_len = len < sizeof(command) - 1 ? len : sizeof(command) - 1;
    memcpy(command, data, copy_len);
    command[copy_len] = '\0';

    Serial.printf("Comando recibido de %02X:%02X:%02X:%02X:%02X:%02X: %s\n",
                  esp_now_info->src_addr[0], esp_now_info->src_addr[1], esp_now_info->src_addr[2],
                  esp_now_info->src_addr[3], esp_now_info->src_addr[4], esp_now_info->src_addr[5], command);

    // comando para actualizar dimensiones, formato: "TANK:diametro,altura"
    if (strncmp(command, "TANK:", 5) == 0) {
        int new_diameter, new_height;
        if (sscanf(command + 5, "%d,%d", &new_diameter, &new_height) == 2) {
            g_tank_diameter = new_diameter;
            g_tank_height = new_height;
            Serial.printf("Dimensiones del tinaco actualizadas: diametro=%d cm, altura=%d cm\n",
                          new_diameter, new_height);
            dimensions_updated = true; 
            EEPROM.write(0, 1); 
            EEPROM.commit();
        } else {
            Serial.println("Formato invalido para comando TANK. Use: TANK:diametro,altura");
        }
        return;
    }

    if (strcmp(command, "START") == 0) {
        send_data = true;
        start_count++;
        Serial.printf("Contador START: %d\n", start_count);
    } else if (strcmp(command, "STOP") == 0) {
        send_data = false;
        Serial.println("Contador START reiniciado.");
    }
}

void send_sensor_data(void *parameter) {
    static bool peer_added = false;
    if (!peer_added) {
        esp_now_peer_info_t peer_info = {};
        memcpy(peer_info.peer_addr, master_mac, 6);
        peer_info.channel = 0;
        peer_info.encrypt = false;

        if (esp_now_add_peer(&peer_info) != ESP_OK) {
            Serial.println("Error al agregar peer maestro");
            return;
        }
        peer_added = true;
    }

    while (1) {
        if (send_data) {
            digitalWrite(PULSE_GPIO, HIGH);

            updateBatteryPercentage();

            float distance = ultrasonic_sensor_measure_distance();
            float volumen_litros = calcular_litros_restantes(distance);
            adcValueChannel2 = analogRead(ADC_PIN_CHANNEL2);
            voltageChannel2 = (adcValueChannel2 * 3.3 / 4095.0) * 2; 

            if (distance > 20) {
                char message[100];
                snprintf(message, sizeof(message), "T:%.2f,%d,%.2f",
                         volumen_litros, porcentaje, voltageChannel2);
                Serial.printf("Enviando datos: %s\n", message);

                if (start_count == 2) {
                    if (abs(volumen_litros - last_volume) < 5.0) {
                        char alert_message[150];
                        snprintf(alert_message, sizeof(alert_message), "%s:ALERT: No se ha detectado aumento en el volumen de agua.", message);
                        esp_err_t alert_err = esp_now_send(master_mac, (uint8_t *)alert_message, strlen(alert_message));
                        if (alert_err != ESP_OK) {
                            Serial.printf("Error al enviar alerta: %s\n", esp_err_to_name(alert_err));
                        } else {
                            Serial.println("Alerta enviada: No se ha detectado aumento en el volumen de agua.");
                        }
                        start_count = 0; 
                    } else {
                        esp_err_t err = esp_now_send(master_mac, (uint8_t *)message, strlen(message));
                        if (err != ESP_OK) {
                            Serial.printf("Error al enviar datos: %s\n", esp_err_to_name(err));
                        }
                    }
                } else {
                    esp_err_t err = esp_now_send(master_mac, (uint8_t *)message, strlen(message));
                    if (err != ESP_OK) {
                        Serial.printf("Error al enviar datos: %s\n", esp_err_to_name(err));
                    }
                }

                last_volume = volumen_litros;
            }

            digitalWrite(PULSE_GPIO, LOW);
        }
        delay(1000);
    }
}

float calcular_litros_restantes(float distancia_sensor_cm) {
    float radio_cm = g_tank_diameter / 2.0; 
    float altura_agua_cm = g_tank_height - distancia_sensor_cm;

    if (altura_agua_cm < 0) { 
        return 0.0;
    }

    float volumen_cm3 = M_PI * pow(radio_cm, 2) * altura_agua_cm; 
    return volumen_cm3 / 1000.0; 
}

void updateBatteryPercentage() {
    adcValueBattery = analogRead(ADC_PIN_BATTERY); 
    voltageBattery = (adcValueBattery * 3.3 / 4095.0) * 2; 

    porcentaje = (int)((voltageBattery - 3.4) / (4.1 - 3.4) * 100);

    if (porcentaje > 100) porcentaje = 100;
    if (porcentaje < 0) porcentaje = 0;

    Serial.printf("Porcentaje de batería: %d%%\n", porcentaje);
}

void setup() {
    Serial.begin(115200);

    EEPROM.begin(1);
    dimensions_updated = EEPROM.read(0);

    if (!dimensions_updated) {
        Serial.println("Esperando actualización de dimensiones del tinaco...");
        while (!dimensions_updated) {
            delay(1000); 
        }
    }

    WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) {
        Serial.println("Error inicializando ESP-NOW");
        return;
    }

    esp_now_register_recv_cb(espnow_recv_cb);

    ultrasonic_sensor_init();

    pinMode(PULSE_GPIO, OUTPUT);
    digitalWrite(PULSE_GPIO, LOW); 

    xTaskCreate(send_sensor_data, "send_sensor_data", 1024 * 3, NULL, 5, NULL);
}

void loop() {
    updateBatteryPercentage();

    if (porcentaje <= 20 && !batteryAlertActive) {
        digitalWrite(BATTERY_ALERT_GPIO, HIGH); 
        batteryAlertActive = true;
    }
    else if (porcentaje >= 95 && batteryAlertActive) {
        digitalWrite(BATTERY_ALERT_GPIO, LOW); 
        batteryAlertActive = false; 
    }

    delay(1000); 
}