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

// Variables de estado (compartidas entre tasks)
volatile int current_menu_index = 0;
volatile int current_consultar_menu_index = 0;
volatile bool in_data_screen = false;
volatile bool in_consultar_submenu = false;
volatile bool in_placa_selection = false;
volatile bool in_mandar_mensaje_mode = false;
volatile bool in_muestreo_mode = false;
volatile bool system_initialized = false;
volatile bool in_consultar_mode = false;
volatile bool menuAlreadyShown = false;

// Direcciones MAC
const uint8_t mac_addresses[menu_size][6] = {
    {0x68, 0xB6, 0xB3, 0x54, 0xB4, 0xC4},
    {0x68, 0xB6, 0xB3, 0x52, 0xF4, 0x90},
    {0x68, 0xB6, 0xB3, 0x54, 0xA9, 0x0C},
    {0x48, 0xBF, 0x6B, 0xA9, 0x87, 0x65}
};

// Gestión de conexión peer
struct PeerConnection {
  uint8_t mac[6];
  bool connected;
  unsigned long last_command_time;
};
PeerConnection peer = {0};

// Configuración de tiempos
const int STOP_DELAY_MS = 3000; // 3 segundos antes de enviar STOP
const int COMMAND_DELAY = 50;
const int DISPLAY_UPDATE_INTERVAL = 1000; // Actualizar pantalla cada segundo

// Botón
volatile bool button_state = false;
volatile bool last_button_state = false;
volatile unsigned long button_down_time = 0;
volatile unsigned long button_up_time = 0;
volatile bool is_short_press = false;

// Mensajes
char received_message[32] = "";
char status_message[64] = "";

// Batería
volatile int adcValue = 0;
volatile float voltage = 0;
volatile float porcentaje = 0;
volatile unsigned long lastUpdateTime = 0;
volatile unsigned long lastDisplayUpdate = 0;

// Muestreo
TaskHandle_t timer_taskHandle = NULL;
TaskHandle_t display_taskHandle = NULL;
TaskHandle_t button_taskHandle = NULL;
TaskHandle_t serial_taskHandle = NULL;
SemaphoreHandle_t xMutex = NULL;

volatile bool sampling_active = false;
volatile int samplingInterval = 30;
volatile int target_placa_index = -1;
volatile bool waitingForStop = false;
volatile unsigned long sampling_start_time = 0;
volatile unsigned long cycle_start_time = 0;
volatile int minutes_elapsed = 0;
volatile bool cycle_active = false;

// Prototipos de funciones
void showInitialMenu();
void displayStatus();
void displayReceivedMessage();
void updateDisplayStatus();
void serialMenu();
void handleButton();
void nextMenuItem();
void displayMenu();
void displayConsultarMenu();
void updateBatteryPercentage();
void selectPlaca(int placa_index);
void returnToMainMenu();
void printMacAddress(const uint8_t *mac);
bool managePeerConnection(int menu_index);
bool sendCommand(const char *command, int menu_index);
void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void sendMessageToPlaca(const char *message);
void startSampling();
void stopSampling();
void handleMuestreo();

// Tasks
void timer_task(void *pvParameters);
void display_task(void *pvParameters);
void button_task(void *pvParameters);
void serial_task(void *pvParameters);

void showInitialMenu() {
  if(xSemaphoreTake(xMutex, portMAX_DELAY)) {  // Corregido: paréntesis de cierre
    display.clearDisplay();
    display.setCursor(0, 10);
    display.println("Menu Principal:");
    display.println("1. Consultar");
    display.println("2. Añadir");
    display.print("Batería: ");
    display.print(porcentaje, 0);
    display.println(" %");
    display.refresh();
    xSemaphoreGive(xMutex);
  }
  
  Serial.println("\n--- MENU PRINCIPAL ---");
  Serial.println("1. Consultar");
  Serial.println("2. Añadir");
}

void displayStatus() {
  if(xSemaphoreTake(xMutex, portMAX_DELAY)) {  // Nota el doble paréntesis al final
    display.clearDisplay();
    display.setCursor(0, 10);
    
    if (in_muestreo_mode && sampling_active) {
      display.println("Modo Muestreo:");
      display.print("Intervalo: ");
      display.print(samplingInterval);
      display.println(" min");
      display.print("Transcurrido: ");
      display.print(minutes_elapsed);
      display.println(" min");
      
      if (cycle_active) {
        display.println("Estado: ACTIVO");
      } else {
        display.println("Estado: EN ESPERA");
      }
    } else {
      display.println(status_message);
    }
    
    display.print("Batería: ");
    display.print(porcentaje, 0);
    display.println(" %");
    display.refresh();
    xSemaphoreGive(xMutex);
  }
}

