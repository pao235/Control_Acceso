/***************************************************************************************************************************
 * FileName:     main.cpp
 * Processor:    Tensilica Xtensa LX6 160 MHz
 * Board:        ESP32 Dev Module
 * Company:      TecNM /IT Chihuahua
 * Description:  Control Access with ESP32, RFID, RTC DS1302, LCD I2C, MQTT and NTP
 * Authors:      Ana Cardona, Emiliano Pérez, Luis Anchondo
 * Updated:      05/2026
 * Created on:   15 jun. 2026
 * updated:      24/08/2026
 **************************************************************************************************************************/
/**************************************************************************************************************************
 * * Copyright (C) 2026 by Ana Cardona, Emiliano Pérez, Luis Anchondo - TecNM /IT Chihuahua
 *
 * Se permite la redistribucion, modificacion o uso de este software en formato fuente o binario
 * siempre que los archivos mantengan estos derechos de autor.
 * Los usuarios pueden modificar esto y usarlo para aprender sobre el campo de IoT, control de acceso, microcontroladores 
 * y sistemas embebidos.Se permite el uso de este software para fines educativos y de investigación.
 * Ana Cardona, Emiliano Pérez, Luis Anchondo y el TecNM /IT Chihuahua no son responsables del mal uso de este material.
 **************************************************************************************************************************/

 /*****************************************
  *          LIBRERÍAS USADAS
  *****************************************/
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Wiegand.h>           // Lectura de lector Wiegand (teclado + RFID)
#include <ThreeWire.h>        // Interfaz de 3 hilos para el RTC DS1302
#include <RtcDS1302.h>
#include <time.h>             // Para funciones de hora vía NTP (configTime, getLocalTime)
#include <LiquidCrystal_I2C.h>
#include <PubSubClient.h>     // Cliente MQTT
#include <ArduinoJSON.h>      // Uso de JSON para armar el payload a enviar por MQTT

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <SD.h>
#include <SPI.h>

// ==================== CONFIGURACIÓN DE RED Y SERVICIOS ====================
const char* ssid = "FAM_VALENZUELA";
const char* password = "6143973503valenzuela";
const char* ntpServer = "mx.pool.ntp.org";
const long gmtOffset_sec = -21600;     // UTC-6 (Chihuahua, sin horario de verano)
const int daylightOffset_sec = 0;

//const char* mqtt_server = "meztli.ddns.net";
//const int mqtt_port = 1883;
//const char* mqtt_user = "usuario";
//const char* mqtt_passwd = "a12345";
const char* mqtt_server = "mqtt-dashboard.com";
const int mqtt_port = 1883;
const char* mqtt_topic = "esp32/control/numeros";
const char* mqtt_user = "";
const char* mqtt_passwd = "";

String stMac = ""; // MAC del dispositivo, se llena una sola vez en setup()
String formatDateTime(const RtcDateTime& dt);

// ==================== OBJETOS DE PERIFÉRICOS ====================
WiFiClient espClient;
PubSubClient client(espClient);

ThreeWire myWire(4, 15, 25);       // Pines DAT, CLK, RST del RTC DS1302
RtcDS1302<ThreeWire> rtc(myWire);

WIEGAND wg;                        // Lector Wiegand (teclado matricial + RFID)
LiquidCrystal_I2C lcd(0x27, 16, 2);

const String buildingCode = "1";     
String codigoAcumulado = "";           

#define countof(a) (sizeof(a) / sizeof(a[0]))

// ==================== STRUCTS Y ENUMS ====================
// Evento crudo leído del Wiegand (código + tipo de trama, ej. 26/34 bits para
// tarjetas, 4/8 para teclado). Actualmente no se usa como tal para pasar por
// cola (el envío a MQTT se arma directo en TaskWiegand), pero queda definido
typedef struct {
    uint32_t code;
    int      type;
} EventoAcceso;

// Mensaje que viaja por queueMQTT un payload JSON ya serializado como texto
typedef struct {
    char payload[200];   // Si el JSON crece (más campos), revisar que no se trunque
} MQTTMessage;

