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
#define ADC_PIN 1       // Pin del ADC para leer la batería

// Tiempos de pulsación del botón
#define DEBOUNCE_DELAY_MS 50
#define SHORT_PRESS_TIME_MS 500
#define LONG_PRESS_TIME_MS 1500

// Objetos para la pantalla Sharp
Adafruit_SharpMem display(SHARP_SCK, SHARP_MOSI, SHARP_SS, 128, 128);
#define BLACK 0
#define WHITE 1

// Menú y estado
const char *menu_items[] = {"Placa_1", "Placa_2", "Placa_3", "Placa_4"};
const char *consultar_menu_items[] = {"Mandar Mensaje", "Muestreo", "Salir"};
const int menu_size = sizeof(menu_items) / sizeof(menu_items[0]);
const int consultar_menu_size = sizeof(consultar_menu_items) / sizeof(consultar_menu_items[0]);
int current_menu_index = 0;
int current_consultar_menu_index = 0;
bool in_data_screen = false;
bool in_consultar_submenu = false;
bool in_placa_selection = false;
bool in_mandar_mensaje_mode = false;
bool in_muestreo_mode = false;

// Direcciones MAC específicas para cada placa
const uint8_t mac_addresses[menu_size][6] = {
    {0x68, 0xB6, 0xB3, 0x54, 0xB4, 0xC4}, // MAC de Placa_1
    {0x68, 0xB6, 0xB3, 0x52, 0xF4, 0x90}, // MAC de Placa_2
    {0x68, 0xB6, 0xB3, 0x54, 0xA9, 0x0C}, // MAC de Placa_3
    {0x48, 0xBF, 0x6B, 0xA9, 0x87, 0x65}  // MAC de Placa_4
};

// Variables para el manejo del botón
volatile bool button_state = false;
volatile bool last_button_state = false;
unsigned long button_down_time = 0;
unsigned long button_up_time = 0;
bool is_short_press = false;

// Buffer para almacenar el mensaje recibido
char received_message[32] = "";

// Variables para la lectura de la batería
int adcValue = 0;          // Valor leído del ADC
float voltage = 0;         // Voltaje calculado
float porcentaje = 0;      // Porcentaje de batería calculado
unsigned long lastUpdateTime = 0; // Tiempo de la última actualización

// Estado del sistema
bool system_initialized = false; // Indica si el sistema está listo para funcionar
bool in_consultar_mode = false;  // Indica si estamos en modo "consultar"
bool placa_selected_by_serial = false; // Indica si la placa fue seleccionada por serial

// Variables para el muestreo
TaskHandle_t samplingTaskHandle = NULL;
bool sampling = false;
int samplingInterval = 0; // 0: 5 minutos, 1: 30 minutos, 2: 60 minutos
int readingsCount = 0;

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
void samplingTask(void *parameter);
void startSampling();
void stopSampling();
void displayConsultarMenu();
void returnToMainMenu();
void handleMandarMensaje();
void handleMuestreo();

void setup() {
  Serial.begin(115200);
  Serial.println(F("Iniciando setup..."));

  // Inicializar la pantalla Sharp
  if (!display.begin()) {
    Serial.println(F("No se pudo inicializar la pantalla Sharp."));
    while (1);
  }
  Serial.println(F("Pantalla Sharp inicializada."));

  // Mostrar mensaje inicial en la pantalla
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(BLACK);
  display.setCursor(0, 0);
  display.println(F("Pantalla OK!"));
  display.println(F("Iniciando..."));
  display.refresh();
  Serial.println(F("Mensaje inicial mostrado en la pantalla."));

  // Inicializar WiFi en modo STA
  WiFi.mode(WIFI_STA);
  Serial.println(F("WiFi inicializado en modo STA."));

  // Inicializar ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("Error inicializando ESP-NOW"));
    return;
  }
  Serial.println(F("ESP-NOW inicializado."));

  // Registrar callbacks
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // Configurar el botón
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.println(F("Setup completado."));

  // Mostrar el menú inicial
  showInitialMenu();
}

