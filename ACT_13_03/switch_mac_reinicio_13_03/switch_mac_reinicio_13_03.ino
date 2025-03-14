#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SharpMem.h>
#include <esp_now.h>
#include <WiFi.h>
#include <EEPROM.h>

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
const int main_menu_size = sizeof(main_menu_items) / sizeof(main_menu_items[0]);
int current_menu_index = 0;
bool in_main_menu = true;
bool in_sub_menu = false;

struct PeerInfo {
  uint8_t mac[6];
  char name[16]; 
};

PeerInfo peers[10]; 
int peer_count = 0; 

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
  STATE_SUB_MENU,
  STATE_ADD_MAC,
  STATE_ADD_NAME
};
SystemState system_state = STATE_MAIN_MENU;

int selected_peer = -1; 

#define EEPROM_FLAG_ADDRESS 0
#define EEPROM_FLAG_INITIAL 0 
#define EEPROM_FLAG_SET 1    

void handleButton();
void nextMenuItem();
void displayMainMenu();
void displaySubMenu();
void displayReceivedMessage();
void send_command_to_mac(const char *command, int peer_index);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len);
void updateBatteryPercentage();
void handleSerialInput();
void showInitialMenu();
void selectPeer(int peer_index);
void addMacAddress();
void addPeerName();
void savePeers();
void loadPeers();
void initializeEEPROM();
void clearEEPROM();

void setup() {
  Serial.begin(115200);
  Serial.println(F("Iniciando setup..."));

  EEPROM.begin(512);

  uint8_t flag = EEPROM.read(EEPROM_FLAG_ADDRESS);
  if (flag == EEPROM_FLAG_INITIAL) {
    Serial.println(F("Primera ejecución: Borrando peers..."));
    clearEEPROM(); 
    peer_count = 0; 
    savePeers(); 

    EEPROM.write(EEPROM_FLAG_ADDRESS, EEPROM_FLAG_SET);
    EEPROM.commit();
    Serial.println(F("EEPROM inicializada por primera vez."));
  } else {
    Serial.println(F("EEPROM ya fue inicializada anteriormente."));
  }

  loadPeers();

  EEPROM.end();

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
    case STATE_ADD_MAC:
      addMacAddress();
      break;
    case STATE_ADD_NAME:
      addPeerName();
      break;
  }
}

void clearEEPROM() {
  Serial.println(F("Borrando toda la EEPROM..."));
  for (int i = 0; i < EEPROM.length(); i++) {
    EEPROM.write(i, 0xFF); 
  }
  EEPROM.commit();
  Serial.println(F("EEPROM borrada completamente."));
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
      selected_peer = 0; 
      Serial.println(F("Modo selección de peer activado."));
      Serial.println(F("Peers disponibles:"));
      for (int i = 0; i < peer_count; i++) {
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.println(peers[i].name);
      }
      Serial.print(F("Peer seleccionado: "));
      Serial.println(peers[selected_peer].name);
    }
  }

  if (is_long_press) {
    is_long_press = false; 
    if (selected_peer != -1) { 
      Serial.println(F("Botón presionado largo: Enviando comando DATO"));
      send_command_to_mac("DATO", selected_peer); 
    }
  }
}

