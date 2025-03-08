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
#define LONG_PRESS_TIME_MS 1500

Adafruit_SharpMem display(SHARP_SCK, SHARP_MOSI, SHARP_SS, 128, 128);
#define BLACK 0
#define WHITE 1

const char *main_menu_items[] = {"1. Consultar", "2. Añadir"};
const char *sub_menu_items[] = {"Placa_1", "Placa_2", "Placa_3", "Placa_4"};
const int main_menu_size = sizeof(main_menu_items) / sizeof(main_menu_items[0]);
const int sub_menu_size = sizeof(sub_menu_items) / sizeof(sub_menu_items[0]);
int current_menu_index = 0;
bool in_main_menu = true;
bool in_sub_menu = false;

const uint8_t mac_addresses[sub_menu_size][6] = {
    {0x68, 0xB6, 0xB3, 0x54, 0xB4, 0xC4}, // MAC de Placa_1
    {0x68, 0xB6, 0xB3, 0x52, 0xF4, 0x90}, // MAC de Placa_2
    {0x68, 0xB6, 0xB3, 0x54, 0xA9, 0x0C}, // MAC de Placa_3
    {0x48, 0xBF, 0x6B, 0xA9, 0x87, 0x65}  // MAC de Placa_4
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

enum SystemState {
  STATE_MAIN_MENU,
  STATE_SUB_MENU
};
SystemState system_state = STATE_MAIN_MENU;

int selected_placa = -1; 

void handleButton();
void nextMenuItem();
void displayMainMenu();
void displaySubMenu();
void displayReceivedMessage();
void send_command_to_mac(const char *command, int menu_index);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len);
void updateBatteryPercentage();
void handleSerialInput();
void showInitialMenu();
void selectPlaca(int placa_index);

void setup() {
  Serial.begin(115200);
  Serial.println(F("Iniciando setup..."));

  if (!display.begin()) {
    Serial.println(F("No se pudo inicializar la pantalla Sharp."));
    while (1);
  }
  Serial.println(F("Pantalla Sharp inicializada."));

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(BLACK);
  display.setCursor(0, 0);
  display.println(F("Pantalla OK!"));
  display.println(F("Iniciando..."));
  display.refresh();
  Serial.println(F("Mensaje inicial mostrado en la pantalla."));

  WiFi.mode(WIFI_STA);
  Serial.println(F("WiFi inicializado en modo STA."));

  if (esp_now_init() != ESP_OK) {
    Serial.println(F("Error inicializando ESP-NOW"));
    return;
  }
  Serial.println(F("ESP-NOW inicializado."));

  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.println(F("Setup completado."));

  showInitialMenu();
}

void loop() {
  handleButton();

  if (millis() - lastUpdateTime >= 2000) {
    updateBatteryPercentage();
    lastUpdateTime = millis();
  }

  handleSerialInput();

  switch (system_state) {
    case STATE_MAIN_MENU:
      displayMainMenu();
      break;
    case STATE_SUB_MENU:
      displaySubMenu();
      break;
  }
}

void updateBatteryPercentage() {
  adcValue = analogRead(ADC_PIN); 
  voltage = (adcValue * 3.3 / 4095.0) * 2; 

  porcentaje = (int)((voltage - 3.4) / (4.1 - 3.4) * 100);

  if (porcentaje > 100) porcentaje = 100;
  if (porcentaje < 0) porcentaje = 0;
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
    if (system_state == STATE_SUB_MENU) {
      nextMenuItem();
    } else {
      system_state = STATE_SUB_MENU;
      selected_placa = 0; 
      Serial.println(F("Modo selección de placa activado."));
      Serial.println(F("Placas disponibles:"));
      for (int i = 0; i < sub_menu_size; i++) {
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.println(sub_menu_items[i]);
      }
      Serial.print(F("Placa seleccionada: "));
      Serial.println(sub_menu_items[selected_placa]);
    }
  }

  if (is_long_press) {
    is_long_press = false; 
    if (selected_placa != -1) { 
      Serial.println(F("Botón presionado largo: Enviando comando DATO"));
      send_command_to_mac("DATO", selected_placa); 
    }
  }
}

void nextMenuItem() {
  current_menu_index = (current_menu_index + 1) % sub_menu_size;
  selected_placa = current_menu_index;
  Serial.print(F("Placa seleccionada: "));
  Serial.println(sub_menu_items[current_menu_index]);
}

void displayMainMenu() {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println(F("Menu Principal:"));
  display.println(main_menu_items[current_menu_index]);
  display.print(F("Batería: "));
  display.print(porcentaje, 0);
  display.println(F(" %"));
  display.refresh();
}

void displaySubMenu() {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println(F("Selecciona Placa:"));
  display.println(sub_menu_items[current_menu_index]);
  display.print(F("Batería: "));
  display.print(porcentaje, 0);
  display.println(F(" %"));
  display.refresh();
}

void displayReceivedMessage() {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println(F("Datos recibidos:"));
  if (strlen(received_message) > 0) {
    display.println(received_message);
  } else {
    display.println(F("No hay datos"));
  }
  display.print(F("Batería: "));
  display.print(porcentaje, 0);
  display.println(F(" %"));
  display.refresh();
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

  displayReceivedMessage();
}

void handleSerialInput() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (system_state == STATE_MAIN_MENU) {
      if (input.equalsIgnoreCase("consultar")) {
        system_state = STATE_SUB_MENU; 
        Serial.println(F("Placas disponibles:"));
        for (int i = 0; i < sub_menu_size; i++) {
          Serial.print(i + 1);
          Serial.print(": ");
          Serial.println(sub_menu_items[i]);
        }
      } else if (input.equalsIgnoreCase("añadir")) {
        Serial.println(F("Modo 'añadir' activado."));
      } else {
        Serial.println(F("Opción no válida. Por favor, elige 'consultar' o 'añadir'."));
      }
    } else if (system_state == STATE_SUB_MENU) {
      int placa_index = input.toInt() - 1; 
      if (placa_index >= 0 && placa_index < sub_menu_size) {
        selectPlaca(placa_index);
        Serial.println(F("Enviando mensaje 'DATO' a la placa seleccionada..."));
        send_command_to_mac("DATO", selected_placa); 
        Serial.println(F("Placas disponibles:"));
        for (int i = 0; i < sub_menu_size; i++) {
          Serial.print(i + 1);
          Serial.print(": ");
          Serial.println(sub_menu_items[i]);
        }
      } else {
        Serial.println(F("Número de placa no válido."));
      }
    }
  }
}

void showInitialMenu() {
  Serial.println(F("Selecciona una opción:"));
  Serial.println(F("1. Consultar"));
  Serial.println(F("2. Añadir"));
}

void selectPlaca(int placa_index) {
  selected_placa = placa_index;
  Serial.print(F("Placa seleccionada: "));
  Serial.println(sub_menu_items[placa_index]);
}