void loop() {
  // Si el sistema no está inicializado, solo manejar el menú por serial
  if (!system_initialized) {
    serialMenu();
    return; // Salir del loop hasta que se inicialice el sistema
  }

  // Si el sistema está inicializado, manejar el botón y la batería
  handleButton();  // Manejar las acciones del botón

  // Actualizar el porcentaje de batería cada 2 segundos
  if (millis() - lastUpdateTime >= 2000) {
    updateBatteryPercentage();
    lastUpdateTime = millis();
  }

  if (!in_data_screen) {
    if (in_placa_selection) {
      displayMenu(); // Mostrar el menú de selección de placas
    } else if (in_consultar_submenu) {
      displayConsultarMenu(); // Mostrar el submenú de consultar
    } else {
      showInitialMenu(); // Mostrar el menú principal
    }
  } else {
    displayReceivedMessage(); // Mostrar el mensaje recibido en la pantalla de datos
  }

  // Manejar la selección de placas por serial
  if (in_consultar_mode && Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (in_placa_selection) {
      // Seleccionar la placa
      int placa_index = input.toInt() - 1; // Convertir a índice (restar 1)
      if (placa_index >= 0 && placa_index < menu_size) {
        selectPlaca(placa_index);
        placa_selected_by_serial = true; // Marcar que la placa fue seleccionada por serial
        in_placa_selection = false; // Salir de la selección de placas
        in_consultar_submenu = true; // Entrar en el submenú de consultar
        Serial.println(F("Opciones de consultar:"));
        for (int i = 0; i < consultar_menu_size; i++) {
          Serial.print(i + 1);
          Serial.print(": ");
          Serial.println(consultar_menu_items[i]);
        }
      } else {
        Serial.println(F("Número de placa no válido."));
      }
    } else if (in_consultar_submenu) {
      // Seleccionar una opción en el submenú de consultar
      if (input.equals("1")) {
        in_mandar_mensaje_mode = true;
        in_muestreo_mode = false;
        Serial.println(F("Modo 'Mandar Mensaje' activado. Escribe el mensaje que deseas enviar:"));
      } else if (input.equals("2")) {
        in_muestreo_mode = true;
        in_mandar_mensaje_mode = false;
        Serial.println(F("Modo 'Muestreo' activado. Selecciona el intervalo de tiempo:"));
        Serial.println(F("1. 5 minutos"));
        Serial.println(F("2. 30 minutos"));
        Serial.println(F("3. 60 minutos"));
        Serial.println(F("4. Salir"));
      } else if (input.equals("3")) {
        returnToMainMenu();
      } else {
        Serial.println(F("Opción no válida."));
      }
    } else if (in_mandar_mensaje_mode) {
      // Enviar mensaje a la placa
      sendMessageToPlaca(input.c_str());
      Serial.println(F("Mensaje enviado. Escribe otro mensaje o selecciona 'Salir' para volver al menú principal."));
    } else if (in_muestreo_mode) {
      // Configurar el intervalo de muestreo
      if (input.equals("1")) {
        samplingInterval = 5;
        startSampling();
      } else if (input.equals("2")) {
        samplingInterval = 30;
        startSampling();
      } else if (input.equals("3")) {
        samplingInterval = 60;
        startSampling();
      } else if (input.equals("4")) {
        returnToMainMenu();
      } else {
        Serial.println(F("Opción no válida."));
      }
    }
  }
}

// Función para actualizar el porcentaje de batería
void updateBatteryPercentage() {
    adcValue = analogRead(ADC_PIN); // Leer el valor del ADC para la batería
    voltage = (adcValue * 3.3 / 4095.0) * 2; // Calcular el voltaje (ajustar según el divisor de voltaje)

    // Ajustar el porcentaje de batería para que 3.4V sea 0% y 4.1V sea 100%
    porcentaje = (int)((voltage - 3.4) / (4.1 - 3.4) * 100);

    // Limitar el porcentaje a un rango válido (0% - 100%)
    if (porcentaje > 100) porcentaje = 100;
    if (porcentaje < 0) porcentaje = 0;

    //Serial.printf("Porcentaje de batería: %d%%\n", porcentaje);
}

