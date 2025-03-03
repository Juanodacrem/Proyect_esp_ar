#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SharpMem.h>
#include <esp_now.h>
#include <WiFi.h>

#define SHARP_SCK  12 
#define SHARP_MOSI 11  
#define SHARP_SS   10  
#define BUTTON_PIN 18 
#define ADC_PIN 1      
#define DEBOUNCE_DELAY_MS 50
#define SHORT_PRESS_TIME_MS 500
#define LONG_PRESS_TIME_MS 1000

Adafruit_SharpMem display(SHARP_SCK, SHARP_MOSI, SHARP_SS, 128, 128);
#define BLACK 0
#define WHITE 1


const char *menu_items[] = {"Placa_1", "Placa_2", "Placa_3"};
const int menu_size = sizeof(menu_items) / sizeof(menu_items[0]);
int current_menu_index = 0;
bool in_data_screen = false;


const uint8_t mac_addresses[menu_size][6] = {
    {0x68, 0xB6, 0xB3, 0x52, 0xF4, 0x90},
    {0x68, 0xB6, 0xB3, 0x54, 0x31, 0x24},
    {0x68, 0xB6, 0xB3, 0x54, 0xA9, 0x0C}
};


volatile bool button_state = false;
volatile bool last_button_state = false;
unsigned long button_down_time = 0;
unsigned long button_up_time = 0;
bool is_short_press = false;
bool is_long_press = false;

char received_message[32] = "";
int adcValue = 0; 
float voltage = 0;
float porcentaje = 0;
unsigned long lastUpdateTime = 0; 

void handleButton();
void nextMenuItem();
void displayMenu();
void displayMessage(const char *message);
void displayMACAddress(const uint8_t *mac);
void send_command_to_mac(const char *command, int menu_index);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len);
void updateBatteryPercentage();

void setup() {
  Serial.begin(115200);
  if (!display.begin()) while(1);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(BLACK);
  display.setCursor(0, 0);
  display.println(F("Pantalla OK!"));
  display.refresh();
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
}

void loop() {
  handleButton();

  if (millis() - lastUpdateTime >= 2000) {
    updateBatteryPercentage();
    lastUpdateTime = millis();
  }

  if (!in_data_screen) {
    displayMenu();
  } else {
    displayMessage(received_message);
  }
}

void displayMenu() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(F("Menu:"));
  display.println(menu_items[current_menu_index]);
  display.print(F("[_]p:"));      
  display.print(porcentaje, 0);
  display.print(F(" %"));         
  display.refresh();
}

void displayMessage(const char *message) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(F("Mensaje enviado:"));
  display.println(message);
  display.setCursor(0, 0);
  display.print(F("[_]p: "));       
  display.print(porcentaje, 0);   
  display.print(F(" %"));       
  display.refresh();
}

void displayMACAddress(const uint8_t *mac) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  display.println(macStr);
}

void send_command_to_mac(const char *command, int menu_index) {
  const uint8_t *mac = mac_addresses[menu_index];
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo)); 
  memcpy(peerInfo.peer_addr, mac, 6); 
  peerInfo.channel = 0; 
  peerInfo.encrypt = false; 


  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result == ESP_OK) {
    Serial.println(F("Peer agregado correctamente."));

    result = esp_now_send(mac, (uint8_t *)command, strlen(command));
    if (result == ESP_OK) {
      Serial.println(F("Comando enviado correctamente."));
    } else {
      Serial.println(F("Error al enviar el comando."));
    }

    esp_now_del_peer(mac);
  } else {
    Serial.println(F("Error al agregar el peer."));
  }
}


void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print(F("Mensaje enviado a: "));
  for (int i = 0; i < 6; i++) {
    Serial.print(mac_addr[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? " Éxito" : " Fallo");
}

void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len) {
  Serial.print(F("Datos recibidos de: "));
  for (int i = 0; i < 6; i++) {
    Serial.print(esp_now_info->src_addr[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  Serial.print(F("Longitud de los datos: "));
  Serial.println(len);
  Serial.print(F("Datos: "));
  for (int i = 0; i < len; i++) {
    Serial.print((char)incomingData[i]);
  }
  Serial.println();

  memset(received_message, 0, sizeof(received_message)); 
  memcpy(received_message, incomingData, len); 

  displayMessage(received_message);
}

void handleButton() {
  bool current_button_state = !digitalRead(BUTTON_PIN); 
  unsigned long now = millis();

  if (current_button_state != last_button_state) {
    if (current_button_state) { 
      button_down_time = now;
    } else { 
      button_up_time = now;
      unsigned long press_duration = button_up_time - button_down_time;

      if (press_duration > DEBOUNCE_DELAY_MS) {
        if (press_duration < SHORT_PRESS_TIME_MS) {
          is_short_press = true;
        } else if (press_duration < LONG_PRESS_TIME_MS) {
          is_long_press = true;
        }
      }
    }
    last_button_state = current_button_state;
  }

  if (is_short_press) {
    is_short_press = false; 
    if (!in_data_screen) nextMenuItem();
  }
  if (is_long_press) {
    is_long_press = false; 

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("Enviando a:"));
    display.println(menu_items[current_menu_index]);
    displayMACAddress(mac_addresses[current_menu_index]);
    display.refresh();  
    delay(1000);  

    send_command_to_mac("DATO", current_menu_index);
    displayMessage("Dato");
    
  }
}

void nextMenuItem() {
  current_menu_index = (current_menu_index + 1) % menu_size;
  displayMenu();
}

void updateBatteryPercentage() {
  adcValue = analogRead(ADC_PIN); 
  voltage = (adcValue * 3.3 / 4095.0) * 2; 
  porcentaje = (voltage / 4.1) * 100; 

  if (porcentaje > 100) porcentaje = 100;
  if (porcentaje < 0) porcentaje = 0;

  Serial.print(F("Voltaje: "));
  Serial.print(voltage, 2);
  Serial.print(F(" V, Porcentaje: "));
  Serial.print(porcentaje, 0);
  Serial.println(F(" %"));
}