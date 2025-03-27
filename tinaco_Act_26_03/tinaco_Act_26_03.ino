#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SharpMem.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

// Configuración de la pantalla Sharp
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

// Menú
const char *menu_items[] = {"Placa_1", "Placa_2", "Placa_3", "Placa_4"};
const char *consultar_menu_items[] = {"Mandar Mensaje", "Muestreo", "Salir"};
const int menu_size = sizeof(menu_items) / sizeof(menu_items[0]);
const int consultar_menu_size = sizeof(consultar_menu_items) / sizeof(consultar_menu_items[0]);

// Variables de estado
int current_menu_index = 0;
int current_consultar_menu_index = 0;
bool in_data_screen = false;
bool in_consultar_submenu = false;
bool in_placa_selection = false;
bool in_mandar_mensaje_mode = false;
bool in_muestreo_mode = false;
bool system_initialized = false;
bool in_consultar_mode = false;
bool menuAlreadyShown = false;

// Direcciones MAC
const uint8_t mac_addresses[menu_size][6] = {
    {0x68, 0xB6, 0xB3, 0x54, 0xB4, 0xC4},
    {0x68, 0xB6, 0xB3, 0x52, 0xF4, 0x90},
    {0x68, 0xB6, 0xB3, 0x54, 0xA9, 0x0C},
    {0x48, 0xBF, 0x6B, 0xA9, 0x87, 0x65}
};

// Botón
volatile bool button_state = false;
volatile bool last_button_state = false;
unsigned long button_down_time = 0;
unsigned long button_up_time = 0;
bool is_short_press = false;

// Mensajes
char received_message[32] = "";

// Batería
int adcValue = 0;
float voltage = 0;
float porcentaje = 0;
unsigned long lastUpdateTime = 0;

// Muestreo
TaskHandle_t timer_taskHandle = NULL;
TaskHandle_t message_taskHandle = NULL;  
bool sampling_active = false;
int samplingInterval = 30;
bool send_start_command = false;
bool send_stop_command = false;
int target_placa_index = -1;

// Lecturas
int readingNumber = 0;
char firstReading[32] = "";
char secondReading[32] = "";
bool waitingForReadings = false;
unsigned long last_sampling_time = 0;
bool first_cycle = true;

// Prototipos de funciones
void handleButton();
void nextMenuItem();
void displayMenu();
void displayReceivedMessage();
void send_command_to_mac(const char *command, int menu_index);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len);
void updateBatteryPercentage();
void serialMenu();
void showInitialMenu();
void selectPlaca(int placa_index);
void sendMessageToPlaca(const char *message);
void startSampling();
void stopSampling();
void displayConsultarMenu();
void returnToMainMenu();
void handleMuestreo();
void timer_task(void *pvParameters);
void message_task(void *pvParameters);

void setup() {
  Serial.begin(115200);
  
  if (!display.begin()) {
    Serial.println("Error en pantalla");
    while(1);
  }
  
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error ESP-NOW");
    return;
  }
  
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  showInitialMenu();
  xTaskCreatePinnedToCore(timer_task, "timer_task", 10000, NULL, 1, &timer_taskHandle, 1);
  xTaskCreatePinnedToCore(message_task, "message_task", 10000, NULL, 1, &message_taskHandle, 1);
}

