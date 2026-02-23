#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>  // Librería para configuración WiFi sin recompilar
#include <ArduinoJson.h>
#include <AudioOutputI2S.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioFileSource.h>
#include <time.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <DHT.h>

// Configuracion WiFi 
const char* ssid = "WIFI_SSID";
const char* password = "WIFI_PASSWORD";

// OpenAI API Key 
const char* openai_api_key = "OPENAI_API_KEY";

// Pines I2S (ESP32 → PCM5102A)
#define I2S_BCK  26
#define I2S_WS   25
#define I2S_DO   22

// Pines de sensores 
#define HUMIDITY_PIN 34          // Sensor capacitivo de humedad (ADC)

// Calibración sensor de humedad del suelo
const int SOIL_RAW_SECO = 0;       // lectura en aire (seco)
const int SOIL_RAW_MOJADO = 2319;  // lectura en tierra mojada 

// Pin del boton tactil
#define TOUCH_BUTTON_PIN 32

// Pin del servomotor (saludo)
#define SERVO_PIN 18

// Sensor DHT11 (solo temperatura)
#define DHT_PIN 27
#define DHTTYPE DHT11

// Variables globales
WebServer server(80);
AudioOutputI2S* out = nullptr;
AudioGeneratorMP3* mp3 = nullptr;
AudioFileSourceHTTPStream* stream = nullptr;

// Clase personalizada para leer audio desde memoria
class AudioFileSourceMemory : public AudioFileSource {
public:
  AudioFileSourceMemory(uint8_t *data, uint32_t len) {
    buffer = data;
    length = len;
    pos = 0;
  }
  
  virtual uint32_t read(void *data, uint32_t len) override {
    if (pos >= length) return 0;
    uint32_t toRead = (length - pos < len) ? (length - pos) : len;
    memcpy(data, buffer + pos, toRead);
    pos += toRead;
    return toRead;
  }
  
  virtual bool seek(int32_t pos, int dir) override {
    if (dir == SEEK_SET) {
      this->pos = pos;
    } else if (dir == SEEK_CUR) {
      this->pos += pos;
    } else if (dir == SEEK_END) {
      this->pos = length + pos;
    }
    if (this->pos > length) this->pos = length;
    if (this->pos < 0) this->pos = 0;
    return true;
  }
  
  virtual bool close() override {
    return true;
  }
  
  virtual bool isOpen() override {
    return buffer != nullptr;
  }
  
  virtual uint32_t getSize() override {
    return length;
  }
  
  virtual uint32_t getPos() override {
    return pos;
  }
  
private:
  uint8_t *buffer;
  uint32_t length;
  uint32_t pos;
};

AudioFileSourceMemory* memoryStream = nullptr;
uint8_t* audioBuffer = nullptr;
size_t audioBufferSize = 0;
Preferences preferences;
Servo servo;
DHT dht(DHT_PIN, DHTTYPE);

// Variables de sensores
float humidity = 0.0;
float temperature = 0.0;
unsigned long lastSensorUpdate = 0;

// Bandera para deshabilitar TTS si falla 
bool ttsDisabled = false;

// Intervalos de tiempo (en milisegundos)
const unsigned long INTERVALO_SENSORES = 10000;   // 10 segundos - actualizar sensores
const unsigned long INTERVALO_ALERTAS = 1800000;   // 30 minutos - alertas de voz normales
const unsigned long INTERVALO_CRITICO = 300000;   // 5 minutos - alertas críticas

// Configuracion de planta y horarios
String plantType = "lavanda";
int horaDiaInicio = 7;
int horaDiaFin = 19;

// Rangos optimos de cada planta (se actualizan segun el tipo)
struct PlantRanges {
  int humMin, humMax;
  int tempMin, tempMax;
};

// Rangos predefinidos por tipo de planta
PlantRanges plantRanges = {30, 40, 15, 26}; // Valores por defecto (lavanda)

// Banderas para alertas de "una sola vez" (problemas dificiles de resolver)
bool alertaHumedadAltaDada = false;

// Personalidad de la planta (3 palabras)
String personalidad1 = "sarcastica";
String personalidad2 = "graciosa";
String personalidad3 = "dramatica";

// Variables del boton tactil
volatile bool botonPresionado = false;
unsigned long ultimaInteraccion = 0;
const unsigned long DEBOUNCE_TIEMPO = 150; // anti-rebote un poco mayor para evitar falsos largos
volatile uint8_t botonClickCount = 0;     // contador de clics detectados
volatile unsigned long botonUltimoClickMs = 0; // tiempo del último clic
const unsigned long VENTANA_DOBLE_CLIC = 400;  // ventana para considerar doble clic
const unsigned long LONG_PRESS_MS = 1500;      // duración para pulsación larga
bool botonLongPressArmed = false;
bool botonLongHandled = false;
unsigned long botonPressStartMs = 0;
// Estado para detección por flancos
bool botonLastPressed = false;
unsigned long botonLastChangeMs = 0;
// Ignorar eventos durante arranque y detectar nivel bajo sostenido
unsigned long bootIgnoreUntilMs = 0;
unsigned long buttonLowSinceMs = 0;
// Estabilidad del botón
unsigned long buttonHighSinceMs = 0;
unsigned long buttonLowStableSinceMs = 0;
// Habilitar/Deshabilitar pulsación larga con generacion de respuesta
const bool ENABLE_LONG_PRESS = false;

// Declaraciones forward de funciones
void updateSensors();
void checkPlantStatus();
bool esDia();
void updatePlantRanges();
void loadConfiguration();
void saveConfiguration();
String getChatGPTResponse(String prompt);
void handleButtonPress();
void IRAM_ATTR onButtonPress();
void waveServoGreeting();

String isoNow() {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  char buf[25];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(buf);
}

void setup() {
  Serial.begin(115200); // Mayor velocidad para mejor visualización
  Serial.println("Iniciando Planta que Habla...");
  
  // Configurar boton tactil con interrupcion
  pinMode(TOUCH_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TOUCH_BUTTON_PIN), onButtonPress, FALLING);
  // Inicializar estado del botón y ventana de ignorar tras arranque
  botonLastPressed = (digitalRead(TOUCH_BUTTON_PIN) == LOW);
  unsigned long nowMs = millis();
  bootIgnoreUntilMs = nowMs + 3000; // ignorar 3s tras arranque
  if (botonLastPressed) {
    buttonLowSinceMs = nowMs;
    buttonLowStableSinceMs = nowMs;
  } else {
    buttonHighSinceMs = nowMs;
  }
  
  // Cargar configuracion guardada desde memoria persistente
  loadConfiguration();
  
  // Configurar WiFi
  setupWiFi();
  
  // Configurar I2S y TTS
  setupAudio();
  
  // Configurar sensores
  setupSensors();
  
  // Configurar servo
  servo.attach(SERVO_PIN);
  servo.write(90); // posicion neutra
  
  // Configurar rutas del servidor
  setupRoutes();
  
  server.begin();
  Serial.println("Servidor web iniciado!");
  Serial.println("IP local: " + WiFi.localIP().toString());
  
}

