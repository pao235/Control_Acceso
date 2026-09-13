/***************************************************************************************************************************
 * FileName:     main.cpp
 * Processor:    Tensilica Xtensa LX6 160 MHz
 * Board:        ESP32 Dev Module
 * Company:      TecNM /IT Chihuahua
 * Description:  Control Access with ESP32, RFID, RTC DS1302, LCD I2C, MQTT and NTP
 * Authors:      Ana Cardona, Emiliano Pérez, Luis Anchondo
 * Updated:      05/2026
 * Created on:   15 jun. 2026
 * updated:      13/09/2026
 **************************************************************************************************************************/
/**************************************************************************************************************************
 * * Copyright (C) 2026 by Ana Cardona, Emiliano Pérez, Luis Anchondo - TecNM /IT Chihuahua
 *
 * Se permite la redistribucion, modificacion o uso de este software en formato fuente o binario
 * siempre que los archivos mantengan estos derechos de autor.
 * Los usuarios pueden modificar esto y usarlo para aprender sobre el campo de IoT, control de acceso, microcontroladores 
 * y sistemas embebidos. Se permite el uso de este software para fines educativos y de investigación.
 * Ana Cardona, Emiliano Pérez, Luis Anchondo y el TecNM /IT Chihuahua no son responsables del mal uso de este material.
 **************************************************************************************************************************/

 /*****************************************
  *          LIBRERÍAS USADAS
  *****************************************/
#include <Arduino.h>
#include <Wire.h>              // Maneja la comunicación I2C (usada por la pantalla LCD)
#include <WiFi.h>              // Funciones específicas para controlar el módulo de reloj
#include <Wiegand.h>           // Lectura de lector Wiegand (teclado + RFID)
#include <ThreeWire.h>         // Interfaz de 3 hilos para el RTC DS1302
#include <RtcDS1302.h>
#include <time.h>              // Para funciones de hora vía NTP (configTime, getLocalTime)
#include <LiquidCrystal_I2C.h>
#include <PubSubClient.h>      // Cliente MQTT para conectarse al broker, publicar y suscribirse
#include <ArduinoJSON.h>       // Permite construir y leer estructuras JSON (el formato de los mensajes)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <SD.h>                // Sistema de archivos para leer y crear archivos (como el .csv)
#include <SPI.h>               // Maneja el bus de comunicación SPI que utiliza el lector MicroSD



// ==================== CONFIGURACIÓN DE RED Y SERVICIOS ====================
// Credenciales para la conexión a la red WiFi local
const char* ssid = "FAM_VALENZUELA";
const char* password = "6143973503valenzuela";
// Servidor NTP para sincronizar la hora por internet
const char* ntpServer = "mx.pool.ntp.org";
// Desplazamiento horario, -21600 segundos equivale a UTC-6 (Zona horaria de Chihuahua)
const long gmtOffset_sec = -21600;     
// Ajuste para horario de verano (0 porque en Chihuahua ya no se aplica)
const int daylightOffset_sec = 0;

// ======================= BLOQUE DEL BROKER PRIVADO ==========================
const char* mqtt_server = "meztli.ddns.net";
const int mqtt_port = 1883;
const char* mqtt_topic = "control"; // Canal donde el ESP32 publicará los registros
const char* mqtt_user = "usuario";
const char* mqtt_passwd = "a12345";

/*// ======================= BLOQUE DEL BROKER DE PRUEBAS ======================
const char* mqtt_server = "mqtt-dashboard.com"; // Servidor público gratuito para pruebas
const int mqtt_port = 1883;                     // Puerto estándar para MQTT
const char* mqtt_topic = "esp32/control/numeros"; // Canal donde el ESP32 publicará los registros
const char* mqtt_user = "";                     // Sin usuario para este broker de prueba
const char* mqtt_passwd = "";                   // Sin contraseña para este broker de prueba*/


// ==================== OBJETOS DE PERIFÉRICOS ====================
WiFiClient espClient; // Crea un cliente de red básico usando la antena WiFi del ESP32
PubSubClient client(espClient); // Crea el cliente MQTT. Toma el espClient de red para poder enviar y recibir mensajes del broker

ThreeWire myWire(4, 15, 25);       // Pines DAT, CLK, RST del RTC DS1302
RtcDS1302<ThreeWire> rtc(myWire); // Inicializa el objeto principal del reloj usando los pines declarados arriba

WIEGAND wg;                        // Lector Wiegand (teclado matricial + RFID)
LiquidCrystal_I2C lcd(0x27, 16, 2);

const String buildingCode = "1"; // Identificador del edificio donde vino el registro 
String codigoAcumulado = "";     // Para acumular los dígitos que se van ingresando desde el teclado Wiegand     

// Para calcular el tamaño de un arreglo
#define countof(a) (sizeof(a) / sizeof(a[0]))

