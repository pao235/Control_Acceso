#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Wiegand.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include <time.h>
#include <LiquidCrystal_I2C.h>
#include <PubSubClient.h>
#include <ArduinoJSON.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

const char* ssid = "INFINITUM40CE";
const char* password = "Vxs78tcu7z";
const char* ntpServer = "mx.pool.ntp.org";
const long gmtOffset_sec = -21600;
const int daylightOffset_sec = 0;

const char* mqtt_server = "meztli.ddns.net";
const int mqtt_port = 1883;
const char* mqtt_topic = "esp32/control/numeros";
const char* mqtt_user = "usuario";
const char* mqtt_passwd = "a12345";

String stMac = "";
String formatDateTime(const RtcDateTime& dt);

WiFiClient espClient;
PubSubClient client(espClient);

ThreeWire myWire(4, 15, 23);
RtcDS1302<ThreeWire> rtc(myWire);

WIEGAND wg;
LiquidCrystal_I2C lcd(0x27, 16, 2);

const String buildingCode = "1";
String codigoAcumulado = "";

#define countof(a) (sizeof(a) / sizeof(a[0]))

// Structs y enums

typedef struct {
    uint32_t code;
    int      type;
} EventoAcceso;

typedef struct {
    char payload[200];
} MQTTMessage;

typedef enum {
    LCD_CODIGO,
    LCD_ERROR,
    LCD_OK
} LCDTipo;

typedef struct {
    LCDTipo tipo;
    char    texto[32];
} LCDMessage;

typedef enum {
    TIME_SRC_NTP,
    TIME_SRC_RTC
} TimeSource;

volatile TimeSource timeSource = TIME_SRC_RTC;

// Estado de confirmación (uso exclusivo de TaskMQTT)
volatile bool esperandoConfirmacion = false;
char ultimoPayloadEnviado[200];
unsigned long tiempoEnvio = 0;
const unsigned long TIMEOUT_CONFIRMACION_MS = 5000;

// Handles de colas

QueueHandle_t queueWiegand;
QueueHandle_t queueMQTT;
QueueHandle_t queueLCD;
QueueHandle_t queueRTC;

// Prototipos

void printDateTime(const RtcDateTime& dt, char* buffer, size_t length);
void syncRTCFromNTP();
char decodeKeypad(uint32_t code);
void lcdInicio();
void lcdMostrarCodigo(String codigo);
void lcdError();
void lcdMensajeCorrecto();
void lcdMostrarHora();
void reconnectMQTT();

void enviarJson(const String& id, const RtcDateTime& dt);

void TaskLCD(void *pvParameters);
void TaskWiegand(void *pvParameters);
void TaskRTC(void *pvParameters);
void TaskMQTT(void *pvParameters);
void TaskWIFI(void *pvParameters);

void mqttCallback(char* topic, byte* payload, unsigned int length);



// Setup

void setup()
{
    Serial.begin(115200);
    Wire.begin();
    rtc.Begin();
    wg.begin(16, 17); 

    lcd.init();
    lcd.backlight();

    stMac = WiFi.macAddress();
    Serial.print("MAC: ");
    Serial.println(stMac);

    queueWiegand = xQueueCreate(10, sizeof(EventoAcceso));
    queueMQTT    = xQueueCreate(10, sizeof(MQTTMessage));
    queueLCD     = xQueueCreate(10, sizeof(LCDMessage));
    queueRTC     = xQueueCreate(1,  sizeof(RtcDateTime));

    xTaskCreatePinnedToCore(TaskWIFI,    "WiFi",    4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(TaskMQTT,    "MQTT",    4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(TaskWiegand, "Wiegand", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(TaskLCD,     "LCD",     4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(TaskRTC,     "RTC",     4096, NULL, 1, NULL, 0);
}

void loop() {}

// Utilidades

void printDateTime(const RtcDateTime& dt, char* buffer, size_t length) {
    snprintf_P(buffer, length,
        PSTR("%02u/%02u/%04u %02u:%02u:%02u"),
        dt.Month(), dt.Day(), dt.Year(),
        dt.Hour(), dt.Minute(), dt.Second());
}

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

// LCD (solo llamar desde TaskLCD)

void lcdInicio() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Inserta num.");
    lcd.setCursor(0, 1);
    lcd.print("de control:");
}

void lcdMostrarCodigo(String codigo) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("NC: ");
    lcd.print(codigo);
    lcdMostrarHora();
}

void lcdError() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Error:");
    lcd.setCursor(0, 1);
    lcd.print("8 digitos req.");
    vTaskDelay(pdMS_TO_TICKS(1500));  // no-blocking en tarea
    lcdInicio();
}

void lcdMensajeCorrecto() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Bienvenido");
    lcdMostrarHora();
    vTaskDelay(pdMS_TO_TICKS(1500));  // no-blocking en tarea
    lcdInicio();
}

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

String formatDateTime(const RtcDateTime& dt) {
    char buf[20];
    snprintf_P(buf, sizeof(buf),
        PSTR("%02u/%02u/%04u %02u:%02u:%02u"),
        dt.Day(), dt.Month(), dt.Year(),
        dt.Hour(), dt.Minute(), dt.Second());
    return String(buf);
}

// MQTT

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
    client.subscribe(mqtt_topic);
    } else {
        Serial.print("Error rc=");
        Serial.println(client.state());
    }
}

// Tareas

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