void loop() {
  // Manejar peticiones del servidor web (prioridad alta)
  server.handleClient();
  
  // Ceder tiempo al scheduler para evitar WDT
  yield();
  
  // Mensaje de bienvenida retrasado (solo una vez al inicio)
  static bool welcomeMessageSent = false;
  static unsigned long welcomeDelay = 0;
  if (!welcomeMessageSent && WiFi.status() == WL_CONNECTED) {
    if (welcomeDelay == 0) {
      welcomeDelay = millis() + 5000; // Esperar 5 segundos después del inicio
    } else if (millis() > welcomeDelay) {
      // Solo enviar mensaje si no hay peticiones activas
      speakGoogleTTS("Estoy conectado a internet!");
      delay(1200);
      String ipMsg = String("IP local ") + WiFi.localIP().toString();
      speakGoogleTTS(ipMsg);
      welcomeMessageSent = true;
    }
  }
  
  // Manejar interaccion por boton tactil
  if (botonPresionado) {
    botonPresionado = false;
    // contamos el clic y actualizamos timestamp
    botonClickCount++;
    botonUltimoClickMs = millis();
  }
  // Lectura del estado del botón para detectar pulsación larga
  bool pressedNow = (digitalRead(TOUCH_BUTTON_PIN) == LOW);
  unsigned long nowMs = millis();

  // Actualizar estabilidad
  if (pressedNow) {
    if (buttonLowSinceMs == 0) buttonLowSinceMs = nowMs;
    if (buttonLowStableSinceMs == 0) buttonLowStableSinceMs = nowMs;
    buttonHighSinceMs = 0;
  } else {
    if (buttonHighSinceMs == 0) buttonHighSinceMs = nowMs;
    buttonLowSinceMs = 0;
    buttonLowStableSinceMs = 0;
  }

  // Detección por flancos (edge detection)
  if (pressedNow != botonLastPressed) {
    botonLastPressed = pressedNow;
    botonLastChangeMs = nowMs;
    if (pressedNow) {
      // Flanco de bajada (inicio de pulsación)
      // Armar pulsación larga SOLO si está habilitada, sin clics previos y fuera de ventana de arranque
      if (ENABLE_LONG_PRESS && (botonClickCount == 0) && (nowMs > bootIgnoreUntilMs)) {
        botonLongPressArmed = true;
        botonPressStartMs = nowMs;
        botonLongHandled = false;
      } else {
        // No armar larga; permitir clic simple/doble
        botonLongPressArmed = false;
        botonLongHandled = false;
      }
    } else {
      // Flanco de subida (fin de pulsación)
      botonLongPressArmed = false;
      botonPressStartMs = 0;
      botonLongHandled = false;
    }
  }

  // Detectar pulsación larga
  if (ENABLE_LONG_PRESS && pressedNow && botonLongPressArmed && !botonLongHandled && botonClickCount == 0 && nowMs > bootIgnoreUntilMs) {
    unsigned long heldMs = nowMs - botonPressStartMs;
    if ((heldMs >= LONG_PRESS_MS) && (buttonLowSinceMs != 0) && (nowMs - buttonLowSinceMs >= 80)) {
      botonLongHandled = true;
      handleButtonPress();
    }
  }

  // Si el pin permanece LOW mucho tiempo, considerarlo atascado y desactivar larga
  if (pressedNow) {
    if (buttonLowSinceMs == 0) buttonLowSinceMs = millis();
    else if (millis() - buttonLowSinceMs > 10000) { // 10s LOW continuo
      botonLongPressArmed = false;
      botonLongHandled = true;
    }
  } else {
    buttonLowSinceMs = 0;
  }

  // Si pasó la ventana sin nuevos clics, decidir acción
  if (botonClickCount > 0 && (millis() - botonUltimoClickMs) > VENTANA_DOBLE_CLIC) {
    uint8_t clicks = botonClickCount;
    botonClickCount = 0;
    if (clicks >= 2) {
      speakGoogleTTS("Hola");
      waveServoGreeting();
    } else {
      handleButtonPress();
    }
  }
  
  // Actualizar sensores cada 10 segundos (para web responsive)
  if (millis() - lastSensorUpdate > INTERVALO_SENSORES) {
    updateSensors();
    lastSensorUpdate = millis();
  }
  
  // Verificar alertas críticas cada 5 minutos
  static unsigned long lastCriticalCheck = 0;
  if (millis() - lastCriticalCheck > INTERVALO_CRITICO) {
    // Calcular umbrales críticos para verificar
    int humedadCritica = plantRanges.humMin * 0.6; // 40% menos del mínimo
    int temperaturaCritica = 35; // Temperatura crítica universal
    
    // Solo revisar condiciones críticas
    if (humidity < humedadCritica || temperature > temperaturaCritica) {
      checkPlantStatus();
      yield();
    }
    lastCriticalCheck = millis();
  }
  
  // Verificar estado de la planta cada 30 minutos (alertas normales)
  static unsigned long lastPlantCheck = 0;
  if (millis() - lastPlantCheck > INTERVALO_ALERTAS) {
    checkPlantStatus();
    lastPlantCheck = millis();
  }
}

void setupWiFi() {
  // WiFiManager - Configuración automática de WiFi sin recompilar
  WiFiManager wifiManager;
  
  
  // Timeout del portal de configuración (3 minutos)
  wifiManager.setConfigPortalTimeout(180);
  
  // Personalizar el portal
  wifiManager.setAPStaticIPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  
  Serial.println("=======================================");
  Serial.println("PlantTalk - Configuración WiFi");
  Serial.println("=======================================");
  
  // Intentar conectarse a WiFi guardado, si falla crea AP "PlantTalk-Setup"
  if (!wifiManager.autoConnect("PlantTalk-Setup", "planttalk123")) {
    Serial.println("Fallo al conectar WiFi y timeout alcanzado");
    Serial.println("Reiniciando ESP32...");
    delay(3000);
    ESP.restart();
  }
  
  // Si llegamos aquí, estamos conectados!
  Serial.println("¡Conectado a WiFi!");
  Serial.println("SSID: " + WiFi.SSID());
  Serial.println("IP local: " + WiFi.localIP().toString());
  Serial.println("Accede a: http://" + WiFi.localIP().toString());
  Serial.println("===========================================");

  // Sincronizar hora con NTP 
  configTime(-8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Sincronizando hora NTP");
  struct tm timeinfo;
  int retries = 0;
  while (!getLocalTime(&timeinfo) && retries < 20) {
    Serial.print(".");
    delay(500);
    retries++;
  }
  if (retries < 20) {
    Serial.println("\nHora sincronizada: " + String(asctime(&timeinfo)));
  } else {
    Serial.println("\nNo se pudo sincronizar hora (continuando con fallback)");
  }
}

void setupAudio() {
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCK, I2S_WS, I2S_DO);
  out->SetChannels(1); // Google TTS es mono
  out->SetRate(24000);
  out->SetGain(1.0f);
  
  Serial.println("Audio I2S configurado para Google TTS");
}

// Función para hablar con Google Translate TTS
void speakGoogleTTS(const String& text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TTS] WiFi no conectado, no se puede usar TTS");
    return;
  }
  
  if (text.length() == 0) {
    Serial.println("[TTS] Texto vacío, no se puede hablar");
    return;
  }
  
  Serial.println("[TTS] Iniciando TTS para: " + text);
  
  // URL encode del texto
  String encodedText = "";
  for (size_t i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    if (c == ' ') {
      encodedText += '+';
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encodedText += c;
    } else {
      encodedText += '%';
      encodedText += String((c >> 4) & 0xF, HEX);
      encodedText += String(c & 0xF, HEX);
    }
  }
  
  // URL de Google Translate TTS
  String url = "https://translate.google.com/translate_tts?ie=UTF-8&tl=es-MX&client=tw-ob&q=" + encodedText;
  
  Serial.println("[TTS] Descargando audio desde: " + url.substring(0, 80) + "...");
  
  // Limpiar recursos previos
  if (mp3) {
    if (mp3->isRunning()) {
      mp3->stop();
    }
    delete mp3;
    mp3 = nullptr;
  }
  if (memoryStream) {
    delete memoryStream;
    memoryStream = nullptr;
  }
  if (audioBuffer) {
    free(audioBuffer);
    audioBuffer = nullptr;
    audioBufferSize = 0;
  }
  
  // Descargar audio usando HTTPClient
  HTTPClient http;
  http.begin(url);
  http.setTimeout(10000);
  http.addHeader("User-Agent", "Mozilla/5.0");
  
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    WiFiClient* stream = http.getStreamPtr();
    
    // Leer audio en chunks sin conocer el tamaño previo
    const size_t CHUNK_SIZE = 1024;
    const size_t MAX_SIZE = 50000; // Limitar a 50KB máximo
    size_t bufferSize = CHUNK_SIZE;
    size_t bytesRead = 0;
    
    audioBuffer = (uint8_t*)malloc(bufferSize);
    if (!audioBuffer) {
      Serial.println("[TTS] ERROR: No se pudo asignar memoria inicial");
      http.end();
      return;
    }
    
    Serial.println("[TTS] Descargando audio (tamaño desconocido)...");
    unsigned long startTime = millis();
    bool downloadComplete = false;
    
    while ((millis() - startTime < 15000) && bytesRead < MAX_SIZE) {
      // Permitir que el servidor web responda durante la descarga
      server.handleClient();
      yield();
      
      if (stream->available()) {
        // Si necesitamos más espacio, redimensionar
        if (bytesRead + CHUNK_SIZE > bufferSize) {
          if (bufferSize * 2 > MAX_SIZE) {
            bufferSize = MAX_SIZE;
          } else {
            bufferSize *= 2;
          }
          
          uint8_t* newBuffer = (uint8_t*)realloc(audioBuffer, bufferSize);
          if (!newBuffer) {
            Serial.println("[TTS] ERROR: No se pudo redimensionar buffer");
            free(audioBuffer);
            audioBuffer = nullptr;
            http.end();
            return;
          }
          audioBuffer = newBuffer;
        }
        
        // Leer chunk
        size_t toRead = (bufferSize - bytesRead < CHUNK_SIZE) ? (bufferSize - bytesRead) : CHUNK_SIZE;
        int bytes = stream->readBytes(audioBuffer + bytesRead, toRead);
        
        if (bytes > 0) {
          bytesRead += bytes;
        } else {
          // No hay más datos disponibles, esperar 
          delay(50);
          server.handleClient();
          if (!stream->available()) {
            downloadComplete = true;
            break;
          }
        }
      } else {
        delay(10);
        server.handleClient();
        // Si no hay datos disponibles por un tiempo, considerar descarga completa
        if (millis() - startTime > 5000 && bytesRead > 0) {
          downloadComplete = true;
          break;
        }
      }
    }
    
    http.end();
    
    if (bytesRead > 0) {
      Serial.println("[TTS] Audio descargado: " + String(bytesRead) + " bytes");
      audioBufferSize = bytesRead;
      
      // Crear stream desde memoria
      memoryStream = new AudioFileSourceMemory(audioBuffer, audioBufferSize);
      mp3 = new AudioGeneratorMP3();
      
      if (memoryStream && mp3) {
        Serial.println("[TTS] Reproduciendo audio...");
        
        if (mp3->begin(memoryStream, out)) {
          unsigned long playStartTime = millis();
          const unsigned long MAX_PLAY_TIME = 30000;
          
          while (mp3->isRunning() && (millis() - playStartTime < MAX_PLAY_TIME)) {
            // Permitir que el servidor web responda durante la reproducción
            server.handleClient();
            if (!mp3->loop()) {
              mp3->stop();
              break;
            }
            yield();
          }
          
          Serial.println("[TTS] Reproducción completada");
        } else {
          Serial.println("[TTS] ERROR: No se pudo iniciar MP3");
        }
      } else {
        Serial.println("[TTS] ERROR: No se pudieron crear objetos de audio");
      }
      
      // Limpiar
      if (mp3) {
        delete mp3;
        mp3 = nullptr;
      }
      if (memoryStream) {
        delete memoryStream;
        memoryStream = nullptr;
      }
    } else {
      Serial.println("[TTS] ERROR: No se pudo descargar audio (0 bytes)");
      if (audioBuffer) {
        free(audioBuffer);
        audioBuffer = nullptr;
      }
    }
  } else {
    Serial.println("[TTS] ERROR: HTTP " + String(httpCode));
    http.end();
  }
  
  // Limpiar buffer al final
  if (audioBuffer) {
    free(audioBuffer);
    audioBuffer = nullptr;
    audioBufferSize = 0;
  }
  
  delay(100);
}