void loop() {
  if (!system_initialized) {
    serialMenu();
    return;
  }

  handleButton();

  if (millis() - lastUpdateTime >= 2000) {
    updateBatteryPercentage();
    lastUpdateTime = millis();
  }

  if (!in_data_screen) {
    if (in_placa_selection) {
      displayMenu();
      menuAlreadyShown = false;
    } else if (in_consultar_submenu) {
      displayConsultarMenu();
      menuAlreadyShown = false;
    } else {
      if (!menuAlreadyShown) {
        showInitialMenu();
        menuAlreadyShown = true;
      }
    }
  } else {
    displayReceivedMessage();
    menuAlreadyShown = false;
  }

  // Verificación adicional para asegurar el envío de STOP
  static unsigned long last_stop_check = 0;
  if (in_muestreo_mode && millis() - last_stop_check > 1000) {
    last_stop_check = millis();
    if (waitingForReadings && readingNumber >= 1) {
      // Seguridad: si por alguna razón no se envió STOP después de la segunda lectura
      if (target_placa_index >= 0) {
        send_command_to_mac("STOP", target_placa_index);
        waitingForReadings = false;
        readingNumber = 0;
        Serial.println("STOP enviado por verificación de seguridad");
      }
    }
  }

  if (in_consultar_mode && Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (in_placa_selection) {
      int placa_index = input.toInt() - 1;
      if (placa_index >= 0 && placa_index < menu_size) {
        selectPlaca(placa_index);
        in_placa_selection = false;
        in_consultar_submenu = true;
        Serial.println("Opciones:");
        for (int i = 0; i < consultar_menu_size; i++) {
          Serial.print(i + 1); Serial.print(": "); Serial.println(consultar_menu_items[i]);
        }
      }
    } else if (in_consultar_submenu && !in_muestreo_mode) {
      if (input.equals("1")) {
        in_mandar_mensaje_mode = true;
        in_muestreo_mode = false;
        Serial.println("Escribe mensaje:");
      } else if (input.equals("2")) {
        in_muestreo_mode = true;
        in_mandar_mensaje_mode = false;
        handleMuestreo();
      } else if (input.equals("3")) {
        returnToMainMenu();
      }
    } else if (in_mandar_mensaje_mode) {
      sendMessageToPlaca(input.c_str());
    }
  }
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Mensaje enviado a: ");
  for (int i = 0; i < 6; i++) {
    Serial.print(mac_addr[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? " Éxito" : " Fallo");
}

void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len) {
  Serial.print("Datos recibidos de: ");
  for (int i = 0; i < 6; i++) {
    Serial.print(esp_now_info->src_addr[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println();

  if (in_muestreo_mode && waitingForReadings) {
    if (readingNumber == 0) {
      // Primera lectura
      memset(firstReading, 0, sizeof(firstReading));
      memcpy(firstReading, incomingData, len);
      readingNumber++;
      Serial.println("Primera lectura recibida (no mostrada)");
    } 
    else if (readingNumber == 1) {
      // Segunda lectura
      memset(secondReading, 0, sizeof(secondReading));
      memcpy(secondReading, incomingData, len);
      
      // Mostrar solo la segunda lectura
      memset(received_message, 0, sizeof(received_message));
      memcpy(received_message, secondReading, strlen(secondReading));
      
      // Enviar STOP inmediatamente después de la segunda lectura
      if (target_placa_index >= 0) {
        Serial.println("Enviando STOP después de segunda lectura...");
        send_command_to_mac("STOP", target_placa_index);
      }
      
      waitingForReadings = false;
      readingNumber = 0;
      
      if (in_data_screen) {
        displayReceivedMessage();
      }
      
      Serial.print("Segunda lectura recibida y mostrada: ");
      Serial.println(received_message);
    }
  } 
  else {
    // Comportamiento normal cuando no estamos en modo muestreo
    memset(received_message, 0, sizeof(received_message));
    memcpy(received_message, incomingData, len);
    
    if (in_data_screen) {
      displayReceivedMessage();
    }
  }
}

void send_command_to_mac(const char *command, int menu_index) {
  if (menu_index < 0 || menu_index >= menu_size) {
    Serial.println("Índice de placa inválido");
    return;
  }

  const uint8_t *mac = mac_addresses[menu_index];
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  // Verificar si el peer ya existe
  if (esp_now_is_peer_exist(mac)) {
    esp_now_del_peer(mac);
  }

  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result != ESP_OK) {
    Serial.print("Error añadiendo peer: ");
    Serial.println(result);
    return;
  }

  result = esp_now_send(mac, (uint8_t *)command, strlen(command));
  if (result == ESP_OK) {
    Serial.print("Comando ");
    Serial.print(command);
    Serial.print(" enviado a ");
    Serial.println(menu_items[menu_index]);
  } else {
    Serial.print("Error enviando comando: ");
    Serial.println(result);
  }

  // Pequeño delay para asegurar la transmisión
  delay(100);
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
          if (!in_data_screen) {
            in_data_screen = true;
            send_start_command = true;
            target_placa_index = current_menu_index;
          }
        } else {
          if (in_data_screen) {
            in_data_screen = false;
            send_stop_command = true;
            target_placa_index = current_menu_index;
          }
        }
      }
    }
    last_button_state = current_button_state;
  }

  if (is_short_press) {
    is_short_press = false;
    if (!in_data_screen) {
      if (in_placa_selection) {
        nextMenuItem();
      } else if (in_consultar_submenu) {
        current_consultar_menu_index = (current_consultar_menu_index + 1) % consultar_menu_size;
        if (current_consultar_menu_index == consultar_menu_size - 1) {
          returnToMainMenu();
        } else if (current_consultar_menu_index == 0) {
          in_mandar_mensaje_mode = true;
          in_muestreo_mode = false;
        } else if (current_consultar_menu_index == 1) {
          in_muestreo_mode = true;
          in_mandar_mensaje_mode = false;
        }
        displayConsultarMenu();
      } else if (in_consultar_mode) {
        in_placa_selection = true;
        displayMenu();
      }
    }
  }
}

void nextMenuItem() {
  current_menu_index = (current_menu_index + 1) % menu_size;
  displayMenu();
}

void displayMenu() {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("Selecciona una placa:");
  display.println(menu_items[current_menu_index]);
  display.print("Batería: ");
  display.print(porcentaje, 0);
  display.println(" %");
  display.refresh();
}

void displayConsultarMenu() {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("Consultar:");
  display.println(consultar_menu_items[current_consultar_menu_index]);
  display.print("Batería: ");
  display.print(porcentaje, 0);
  display.println(" %");
  display.refresh();
}

void displayReceivedMessage() {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("Datos recibidos:");
  if (strlen(received_message) > 0) {
    display.println(received_message);
  } else {
    display.println("No hay datos");
  }
  display.print("Batería: ");
  display.print(porcentaje, 0);
  display.println(" %");
  display.refresh();
}