// Tipos de mensaje que puede recibir la tarea de LCD
typedef enum {
    LCD_CODIGO,   // Mostrar dígitos que se van acumulando en el teclado
    LCD_ERROR,    // Mostrar mensaje de error (código incompleto)
    LCD_OK        // Mostrar mensaje de bienvenida / acceso aceptado
} LCDTipo;

// Mensaje que viaja por queueLCD.
typedef struct {
    LCDTipo tipo;
    char    texto[32];   // Usado solo por LCD_CODIGO para mostrar el código acumulado
} LCDMessage;

// Fuente de tiempo activa, NTP (mientras haya WiFi) o RTC físico como respaldo.
typedef enum {
    TIME_SRC_NTP,
    TIME_SRC_RTC
} TimeSource;

// Mensaje para la cola de la SD
typedef struct {
    char csvLinea[100]; 
} SDMessage;


volatile TimeSource timeSource = TIME_SRC_RTC;   // Se actualiza solo desde TaskRTC

// ==================== ESTADO DE CONFIRMACIÓN MQTT ====================
// Como PubSubClient usa QoS 0 (sin ACK de aplicación),
// se asume que el broker/topic re-envía el mismo payload que se publicó, y
// eso se usa como confirmación de entrega. Uso exclusivo de TaskMQTT y su
// callback (mqttCallback), que corre en el mismo contexto que client.loop().
volatile bool esperandoConfirmacion = false;
char ultimoPayloadEnviado[200];
unsigned long tiempoEnvio = 0;
const unsigned long TIMEOUT_CONFIRMACION_MS = 5000;

// ==================== COLAS (comunicación entre tareas) ====================
QueueHandle_t queueWiegand;   // Declarada pero no usada actualmente en el flujo mostrado
QueueHandle_t queueMQTT;      // TaskWiegand -> TaskMQTT (payloads JSON a publicar)
QueueHandle_t queueLCD;       // TaskWiegand -> TaskLCD (qué mostrar en pantalla)
QueueHandle_t queueRTC;       // TaskRTC -> TaskWiegand (hora actual, tamaño 1 + overwrite)
QueueHandle_t queueSD;        // Para la escritura en la memoria SD
SemaphoreHandle_t mutexSD; // Protege el acceso a la memoria
// ==================== PROTOTIPOS ====================

void printDateTime(const RtcDateTime& dt, char* buffer, size_t length);
void syncRTCFromNTP();
char decodeKeypad(uint32_t code);
void lcdInicio();
void lcdMostrarCodigo(String codigo);
void lcdError();
void lcdMensajeCorrecto();
void lcdMostrarHora();
void reconnectMQTT();

void enviarJson(const String& id, const RtcDateTime& dt);   // NOTA: prototipada pero
                                                              // nunca definida ni llamada.

void TaskLCD(void *pvParameters);
void TaskWiegand(void *pvParameters);
void TaskRTC(void *pvParameters);
void TaskMQTT(void *pvParameters);
void TaskWIFI(void *pvParameters);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void TaskSD(void *pvParameters);