void displayReceivedMessage() {
  if(xSemaphoreTake(xMutex, portMAX_DELAY)) {
    display.clearDisplay();
    display.setCursor(0, 10);
    display.println("Datos recibidos:");
    if (strlen(received_message) > 0) {
      display.println(received_message);
    } else {
      display.println("No hay datos");
    }
    display.print("Tiempo: ");
    display.print(minutes_elapsed);
    display.println(" min");
    display.print("Batería: ");
    display.print(porcentaje, 0);
    display.println(" %");
    display.refresh();
    xSemaphoreGive(xMutex);
  }
}

void updateDisplayStatus() {
  if (millis() - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
    if (in_data_screen) {
      displayReceivedMessage();
    } else if (in_muestreo_mode && sampling_active) {
      displayStatus();
    } else if (in_consultar_submenu) {
      displayConsultarMenu();
    } else if (in_placa_selection) {
      displayMenu();
    } else {
      showInitialMenu();
    }
    lastDisplayUpdate = millis();
  }
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
            target_placa_index = current_menu_index;
          }
        } else {
          if (in_data_screen) {
            in_data_screen = false;
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
      } else if (in_consultar_mode) {
        in_placa_selection = true;
      }
    }
  }
}

void nextMenuItem() {
  current_menu_index = (current_menu_index + 1) % menu_size;
  displayMenu();
}

void displayMenu() {
  if(xSemaphoreTake(xMutex, portMAX_DELAY)) {
    display.clearDisplay();
    display.setCursor(0, 10);
    display.println("Selecciona una placa:");
    display.println(menu_items[current_menu_index]);
    display.print("Batería: ");
    display.print(porcentaje, 0);
    display.println(" %");
    display.refresh();
    xSemaphoreGive(xMutex);
  }
}

void displayConsultarMenu() {
  if(xSemaphoreTake(xMutex, portMAX_DELAY)) {
    display.clearDisplay();
    display.setCursor(0, 10);
    display.println("Consultar:");
    display.println(consultar_menu_items[current_consultar_menu_index]);
    display.print("Batería: ");
    display.print(porcentaje, 0);
    display.println(" %");
    display.refresh();
    xSemaphoreGive(xMutex);
  }
}

void updateBatteryPercentage() {
  adcValue = analogRead(ADC_PIN);
  voltage = (adcValue * 3.3 / 4095.0) * 2;
  porcentaje = (int)((voltage - 3.4) / (4.1 - 3.4) * 100);
  if (porcentaje > 100) porcentaje = 100;
  if (porcentaje < 0) porcentaje = 0;
}