// ==================== STRUCTS Y ENUMS ====================
// Guarda el número leído (code) y de qué tipo es (type = 4 u 8 bits para teclas, 26 o 34 bits para tarjetas RFID)
// Actualmente no se usa como tal para pasar porcola (el envío a MQTT se arma directo en TaskWiegand),
// pero queda definido
typedef struct {
    uint32_t code;
    int      type;
} EventoAcceso;

// Paquete que viaja por la cola queueMQTT. Contiene un arreglo de 200 letras
// donde guardamos el mensaje JSON ya formateado para internet
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

// Fuente de tiempo activa, NTP (mientras haya WiFi) o RTC físico como respaldo
typedef enum {
    TIME_SRC_NTP,
    TIME_SRC_RTC
} TimeSource;

// Paquete que viaja por la cola queueSD. Contiene una línea de texto de 100 caracteres lista para guardarse en el 
// archivo CSV de la memoria (ej. "ID,MAC,FECHA\n")
typedef struct {
    char csvLinea[100]; 
} SDMessage;


// ==================== ESTADO DE CONFIRMACIÓN MQTT ====================
// Como la librería PubSubClient usa calidad de servicio 0 (QoS 0), envía los datos
// pero el servidor no le responde un Recibido. Para solucionar esto, el ESP32
// se suscribe a su propio canal. Si envía un mensaje y luego lo recibe de 
// vuelta (un eco), sabe que el mensaje sí llegó al servidor

// Bandera para saber si el ESP32 está esperando ese eco del servidor
volatile bool esperandoConfirmacion = false;

// Búfer (memoria temporal) para guardar una copia del último mensaje enviado,
// así podemos compararlo con el eco cuando regrese.
char ultimoPayloadEnviado[200];

// Guarda el momento exacto (en milisegundos) en que se envió el mensaje
unsigned long tiempoEnvio = 0;

// Tiempo máximo de espera para recibir el eco
const unsigned long TIMEOUT_CONFIRMACION_MS = 5000;

// ==================== VARIABLES GLOBALES ====================

// Variable global para guardar la dirección MAC del ESP32 (sirve como ID único del dispositivo)
// Se inicializa en blanco y se le asigna su valor una sola vez en el setup()
String stMac = ""; 

// Variable global que indica si estamos usando la hora de internet (NTP) o del módulo físico (RTC)
volatile TimeSource timeSource = TIME_SRC_RTC;   // Se actualiza solo desde TaskRTC

// ==================== COLAS (comunicación entre tareas) ====================
QueueHandle_t queueWiegand;   // Declarada pero no usada actualmente en el flujo mostrado
// Buzón donde la tarea de lectura deja el JSON; la tarea MQTT lo recoge y lo envía a internet
QueueHandle_t queueMQTT;      
// Buzón donde la tarea de lectura deja las instrucciones; la tarea LCD las recoge y actualiza la pantalla
QueueHandle_t queueLCD;       
// Buzón especial de tamaño 1. La tarea del reloj pone aquí la hora más reciente, y cualquier otra tarea la puede leer
QueueHandle_t queueRTC;       
// Buzón donde la tarea de lectura deja el texto CSV para que la tarea SD lo guarde físicamente
QueueHandle_t queueSD;
SemaphoreHandle_t mutexSD; // Protege el acceso a la memoria

// ==================== PROTOTIPOS DE FUNCIONES ====================

/**
 * @brief  Formatea un objeto de tiempo en una cadena de texto legible
 *         Toma las propiedades del objeto RtcDateTime (día, mes, año,
 *         hora, minuto y segundo) y las formatea bajo el estándar
 *         DD/MM/YYYY HH:MM:SS utilizando un buffer temporal.
 *
 * @param  dt      Referencia constante al objeto RtcDateTime a formatear
 * @return String  Cadena de texto con la fecha y hora lista para el JSON
 */
String formatDateTime(const RtcDateTime& dt);


/**
 * @brief  Sincroniza el RTC físico con la hora obtenida por NTP
 *         Se conecta al servidor NTP configurado, intenta obtener la hora 
 *         local (bloqueando la ejecución hasta 10 segundos) y actualiza 
 *         el módulo RTC DS1302 con los datos recibidos.
 *
 * @param  Ninguno
 * @return void
 */
void syncRTCFromNTP();


/**
 * @brief  Traduce los códigos que entrega el lector Wiegand a caracteres
 *         Recibe el código en formato de 4 u 8 bits y devuelve la tecla
 *         correspondiente en formato ASCII. Si el código no está en la
 *         lista, devuelve un carácter de error ('?').
 *
 * @param  code  Código numérico de 32 bits leído por el protocolo Wiegand
 * @return char  Carácter ASCII de la tecla presionada ('0'-'9', '*', '#') o '?' si es inválido
 */