// ==================== SETUP ====================
// Inicializa periféricos, crea las colas y lanza las 5 tareas de FreeRTOS
// No se conecta a WiFi aquí, eso lo hace TaskWIFI
void setup()
{
    Serial.begin(115200);
    Wire.begin();
    rtc.Begin();
    wg.begin(16, 17);          // Pines D0/D1 del lector Wiegand

    lcd.init();
    lcd.backlight();

    // Crear Mutex y Cola para la SD
    mutexSD = xSemaphoreCreateMutex();
    queueSD = xQueueCreate(10, sizeof(SDMessage));

    // Inicializar la tarjeta SD en el pin CS (5)
    if (!SD.begin(5)) {
        Serial.println("Error inicializando la MicroSD.");
    } else {
        Serial.println("MicroSD inicializada correctamente.");
    }

    stMac = WiFi.macAddress(); // Se puede leer la MAC sin estar conectado
    Serial.print("MAC: ");
    Serial.println(stMac);

    // Colas de tamaño 10 para eventos/mensajes normales, tamaño 1 para el buzón
    // de hora (siempre se sobrescribe con el valor más reciente)
    queueWiegand = xQueueCreate(10, sizeof(EventoAcceso));
    queueMQTT    = xQueueCreate(10, sizeof(MQTTMessage));
    queueLCD     = xQueueCreate(10, sizeof(LCDMessage));
    queueRTC     = xQueueCreate(1,  sizeof(RtcDateTime));

    // Distribución de tareas entre los dos cores del ESP32:
    // - Core 1: solo Wiegand, con la prioridad más alta (3), para no perder
    //   pulsos del lector por bloqueos de red/LCD
    // - Core 0: WiFi, MQTT, LCD y RTC, con prioridades más bajas
    xTaskCreatePinnedToCore(TaskWIFI,    "WiFi",    4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(TaskMQTT,    "MQTT",    4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(TaskWiegand, "Wiegand", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(TaskLCD,     "LCD",     4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(TaskRTC,     "RTC",     4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(TaskSD,     "TaskSD",   4096, NULL, 1, NULL, 0);
}

// todo el trabajo corre en las tareas de FreeRTOS.
void loop() {}

// ==================== OTRAS FUNCIONES ====================

/*// Formatea fecha/hora como MM/DD/YYYY HH:MM:SS.
void printDateTime(const RtcDateTime& dt, char* buffer, size_t length) {
    snprintf_P(buffer, length,
        PSTR("%02u/%02u/%04u %02u:%02u:%02u"),
        dt.Month(), dt.Day(), dt.Year(),
        dt.Hour(), dt.Minute(), dt.Second());
}*/

// Sincroniza el RTC físico con la hora obtenida por NTP. Bloquea hasta 10s
// esperando respuesta del servidor. No se usa directamente en el flujo de
// tareas (TaskRTC hace su propia sincronización en línea), pero queda
// disponible para llamarse manualmente si se requiere
void syncRTCFromNTP() {
    // Solo sincroniza si ya hay WiFi conectado
    //if (WiFi.status() != WL_CONNECTED) return;

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    int intentos = 0;
    while (!getLocalTime(&timeinfo) && intentos < 10) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        intentos++;
    }
    if (!getLocalTime(&timeinfo)) return;

    RtcDateTime now(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
    );
    rtc.SetDateTime(now);
    Serial.println("RTC actualizado con hora NTP.");
}

// Traduce los códigos que entrega el Wiegand en modo teclado (tipo 4/8)
// a su tecla correspondiente. '?' indica una tecla no reconocida.
char decodeKeypad(uint32_t code) {
    switch (code) {
        case 0x1B: return '*';
        case 0x00: return '0';
        case 0x0D: return '#';
        case 0x01: return '1';
        case 0x02: return '2';
        case 0x03: return '3';
        case 0x04: return '4';
        case 0x05: return '5';
        case 0x06: return '6';
        case 0x07: return '7';
        case 0x08: return '8';
        case 0x09: return '9';
        default:   return '?';
    }
}

// ==================== LCD (solo llamar desde TaskLCD) ====================
// Estas funciones tocan el objeto lcd directamente y usan vTaskDelay, por
// lo que solo deben invocarse desde el contexto de TaskLCD para no bloquear
// otras tareas ni generar condiciones de carrera sobre el bus I2C

void lcdInicio() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Inserta num.");
    lcd.setCursor(0, 1);
    lcd.print("de control:");
}

// Muestra el código de control que se va acumulando, junto con la hora
void lcdMostrarCodigo(String codigo) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("NC: ");
    lcd.print(codigo);
    lcdMostrarHora();
}

// Pantalla de error (código incompleto). Bloquea 1.5 s dentro de la propia
// tarea de LCD (no afecta a las demás) y regresa a la pantalla de inicio
void lcdError() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Error:");
    lcd.setCursor(0, 1);
    lcd.print("8 digitos req.");
    vTaskDelay(pdMS_TO_TICKS(1500));  // no-blocking en tarea
    lcdInicio();
}

// Pantalla de acceso correcto (código completo o tarjeta válida)
void lcdMensajeCorrecto() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Bienvenido");
    lcdMostrarHora();
    vTaskDelay(pdMS_TO_TICKS(1500));  // no-blocking en tarea
    lcdInicio();
}

