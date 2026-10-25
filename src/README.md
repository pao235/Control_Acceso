# Firmware ESP32 · Control de Acceso IoT con FreeRTOS y MQTT

**Sistema de Control de Acceso Institucional Multi-Tarea (RFID / Teclado) con Tolerancia a Fallos y Sincronización Offline**  
Proyecto Integrador / Trabajo de Investigación — Instituto Tecnológico de Chihuahua (ITCH) · 2026

![ESP32](https://img.shields.io/badge/Plataforma-ESP32-blue?logo=espressif)
![FreeRTOS](https://img.shields.io/badge/RTOS-FreeRTOS-green)
![MQTT](https://img.shields.io/badge/Protocolo-MQTT%20%2F%20JSON-orange)
![Licencia](https://img.shields.io/badge/Licencia-Educativa-brightgreen)
![Estado](https://img.shields.io/badge/Estado-Completado-brightgreen)

---

## Descripción

Firmware para microcontrolador ESP32 (DevKitC / Xtensa LX6) orientado a la automatización de registros de acceso en redes IoT institucionales. El sistema implementa autenticación dual mediante lectura de tarjetas/tags RFID y teclado matricial bajo protocolo Wiegand, gestión temporal con servidor NTP y RTC DS1302 como respaldo, interfaz visual en pantalla LCD I2C, e integración con brokers MQTT.

Diseñado bajo la arquitectura FreeRTOS, las tareas operativas se distribuyen en ambos núcleos del procesador, comunicándose mediante colas y semáforos mutex. Para mitigar pérdidas de información ante interrupciones de red, el dispositivo guarda los registros localmente en un sistema de archivos CSV alojado en una tarjeta MicroSD, resincronizando los eventos de forma automática al restablecerse la conectividad.

---

## Artículo de Referencia

Este desarrollo representa la implementación práctica del trabajo de investigación presentado en el TecNM / Instituto Tecnológico de Chihuahua:

> **Referencia Bibliográfica:**  
> E. Trujillo-Preciado, J. A. Dominguez-Terrazas, O. Beltrán-Gómez, L. A. Anchondo-Carreón, A. P. Cardona-Valenzuela, y E. Pérez-Dyck, *"Implementación y uso de nodos de registro de acceso RFID basados en arquitectura publicador-suscriptor en la red IoT del Instituto Tecnológico de Chihuahua"*, Departamento de Eléctrica y Electrónica / Ciencias Básicas, Instituto Tecnológico de Chihuahua, 2026.

---

## Arquitectura del Sistema

El flujo de información se basa en el modelo Publicador-Suscriptor utilizando el protocolo MQTT.

```
┌─────────────────┐       MQTT       ┌──────────────────┐               ┌─────────────────┐
│ Dispositivo IoT │ ────────────────>│   Broker MQTT    │ ─────────────>│    Node-RED     │
│     (ESP32)     │   (JSON/Wi-Fi)   │   (RabbitMQ)     │ (Suscriptor)  │ (Procesamiento) │
└─────────────────┘                  └──────────────────┘               └────────┬────────┘
                                                                                 │
                                                                                 ▼
                                                                        ┌─────────────────┐
                                                                        │  Base de Datos  │
                                                                        │     (MySQL)     │
                                                                        └─────────────────┘
```

---

## Arquitectura de Software (FreeRTOS)

Las responsabilidades del firmware están separadas en tareas independientes distribuidas en los dos núcleos del ESP32.

```
┌────────────────────────────────────────────────────────────────────────┐
│                               CORE 1                                   │
├────────────────────────────────────────────────────────────────────────┤
│  TaskWiegand (Prioridad 3)                                             │
│  - Muestreo directo del lector Wiegand (RFID / Teclado)                │
│  - Captura prioritaria para evitar pérdida de pulsos                   │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
                         Colas de Comunicación
                                   │
┌──────────────────────────────────┴─────────────────────────────────────┐
│                               CORE 0                                   │
├────────────────────────────────────────────────────────────────────────┤
│  TaskWIFI   (Prio 2) : Gestión y reconexión automática a la red Wi-Fi  │
│  TaskMQTT   (Prio 2) : Publicación de mensajes JSON y confirmación     │
│  TaskSD     (Prio 1) : Escritura segura (Mutex) en tarjeta MicroSD     │
│  TaskRTC    (Prio 1) : Sincronización NTP y lectura del RTC DS1302     │
│  TaskLCD    (Prio 1) : Actualización de la pantalla I2C 16x2            │
│  MonColas   (Prio 2) : Monitoreo periódico de métricas del sistema     │
└────────────────────────────────────────────────────────────────────────┘
```

### Detalle de Tareas

| Tarea | Núcleo | Prioridad | Descripción |
|---|---|---|---|
| `TaskWiegand` | Core 1 | 3 | Captura códigos de 8 dígitos tecleados o UID de tarjetas RFID |
| `TaskWIFI` | Core 0 | 2 | Supervisa la conexión Wi-Fi y ejecuta reconexiones automáticas |
| `TaskMQTT` | Core 0 | 2 | Gestiona la conexión al broker, publica paquetes JSON y recibe confirmación por eco |
| `TaskSD` | Core 0 | 1 | Escribe líneas CSV en `/registros.csv` usando Mutex cuando el sistema está offline |
| `TaskRTC` | Core 0 | 1 | Sincroniza la hora con servidores NTP y conmuta a RTC DS1302 si no hay red |
| `TaskLCD` | Core 0 | 1 | Procesa eventos visuales (mensajes, horas y códigos) en la LCD 16x2 |
| `TaskMonitorColas` | Core 0 | 2 | Reporta el uso pico de colas y conteo de eventos descartados por el puerto serie |

---

## Hardware y Pinout

| Periférico / Módulo | Componente / Protocolo | Pin ESP32 | Notas |
|---|---|---|---|
| **Lector Wiegand** | Teclado + Lector RFID | IO16 (D0), IO17 (D1) | Entrada de pulsos de alta prioridad |
| **Reloj RTC** | DS1302 (3-Wire) | IO4 (DAT), IO15 (CLK), IO25 (RST) | Sincronizado vía NTP / Respaldo local |
| **Pantalla LCD** | 16x2 con Mochila I2C | IO21 (SDA), IO22 (SCL) | Dirección I2C predeterminada `0x27` |
| **Módulo MicroSD** | Lector SPI | IO5 (CS), IO23 (MOSI), IO19 (MISO), IO18 (SCK) | Almacenamiento fuera de línea |

---

## Estructura del Repositorio

```
Control-Acceso-ESP32/
├── main.cpp                 # Firmware completo estructurado en tareas FreeRTOS
├── README.md                # Documentación del proyecto
└── docs/                    # Diagramas de flujo y esquemas de conexión
```

---

## Formato de Mensajes y Sincronización

### Mensaje en Red (JSON vía MQTT)
Cuando el dispositivo cuenta con conectividad Wi-Fi y MQTT activa, el evento se emite directamente en el tópico configurado (`esp32/control/numeros`):

```json
{
  "id": "122061081",
  "direccionMAC": "38:18:26:8A:17:B8",
  "fecha": "04/03/2026 08:53:41"
}
```

### Guardado y Sincronización Offline (MicroSD)
Si la red Wi-Fi o el servidor MQTT no están disponibles, `TaskWiegand` envía la información a `queueSD` para registrar el evento en el archivo `registros.csv`:

```csv
122061081,38:18:26:8A:17:B8,04/03/2026 08:53:41
```

Al restablecerse la conexión, la función `sincronizarDatosOffline()` toma el control de la tarjeta mediante un semáforo Mutex, lee el archivo línea por línea, transforma los registros a JSON, los publica en el broker MQTT y finalmente elimina el archivo local para evitar duplicados.

---

## Requisitos y Librerías

Para compilar este firmware en PlatformIO o Arduino IDE para ESP32, se requieren las siguientes dependencias:

| Librería | Propósito |
|---|---|
| `WiFi.h` | Control de la interfaz Wi-Fi del ESP32 |
| `PubSubClient` | Cliente para la comunicación mediante MQTT |
| `ArduinoJson` | Construcción y parseo de estructuras JSON |
| `Wiegand` | Decodificación del protocolo Wiegand (RFID/Teclado) |
| `RtcDS1302` | Control de la comunicación con el módulo RTC |
| `LiquidCrystal_I2C` | Manejo de la pantalla LCD I2C |
| `SD.h` y `SPI.h` | Gestión de archivos en la tarjeta MicroSD |

---

## Equipo de Desarrollo

**Autores del Código y Firmware:**
* Ana Paola Cardona Valenzuela
* Emiliano Pérez Dyck
* Luis Adrián Anchondo Carreón

**Colaboradores del Proyecto de Investigación (ITCH):**
* Edgar Trujillo-Preciado
* José Alberto Dominguez-Terrazas
* Oscar Beltrán-Gómez

**Institución:** Instituto Tecnológico de Chihuahua (TecNM / ITCH)  
**Departamento:** Eléctrica y Electrónica / Ciencias Básicas  
**Actualización:** 2026

---

<p align="center">
  Instituto Tecnológico de Chihuahua · Chihuahua, México · 2026
</p>