void setupSensors() {
  // Configurar ADC en modo legacy 
  analogSetAttenuation(ADC_11db);  // Rango 0-3.3V
  
  // Configurar pin ADC para humedad de suelo
  pinMode(HUMIDITY_PIN, INPUT);
  
  // Inicializar DHT11
  dht.begin();
  
  Serial.println("Sensores inicializados:");
  Serial.println("  - Sensor capacitivo (Humedad) en pin 34");
}

void setupRoutes() {
  Serial.println("[WEB] Configurando rutas del servidor...");
  
  // Pagina principal
  server.on("/", handleRoot);
  Serial.println("[WEB] Ruta configurada: /");
  
  // API de sensores
  server.on("/api/sensors", handleSensors);
  Serial.println("[WEB] Ruta configurada: /api/sensors");
  
  server.on("/api/plant-config", HTTP_POST, handlePlantConfig);
  server.on("/api/plant-config", HTTP_GET, handleGetPlantConfig);
  Serial.println("[WEB] Ruta configurada: /api/plant-config (GET y POST)");
  
  server.on("/api/personality", HTTP_POST, handlePersonality);
  server.on("/api/personality", HTTP_GET, handleGetPersonality);
  Serial.println("[WEB] Ruta configurada: /api/personality (GET y POST)");
  
  // Archivos estaticos
  server.on("/style.css", handleCSS);
  server.on("/script.js", handleJS);
  Serial.println("[WEB] Rutas estáticas configuradas: /style.css, /script.js");
  
  // Resetear WiFi
  server.on("/api/reset-wifi", HTTP_POST, handleResetWiFi);
  Serial.println("[WEB] Ruta configurada: /api/reset-wifi");
  
  // Test de audio (simular boton)
  server.on("/api/test-audio", HTTP_POST, handleTestAudio);
  Serial.println("[WEB] Ruta configurada: /api/test-audio");
  
  Serial.println("[WEB] Todas las rutas configuradas correctamente");
}

void handleRoot() {
  // Construccion del HTML
  String html;
  html.reserve(3000);
  
  html = "<!DOCTYPE html>"
                "<html lang=\"es\">"
                "<head>"
                  "<meta charset=\"UTF-8\">"
                  "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
                  "<title>PlantTalk - Planta Inteligente que Habla</title>"
                  "<link rel=\"stylesheet\" href=\"/style.css\">"
                  "<link rel=\"stylesheet\" href=\"https://fonts.googleapis.com/icon?family=Material+Icons\">"
                "</head>"
                "<body>"
                  "<header>"
                    "<div class=\"header-content\">"
                      "<div class=\"logo\">"
                        "<span class=\"leaf-icon\">🌿</span>"
                        "<span class=\"logo-text\">PlantTalk</span>"
                      "</div>"
                      "<nav>"
                        "<a href=\"/\" class=\"nav-link active\">Monitor de Planta</a>"
                      "</nav>"
                    "</div>"
                  "</header>"
                  "<main>"
                    "<div class=\"main-container\">"
                      "<div class=\"plant-health-section\">"
                        "<div class=\"section-header\">"
                          "<span class=\"section-icon\">🌱</span>"
                          "<h2>Monitor de Salud de Planta</h2>"
                        "</div>"
                        "<div class=\"monitor-grid\">"
                          "<div class=\"status-card\">"
                            "<div class=\"card-header\">"
                              "<h3>Estado de la Planta</h3>"
                            "</div>"
                            "<div class=\"status-content\">"
                              "<span class=\"material-icons status-icon\" id=\"statusIcon\">sentiment_satisfied</span>"
                              "<div class=\"plant-name\" id=\"plantName\">Planta Actual: —</div>"
                            "</div>"
                          "</div>"
                          "<div class=\"temp-card\">"
                          "  <div class=\"card-header\">"
                          "    <span class=\"card-icon\"></span>"
                          "    <h3>Temperatura</h3>"
                          "  </div>"
                          "  <div class=\"gauge-container\">"
                          "    <div class=\"gauge\">"
                          "      <div class=\"gauge-fill\" id=\"tempGauge\"></div>"
                          "      <div class=\"gauge-value\" id=\"temp\">25.0 °C</div>"
                          "    </div>"
                          "    <div class=\"gauge-labels\">"
                          "      <span id=\"tempMin\">0</span>"
                          "      <span id=\"tempMax\">40</span>"
                          "    </div>"
                          "  </div>"
                          "</div>"
                          "<div class=\"humidity-card\">"
                            "<div class=\"card-header\">"
                              "<span class=\"card-icon\"></span>"
                              "<h3>Nivel de Humedad</h3>"
                            "</div>"
                            "<div class=\"gauge-container\">"
                              "<div class=\"gauge\">"
                                "<div class=\"gauge-fill\" id=\"humidityGauge\"></div>"
                                "<div class=\"gauge-value\" id=\"humidity\">65%</div>"
                              "</div>"
                              "<div class=\"gauge-labels\">"
                                "<span>0</span>"
                                "<span>100</span>"
                              "</div>"
                            "</div>"
                          "</div>"
                          "<div class=\"config-card\">"
                            "<div class=\"card-header\">"
                              "<span class=\"card-icon\"></span>"
                              "<h3>Configuración de Planta</h3>"
                            "</div>"
                            "<div class=\"config-content\">"
                              "<div class=\"config-item\">"
                              "<label>Tipo de Planta</label>"
                              "<select id=\"plantType\">"
                                  "<option value=\"lavanda\">Lavanda</option>"
                                  "<option value=\"cardo\">Cardo</option>"
                                  "<option value=\"trebol\">Trébol</option>"
                                  "<option value=\"aeonium\">Aeonium haworthii</option>"
                                  "<option value=\"custom\">Personalizada</option>"
                                "</select>"
                              "</div>"
                              "<div class=\"custom-config\" id=\"customConfig\" style=\"display:none; margin-top:1rem;\">"
                                "<div class=\"config-item\">"
                                  "<label>Humedad Óptima (%)</label>"
                                  "<input type=\"number\" id=\"customMinH\" placeholder=\"Mín\" min=\"0\" max=\"100\" style=\"width:80px;\"> - <input type=\"number\" id=\"customMaxH\" placeholder=\"Máx\" min=\"0\" max=\"100\" style=\"width:80px;\">"
                                "</div>"
                                "<div class=\"config-item\">"
                                  "<label>Temperatura Óptima (°C)</label>"
                                  "<input type=\"number\" id=\"customMinT\" placeholder=\"Mín\" min=\"0\" max=\"50\" style=\"width:80px;\"> - <input type=\"number\" id=\"customMaxT\" placeholder=\"Máx\" min=\"0\" max=\"50\" style=\"width:80px;\">"
                              "</div>"
                              "</div>"
                              "<div style=\"margin-top:0.8rem;\">"
                              "  <button class=\"btn btn-primary\" id=\"saveDayHours\" style=\"padding:0.5rem 1.2rem; width:100%;\">Guardar</button>"
                              "</div>"
                              "<div id=\"configHoras\" class=\"config-item\" style=\"margin-top:1.4rem\">"
                                "<label style=\"font-weight:800;margin-bottom:0.2rem;\">Configuración horario día</label>"
                                "<div style=\"display:flex; gap:1.2rem; align-items:center;\">"
                                  "<span>Inicio: <input type=\"number\" min=\"0\" max=\"23\" id=\"horaDiaIni\" style=\"width:60px;\" value=\"7\">:00</span>"
                                  "<span>Fin: <input type=\"number\" min=\"0\" max=\"23\" id=\"horaDiaFin\" style=\"width:60px;\" value=\"19\">:00</span>"
                                "</div>"
                              "</div>"
                              "<div class=\"care-schedule\">"
                              "<div class=\"schedule-item\">"
                                "<span>Necesidad de Humedad:</span>"
                                "<span class=\"schedule-value\" id=\"infoHumedad\">—</span>"
                              "</div>"
                              "<div class=\"schedule-item\">"
                                "<span>Rango de Temperatura:</span>"
                                "<span class=\"schedule-value\" id=\"infoTemp\">—</span>"
                              "</div>"
                              "</div>"
                            "</div>"
                          "</div>"
                          "<div class=\"personality-card\">"
                            "<div class=\"card-header\">"
                              "<span class=\"card-icon\"></span>"
                              "<h3>Personalidad de la Planta</h3>"
                            "</div>"
                            "<div class=\"personality-content\">"
                              "<p style=\"font-size:0.9rem;color:var(--muted);margin-bottom:1rem;\">Define cómo responderá tu planta con 3 palabras (ej: sarcástica, graciosa, dramática)</p>"
                              "<div class=\"config-item\">"
                                "<label>Palabra 1</label>"
                                "<input type=\"text\" id=\"personality1\" placeholder=\"Ej: sarcástica\" maxlength=\"20\" value=\"sarcastica\">"
                              "</div>"
                              "<div class=\"config-item\">"
                                "<label>Palabra 2</label>"
                                "<input type=\"text\" id=\"personality2\" placeholder=\"Ej: graciosa\" maxlength=\"20\" value=\"graciosa\">"
                              "</div>"
                              "<div class=\"config-item\">"
                                "<label>Palabra 3</label>"
                                "<input type=\"text\" id=\"personality3\" placeholder=\"Ej: dramática\" maxlength=\"20\" value=\"dramatica\">"
                              "</div>"
                              "<button class=\"btn btn-primary\" id=\"savePersonality\" style=\"width:100%;margin-top:1rem;\">Guardar Personalidad</button>"
                            "</div>"
                          "</div>"
                          "<div class=\"dev-tools-card\">"
                            "<div class=\"card-header\">"
                              "<h3>Herramientas de Desarrollo</h3>"
                            "</div>"
                            "<div class=\"tools-content\">"
                              "<button class=\"tool-btn audio-btn\" onclick=\"testAudio()\">Probar Alerta de Audio</button>"
                              "<button class=\"tool-btn\" style=\"background:#FF6B6B;color:#fff\" onclick=\"resetWiFi()\">📡 Cambiar WiFi</button>"
                            "</div>"
                          "</div>"
                        "</div>"
                      "</div>"
                    "</div>"
                  "</main>"
                  "<footer>"
                    "<p>© 2024 PlantTalk. Manteniendo tus plantas felices y saludables.</p>"
                  "</footer>"
                 "<script src=\"/script.js\"></script>"
                 "<script>"
                   "window.addEventListener('load', function(){"
                     "try{ if(typeof loadConfigUI==='function'){loadConfigUI();} }catch(e){ console.log('loadConfigUI FAIL', e);}"
                     "try{ if(typeof startSensorUpdates==='function'){startSensorUpdates();} }catch(e){ console.log('startSensorUpdates FAIL', e);}"
                     "var u='/api/sensors';"
                     "fetch(u).then(function(r){return r.json()}).then(function(d){console.log('Ping /api/sensors OK', d)}).catch(function(e){console.log('Ping /api/sensors FAIL', e)});"
                   "});"
                 "</script>"
                "</body>"
                "</html>";
  
  server.handleClient();
  yield();
  server.send(200, "text/html", html);
}