char decodeKeypad(uint32_t code);


/**
 * @brief  Muestra la pantalla inicial de inserción de código
 *         Limpia el display LCD y coloca el texto de bienvenida solicitando 
 *         al usuario que inserte su número de control. Debe ser llamada 
 *         exclusivamente por la tarea del LCD.
 *
 * @param  Ninguno
 * @return void
 */
void lcdInicio();


/**
 * @brief  Muestra en pantalla el código de control que se va acumulando
 *         Limpia la pantalla LCD, muestra el texto "NC: " seguido del 
 *         código acumulado y la hora actual. Debe ser llamada exclusivamente 
 *         por la tarea del LCD.
 *
 * @param  codigo  Cadena de texto con los dígitos ingresados hasta el momento
 * @return void
 */
void lcdMostrarCodigo(String codigo);


/**
 * @brief  Muestra una advertencia en pantalla por código incompleto
 *         Limpia la pantalla LCD, muestra un mensaje indicando que faltan 
 *         dígitos, bloquea la tarea de la pantalla por 1.5 segundos (sin 
 *         afectar el resto del sistema) y regresa a la pantalla de inicio.
 *
 * @param  Ninguno
 * @return void
 */
void lcdError();


/**
 * @brief  Muestra un mensaje de éxito en pantalla
 *         Limpia la pantalla LCD, muestra un mensaje indicando que el código es válido
 *         y bloquea la tarea de la pantalla por 1.5 segundos (sin afectar el resto del sistema).
 *
 * @param  Ninguno
 * @return void
 */
void lcdMensajeCorrecto();

/**
 * @brief  Muestra la hora actual en pantalla
 *         Limpia la pantalla LCD y muestra la hora actual del RTC.
 *
 * @param  Ninguno
 * @return void
 */
void lcdMostrarHora();


/**
 * @brief  Intenta reconectar el dispositivo al broker MQTT
 *         Verifica si hay conexión WiFi y si han pasado al menos 5 segundos 
 *         desde el último intento. Si las condiciones se cumplen, intenta 
 *         conectar al broker y llama a sincronizar los datos offline.
 *
 * @param  Ninguno
 * @return void
 */
void reconnectMQTT();


/**
 * @brief  Envía por MQTT los registros guardados en la MicroSD
 *         Toma el control del Mutex de la tarjeta SD, abre el archivo CSV
 *         donde se guardaron los accesos fuera de línea, lee cada línea,
 *         la transforma a formato JSON y la publica en el broker MQTT.
 *         Al finalizar con éxito, elimina el archivo CSV.
 *
 * @param  Ninguno
 * @return void
 */
void sincronizarDatosOffline();


/**
 * @brief  Función de respuesta (callback) del cliente MQTT
 *         Se ejecuta internamente cada vez que llega un mensaje al tópico 
 *         suscrito. Compara el payload recibido con el último enviado para 
 *         validar si el broker distribuyó exitosamente el dato (eco).
 *
 * @param  topic    Cadena con el nombre del tópico que recibió el mensaje
 * @param  payload  Puntero al arreglo de bytes recibidos (contenido del mensaje)
 * @param  length   Longitud total del mensaje recibido en bytes
 * @return void
 */
void mqttCallback(char* topic, byte* payload, unsigned int length);


// ==================== PROTOTIPOS DE TAREAS DE FREERTOS ====================

/**
 * @brief  Controla la visualización de datos en la pantalla I2C
 *         Se mantiene a la espera de mensajes a través de su propia cola. 
 *         Dependiendo del tipo de instrucción recibida, muestra el código 
 *         tecleado, advertencias o confirmación, y siempre actualiza la hora.
 *
 * @param  pvParameters  Puntero a parámetros de FreeRTOS (no utilizado)
 * @return void          Las tareas de FreeRTOS no retornan valores
 */
void TaskLCD(void *pvParameters);


/**
 * @brief  Tarea de FreeRTOS para la lectura del periférico Wiegand
 *         Se ejecuta en un bucle infinito muestreando el lector cada 10 ms.
 *         Acumula los dígitos si es teclado (hasta 8) o lee la tarjeta RFID.
 *         Empaqueta la información con la fecha/hora del RTC y la envía a 
 *         la cola MQTT (si hay red) o a la SD (si está offline).
 *
 * @param  pvParameters  Puntero a parámetros de FreeRTOS (típicamente NULL, no utilizado)
 * @return void          Las tareas de FreeRTOS no retornan valores
 */
void TaskWiegand(void *pvParameters);


/**
 * @brief  Gestiona y provee la fuente de tiempo del sistema
 *         Intenta sincronizar la hora por servidor NTP si hay WiFi. 
 *         Determina si la fuente activa es NTP o el RTC físico, y publica 
 *         la hora exacta cada segundo en una cola que reemplaza su valor.
 *
 * @param  pvParameters  Puntero a parámetros de FreeRTOS (no utilizado)
 * @return void          Las tareas de FreeRTOS no retornan valores
 */