// Manejar las acciones del botón
void handleButton() {
  bool current_button_state = !digitalRead(BUTTON_PIN); // Leer estado del botón (invertido porque es PULLUP)
  unsigned long now = millis();

  // Detectar cambios de estado (borde)
  if (current_button_state != last_button_state) {
    if (current_button_state) { // Botón presionado
      button_down_time = now;
    } else { // Botón liberado
      button_up_time = now;
      unsigned long press_duration = button_up_time - button_down_time;

      if (press_duration > DEBOUNCE_DELAY_MS) {
        if (press_duration < SHORT_PRESS_TIME_MS) {
          is_short_press = true;
        } else if (press_duration < LONG_PRESS_TIME_MS) {
          if (!in_data_screen) {
            in_data_screen = true; // Ingresar a la pantalla de datos
            Serial.println(F("Entrando en pantalla de datos"));
            send_command_to_mac("START", current_menu_index);
          }
        } else {
          if (in_data_screen) {
            in_data_screen = false; // Salir de la pantalla de datos
            Serial.println(F("Saliendo de pantalla de datos"));
            send_command_to_mac("STOP", current_menu_index);
          }
        }
      }
    }
    last_button_state = current_button_state;
  }

  // Manejar la pulsación corta
  if (is_short_press) {
    is_short_press = false; // Restablecer el estado de la pulsación corta
    if (!in_data_screen) {
      if (in_placa_selection) {
        nextMenuItem(); // Cambiar de placa en el menú de selección
      } else if (in_consultar_submenu) {
        current_consultar_menu_index = (current_consultar_menu_index + 1) % consultar_menu_size;
        if (current_consultar_menu_index == consultar_menu_size - 1) {
          // Si se selecciona "Salir", volver al menú principal
          returnToMainMenu();
        } else if (current_consultar_menu_index == 0) {
          // Si se selecciona "Mandar Mensaje", entrar en ese modo
          in_mandar_mensaje_mode = true;
          in_muestreo_mode = false;
          Serial.println(F("Modo 'Mandar Mensaje' activado."));
        } else if (current_consultar_menu_index == 1) {
          // Si se selecciona "Muestreo", entrar en ese modo
          in_muestreo_mode = true;
          in_mandar_mensaje_mode = false;
          Serial.println(F("Modo 'Muestreo' activado."));
        }
        displayConsultarMenu();
      } else if (in_consultar_mode) {
        in_placa_selection = true; // Entrar en la selección de placas
        displayMenu();
      }
    }
  }
}

// Cambiar al siguiente ítem del menú
void nextMenuItem() {
  current_menu_index = (current_menu_index + 1) % menu_size;
  displayMenu();
}

// Mostrar el menú en la pantalla
void displayMenu() {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println(F("Selecciona una placa:"));
  display.println(menu_items[current_menu_index]);
  display.print(F("Batería: "));
  display.print(porcentaje, 0);
  display.println(F(" %"));
  display.refresh();
}

// Mostrar el menú de consultar en la pantalla
void displayConsultarMenu() {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println(F("Consultar:"));
  display.println(consultar_menu_items[current_consultar_menu_index]);
  display.print(F("Batería: "));
  display.print(porcentaje, 0);
  display.println(F(" %"));
  display.refresh();
}

// Mostrar el mensaje recibido en la pantalla
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

// Enviar comando a la MAC seleccionada
void send_command_to_mac(const char *command, int menu_index) {
  const uint8_t *mac = mac_addresses[menu_index];
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo)); // Limpiar la estructura
  memcpy(peerInfo.peer_addr, mac, 6); // Copiar la dirección MAC
  peerInfo.channel = 0; // Usar el canal 0
  peerInfo.encrypt = false; // Sin encriptación

  // Agregar el peer
  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result == ESP_OK) {
    Serial.println(F("Peer agregado correctamente."));
    // Enviar el comando
    result = esp_now_send(mac, (uint8_t *)command, strlen(command));
    if (result == ESP_OK) {
      Serial.println(F("Comando enviado correctamente."));
    } else {
      Serial.println(F("Error al enviar el comando."));
    }
    // Eliminar el peer después de enviar el comando
    esp_now_del_peer(mac);
  } else {
    Serial.println(F("Error al agregar el peer."));
  }
}

// Callback cuando los datos son enviados
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print(F("Mensaje enviado a: "));
  for (int i = 0; i < 6; i++) {
    Serial.print(mac_addr[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? " Éxito" : " Fallo");
}

// Callback cuando los datos son recibidos
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

  // Copiar el mensaje recibido al buffer
  memset(received_message, 0, sizeof(received_message)); // Limpiar el buffer
  memcpy(received_message, incomingData, len); // Copiar los datos recibidos

  // Si estamos en la pantalla de datos, actualizar la pantalla
  if (in_data_screen) {
    displayReceivedMessage();
  }
}