void handleSensors() {
  
  // Asegurar que el servidor pueda manejar otras peticiones
  server.handleClient();
  yield();
  
  DynamicJsonDocument doc(1024);
  doc["humidity"] = humidity;
  doc["temperature"] = temperature;
  doc["timestamp"] = millis();
  
  String response;
  serializeJson(doc, response);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(200, "application/json", response);
}

void handlePlantConfig() {
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, server.arg("plain"));
    
    if (doc.containsKey("plantType")) {
      plantType = doc["plantType"].as<String>();
      updatePlantRanges(); // Actualizar rangos segun el tipo
    }
    if (doc.containsKey("horaDiaInicio")) {
      horaDiaInicio = doc["horaDiaInicio"].as<int>();
    }
    if (doc.containsKey("horaDiaFin")) {
      horaDiaFin = doc["horaDiaFin"].as<int>();
    }
    
    // Si es una planta personalizada, recibir los rangos
    if (plantType == "custom") {
      if (doc.containsKey("humMin")) plantRanges.humMin = doc["humMin"].as<int>();
      if (doc.containsKey("humMax")) plantRanges.humMax = doc["humMax"].as<int>();
      if (doc.containsKey("tempMin")) plantRanges.tempMin = doc["tempMin"].as<int>();
      if (doc.containsKey("tempMax")) plantRanges.tempMax = doc["tempMax"].as<int>();
    }
    
    // Guardar configuracion en memoria persistente
    saveConfiguration();
    
    server.send(200, "application/json", "{\"status\":\"success\"}");
  }
}