void TaskRTC(void *pvParameters) {
    // Esperar WiFi hasta 15 segundos
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
            // Actualizar RTC físico con hora NTP
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

    while (1) {
        RtcDateTime now;

        if (timeSource == TIME_SRC_NTP && WiFi.status() == WL_CONNECTED) {
            // Leer hora directamente del sistema (sincronizada con NTP)
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
                // NTP perdió sync, caer a RTC
                now = rtc.GetDateTime();
                timeSource = TIME_SRC_RTC;
                Serial.println("[TIME] NTP perdió sync. Fuente: RTC.");
            }
        } else {
            now = rtc.GetDateTime();
        }

        xQueueOverwrite(queueRTC, &now);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void TaskWiegand(void *pvParameters) {
    String codigoAcumulado = "";

    while (1) {
        if (wg.available()) {
            EventoAcceso evento;
            evento.code = wg.getCode();
            evento.type = wg.getWiegandType();

            Serial.printf("[Wiegand] code=0x%08X  type=%d\n", evento.code, evento.type);

            if (evento.type == 4 || evento.type == 8) {
                // ── Teclado ──────────────────────────────────────────────
                char tecla = decodeKeypad(evento.code);

                if (tecla == '*') {
                    codigoAcumulado = "";
                    LCDMessage lcdMsg = { LCD_CODIGO, "" };
                    xQueueSend(queueLCD, &lcdMsg, 0);
                    Serial.println("[Wiegand] Entrada cancelada (*).");

                } else if (tecla == '#') {
                    if (codigoAcumulado.length() == 8) {
                        LCDMessage lcdMsg = { LCD_OK, "" };
                        xQueueSend(queueLCD, &lcdMsg, 0);

                        RtcDateTime now;
                        if (xQueuePeek(queueRTC, &now, pdMS_TO_TICKS(200)) != pdTRUE) {
                            now = rtc.GetDateTime();
                            Serial.println("[Wiegand] queueRTC vacia, leyendo RTC directo.");
                        }

                        JsonDocument doc;
                        doc["id"]           = codigoAcumulado + buildingCode;
                        doc["direccionMAC"] = stMac;
                        doc["fecha"]        = formatDateTime(now);

                        MQTTMessage mqttMsg;
                        serializeJson(doc, mqttMsg.payload, sizeof(mqttMsg.payload));
                        xQueueSend(queueMQTT, &mqttMsg, 0);

                        Serial.printf("[Wiegand] Codigo enviado: %s\n", mqttMsg.payload);
                    } else {
                        LCDMessage lcdMsg = { LCD_ERROR, "" };
                        xQueueSend(queueLCD, &lcdMsg, 0);
                        Serial.printf("[Wiegand] Error: %d digitos (se requieren 8).\n",
                                      codigoAcumulado.length());
                    }
                    codigoAcumulado = "";

                } else if (tecla != '?') {
                    codigoAcumulado += tecla;
                    Serial.printf("[Wiegand] Acumulado: %s\n", codigoAcumulado.c_str());

                    LCDMessage lcdMsg;
                    lcdMsg.tipo = LCD_CODIGO;
                    strncpy(lcdMsg.texto, codigoAcumulado.c_str(), sizeof(lcdMsg.texto) - 1);
                    lcdMsg.texto[sizeof(lcdMsg.texto) - 1] = '\0';
                    xQueueSend(queueLCD, &lcdMsg, 0);
                }

            } else {
                // ── Tarjeta RFID (Wiegand 26 / 34) ───────────────────────
                RtcDateTime now;
                if (xQueuePeek(queueRTC, &now, pdMS_TO_TICKS(200)) != pdTRUE) {
                    now = rtc.GetDateTime();
                    Serial.println("[Wiegand] queueRTC vacia, leyendo RTC directo.");
                }

                JsonDocument doc;
                doc["id"]           = "0" + String(evento.code);
                doc["direccionMAC"] = stMac;
                doc["fecha"]        = formatDateTime(now);

                MQTTMessage mqttMsg;
                serializeJson(doc, mqttMsg.payload, sizeof(mqttMsg.payload));
                xQueueSend(queueMQTT, &mqttMsg, 0);

                LCDMessage lcdMsg = { LCD_OK, "" };
                xQueueSend(queueLCD, &lcdMsg, 0);

                Serial.printf("[Wiegand] Tarjeta: %s\n", mqttMsg.payload);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

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

void TaskMQTT(void *pvParameters) {
    MQTTMessage msg;

    espClient.setTimeout(3);
    client.setServer(mqtt_server, mqtt_port);
    client.setKeepAlive(15);
    client.setSocketTimeout(3);
    client.setBufferSize(512);        // ← nuevo: margen para tópico + payload
    client.setCallback(mqttCallback); // ← nuevo

    while (1) {
        if (!client.connected()) {
            reconnectMQTT();
        }
        client.loop();

        if (xQueueReceive(queueMQTT, &msg, pdMS_TO_TICKS(50))) {
            if (client.connected()) {
                client.publish(mqtt_topic, msg.payload);

                strncpy(ultimoPayloadEnviado, msg.payload, sizeof(ultimoPayloadEnviado) - 1);
                ultimoPayloadEnviado[sizeof(ultimoPayloadEnviado) - 1] = '\0';
                esperandoConfirmacion = true;
                tiempoEnvio = millis();

                Serial.printf("[MQTT] Publicado: %s\n", msg.payload);
            }
        }

        // ← nuevo: aviso si no llegó confirmación a tiempo
        if (esperandoConfirmacion && (millis() - tiempoEnvio > TIMEOUT_CONFIRMACION_MS)) {
            Serial.println("[MQTT] Advertencia: sin confirmacion del ultimo mensaje publicado.");
            esperandoConfirmacion = false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

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