void selectPlaca(int placa_index) {
  current_menu_index = placa_index;
  Serial.print("Placa seleccionada: ");
  Serial.println(menu_items[placa_index]);
  displayMenu();
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

void printMacAddress(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
}

bool managePeerConnection(int menu_index) {
  if (menu_index < 0 || menu_index >= menu_size) return false;

  // Verificar si ya está conectado al peer correcto
  if (peer.connected && memcmp(peer.mac, mac_addresses[menu_index], 6) == 0) {
    return true;
  }

  // Si estaba conectado a otro peer, desconectar
  if (peer.connected) {
    esp_now_del_peer(peer.mac);
    peer.connected = false;
    vTaskDelay(50 / portTICK_PERIOD_MS); // Pequeña pausa
  }

  // Configurar nuevo peer
  memcpy(peer.mac, mac_addresses[menu_index], 6);
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, peer.mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  // Intentar conexión con reintentos
  int retries = 3;
  while (retries-- > 0) {
    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
      peer.connected = true;
      Serial.print("Conexión establecida con ");
      printMacAddress(peer.mac);
      return true;
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }

  Serial.println("Error al añadir peer después de 3 intentos");
  return false;
}

bool sendCommand(const char *command, int menu_index) {
  if (!managePeerConnection(menu_index)) {
    Serial.println("No se pudo conectar para enviar comando");
    return false;
  }

  // Configurar tiempo de espera
  unsigned long startTime = millis();
  const unsigned long timeout = 1000; // 1 segundo de timeout

  while (millis() - startTime < timeout) {
    esp_err_t result = esp_now_send(peer.mac, (const uint8_t *)command, strlen(command));
    
    if (result == ESP_OK) {
      Serial.print("Comando ");
      Serial.print(command);
      Serial.print(" enviado a ");
      Serial.println(menu_items[menu_index]);
      peer.last_command_time = millis();
      return true;
    }
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  
  Serial.print("Error enviando comando ");
  Serial.println(command);
  return false;
}

void verifyStopConfirmation() {
  if (!waitingForStop) return;

  unsigned long current_time = millis();
  
  // Si ha pasado más tiempo del esperado sin confirmación
  if (current_time - sampling_start_time > (STOP_DELAY_MS + 5000)) {
    Serial.println("No se recibió confirmación de STOP, reintentando...");
    
    // Enviar STOP nuevamente
    if (target_placa_index >= 0) {
      sendCommand("STOP", target_placa_index);
      vTaskDelay(500 / portTICK_PERIOD_MS);
      sendCommand("STOP", target_placa_index);
    }
    
    // Reiniciar el temporizador
    sampling_start_time = current_time;
  }
}

void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len) {
  Serial.print("Datos recibidos de: ");
  printMacAddress(esp_now_info->src_addr);

  if(xSemaphoreTake(xMutex, portMAX_DELAY)) {
    memset(received_message, 0, sizeof(received_message));
    memcpy(received_message, incomingData, len);
    xSemaphoreGive(xMutex);
  }
  
  if (in_data_screen) {
    displayReceivedMessage();
  }
  
  Serial.print("Datos recibidos: ");
  Serial.println(received_message);
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Mensaje enviado a: ");
  printMacAddress(mac_addr);
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? " Éxito" : " Fallo");
}

void sendMessageToPlaca(const char *message) {
  if (strlen(message) > 0) {
    if (sendCommand(message, current_menu_index)) {
      Serial.print("Mensaje enviado: ");
      Serial.println(message);
    }
  } else {
    Serial.println("Mensaje vacío, no se envía");
  }
}

void startSampling() {
  if (!sampling_active) {
    sampling_active = true;
    sampling_start_time = millis();
    cycle_start_time = millis();
    minutes_elapsed = 0;
    cycle_active = true;
    
    sprintf(status_message, "Muestreo iniciado\nIntervalo: %d min", samplingInterval);
    Serial.println(status_message);
    
    if (target_placa_index >= 0) {
      if (sendCommand("START", target_placa_index)) {
        waitingForStop = true;
        sampling_start_time = millis();
        Serial.println("START enviado. Enviando STOP en 3 segundos...");
      }
    }
  }
}

void stopSampling() {
  if (sampling_active) {
    sampling_active = false;
    waitingForStop = false;
    cycle_active = false;
    
    strcpy(status_message, "Muestreo detenido");
    Serial.println(status_message);
    
    if (target_placa_index >= 0) {
      sendCommand("STOP", target_placa_index);
    }
  }
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
    if (input.equals("1")) samplingInterval = 5;
    else if (input.equals("2")) samplingInterval = 30;
    else if (input.equals("3")) samplingInterval = 60;
    
    Serial.print("Intervalo: "); 
    Serial.print(samplingInterval); 
    Serial.println(" minutos");
    
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
}