void handleGetPlantConfig() {
  
  DynamicJsonDocument doc(512);
  doc["plantType"] = plantType;
  doc["horaDiaInicio"] = horaDiaInicio;
  doc["horaDiaFin"] = horaDiaFin;
  if (plantType == "custom") {
    doc["humMin"] = plantRanges.humMin;
    doc["humMax"] = plantRanges.humMax;
    doc["tempMin"] = plantRanges.tempMin;
    doc["tempMax"] = plantRanges.tempMax;
  }
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

void handleCSS() {
  String css = ":root{--bg:#EFF2E4;--card:#FFF;--text:#22401E;--muted:rgba(34,64,30,.65);--primary:#5A7326;--primary-strong:#22401E;--accent:#C0D9A7;--accent-2:#C2DDAA;--shadow:0 8px 18px rgba(34,64,30,.06)}*{margin:0;padding:0;box-sizing:border-box}body{font-family:'Inter','Segoe UI',Tahoma,Geneva,Verdana,sans-serif;background:var(--bg);color:var(--text);min-height:100vh}header{position:sticky;top:0;z-index:10;background:rgba(255,255,255,.9);backdrop-filter:saturate(140%) blur(8px);border-bottom:1px solid var(--accent-2);padding:.9rem 1.1rem}.header-content{max-width:980px;margin:0 auto;display:flex;align-items:center;justify-content:space-between}.logo{display:flex;align-items:center;gap:.6rem}.leaf-icon{font-size:1.3rem}.logo-text{font-weight:800;letter-spacing:.3px;color:var(--text)}nav{display:flex;gap:.5rem}.nav-link{text-decoration:none;color:var(--muted);padding:.55rem .9rem;border-radius:999px;font-weight:600;background:rgba(192,217,167,.35);transition:.2s}.nav-link:hover,.nav-link.active{color:#fff;background:var(--primary)}main{max-width:980px;margin:0 auto;padding:1rem}.main-container{display:flex;flex-direction:column;gap:1rem}.plant-health-section,.analyzer-section{background:var(--card);border-radius:22px;padding:1.2rem;box-shadow:var(--shadow);border:1px solid var(--accent-2)}.section-header{display:flex;align-items:center;gap:.6rem;margin-bottom:1rem}.section-icon{font-size:1.3rem}.section-header h2{font-size:1.2rem;color:var(--text)}.monitor-grid{display:grid;grid-template-columns:1fr;gap:1rem;margin-bottom:1rem}.status-card,.humidity-card,.light-card,.config-card{border-radius:20px;padding:1rem;position:relative;overflow:hidden}.status-card{background:#C2DDAA}.humidity-card{background:#C0D9A7}.light-card{background:#E7EDD7}.temp-card{background:#E7EDD7;border-radius:20px;padding:1rem}.config-card{background:#C2DDAA}.personality-card{background:#E7EDD7;border-radius:20px;padding:1rem}.personality-content{display:flex;flex-direction:column;gap:.9rem}.personality-content input[type=\"text\"]{padding:.6rem .7rem;border:2px solid var(--accent-2);border-radius:12px;background:#fff;font-weight:600;font-size:.95rem}.dev-tools-card{background:#f8faf9;border-radius:20px;padding:1rem}.card-header{display:flex;align-items:center;gap:.6rem;margin-bottom:.8rem}.card-icon{font-size:1.1rem}.card-header h3{font-size:1rem;color:var(--text)}.status-content{text-align:center}.status-emoji{font-size:3.1rem;margin-bottom:.35rem}.status-text{font-size:1.05rem;font-weight:800;color:var(--text);margin-bottom:.35rem}.plant-name{font-size:.85rem;color:var(--muted);background:rgba(255,255,255,.7);padding:.35rem .8rem;border-radius:999px;display:inline-block}.gauge-container{text-align:center}.gauge{width:140px;height:70px;border:0;background:linear-gradient(180deg,rgba(34,64,30,.06),rgba(34,64,30,0));border-radius:90px 90px 0 0;position:relative;margin:.2rem auto .6rem;box-shadow:inset 0 6px 14px rgba(34,64,30,.06)}.gauge-fill{height:100%;background:linear-gradient(90deg,var(--primary),var(--primary-strong));border-radius:90px 90px 0 0;transition:width .45s ease}.humidity-card .gauge-fill{background:linear-gradient(90deg,var(--primary-strong),var(--primary))}.gauge-value{font-size:1.1rem;font-weight:800;color:var(--text)}.gauge-labels{display:flex;justify-content:space-between;font-size:.8rem;color:var(--muted)}.config-content{display:flex;flex-direction:column;gap:.9rem}.config-item{display:flex;flex-direction:column;gap:.4rem}.config-item label{font-weight:700;color:var(--text)}.config-item select{padding:.6rem .7rem;border:2px solid var(--accent-2);border-radius:12px;background:#fff;font-weight:600}.care-schedule{display:flex;flex-direction:column;gap:.4rem}.schedule-item{display:flex;justify-content:space-between;align-items:center}.schedule-value{color:var(--primary-strong);font-weight:800}.tools-content{display:flex;flex-direction:column;gap:.7rem}.tool-btn{display:flex;align-items:center;justify-content:center;gap:.5rem;padding:.85rem 1rem;border:0;border-radius:14px;font-weight:800;cursor:pointer;transition:.2s;box-shadow:var(--shadow)}.tool-btn:hover{transform:translateY(-2px)}.audio-btn{background:var(--accent);color:var(--text)}.restart-btn{background:var(--accent-2);color:var(--text)}.export-btn{background:var(--primary);color:#fff}.analyzer-grid{display:grid;grid-template-columns:1fr;gap:1rem}.source-card,.lexical-card,.syntax-card{background:var(--card);border-radius:20px;padding:1rem;box-shadow:var(--shadow);border:1px solid var(--accent-2);overflow:hidden}.lexical-card{overflow-x:auto;overflow-y:visible}.code-editor{background:#1f2937;border-radius:12px;padding:1rem;overflow-x:auto}.code-editor textarea{width:100%;height:420px;background:transparent;border:none;color:#e2e8f0;font-family:'Courier New',monospace;font-size:.95rem;line-height:1.55;resize:vertical;overflow-y:auto;scrollbar-width:none;-ms-overflow-style:none}.code-editor textarea::-webkit-scrollbar{display:none}.code-actions{display:flex;gap:.7rem;margin-top:.9rem}.btn{padding:.85rem 1.2rem;border:none;border-radius:12px;font-size:.95rem;font-weight:800;cursor:pointer;transition:.2s}.btn-primary{background:var(--primary);color:#fff}.btn-secondary{background:#dfe7d2;color:var(--text)}.btn:hover{transform:translateY(-2px);box-shadow:0 8px 16px rgba(0,0,0,.12)}.tokens-container{display:flex;flex-direction:column;gap:.8rem;overflow-x:auto}.tokens-summary{display:grid;grid-template-columns:repeat(3,1fr);gap:.7rem;margin-bottom:.6rem;min-width:300px}.summary-item{background:#E7EDD7;padding:.9rem;border-radius:12px;border-left:4px solid var(--primary);display:flex;justify-content:space-between;align-items:center}.summary-label{font-weight:700;color:var(--text)}.summary-value{font-weight:900;color:var(--primary-strong);font-size:1.1rem}.tokens-list{display:flex;flex-wrap:wrap;gap:.45rem;overflow-x:auto}.tokens-table{width:100%;min-width:600px;border-collapse:separate;border-spacing:0 8px}.tokens-table thead th{text-align:left;font-weight:800;color:var(--text);font-size:.9rem;padding:.5rem .8rem}.tokens-table tbody tr{background:#F1F5EA;box-shadow:var(--shadow)}.tokens-table tbody td{padding:.6rem .8rem;font-weight:700;color:var(--text);border-top:1px solid var(--accent-2);border-bottom:1px solid var(--accent-2);word-break:break-word}.tokens-table tbody td:first-child{border-left:1px solid var(--accent-2);border-top-left-radius:10px;border-bottom-left-radius:10px}.tokens-table tbody td:last-child{border-right:1px solid var(--accent-2);border-top-right-radius:10px;border-bottom-right-radius:10px}.token{padding:.35rem .8rem;border-radius:999px;font-size:.8rem;font-weight:800}.token.keyword{background:#E7EDD7;color:var(--text)}.token.preprocessor{background:#5A7326;color:#fff;border:0;box-shadow:0 2px 8px rgba(34,64,30,.25)}.token.library{background:#22401E;color:#C0D9A7;border:0;box-shadow:0 2px 8px rgba(34,64,30,.25)}.token.function{background:#C0D9A7;color:#22401E;border:0;box-shadow:0 2px 8px rgba(34,64,30,.20)}.token.identifier{background:#F1F5EA;color:var(--text)}.token.symbol{background:#E9F1E0;color:var(--text)}.token.number{background:#E3ECD8;color:var(--text)}.token.string{background:#E9F3E2;color:var(--text)}.syntax-content{display:flex;flex-direction:column;gap:.8rem}.parse-tree h4,.grammar-rules h4{color:var(--text);margin-bottom:.4rem}.mermaid-tree{min-height:220px;background:#F1F5EA;border-radius:12px;padding:1rem;border:1px solid var(--accent-2)}.rules-list{display:flex;flex-direction:column;gap:.3rem}.rule{font-family:'Courier New',monospace;font-size:.9rem;color:var(--muted);padding:.35rem .5rem;background:#F1F5EA;border-radius:8px}footer{text-align:center;padding:2rem;color:var(--muted)}.status-icon,.material-icons.status-icon{font-size:6rem!important;margin:0 auto .7rem auto;display:block;line-height:1;text-align:center;transition:.2s}@media (min-width:720px){.monitor-grid{grid-template-columns:repeat(3,1fr);gap:1.4rem}.analyzer-grid{grid-template-columns:1.5fr 1fr 1fr}main{padding:2rem 1.3rem}}";
  server.send(200, "text/css", css);
}

void handleJS() {
  // Debido a la longitud del JS se divide en partes
  String js1 = "var sensorUpdateInterval;console.log('script.js parsed');document.addEventListener('DOMContentLoaded',function(){console.log('DOMContentLoaded fired');loadConfigUI()});";
  String js1a = "function loadConfigUI(){fetch('/api/plant-config').then(function(r){return r.json()}).then(function(cfg){var sel=document.getElementById('plantType');if(sel&&cfg.plantType){sel.value=cfg.plantType}var ini=document.getElementById('horaDiaIni');var fin=document.getElementById('horaDiaFin');if(ini&&typeof cfg.horaDiaInicio==='number'){ini.value=cfg.horaDiaInicio}if(fin&&typeof cfg.horaDiaFin==='number'){fin.value=cfg.horaDiaFin}if(cfg.plantType==='custom'){document.getElementById('customMinH').value=(cfg.humMin!==undefined?cfg.humMin:'');document.getElementById('customMaxH').value=(cfg.humMax!==undefined?cfg.humMax:'');document.getElementById('customMinT').value=(cfg.tempMin!==undefined?cfg.tempMin:'');document.getElementById('customMaxT').value=(cfg.tempMax!==undefined?cfg.tempMax:'')}updateCustomInputs();updatePlantInfo();updatePlantName();updateGaugeScales();startSensorUpdates()}).catch(function(e){console.error(e);startSensorUpdates();updateCustomInputs();updatePlantInfo();updatePlantName();updateGaugeScales()})};";
  String js2 = "function startSensorUpdates(){updateSensors();sensorUpdateInterval=setInterval(updateSensors,3000)};";
  String js3 = "function updateSensors(){fetch('/api/sensors').then(function(r){return r.json()}).then(function(d){document.getElementById('humidity').textContent=d.humidity.toFixed(1)+'%';document.getElementById('temp').textContent=d.temperature.toFixed(1)+' °C';updateGaugesFromSensors(d.humidity,d.temperature);checkPlantStatus(d.humidity,d.temperature)}).catch(function(e){console.error('Error:',e)})};";

  String js4 = "function updateGauge(gaugeId,value,max){var g=document.getElementById(gaugeId);if(g){var p=Math.min((value/max)*100,100);g.style.width=p+'%'}};";
  String js5 = "var plantaRangos={lavanda:{nombre:'Lavanda',humedad:{min:30,max:45},temp:{min:15,max:26}},cardo:{nombre:'Cardo',humedad:{min:30,max:50},temp:{min:15,max:28}},trebol:{nombre:'Trébol',humedad:{min:55,max:75},temp:{min:18,max:26}},aeonium:{nombre:'Aeonium haworthii',humedad:{min:15,max:35},temp:{min:15,max:28}},custom:{nombre:'Personalizada',humedad:{min:0,max:100},temp:{min:0,max:40}}};";
  String js6 = "var plantaInfo={lavanda:{humedad:'Ambiente seco, 30–45% RH'},cardo:{humedad:'Moderada a baja, 30–50% RH'},trebol:{humedad:'Fresca y regular, 55–75% RH'},aeonium:{humedad:'Baja, 15–35% RH'},custom:{humedad:''}};";
  String js7 = "function updatePlantInfo(){var t=document.getElementById('plantType').value;var ih=document.getElementById('infoHumedad');var it=document.getElementById('infoTemp');var c=getActiveRangos();if(t!=='custom'){ih.textContent=plantaInfo[t].humedad;if(it){var r=plantaRangos[t];if(r&&r.temp){it.textContent=r.temp.min+'–'+r.temp.max+' °C'}else{it.textContent='—'}}}else{ih.textContent='Custom: '+c.humedad.min+'–'+c.humedad.max+'% RH';if(it){it.textContent=c.temp.min+'–'+c.temp.max+' °C'}}};";
  String js8 = "function updateCustomInputs(){var s=document.getElementById('plantType');var cd=document.getElementById('customConfig');if(!s||!cd){return}if(s.value==='custom'){cd.style.display=''}else{cd.style.display='none'}};";
  String js9 = "function getActiveRangos(){var t=document.getElementById('plantType').value;var base=plantaRangos[t]||{};var bh=(base.humedad||{min:0,max:100});var bt=(base.temp||{min:0,max:40});var c={nombre:(base.nombre||''),humedad:{min:bh.min,max:bh.max},temp:{min:bt.min,max:bt.max}};if(t==='custom'){var mh=parseInt(document.getElementById('customMinH').value)||0;var xh=parseInt(document.getElementById('customMaxH').value)||100;var mt=parseInt(document.getElementById('customMinT').value)||0;var xt=parseInt(document.getElementById('customMaxT').value)||40;c.humedad={min:mh,max:xh};c.temp={min:mt,max:xt}}return c};";
  String js10 = "function checkPlantStatus(h,t){var si=document.getElementById('statusIcon');if(!si){return}var c=getActiveRangos();var mh=Math.max(c.humedad.min,0);var xh=Math.min(c.humedad.max,100);var tm=Math.max((c.temp&&typeof c.temp.min==='number'?c.temp.min:0),0);var tx=Math.min((c.temp&&typeof c.temp.max==='number'?c.temp.max:40),50);var okH=h>=mh&&h<=xh;var okT=t>=tm&&t<=tx;var icon='sentiment_satisfied';var color='#5A7326';if(!okH||!okT){icon='sentiment_very_dissatisfied';color='#c62828'}si.innerHTML=icon;si.style.color=color;updateGaugeColor('humidityGauge',okH,h,mh,xh);updateGaugeColor('tempGauge',okT,t,tm,tx)};";
  String js11 = "function updateGaugeColor(gid,inR,val,min,max){var g=document.getElementById(gid);if(!g){return}var rng=Math.max(max-min,1);var near=min+Math.max(2,rng*0.1);if(val<min||val>max){g.style.background='linear-gradient(90deg,#ec2b26,#ef892a)'}else if(val<=near){g.style.background='linear-gradient(90deg,#fdbd18,#ef892a)'}else{g.style.background='linear-gradient(90deg,#19b24e,#7ee681)'}};";
  String js12 = "var MAX_HUM_SENSOR=100;var MAX_TEMP_SENSOR=50;function updateGaugeScales(){var c=getActiveRangos();var xh=Math.min(c.humedad.max,MAX_HUM_SENSOR);var mh=Math.max(c.humedad.min,0);document.querySelector('.humidity-card .gauge-labels span:first-child').textContent=mh;document.querySelector('.humidity-card .gauge-labels span:last-child').textContent=xh;var tmin=Math.max((c.temp&&typeof c.temp.min==='number'?c.temp.min:0),0);var tmax=Math.min((c.temp&&typeof c.temp.max==='number'?c.temp.max:40),MAX_TEMP_SENSOR);document.getElementById('tempMin').textContent=tmin;document.getElementById('tempMax').textContent=tmax;};";
  String js13 = "function updateGaugesFromSensors(h,t){var c=getActiveRangos();var mh=Math.max(c.humedad.min,0);var xh=Math.min(c.humedad.max,MAX_HUM_SENSOR);var tmin=Math.max((c.temp&&typeof c.temp.min==='number'?c.temp.min:0),0);var tmax=Math.min((c.temp&&typeof c.temp.max==='number'?c.temp.max:40),MAX_TEMP_SENSOR);updateGauge('humidityGauge',h-mh,xh-mh);updateGauge('tempGauge',t-tmin,tmax-tmin)};";
  String js14 = "(function(){var ids=['customMinH','customMaxH','customMinT','customMaxT'];for(var i=0;i<ids.length;i++){var el=document.getElementById(ids[i]);if(el){el.addEventListener('input',function(){updateCustomInputs();updatePlantInfo();updatePlantName();updateGaugeScales();var h=parseFloat(document.getElementById('humidity').textContent)||0;var tempEl=document.getElementById('temp');var t=parseFloat(((tempEl?tempEl.textContent:'0')).replace(/[^0-9.\-]/g,''))||0;checkPlantStatus(h,t);updateGaugesFromSensors(h,t);savePlantConfig()})}}})();";
  String js14b = "(function(){var selPT=document.getElementById('plantType');if(selPT){selPT.addEventListener('change',function(){updateCustomInputs();updatePlantInfo();updatePlantName();updateGaugeScales();var h=parseFloat(document.getElementById('humidity').textContent)||0;var tempEl=document.getElementById('temp');var t=parseFloat(((tempEl?tempEl.textContent:'0')).replace(/[^0-9.\-]/g,''))||0;checkPlantStatus(h,t);updateGaugesFromSensors(h,t);savePlantConfig()})}})();";
  String js15 = "function updatePlantName(){var t=document.getElementById('plantType').value;var pn=document.querySelector('.plant-name');if(pn){var nombres={lavanda:'Lavanda',cardo:'Cardo',trebol:'Trébol',aeonium:'Aeonium haworthii',custom:'Planta Personalizada'};pn.textContent='Planta Actual: '+(nombres[t]||'Desconocida')}};";
  String js15b = "function savePlantConfig(){var t=document.getElementById('plantType').value;var cfg={plantType:t};if(t==='custom'){cfg.humMin=parseInt(document.getElementById('customMinH').value)||0;cfg.humMax=parseInt(document.getElementById('customMaxH').value)||100;cfg.tempMin=parseInt(document.getElementById('customMinT').value)||0;cfg.tempMax=parseInt(document.getElementById('customMaxT').value)||40}fetch('/api/plant-config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)}).catch(function(){})};";
  String js16 = "var horaDiaInicio=7;var horaDiaFin=19;function isDay(){var h=new Date().getHours();return h>=horaDiaInicio&&h<horaDiaFin};document.getElementById('saveDayHours').onclick=function(){var ini=parseInt(document.getElementById('horaDiaIni').value);var fin=parseInt(document.getElementById('horaDiaFin').value);var t=document.getElementById('plantType').value;var cfg={horaDiaInicio:ini,horaDiaFin:fin,plantType:t};if(t==='custom'){cfg.humMin=parseInt(document.getElementById('customMinH').value)||0;cfg.humMax=parseInt(document.getElementById('customMaxH').value)||100;cfg.tempMin=parseInt(document.getElementById('customMinT').value)||0;cfg.tempMax=parseInt(document.getElementById('customMaxT').value)||40}if(ini>=0&&ini<=23&&fin>ini&&fin<=23){horaDiaInicio=ini;horaDiaFin=fin;showNotification('Configuración guardada','success');fetch('/api/plant-config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)}).then(function(){}).catch(function(){})}else{showNotification('Rango inválido','error')}};";
  String js17 = "function testAudio(){showNotification('Generando respuesta de audio...','info');fetch('/api/test-audio',{method:'POST'}).then(function(r){return r.json()}).then(function(d){showNotification('Audio generado correctamente','success')}).catch(function(e){showNotification('Error al generar audio','error');console.error(e)})};function resetWiFi(){if(confirm('¿Estás seguro de que quieres cambiar la red WiFi?\\n\\nEl ESP32 se reiniciará y creará una red llamada \"PlantTalk-Setup\" para configurar.')){showNotification('Reseteando WiFi...','warning');fetch('/api/reset-wifi',{method:'POST'}).then(function(){showNotification('Reiniciando... Busca la red \"PlantTalk-Setup\"','info')}).catch(function(e){showNotification('Error al resetear WiFi','error')})}};";
  String js20 = "function escapeHtml(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;').replace(/'/g,'&#039;')};";
  String js21 = "function showNotification(msg,type){type=type||'info';var n=document.createElement('div');n.className='notification '+type;n.textContent=msg;n.style.cssText='position:fixed;top:20px;right:20px;padding:1rem 1.5rem;border-radius:12px;color:white;font-weight:700;z-index:1000;animation:slideIn 0.3s ease;max-width:300px;box-shadow:0 4px 20px rgba(0,0,0,0.2)';switch(type){case 'success':n.style.background='linear-gradient(135deg,#4CAF50,#388e3c)';break;case 'error':n.style.background='linear-gradient(135deg,#f44336,#d32f2f)';break;case 'warning':n.style.background='linear-gradient(135deg,#ff9800,#f57c00)';break;default:n.style.background='linear-gradient(135deg,#2196F3,#1976D2)'}document.body.appendChild(n);setTimeout(function(){n.style.animation='slideOut 0.3s ease';setTimeout(function(){if(n.parentNode){n.parentNode.removeChild(n)}},300)},3000)};";
  String js22 = "var style=document.createElement('style');style.textContent='@keyframes slideIn{from{transform:translateX(100%);opacity:0}to{transform:translateX(0);opacity:1}}@keyframes slideOut{from{transform:translateX(0);opacity:1}to{transform:translateX(100%);opacity:0}}';document.head.appendChild(style);";
  String js16b = "function loadPersonality(){fetch('/api/personality').then(function(r){return r.json()}).then(function(d){document.getElementById('personality1').value=d.word1||'sarcastica';document.getElementById('personality2').value=d.word2||'graciosa';document.getElementById('personality3').value=d.word3||'dramatica'}).catch(function(e){console.error('Error cargando personalidad:',e)})};loadPersonality();document.getElementById('savePersonality').onclick=function(){var w1=document.getElementById('personality1').value.trim();var w2=document.getElementById('personality2').value.trim();var w3=document.getElementById('personality3').value.trim();if(w1&&w2&&w3){fetch('/api/personality',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({word1:w1,word2:w2,word3:w3})}).then(function(r){return r.json()}).then(function(){showNotification('Personalidad guardada: '+w1+', '+w2+', '+w3,'success')}).catch(function(e){showNotification('Error al guardar personalidad','error');console.error(e)})}else{showNotification('Completa las 3 palabras','warning')}};";

  String js = js1 + js1a + js2 + js3 + js4 + js5 + js6 + js7 + js8 + js9 + js10 + js11 + js12 + js13 + js14 + js14b + js15 + js15b + js16 + js16b + js17 + js20 + js21 + js22;
  
  server.send(200, "application/javascript", js);
}

void updateSensors() {
  int rawHumidity = analogRead(HUMIDITY_PIN);
  humidity = map(rawHumidity, SOIL_RAW_SECO, SOIL_RAW_MOJADO, 0, 100);
  humidity = constrain(humidity, 0, 100);

  float t = dht.readTemperature();
  if (isnan(t)) {
    delay(50);
    t = dht.readTemperature();
  }
  static float lastValidTemp = NAN;
  if (!isnan(t) && t > -40 && t < 85) {
    temperature = t;
    lastValidTemp = t;
  } else if (!isnan(lastValidTemp)) {
    temperature = lastValidTemp;
  } else {
    temperature = 25.0;
  }
}

bool esDia() {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  int hora = timeinfo.tm_hour;
  return (hora >= horaDiaInicio && hora < horaDiaFin);
}

void updatePlantRanges() {
  // Actualizar rangos segun el tipo de planta seleccionado
  if (plantType == "lavanda") {
    plantRanges = {30, 40, 15, 26};
  } else if (plantType == "cardo") {
    plantRanges = {20, 35, 15, 28};
  } else if (plantType == "trebol") {
    plantRanges = {50, 70, 18, 26};
  } else if (plantType == "aeonium") {
    plantRanges = {30, 40, 15, 28};
  }
  // Para "custom", los rangos se setean desde handlePlantConfig
}

void loadConfiguration() {
  preferences.begin("plant-config", true); // true = solo lectura
  
  // Cargar tipo de planta (default: "lavanda")
  plantType = preferences.getString("plantType", "lavanda");
  
  // Cargar horarios (defaults: 7 y 19)
  horaDiaInicio = preferences.getInt("horaDiaIni", 7);
  horaDiaFin = preferences.getInt("horaDiaFin", 19);
  
  // Cargar rangos personalizados si existen
  if (plantType == "custom") {
    plantRanges.humMin = preferences.getInt("humMin", 30);
    plantRanges.humMax = preferences.getInt("humMax", 70);
    plantRanges.tempMin = preferences.getInt("tempMin", 0);
    plantRanges.tempMax = preferences.getInt("tempMax", 40);
  } else {
    updatePlantRanges(); // Cargar rangos predefinidos
  }
  
  // Cargar banderas de alertas
  alertaHumedadAltaDada = preferences.getBool("alertHumAlta", false);
  
  // Cargar personalidad (defaults: sarcastica, graciosa, dramatica)
  personalidad1 = preferences.getString("personality1", "sarcastica");
  personalidad2 = preferences.getString("personality2", "graciosa");
  personalidad3 = preferences.getString("dramatica", "dramatica");
  
  preferences.end();
  
  Serial.println("Configuracion cargada desde memoria:");
  Serial.println("  Tipo de planta: " + plantType);
  Serial.println("  Horario dia: " + String(horaDiaInicio) + ":00 - " + String(horaDiaFin) + ":00");
  Serial.println("  Personalidad: " + personalidad1 + ", " + personalidad2 + ", " + personalidad3);
}

void saveConfiguration() {
  preferences.begin("plant-config", false); // false = lectura/escritura
  
  // Guardar tipo de planta
  preferences.putString("plantType", plantType);
  
  // Guardar horarios
  preferences.putInt("horaDiaIni", horaDiaInicio);
  preferences.putInt("horaDiaFin", horaDiaFin);
  
  // Guardar rangos personalizados si es custom
  if (plantType == "custom") {
    preferences.putInt("humMin", plantRanges.humMin);
    preferences.putInt("humMax", plantRanges.humMax);
    preferences.putInt("tempMin", plantRanges.tempMin);
    preferences.putInt("tempMax", plantRanges.tempMax);
  }
  
  // Guardar banderas de alertas
  preferences.putBool("alertHumAlta", alertaHumedadAltaDada);
  
  // Guardar personalidad
  preferences.putString("personality1", personalidad1);
  preferences.putString("personality2", personalidad2);
  preferences.putString("personality3", personalidad3);
  
  preferences.end();
  
  Serial.println("Configuracion guardada en memoria persistente");
}

void checkPlantStatus() {
  // Solo hablar durante el horario de día configurado
  if (!esDia()) {
    return; // No molestar durante la noche
  }
  
  static unsigned long lastCriticalAlert = 0;
  bool alertaCritica = false;
  
  // Calcular umbrales críticos (20% por debajo del mínimo óptimo)
  int humedadCritica = plantRanges.humMin * 0.6; // 40% menos del mínimo
  int temperaturaCritica = 35; // Temperatura crítica universal
  
  // ALERTAS CRÍTICAS (prioridad alta - cada 5 minutos máximo)
  // Humedad extremadamente baja (muy por debajo del rango óptimo)
  String pers = personalidad1 + "," + personalidad2 + "," + personalidad3;
  if (humidity < humedadCritica) {
    if (millis() - lastCriticalAlert > INTERVALO_CRITICO) {
      String prompt = "Personalidad: " + pers + ". Situación: Tengo mucha sed, necesito agua urgente";
      String respuesta = getChatGPTResponse(prompt);
      if (respuesta.length() > 0) {
        speakGoogleTTS(respuesta);
      } else {
        speakGoogleTTS("Necesito agua urgente");
      }
      lastCriticalAlert = millis();
      alertaCritica = true;
    }
    return; // No seguir con alertas normales
  }
  
  // Temperatura peligrosamente alta
  if (temperature > temperaturaCritica) {
    if (millis() - lastCriticalAlert > INTERVALO_CRITICO) {
      String prompt = "Personalidad: " + pers + ". Situación: Hace demasiado calor";
      String respuesta = getChatGPTResponse(prompt);
      if (respuesta.length() > 0) {
        speakGoogleTTS(respuesta);
      } else {
        speakGoogleTTS("Temperatura muy alta");
      }
      lastCriticalAlert = millis();
      alertaCritica = true;
    }
    return;
  }
  
  // ALERTAS NORMALES (cada 30 minutos desde loop) - TODAS CON IA
  // Humedad baja (debajo del rango óptimo) - SE REPITE (fácil de arreglar)
  if (humidity < plantRanges.humMin) {
    String prompt = "Personalidad: " + pers + ". Situación: Tengo sed, necesito agua";
    String respuesta = getChatGPTResponse(prompt);
    if (respuesta.length() > 0) {
      speakGoogleTTS(respuesta);
    } else {
      speakGoogleTTS("Necesito agua");
    }
    if (alertaHumedadAltaDada) {
      alertaHumedadAltaDada = false;
      saveConfiguration();
    }
  } 
  // Humedad alta (arriba del rango óptimo) - UNA SOLA VEZ (difícil de arreglar)
  else if (humidity > plantRanges.humMax) {
    if (!alertaHumedadAltaDada) {
      String prompt = "Personalidad: " + pers + ". Situación: Tengo demasiada agua";
      String respuesta = getChatGPTResponse(prompt);
      if (respuesta.length() > 0) {
        speakGoogleTTS(respuesta);
      } else {
        speakGoogleTTS("Demasiada humedad");
      }
      alertaHumedadAltaDada = true;
      saveConfiguration();
    }
  }
  // Temperatura alta - SE REPITE (fácil de arreglar: mover la planta)
  else if (temperature > plantRanges.tempMax) {
    String prompt = "Hace mucho calor, necesito sombra";
    String respuesta = getChatGPTResponse(prompt);
    if (respuesta.length() > 0) {
      speakGoogleTTS(respuesta);
    } else {
      speakGoogleTTS("Hace mucho calor");
    }
  } 
  // Temperatura baja - SE REPITE (fácil de arreglar: mover la planta)
  else if (temperature < plantRanges.tempMin) {
    String prompt = "Tengo frío, necesito más calor";
    String respuesta = getChatGPTResponse(prompt);
    if (respuesta.length() > 0) {
      speakGoogleTTS(respuesta);
    } else {
      speakGoogleTTS("Tengo frio");
    }
  }
  
  // Todo está bien
  else {
    // Resetear todas las banderas cuando todo vuelve a la normalidad
    bool cambio = false;
    if (alertaHumedadAltaDada) {
      alertaHumedadAltaDada = false;
      cambio = true;
    }
    if (cambio) {
      saveConfiguration();
    }
    
    // Cada tanto dar una señal de vida (opcional) - tambien con IA
    if (random(0, 10) > 8) { // 10% de probabilidad
      // Primero decir "Hola"
      speakGoogleTTS("Hola");
      // Luego hacer el saludo con el servo
      waveServoGreeting();
      // Después la respuesta de IA (si hay)
      String prompt = "Personalidad: " + pers + ". Situación: Estoy bien, me siento feliz";
      String respuesta = getChatGPTResponse(prompt);
      if (respuesta.length() > 0) {
        speakGoogleTTS(respuesta);
      } else {
        speakGoogleTTS("Estoy feliz");
      }
    }
  }
}

void handlePersonality() {
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(512);
    deserializeJson(doc, server.arg("plain"));
    
    if (doc.containsKey("word1")) {
      personalidad1 = doc["word1"].as<String>();
    }
    if (doc.containsKey("word2")) {
      personalidad2 = doc["word2"].as<String>();
    }
    if (doc.containsKey("word3")) {
      personalidad3 = doc["word3"].as<String>();
    }
    
    // Guardar en memoria persistente
    saveConfiguration();
    
    Serial.println("Personalidad actualizada: " + personalidad1 + ", " + personalidad2 + ", " + personalidad3);
    server.send(200, "application/json", "{\"status\":\"success\"}");
  }
}