// Refresca solo la segunda línea con la hora actual, leída directo del RTC
// (no de queueRTC). Se llama en cada ciclo de TaskLCD.
void lcdMostrarHora() {
    RtcDateTime now = rtc.GetDateTime();
    char horaBuffer[10];
    snprintf(horaBuffer, sizeof(horaBuffer),
        "%02u:%02u:%02u",
        now.Hour(), now.Minute(), now.Second());
    lcd.setCursor(0, 1);
    lcd.print("Hora: ");
    lcd.print(horaBuffer);
}

// Formatea fecha/hora como DD/MM/YYYY HH:MM:SS. Esta es la función realmente
// usada para el campo fecha del JSON enviado por MQTT.
String formatDateTime(const RtcDateTime& dt) {
    char buf[20];
    snprintf_P(buf, sizeof(buf),
        PSTR("%02u/%02u/%04u %02u:%02u:%02u"),
        dt.Day(), dt.Month(), dt.Year(),
        dt.Hour(), dt.Minute(), dt.Second());
    return String(buf);
}

// ==================== MQTT ====================

// Intenta reconectar al broker MQTT sin bloquear indefinidamente: solo lo
// intenta si ya pasaron 5s desde el último intento y si hay WiFi disponible
// Se llama en cada ciclo de TaskMQTT
void reconnectMQTT() {
    static unsigned long lastAttempt = 0;
    unsigned long ahora = millis();
    if (ahora - lastAttempt < 5000) return;   
    lastAttempt = ahora;

    if (client.connected()) return;           

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[MQTT] Sin WiFi, omitiendo reconexión.");
        return;
    }

    Serial.print("Intentando conectar a MQTT... ");
    if (client.connect("ESP32Client", mqtt_user, mqtt_passwd)) {
    Serial.println("Conexión exitosa.");
    client.subscribe(mqtt_topic);   // Se suscribe al mismo tópico donde publica,
                                     // necesario para el mecanismo de "eco/confirmación".
    } else {
        Serial.print("Error rc=");
        Serial.println(client.state());
    }
}

// ==================== TAREAS ====================