void updateBatteryPercentage() {
  adcValue = analogRead(ADC_PIN);
  voltage = (adcValue * 3.3 / 4095.0) * 2;
  porcentaje = (int)((voltage - 3.4) / (4.1 - 3.4) * 100);
  if (porcentaje > 100) porcentaje = 100;
  if (porcentaje < 0) porcentaje = 0;
}

void serialMenu() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.equalsIgnoreCase("consultar")) {
      system_initialized = true;
      in_consultar_mode = true;
      in_placa_selection = true;
      displayMenu();
      Serial.println("Placas disponibles:");
      for (int i = 0; i < menu_size; i++) {
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.println(menu_items[i]);
      }
    } else if (input.equalsIgnoreCase("añadir")) {
      system_initialized = true;
      in_consultar_mode = false;
    }
  }
}

void showInitialMenu() {
  Serial.println("\n--- MENU PRINCIPAL ---");
  Serial.println("1. Consultar");
  Serial.println("2. Añadir");
}

void selectPlaca(int placa_index) {
  current_menu_index = placa_index;
  Serial.print("Placa seleccionada: ");
  Serial.println(menu_items[placa_index]);
  displayMenu();
}

void sendMessageToPlaca(const char *message) {
  send_command_to_mac(message, current_menu_index);
  Serial.print("Mensaje enviado a ");
  Serial.print(menu_items[current_menu_index]);
  Serial.print(": ");
  Serial.println(message);
}

void startSampling() {
  if (!sampling_active) {
    sampling_active = true;
    first_cycle = true;
    last_sampling_time = millis();
    waitingForReadings = false;
    readingNumber = 0;
    
    Serial.print("Muestreo iniciado con intervalo de ");
    Serial.print(samplingInterval);
    Serial.println(" minutos");
    
    if (target_placa_index >= 0 && in_muestreo_mode) {
      send_command_to_mac("START", target_placa_index);
      waitingForReadings = true;
    }
  }
}

void stopSampling() {
  if (sampling_active) {
    sampling_active = false;
    waitingForReadings = false;
    readingNumber = 0;
    
    Serial.println("Muestreo detenido");
    
    if (target_placa_index >= 0) {
      send_command_to_mac("STOP", target_placa_index);
    }
  }
}

void returnToMainMenu() {
  in_consultar_submenu = false;
  in_placa_selection = false;
  in_consultar_mode = false;
  in_mandar_mensaje_mode = false;
  in_muestreo_mode = false;
  menuAlreadyShown = false;
  showInitialMenu();
}

void handleMuestreo() {
  Serial.println("\nSelecciona intervalo:");
  Serial.println("1. 5 minutos");
  Serial.println("2. 30 minutos");
  Serial.println("3. 60 minutos");
  Serial.println("4. Salir");

  unsigned long startTime = millis();
  while (!Serial.available() && millis() - startTime < 30000) {
    delay(100);
  }

  if (!Serial.available()) {
    Serial.println("Timeout: Volviendo al menú");
    returnToMainMenu();
    return;
  }

  String input = Serial.readStringUntil('\n');
  input.trim();

  if (input.equals("1") || input.equals("2") || input.equals("3")) {
    // Establecer intervalo
    if (input.equals("1")) samplingInterval = 5;
    else if (input.equals("2")) samplingInterval = 30;
    else if (input.equals("3")) samplingInterval = 60;
    
    Serial.print("Intervalo: "); Serial.print(samplingInterval); Serial.println(" minutos");
    
    // Configurar modo de muestreo
    in_muestreo_mode = true;
    target_placa_index = current_menu_index;
    startSampling();
  }
  else if (input.equals("4")) {
    Serial.println("Saliendo...");
    in_muestreo_mode = false;
    stopSampling();
  }
  else {
    Serial.println("Opción inválida");
  }

  returnToMainMenu();
}

void timer_task(void *pvParameters) {
  while (true) {
    if (sampling_active && in_muestreo_mode) {
      unsigned long current_time = millis();
      
      if (first_cycle || (current_time - last_sampling_time >= (samplingInterval * 60000))) {
        if (!waitingForReadings) {  // Solo iniciar nuevo ciclo si no hay uno en progreso
          first_cycle = false;
          last_sampling_time = current_time;
          waitingForReadings = true;
          readingNumber = 0;
          
          if (target_placa_index >= 0) {
            send_command_to_mac("START", target_placa_index);
            Serial.print("Iniciando ciclo de muestreo (Intervalo: ");
            Serial.print(samplingInterval);
            Serial.println(" minutos)");
            Serial.println("Esperando 2 lecturas...");
          }
        }
      }
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void message_task(void *pvParameters) {
  while (true) {
    if (send_start_command) {
      send_command_to_mac("START", target_placa_index);
      send_start_command = false;
      target_placa_index = -1;
    }
    
    if (send_stop_command) {
      send_command_to_mac("STOP", target_placa_index);
      send_stop_command = false;
      target_placa_index = -1;
    }
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}