void handleGetPersonality() {
  Serial.println("[WEB] Petición recibida: /api/personality (GET)");
  Serial.println("[WEB] Personalidad: " + personalidad1 + ", " + personalidad2 + ", " + personalidad3);
  
  DynamicJsonDocument doc(512);
  doc["word1"] = personalidad1;
  doc["word2"] = personalidad2;
  doc["word3"] = personalidad3;
  
  String response;
  serializeJson(doc, response);
  Serial.println("[WEB] Enviando respuesta: " + response);
  server.send(200, "application/json", response);
  Serial.println("[WEB] Respuesta enviada correctamente");
}

void handleResetWiFi() {
  Serial.println("[WEB] ===== Petición recibida: /api/reset-wifi =====");
  Serial.println("[WIFI] Usuario solicitó resetear configuración WiFi");
  
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Reseteando WiFi...\"}");
  
  Serial.println("[WIFI] Borrando credenciales WiFi guardadas...");
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  
  Serial.println("[WIFI] Credenciales borradas");
  Serial.println("[WIFI] Reiniciando ESP32 en 3 segundos...");
  Serial.println("[WIFI] 'PlantTalk-Setup'");
  
  delay(3000);
  ESP.restart();
}

void handleTestAudio() {
  Serial.println("[WEB] ===== Petición recibida: /api/test-audio =====");
  Serial.println("[AUDIO] Prueba de audio solicitada desde la web");
  
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Generando audio...\"}");
  
  // Simular presión del botón físico
  Serial.println("[AUDIO] Simulando presión del botón táctil...");
  handleButtonPress();
}