// Mantiene la conexión WiFi activa. Si se desconecta, reintenta cada 10 s,
// esperando hasta 10s (20 x 500 ms) por cada intento de conexión.
void TaskWIFI(void *pvParameters) {
    while (1) {
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.begin(ssid, password);
            int intentos = 0;
            while (WiFi.status() != WL_CONNECTED && intentos < 20) {
                vTaskDelay(pdMS_TO_TICKS(500));
                intentos++;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// Gestiona la fuente de tiempo (NTP o RTC) y publica la hora actual en
// queueRTC cada segundo para que otras tareas (principalmente TaskWiegand)
// la consulten sin tener que acceder al bus del RTC directamente
void TaskRTC(void *pvParameters) {
    // Espera inicial de hasta 15s (30 x 500 ms) a que haya WiFi, antes de
    // intentar sincronizar con NTP por primera vez.
    int intentos = 0;
    while (WiFi.status() != WL_CONNECTED && intentos < 30) {
        vTaskDelay(pdMS_TO_TICKS(500));
        intentos++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

        struct tm timeinfo;
        int tries = 0;
        while (!getLocalTime(&timeinfo) && tries < 10) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            tries++;
        }

        if (getLocalTime(&timeinfo)) {
            // Actualizar RTC con hora NTP (para que quede correcto
            // aunque luego se pierda el WiFi y haya que caer a RTC_SRC).
            RtcDateTime ntpNow(
                timeinfo.tm_year + 1900,
                timeinfo.tm_mon  + 1,
                timeinfo.tm_mday,
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec
            );
            rtc.SetDateTime(ntpNow);
            timeSource = TIME_SRC_NTP;
            Serial.println("[TIME] Sincronizado con NTP. Fuente: NTP.");
        } else {
            Serial.println("[TIME] NTP falló. Fuente: RTC.");
        }
    } else {
        Serial.println("[TIME] Sin WiFi. Fuente: RTC.");
    }

    // Bucle principal: cada segundo decide de dónde leer la hora y la
    // publica (con overwrite) en queueRTC
    while (1) {
        RtcDateTime now;

        if (timeSource == TIME_SRC_NTP && WiFi.status() == WL_CONNECTED) {
            // Leer hora directamente del sistema (sincronizada con NTP),
            // más preciso que depender del RTC físico mientras haya WiFi
            struct tm timeinfo;
            if (getLocalTime(&timeinfo)) {
                now = RtcDateTime(
                    timeinfo.tm_year + 1900,
                    timeinfo.tm_mon  + 1,
                    timeinfo.tm_mday,
                    timeinfo.tm_hour,
                    timeinfo.tm_min,
                    timeinfo.tm_sec
                );
            } else {
                // NTP perdió sync (ej. se cayó el WiFi a medias), caer a RTC
                now = rtc.GetDateTime();
                timeSource = TIME_SRC_RTC;
                Serial.println("[TIME] NTP perdió sync. Fuente: RTC.");
            }
        } else {
            // Sin WiFi o fuente ya marcada como RTC: usar el RTC físico
            now = rtc.GetDateTime();
        }

        // Como la cola es de tamaño 1, esto reemplaza el
        // valor anterior en vez de acumular, garantizando que quien lea
        // siempre obtenga la hora más reciente sin llenarse la cola
        xQueueOverwrite(queueRTC, &now);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Tarea principal de captura, muestrea el lector Wiegand cada 10 ms y procesa
// tanto entradas de teclado (tipo 4/8) como tarjetas RFID (26/34 bits)
void TaskWiegand(void *pvParameters) {
    String codigoAcumulado = "";

    while (1) {
        if (wg.available()) {
            EventoAcceso evento;
            evento.code = wg.getCode();
            evento.type = wg.getWiegandType();

            Serial.printf("[Wiegand] code=0x%08X  type=%d\n", evento.code, evento.type);

            if (evento.type == 4 || evento.type == 8) {
                // Teclado 
                char tecla = decodeKeypad(evento.code);

                if (tecla == '*') {
                    // Limpia el buffer y borra la pantalla de código
                    codigoAcumulado = "";
                    LCDMessage lcdMsg = { LCD_CODIGO, "" };
                    xQueueSend(queueLCD, &lcdMsg, 0);
                    Serial.println("[Wiegand] Entrada cancelada (*).");

                } else if (tecla == '#') {
                    // Solo procede si se acumularon exactamente 8 dígitos
                    if (codigoAcumulado.length() == 8) {
                        LCDMessage lcdMsg = { LCD_OK, "" };
                        xQueueSend(queueLCD, &lcdMsg, 0);

                        // Intenta tomar la hora más reciente de queueRTC (peek,
                        // no la saca de la cola); si no llega a tiempo (200 ms)
                        // o la cola está vacía, cae a leer el RTC directamente
                        RtcDateTime now;
                        if (xQueuePeek(queueRTC, &now, pdMS_TO_TICKS(200)) != pdTRUE) {
                            now = rtc.GetDateTime();
                            Serial.println("[Wiegand] queueRTC vacia, leyendo RTC directo.");
                        }

                        // Arma el JSON del evento: id = código + código de edificio
                        JsonDocument doc;
                        doc["id"]           = codigoAcumulado + buildingCode;
                        doc["direccionMAC"] = stMac;
                        doc["fecha"]        = formatDateTime(now);

                        // ---> INICIO DE ENRUTAMIENTO (TECLADO) <---
                        if (WiFi.status() == WL_CONNECTED && client.connected()) {
                            // Flujo normal: enviar a queueMQTT
                            MQTTMessage mqttMsg;
                            serializeJson(doc, mqttMsg.payload, sizeof(mqttMsg.payload));
                            xQueueSend(queueMQTT, &mqttMsg, 0);
                            Serial.printf("[Wiegand] Codigo a MQTT: %s\n", mqttMsg.payload);
                        } else {
                            // Flujo offline: enviar a queueSD en formato CSV
                            SDMessage sdMsg;
                            snprintf(sdMsg.csvLinea, sizeof(sdMsg.csvLinea), "%s,%s,%s\n", 
                                     doc["id"].as<const char*>(), 
                                     stMac.c_str(), 
                                     doc["fecha"].as<const char*>());
                                     
                            xQueueSend(queueSD, &sdMsg, 0);
                            Serial.println("[Wiegand] Codigo a SD (Offline)");
                        }
                        // ---> FIN DE ENRUTAMIENTO (TECLADO) <---

                    } else {
                        // Código incompleto, avisa error en pantalla
                        LCDMessage lcdMsg = { LCD_ERROR, "" };
                        xQueueSend(queueLCD, &lcdMsg, 0);
                        Serial.printf("[Wiegand] Error: %d digitos (se requieren 8).\n",
                                      codigoAcumulado.length());
                    }
                    codigoAcumulado = "";   // Se reinicia tanto en éxito como en error

                } else if (tecla != '?') {
                    // Dígito válido, se acumula y se refleja en pantalla
                    codigoAcumulado += tecla;
                    Serial.printf("[Wiegand] Acumulado: %s\n", codigoAcumulado.c_str());

                    LCDMessage lcdMsg;
                    lcdMsg.tipo = LCD_CODIGO;
                    strncpy(lcdMsg.texto, codigoAcumulado.c_str(), sizeof(lcdMsg.texto) - 1);
                    lcdMsg.texto[sizeof(lcdMsg.texto) - 1] = '\0';
                    xQueueSend(queueLCD, &lcdMsg, 0);
                }
                // Si tecla == '?', se ignora (tecla no reconocida).

            } else {
                // Tarjeta RFID (Wiegand 26/34)
                // Mismo patrón de obtención de hora que en el caso de teclado
                RtcDateTime now;
                if (xQueuePeek(queueRTC, &now, pdMS_TO_TICKS(200)) != pdTRUE) {
                    now = rtc.GetDateTime();
                    Serial.println("[Wiegand] queueRTC vacia, leyendo RTC directo.");
                }

                // Para tarjetas, el id se arma con "0" + el código leído del Wiegand
                // (sin el buildingCode que sí se usa en el flujo de teclado)
                JsonDocument doc;
                doc["id"]           = "0" + String(evento.code);
                doc["direccionMAC"] = stMac;
                doc["fecha"]        = formatDateTime(now);

                // ---> INICIO DE ENRUTAMIENTO (TARJETA) <---
                if (WiFi.status() == WL_CONNECTED && client.connected()) {
                    // Flujo normal: enviar a queueMQTT
                    MQTTMessage mqttMsg;
                    serializeJson(doc, mqttMsg.payload, sizeof(mqttMsg.payload));
                    xQueueSend(queueMQTT, &mqttMsg, 0);
                    Serial.printf("[Wiegand] Tarjeta a MQTT: %s\n", mqttMsg.payload);
                } else {
                    // Flujo offline: enviar a queueSD en formato CSV
                    SDMessage sdMsg;
                    snprintf(sdMsg.csvLinea, sizeof(sdMsg.csvLinea), "%s,%s,%s\n", 
                             doc["id"].as<const char*>(), 
                             stMac.c_str(), 
                             doc["fecha"].as<const char*>());
                             
                    xQueueSend(queueSD, &sdMsg, 0);
                    Serial.println("[Wiegand] Tarjeta a SD (Offline)");
                }
                // ---> FIN DE ENRUTAMIENTO (TARJETA) <---

                LCDMessage lcdMsg = { LCD_OK, "" };
                xQueueSend(queueLCD, &lcdMsg, 0);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

// Consume queueLCD y actualiza la pantalla según el tipo de mensaje recibido
// Si no llega nada en 500 ms, igual refresca la hora (para que el reloj en
// pantalla no se quede congelado mientras no hay eventos)
void TaskLCD(void *pvParameters) {
    LCDMessage msg;

    while (1) {
        if (xQueueReceive(queueLCD, &msg, pdMS_TO_TICKS(500))) {
            switch (msg.tipo) {
                case LCD_CODIGO:  lcdMostrarCodigo(msg.texto); break;
                case LCD_ERROR:   lcdError();                  break;
                case LCD_OK:      lcdMensajeCorrecto();        break;
            }
        }
        lcdMostrarHora();
    }
}

// Gestiona la conexión MQTT, publica los mensajes que llegan por queueMQTT,
// y controla el timeout de confirmación (ver mqttCallback más abajo)
void TaskMQTT(void *pvParameters) {
    MQTTMessage msg;

    espClient.setTimeout(3);
    client.setServer(mqtt_server, mqtt_port);
    client.setKeepAlive(15);
    client.setSocketTimeout(3);
    client.setBufferSize(512);        // Margen para tópico + payload (más que
                                       // los 200 bytes del payload en sí)
    client.setCallback(mqttCallback); // Se dispara cuando llega un mensaje al
                                       // tópico suscrito (el mismo donde se publica)

    while (1) {
        if (!client.connected()) {
            reconnectMQTT();
        }
        client.loop();   // Procesa mensajes entrantes y mantiene el keep-alive

        if (xQueueReceive(queueMQTT, &msg, pdMS_TO_TICKS(50))) {
            if (client.connected()) {
                client.publish(mqtt_topic, msg.payload);

                // Guarda el último payload enviado y arranca el cronómetro
                // de espera de confirmación (ver mqttCallback)
                strncpy(ultimoPayloadEnviado, msg.payload, sizeof(ultimoPayloadEnviado) - 1);
                ultimoPayloadEnviado[sizeof(ultimoPayloadEnviado) - 1] = '\0';
                esperandoConfirmacion = true;
                tiempoEnvio = millis();

                Serial.printf("[MQTT] Publicado: %s\n", msg.payload);
            }
            // Si no está conectado, el mensaje se pierde (no se reintenta ni
            // se vuelve a encolar). Punto a considerar si se necesita
            // garantizar entrega
        }

        // Si pasó el timeout sin recibir el eco de confirmación, solo se
        // loggea una advertencia; no hay reintento automático de publicación
        if (esperandoConfirmacion && (millis() - tiempoEnvio > TIMEOUT_CONFIRMACION_MS)) {
            Serial.println("[MQTT] Advertencia: sin confirmacion del ultimo mensaje publicado.");
            esperandoConfirmacion = false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Callback de PubSubClient: se ejecuta dentro de client.loop() (contexto de
// TaskMQTT) cada vez que llega un mensaje al tópico suscrito. Como el
// dispositivo está suscrito al mismo tópico donde publica, esto funciona
// como una confirmación de que el broker distribuyó el mensaje
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char mensaje[200];
    size_t len = (length < sizeof(mensaje) - 1) ? length : sizeof(mensaje) - 1;
    memcpy(mensaje, payload, len);
    mensaje[len] = '\0';

    if (esperandoConfirmacion && strcmp(mensaje, ultimoPayloadEnviado) == 0) {
        esperandoConfirmacion = false;
        Serial.println("[MQTT] Confirmacion recibida: el broker distribuyo el mensaje.");
    }
}

void TaskSD(void *pvParameters) {
    SDMessage msg;
    while(1) {
        // portMAX_DELAY hace que la tarea no consuma CPU mientras la cola esté vacía
        if (xQueueReceive(queueSD, &msg, portMAX_DELAY)) {
            
            // Tomar el Mutex antes de tocar la SD
            if (xSemaphoreTake(mutexSD, pdMS_TO_TICKS(1000)) == pdTRUE) {
                File file = SD.open("/registros.csv", FILE_APPEND);
                if (file) {
                    file.print(msg.csvLinea);
                    file.close();
                    Serial.println("[SD] Registro guardado offline.");
                } else {
                    Serial.println("[SD] Error abriendo archivo.");
                }
                xSemaphoreGive(mutexSD); // Liberar el Mutex
            }
        }
    }
}