void nextMenuItem() {
  current_menu_index = (current_menu_index + 1) % peer_count;
  selected_peer = current_menu_index;
  Serial.print(F("Peer seleccionado: "));
  Serial.println(peers[current_menu_index].name);
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
  display.println(F("Selecciona Peer:"));
  display.println(peers[current_menu_index].name);
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

void send_command_to_mac(const char *command, int peer_index) {
  const uint8_t *mac = peers[peer_index].mac;
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
        Serial.println(F("Peers disponibles:"));
        for (int i = 0; i < peer_count; i++) {
          Serial.print(i + 1);
          Serial.print(": ");
          Serial.println(peers[i].name);
        }
        Serial.println(F("Ingresa el número del peer o 'salir' para volver al menú principal."));
      } else if (input.equalsIgnoreCase("añadir")) {
        system_state = STATE_ADD_MAC; 
        Serial.println(F("Modo 'añadir' activado. Ingresa la dirección MAC en formato XX:XX:XX:XX:XX:XX"));
      } else {
        Serial.println(F("Opción no válida. Por favor, elige 'consultar' o 'añadir'."));
      }
    } else if (system_state == STATE_SUB_MENU) {
      if (input.equalsIgnoreCase("salir")) {
        system_state = STATE_MAIN_MENU; 
        Serial.println(F("Volviendo al menú principal."));
      } else {
        int peer_index = input.toInt() - 1; 
        if (peer_index >= 0 && peer_index < peer_count) {
          selectPeer(peer_index);
          Serial.println(F("Enviando mensaje 'DATO' al peer seleccionado..."));
          send_command_to_mac("DATO", selected_peer); 
          Serial.println(F("Peers disponibles:"));
          for (int i = 0; i < peer_count; i++) {
            Serial.print(i + 1);
            Serial.print(": ");
            Serial.println(peers[i].name);
          }
          Serial.println(F("Ingresa el número del peer o 'salir' para volver al menú principal."));
        } else {
          Serial.println(F("Número de peer no válido."));
        }
      }
    } else if (system_state == STATE_ADD_MAC) {
      if (input.length() == 17) {
        uint8_t new_mac[6];
        if (sscanf(input.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", 
                   &new_mac[0], &new_mac[1], &new_mac[2], 
                   &new_mac[3], &new_mac[4], &new_mac[5]) == 6) {
          if (peer_count < 10) { 
            memcpy(peers[peer_count].mac, new_mac, 6);
            peer_count++;
            system_state = STATE_ADD_NAME; 
            Serial.println(F("Dirección MAC añadida. Ingresa un nombre para este peer:"));
          } else {
            Serial.println(F("No se pueden añadir más peers."));
            system_state = STATE_MAIN_MENU; 
          }
        } else {
          Serial.println(F("Formato de dirección MAC no válido."));
          system_state = STATE_MAIN_MENU; 
        }
      } else {
        Serial.println(F("Longitud de la dirección MAC no válida."));
        system_state = STATE_MAIN_MENU; 
      }
    } else if (system_state == STATE_ADD_NAME) {
      if (input.length() > 0 && input.length() <= 15) {
        strncpy(peers[peer_count - 1].name, input.c_str(), 15);
        peers[peer_count - 1].name[15] = '\0'; 
        savePeers(); 
        Serial.println(F("Nombre añadido correctamente."));
      } else {
        Serial.println(F("Nombre no válido. Debe tener entre 1 y 15 caracteres."));
      }
      system_state = STATE_MAIN_MENU; 
    }
  }
}

void showInitialMenu() {
  Serial.println(F("Selecciona una opción:"));
  Serial.println(F("1. Consultar"));
  Serial.println(F("2. Añadir"));
}

void selectPeer(int peer_index) {
  selected_peer = peer_index;
  Serial.print(F("Peer seleccionado: "));
  Serial.println(peers[peer_index].name);
}

void addMacAddress() {
}

void addPeerName() {
}

void savePeers() {
  EEPROM.begin(512);
  for (int i = 0; i < peer_count; i++) {
    EEPROM.put(EEPROM_FLAG_ADDRESS + 1 + i * sizeof(PeerInfo), peers[i]);
  }
  EEPROM.commit();
  EEPROM.end();
}

void loadPeers() {
  EEPROM.begin(512);
  peer_count = 0; 

  for (int i = 0; i < 10; i++) { 
    EEPROM.get(EEPROM_FLAG_ADDRESS + 1 + i * sizeof(PeerInfo), peers[i]);

    bool is_valid = false;
    for (int j = 0; j < 6; j++) {
      if (peers[i].mac[j] != 0xFF) {
        is_valid = true;
        break;
      }
    }

    if (is_valid) {
      peer_count++; 
    } else {
      break; 
    }
  }

  EEPROM.end();
  Serial.print(F("Peers cargados: "));
  Serial.println(peer_count);
}