// Interrupcion del boton tactil (ISR)
void IRAM_ATTR onButtonPress() {
  unsigned long ahora = millis();
  // Anti-rebote: ignorar si han pasado menos de 500ms desde la última presión
  if (ahora - ultimaInteraccion > DEBOUNCE_TIEMPO) {
    botonPresionado = true;
    ultimaInteraccion = ahora;
  }
}

// Manejar presion del boton
void handleButtonPress() {
  Serial.println("Boton presionado! Generando respuesta...");
  
  // Construir prompt para generacion de respuesta
  String prompt = "";
  bool esSaludo = false;
  
  // Evaluar estado de la planta
  String pers = personalidad1 + "," + personalidad2 + "," + personalidad3;
  if (humidity < plantRanges.humMin) {
    prompt = "Tengo sed, necesito agua";
  } else if (humidity > plantRanges.humMax) {
    prompt = "Tengo demasiada agua";
  } else if (temperature > plantRanges.tempMax) {
    prompt = "Hace mucho calor";
  } else if (temperature < plantRanges.tempMin) {
    prompt = "Tengo frío";
  } else {
    // Sin problemas: saludo, chiste o comentario random
    prompt = "Estoy bien, me siento feliz";
    esSaludo = true;
  }
  
  Serial.println("Prompt: " + prompt);
  
  // Si es saludo, primero decir "Hola" y luego mover el servo
  if (esSaludo) {
    speakGoogleTTS("Hola");
    waveServoGreeting();
  }
  
  // Obtener respuesta de ChatGPT
  String respuesta = getChatGPTResponse(prompt);
  
  if (respuesta.length() > 0) {
    Serial.println("Respuesta IA: " + respuesta);
    // Verificar que la respuesta no sea demasiado larga para TTS (máximo 200 caracteres)
    if (respuesta.length() > 200) {
      Serial.println("[WARN] Respuesta muy larga, truncando a 200 caracteres");
      int lastSpace = respuesta.lastIndexOf(' ', 200);
      if (lastSpace > 0) {
        respuesta = respuesta.substring(0, lastSpace);
      } else {
        respuesta = respuesta.substring(0, 200);
      }
    }
    speakGoogleTTS(respuesta);
  } else {
    Serial.println("Respuesta IA vacía o error: usando fallback local");
    // Fallback hablado local según estado
    String fallback = "";
    if (humidity < plantRanges.humMin) fallback = "Necesito agua";
    else if (humidity > plantRanges.humMax) fallback = "Demasiada humedad";
    else if (temperature > plantRanges.tempMax) fallback = "Hace mucho calor";
    else if (temperature < plantRanges.tempMin) fallback = "Tengo frio";
    else {
      // Si es saludo y no hay respuesta de IA, ya dijimos "Hola" arriba
      if (!esSaludo) {
        fallback = "Hola";
        speakGoogleTTS(fallback);
      }
      return;
    }
    speakGoogleTTS(fallback);
  }
}