// Tasks implementations
void timer_task(void *pvParameters) {
  while (true) {
    if (sampling_active && in_muestreo_mode) {
      unsigned long current_time = millis();
      minutes_elapsed = (current_time - cycle_start_time) / 60000;

      // Envío de STOP con doble confirmación
      if (waitingForStop && (current_time - sampling_start_time >= STOP_DELAY_MS)) {
        if (target_placa_index >= 0 && peer.connected) {
          // Primer envío de STOP
          if (sendCommand("STOP", target_placa_index)) {
            Serial.println("Primer STOP enviado");
            
            // Esperar 500ms y enviar segundo STOP
            vTaskDelay(500 / portTICK_PERIOD_MS);
            if (sendCommand("STOP", target_placa_index)) {
              Serial.println("Segundo STOP enviado como confirmación");
              waitingForStop = false;
              cycle_active = false;
              
              // Opcional: enviar un tercer STOP después de 1 segundo
              vTaskDelay(1000 / portTICK_PERIOD_MS);
              sendCommand("STOP", target_placa_index);
              Serial.println("Tercer STOP enviado como refuerzo");
            }
          }
        }
      }

      // Ciclo completo de muestreo
      if (!waitingForStop && (current_time - sampling_start_time >= (samplingInterval * 60000))) {
        if (target_placa_index >= 0 && managePeerConnection(target_placa_index)) {
          if (sendCommand("START", target_placa_index)) {
            waitingForStop = true;
            sampling_start_time = current_time;
            cycle_start_time = current_time;
            minutes_elapsed = 0;
            cycle_active = true;
            
            Serial.print("\nNuevo ciclo iniciado. ");
            Serial.print("Intervalo: ");
            Serial.print(samplingInterval);
            Serial.println(" minutos");
            
            // Enviar START de confirmación después de 100ms
            vTaskDelay(100 / portTICK_PERIOD_MS);
            sendCommand("START", target_placa_index);
          }
        }
      }
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void display_task(void *pvParameters) {
  int last_menu_index = -1;
  int last_consultar_index = -1;
  bool last_data_screen_state = false;
  bool last_muestreo_state = false;
  float last_battery = -1;
  unsigned long lastDisplayUpdate = 0;

  while (true) {
    bool needs_update = false;
    unsigned long currentMillis = millis();

    // Verificar si hay cambios que requieran actualización
    if (current_menu_index != last_menu_index ||
        current_consultar_menu_index != last_consultar_index ||
        in_data_screen != last_data_screen_state ||
        in_muestreo_mode != last_muestreo_state ||
        abs(porcentaje - last_battery) > 1.0 ||
        currentMillis - lastDisplayUpdate >= 2000) {
      
      needs_update = true;
      last_menu_index = current_menu_index;
      last_consultar_index = current_consultar_menu_index;
      last_data_screen_state = in_data_screen;
      last_muestreo_state = in_muestreo_mode;
      last_battery = porcentaje;
      lastDisplayUpdate = currentMillis;
    }

    // Solo actualizar si hay cambios o cada 2 segundos para la batería
    if (needs_update) {
      if (in_data_screen) {
        displayReceivedMessage();
      } else if (in_muestreo_mode && sampling_active) {
        displayStatus();
      } else if (in_consultar_submenu) {
        displayConsultarMenu();
      } else if (in_placa_selection) {
        displayMenu();
      } else {
        showInitialMenu();
      }
    }

    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void button_task(void *pvParameters) {
  while (true) {
    handleButton();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void serial_task(void *pvParameters) {
  while (true) {
    if (!system_initialized) {
      serialMenu();
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
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
      } else if (in_consultar_submenu) {
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
        in_mandar_mensaje_mode = false;
        Serial.println("Mensaje enviado. Volviendo al menú.");
      }
    }
    
    // Actualización de batería cada 2 segundos
    if (millis() - lastUpdateTime >= 2000) {
      updateBatteryPercentage();
      lastUpdateTime = millis();
    }
    
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  
  // Inicializar mutex
  xMutex = xSemaphoreCreateMutex();
  
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
  
  // Inicializar variables de estado
  sampling_active = false;
  waitingForStop = false;
  cycle_active = false;
  minutes_elapsed = 0;
  
  showInitialMenu();
  
  // Crear tasks con suficientes recursos
  xTaskCreatePinnedToCore(timer_task, "timer_task", 4096, NULL, 1, &timer_taskHandle, 1);
  xTaskCreatePinnedToCore(display_task, "display_task", 4096, NULL, 2, &display_taskHandle, 1);
  xTaskCreatePinnedToCore(button_task, "button_task", 2048, NULL, 3, &button_taskHandle, 1);
  xTaskCreatePinnedToCore(serial_task, "serial_task", 4096, NULL, 2, &serial_taskHandle, 1);
  
  Serial.println("Sistema inicializado. Listo para operar.");
}

void loop() {
  // Verificar confirmación de STOP periódicamente
  verifyStopConfirmation();
  
  // Actualizar porcentaje de batería cada 2 segundos
  static unsigned long lastBatteryUpdate = 0;
  if (millis() - lastBatteryUpdate >= 2000) {
    updateBatteryPercentage();
    lastBatteryUpdate = millis();
  }
  
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}