void TaskRTC(void *pvParameters);


/**
 * @brief  Administra la comunicación de datos hacia el broker
 *         Mantiene activa la conexión MQTT, extrae los paquetes JSON de la 
 *         cola y los publica. También gestiona un temporizador para 
 *         confirmar (vía eco) si el broker recibió el mensaje correctamente.
 *
 * @param  pvParameters  Puntero a parámetros de FreeRTOS (no utilizado)
 * @return void          Las tareas de FreeRTOS no retornan valores
 */
void TaskMQTT(void *pvParameters);


/**
 * @brief  Tarea de FreeRTOS encargada de la conexión de red
 *         Monitorea constantemente el estado de la conexión WiFi. Si 
 *         detecta una desconexión, realiza múltiples intentos con retardos 
 *         para reconectar al SSID proporcionado utilizando las credenciales.
 *
 * @param  pvParameters  Puntero a parámetros de FreeRTOS (no utilizado)
 * @return void          Las tareas de FreeRTOS no retornan valores
 */
void TaskWIFI(void *pvParameters);


/**
 * @brief  Almacena de manera persistente los registros sin conexión
 *         Espera cadenas de texto en formato CSV a través de su cola. Cuando 
 *         recibe una, toma de manera segura el Mutex de la MicroSD, abre el 
 *         archivo en modo de adición (append) y guarda el nuevo registro.
 *
 * @param  pvParameters  Puntero a parámetros de FreeRTOS (no utilizado)
 * @return void          Las tareas de FreeRTOS no retornan valores
 */
void TaskSD(void *pvParameters);



// ==================== SETUP ====================

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

// ==================== LCD (solo llamar desde TaskLCD) ====================

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
    vTaskDelay(pdMS_TO_TICKS(1500)); 
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

// ==================== MQTT ====================

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
    
    // String clientId = "ControlAcceso-" + stMac;
    
    if (client.connect("ESP32Client", mqtt_user, mqtt_passwd)) {
        Serial.println("Conexión exitosa.");
        client.subscribe(mqtt_topic);

        // Llamada a la función de sincronización de datos offline después de reconectar
        sincronizarDatosOffline();

    } else {
        Serial.print("Error rc=");
        Serial.println(client.state());
    }
}

void sincronizarDatosOffline() {
    // Intentamos tomar el Mutex. Si falla, salimos inmediatamente
    if (xSemaphoreTake(mutexSD, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("[SD] Mutex ocupado, reintentando en el próximo ciclo.");
        return; 
    }

    // Si no hay archivo, devolvemos la llave y salimos (Retorno temprano)
    if (!SD.exists("/registros.csv")) {
        xSemaphoreGive(mutexSD);
        return; 
    }

    Serial.println("[SD] Archivo encontrado. Sincronizando datos...");
    File file = SD.open("/registros.csv", FILE_READ);
    
    // Si el archivo falló al abrirse, devolvemos la llave y salimos
    if (!file) {
        Serial.println("[SD] Error al abrir archivo para lectura.");
        xSemaphoreGive(mutexSD);
        return;
    }

    // Ciclo de lectura
    while (file.available()) {
        String linea = file.readStringUntil('\n');
        linea.trim(); // Limpiamos basura
        
        // Si la línea está en blanco, saltamos a la siguiente vuelta del ciclo (continue)
        if (linea.length() == 0) continue; 
        
        int primeraComa = linea.indexOf(',');
        int segundaComa = linea.indexOf(',', primeraComa + 1);
        
        // Validamos que la línea tenga exactamente el formato esperado
        if (primeraComa > 0 && segundaComa > 0) {
            String idOff    = linea.substring(0, primeraComa);
            String macOff   = linea.substring(primeraComa + 1, segundaComa);
            String fechaOff = linea.substring(segundaComa + 1);

            JsonDocument doc;
            doc["id"]           = idOff;
            doc["direccionMAC"] = macOff;
            doc["fecha"]        = fechaOff;

            char payloadOffline[200];
            serializeJson(doc, payloadOffline, sizeof(payloadOffline));

            client.publish(mqtt_topic, payloadOffline);
            Serial.printf("[MQTT] Sincronizado: %s\n", payloadOffline);
            
            client.loop(); 
            vTaskDelay(pdMS_TO_TICKS(150)); 
        }
    }
    file.close();
    
    // Limpieza final y liberación del Mutex
    SD.remove("/registros.csv"); 
    Serial.println("[SD] Sincronización completada. Archivo borrado.");
    xSemaphoreGive(mutexSD); 
}

// ==================== TAREAS ====================

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