// Función para manejar el menú por serial
void serialMenu() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.equalsIgnoreCase("consultar")) {
      Serial.println(F("Opción 'consultar' seleccionada."));
      system_initialized = true; // Habilitar el sistema
      in_consultar_mode = true; // Entrar en modo "consultar"
      in_placa_selection = true; // Mostrar el menú de selección de placas
      displayMenu();
      Serial.println(F("Placas disponibles:"));
      for (int i = 0; i < menu_size; i++) {
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.println(menu_items[i]);
      }
    } else if (input.equalsIgnoreCase("añadir")) {
      Serial.println(F("Opción 'añadir' seleccionada."));
      system_initialized = true; // Habilitar el sistema
      in_consultar_mode = false; // No estamos en modo "consultar"
      Serial.println(F("Modo 'añadir' activado."));
    } else {
      Serial.println(F("Opción no válida. Por favor, elige 'consultar' o 'añadir'."));
    }
  }
}

// Mostrar el menú inicial
void showInitialMenu() {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println(F("Menú Principal:"));
  display.println(F("1. Consultar"));
  display.println(F("2. Añadir"));
  display.refresh();
  Serial.println(F("Selecciona una opción:"));
  Serial.println(F("1. Consultar"));
  Serial.println(F("2. Añadir"));
}

// Seleccionar una placa por serial
void selectPlaca(int placa_index) {
  current_menu_index = placa_index;
  Serial.print(F("Placa seleccionada: "));
  Serial.println(menu_items[placa_index]);
  displayMenu(); // Actualizar la pantalla con la placa seleccionada
}

// Enviar mensaje a la placa seleccionada
void sendMessageToPlaca(const char *message) {
  send_command_to_mac(message, current_menu_index);
  Serial.print(F("Mensaje enviado a "));
  Serial.print(menu_items[current_menu_index]);
  Serial.print(F(": "));
  Serial.println(message);
}

// Tarea de muestreo en el segundo core
void samplingTask(void *parameter) {
  while (true) {
    if (sampling) {
      send_command_to_mac("START", current_menu_index);
      delay(1000); // Esperar a que la placa responda

      for (int i = 0; i < 3; i++) {
        delay(samplingInterval * 60000); // Esperar el tiempo configurado
        send_command_to_mac("START", current_menu_index);
        delay(1000); // Esperar a que la placa responda
      }

      send_command_to_mac("STOP", current_menu_index);
      sampling = false;
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Esperar 1 segundo
  }
}

// Iniciar el muestreo
void startSampling() {
  if (!sampling) {
    sampling = true;
    xTaskCreatePinnedToCore(samplingTask, "SamplingTask", 10000, NULL, 1, &samplingTaskHandle, 1);
    Serial.println(F("Muestreo iniciado."));
  }
}

// Detener el muestreo
void stopSampling() {
  if (sampling) {
    sampling = false;
    vTaskDelete(samplingTaskHandle);
    Serial.println(F("Muestreo detenido."));
  }
}

// Volver al menú principal
void returnToMainMenu() {
  in_consultar_submenu = false;
  in_placa_selection = false;
  in_consultar_mode = false;
  in_mandar_mensaje_mode = false;
  in_muestreo_mode = false;
  showInitialMenu();
}

// Manejar el modo "Mandar Mensaje"
void handleMandarMensaje() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    sendMessageToPlaca(input.c_str());
    Serial.println(F("Mensaje enviado. Escribe otro mensaje o selecciona 'Salir' para volver al menú principal."));
  }
}

// Manejar el modo "Muestreo"
void handleMuestreo() {
  if (!sampling) {
    Serial.println(F("Selecciona el intervalo de muestreo:"));
    Serial.println(F("1. 5 minutos"));
    Serial.println(F("2. 30 minutos"));
    Serial.println(F("3. 60 minutos"));
    Serial.println(F("4. Salir"));

    while (!Serial.available()); // Esperar a que el usuario ingrese una opción
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.equals("1")) {
      samplingInterval = 5;
      startSampling();
    } else if (input.equals("2")) {
      samplingInterval = 30;
      startSampling();
    } else if (input.equals("3")) {
      samplingInterval = 60;
      startSampling();
    } else if (input.equals("4")) {
      returnToMainMenu();
    } else {
      Serial.println(F("Opción no válida."));
    }
  }
}