// Utilidad para escapar JSON (escapar comillas y backslashes)
String escapeJson(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}


// Llamada a ChatGPT API
String getChatGPTResponse(String prompt) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi no conectado");
    return "";
  }
  
  HTTPClient http;
  http.begin("https://api.openai.com/v1/chat/completions");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(openai_api_key));
  http.setTimeout(15000); // 15 segundos timeout
  
  // Obtener personalidad actual
  String pers = personalidad1 + "," + personalidad2 + "," + personalidad3;
  
  // Escapar prompt y personalidad para JSON válido
  String promptEscaped = escapeJson(prompt);
  String persEscaped = escapeJson(pers);
  
  // JSON ultra-compacto para ahorrar memoria
  String sys = "Eres una planta inteligente que habla con personalidad " + persEscaped + ". Habla SIEMPRE en primera persona (YO, MI, ME) usando expresiones naturales. Responde de forma breve (máximo 15 palabras). Combina estos rasgos de forma natural.";
  String userPrompt = promptEscaped;
  String payload = "{\"model\":\"gpt-4o-mini\",\"messages\":[{\"role\":\"system\",\"content\":\"" + escapeJson(sys) + "\"},{\"role\":\"user\",\"content\":\"" + userPrompt + "\"}],\"max_tokens\":50}";
  Serial.println("Enviando peticion a OpenAI...");
  
  // Permitir que el servidor responda mientras se envía la petición
  server.handleClient();
  yield();
  
  int httpCode = http.POST(payload);
  
  // Permitir que el servidor responda mientras se espera la respuesta
  server.handleClient();
  yield();
  
  String respuesta = "";
  if (httpCode == 200) {
    String response = http.getString();
    Serial.println("Respuesta HTTP: " + response);
    
    // Parsear JSON (extraer solo el contenido)
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, response);
    
    if (doc.containsKey("choices")) {
      respuesta = doc["choices"][0]["message"]["content"].as<String>();
      respuesta.trim(); // Eliminar espacios
      
      // Limitar longitud de respuesta (máximo 120 caracteres para evitar errores HTTP 400)
      if (respuesta.length() > 120) {
        // Cortar en el último espacio antes del límite
        int lastSpace = respuesta.lastIndexOf(' ', 120);
        if (lastSpace > 0) {
          respuesta = respuesta.substring(0, lastSpace);
        } else {
          respuesta = respuesta.substring(0, 120);
        }
        respuesta.trim();
      }
    }
  } else {
    Serial.println("Error HTTP: " + String(httpCode));
    if (httpCode > 0) {
      Serial.println("Respuesta: " + http.getString());
    }
  }
  
  http.end();
  return respuesta;
}

void waveServoGreeting() {
  // Posicion base
  servo.write(90);
  delay(150);
  // 3 oscilaciones tipo saludo
  for (int i = 0; i < 3; i++) {
    servo.write(120);
    delay(180);
    servo.write(70);
    delay(180);
  }
  // Volver al centro
  servo.write(90);
}
