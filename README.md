# ESP32-S3 USB Host para Inversores Voltronic / Axpert / MPP Solar (Protocolo PI30)

Esta sección proporciona una solución completa, ligera y económica para conectar un **ESP32-S3** directamente a inversores **Voltronic Power / Axpert / MPP Solar** (probado específicamente en el modelo **MPP PIP 4024**) utilizando el puerto **USB Host** nativo.

Al aprovechar el controlador USB nativo del ESP32-S3, puedes leer la telemetría en tiempo real (mediante el comando `QPIGS`) y modificar la prioridad de salida del inversor (como cambiar a Prioridad Solar, Batería/SBU, o Prioridad de Red/Utility) a través de tu red WiFi local, eliminando la necesidad de usar una Raspberry Pi, adaptadores USB-a-Serie adicionales, o software propietario pago como Solar Assistant.

---

## ⚡ Características Clave
* **Conexión USB Nativa Directa:** Se comunica mediante USB HID directo al puerto USB del inversor (VID `0x0665`, PID `0x5161`). Sin conversores UART externos.
* **Supera las Limitaciones de la Biblioteca:** Implementa una solución alternativa para la biblioteca `EspUsbHost` evitando fallos en la negociación del descriptor HID.
* **Dashboard Web en Tiempo Real:** Servidor web integrado y autorefresh (cada 5 segundos) para visualizar la telemetría y los eventos recientes.
* **Soporte para Comandos de Control:** Permite cambiar la prioridad de salida del inversor directamente desde el navegador web (`POP00` / `POP01` / `POP02`).
* **Parseo de Telemetría Extendido:** Mapea todos los valores clave del comando `QPIGS` (Voltaje de batería, SOC %, carga AC %, voltaje solar PV, potencia de salida activa/aparente, además de **campos extendidos como la Potencia de Carga Solar en Watts** y **Banderas de Estado del Inversor**).
* **Diagnóstico de Trama Cruda:** Muestra la cantidad de campos y la trama hexadecimal/carácter recibida del inversor para facilitar la depuración en clones y variantes de firmware.

---

## 🛠️ Requisitos de Hardware

1. **Placa de Desarrollo ESP32-S3:**
   * Recomendado: **ESP32-S3-DevKitC-1** (N16R8 o similar) que cuente con **dos puertos USB físicos**:
     * **Puerto UART/COM:** Utilizado para programar la placa y depurar por puerto serie.
     * **Puerto NATIVE USB:** Conectado directamente a las patillas internas del controlador USB del chip (GPIO19 para `D-` y GPIO20 para `D+`). Este es el puerto que actuará como **USB Host**.
2. **Inversor Voltronic / Axpert:**
   * Cualquier inversor compatible con puerto USB tipo B o Micro-USB (VID `0x0665`, PID `0x5161`).
3. **Cable USB OTG / Conexión:**
   * Un cable estándar de **USB-C a Micro-USB** (o **USB-C a USB-B**) para conectar el puerto USB Nativo del ESP32-S3 directamente al puerto USB del inversor. No requiere soldar cables individuales ni divisores.
4. **Fuente de Alimentación Externa de 5V:**
   * > [!IMPORTANT]
     > El puerto USB nativo del ESP32-S3 en modo USB Host debe energizar la línea VBUS de la conexión del inversor. **No alimentes la placa solo con el puerto UART** de depuración de tu PC. Debes suministrar una fuente de **5V estable (de mínimo 1A a 2A)** directo a los pines **`5V/VIN`** y **`GND`** del ESP32-S3.

---

## 💻 Configuración de Software e Instalación

### 1. Preparar el Entorno en Arduino IDE
* Asegúrate de tener instalado **Arduino IDE 1.8.x** o **2.x**.
* En el menú, dirígete a **Archivo ➡️ Preferencias**.
* En el campo **Gestor de URLs Adicionales de Tarjetas**, añade el repositorio de placas de Espressif:
  `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
* Ve a **Herramientas ➡️ Placa ➡️ Gestor de tarjetas**, busca `esp32` por **Espressif**, e instala la versión **3.x** o superior.
* Selecciona la placa en **Herramientas ➡️ Placa ➡️ esp32 ➡️ ESP32S3 Dev Module**.

### 2. Instalar la Biblioteca USB Host
El proyecto utiliza la biblioteca `EspUsbHost` de `tanakamasayuki`. Para instalarla:
1. En el menú superior de Arduino IDE, ve a **Programa ➡️ Incluir biblioteca ➡️ Administrar bibliotecas...** (o presiona `Ctrl+Shift+I`).
2. En la barra de búsqueda escribe **`EspUsbHost`**.
3. Selecciona la biblioteca desarrollada por **tanakamasayuki** y haz clic en **Instalar**.
4. Reinicia el IDE si es necesario.

### 3. Ajustes de la Placa en el IDE
Configura los siguientes parámetros bajo el menú de **Herramientas**:
* **USB CDC on Boot:** *Enabled* (Habilitado, para ver las tramas y logs de depuración por consola serial nativa).
* **Upload Mode:** *UART / Hardware CDC*.
* **Flash Size:** El tamaño correspondiente a tu módulo (habitualmente *8MB* o *16MB*).
* **Partition Scheme:** Acorde a tu módulo (por ejemplo, *Default 4MB with spiffs* o *16MB Flash*).

---

## 🔍 El Reto Técnico y su Solución

### El Problema
La mayoría de los inversores Voltronic se identifican como dispositivos HID genéricos (`STMicroelectronics - HID in FS Mode`). Al intentar comunicarse con ellos:
1. La biblioteca `EspUsbHost` no clasifica la interfaz HID del inversor como una interfaz de tipo *vendor-specific* (debido a que el descriptor reportado no cuenta con la página de uso común vendor). El flag interno `hasVendorInterface_` permanece desactivado, haciendo que `usb.sendVendorOutput()` falle y retorne `false`.
2. Las respuestas del inversor comienzan con el carácter `(` (`0x28`), el cual no se mapea con ningún ID de reporte HID estándar conocido. La biblioteca filtra y descarta estas respuestas, haciendo que el callback `onVendorInput()` nunca se dispare.
3. El inversor no expone un endpoint de interrupción de salida (OUT), por lo que las transferencias de interrupción estándar fallan.

### La Solución
Para resolver estos inconvenientes de negociación de bajo nivel, el código implementa lo siguiente:
1. **Escritura mediante Control EP0:** En lugar de `sendVendorOutput()`, utilizamos **`usb.sendHIDReport()`** enviando reportes de salida (`ESP_USB_HOST_HID_REPORT_TYPE_OUTPUT`) por la interfaz `0` sin Report ID. Esto obliga al Host a usar transferencias de control nativas (Set_Report en el Endpoint 0) que el firmware del inversor sí procesa correctamente.
2. **Fragmentación del Buffer:** Los comandos son empaquetados y enviados estrictamente en bloques de **8 bytes** (rellenados con `0x00` si es necesario).
3. **Lectura HID Cruda:** En lugar de `onVendorInput()`, nos suscribimos a **`usb.onHIDInput()`**. Este callback intercepta todos los reportes entrantes del bus USB sin aplicar filtros de ID, lo que permite reconstruir la respuesta acumulando los bytes hasta detectar el carácter de finalización `\r`.

---

## 📜 Especificaciones del Protocolo PI30

### Estructura de Comandos
Todos los comandos enviados al inversor deben tener la siguiente estructura:
```
[COMANDO] + [CRC16 (2 bytes)] + [Retorno de carro '\r' (0x0D)]
```
*Por ejemplo, el comando `QPIGS` genera un CRC de `0xB7 0xA9`. La cadena de bytes final en hexadecimal para enviar es:*
`0x51` (`Q`), `0x50` (`P`), `0x49` (`I`), `0x47` (`G`), `0x53` (`S`), `0xB7`, `0xA9`, `0x0D`.

### Algoritmo CRC-16 Personalizado de Voltronic
El cálculo del CRC utiliza el polinomio estándar `0x1021` con valor inicial `0x0000`. Sin embargo, cuenta con una regla crítica:
> [!IMPORTANT]
> Los caracteres `(`, `\r` y `\n` (`0x28`, `0x0D`, y `0x0A`) son delimitadores reservados. Si el valor final de cualquiera de los dos bytes calculados para el CRC-16 coincide con estos valores, **deben incrementarse en 1 automáticamente** (por ejemplo, si el byte de CRC resulta ser `0x0D`, se transmite como `0x0E`).

**Valores de referencia validados:**
* `QPIGS` ➡️ `0xB7 0xA9`
* `QPI` ➡️ `0xBE 0xAC`

### Estructura de Telemetría QPIGS (24 Campos)
La respuesta recibida del inversor es una cadena de valores separados por espacios, que inicia con `(` y finaliza con `[CRC][\r]`. En modelos como el **PIP 4024**, el comando retorna hasta **24 campos**. El sketch los parsea en los siguientes parámetros utilizables:

| Índice | Nombre de Campo | Descripción | Unidad |
| :---: | :--- | :--- | :---: |
| **0** | `grid_voltage` | Voltaje de entrada de la red AC | V |
| **1** | `grid_frequency` | Frecuencia de entrada de la red AC | Hz |
| **2** | `ac_output_voltage` | Voltaje de salida de corriente alterna AC | V |
| **3** | `ac_output_frequency` | Frecuencia de salida de corriente alterna AC | Hz |
| **4** | `ac_output_apparent_power` | Potencia aparente de salida | VA |
| **5** | `ac_output_active_power` | Potencia activa de salida (Consumo consumido) | W |
| **6** | `ac_output_load_percent` | Porcentaje de carga de salida del inversor | % |
| **7** | `bus_voltage` | Voltaje de bus DC interno del inversor | V |
| **8** | `battery_voltage` | Voltaje de la batería en bornes | V |
| **9** | `battery_charging_current` | Corriente total de carga de batería | A |
| **10** | `battery_capacity` | Estado de carga de la batería (SOC) | % |
| **11** | `inverter_heat_sink_temp` | Temperatura del disipador del inversor | °C |
| **12** | `pv_input_current_for_bat` | Corriente solar de carga hacia la batería | A |
| **13** | `pv_input_voltage` | Voltaje de paneles solares (PV) | V |
| **14** | `battery_voltage_from_scc` | Voltaje de batería reportado por el regulador solar | V |
| **15** | `battery_discharge_current` | Corriente de descarga de la batería | A |
| **16** | `device_status` | Banderas de estado del inversor (cadena binaria) | Flags |
| **17** | `bat_voltage_offset` | Compensación de voltaje de batería | V |
| **18** | `eeprom_version` | Versión del firmware de carga / EEPROM | - |
| **19** | `pv_power` | **Potencia de carga solar directa** (Generación Watts) | W |
| **20** | `device_status_2` | Banderas de estado secundarias | Flags |

---

## 🚀 Puesta en Marcha

1. Abre el archivo [qpigs_completo.ino](file:///C:/Users/Cleo/Downloads/qpigs_completo/qpigs_completo.ino) en tu Arduino IDE.
2. Edita los parámetros de WiFi en las líneas 32 y 33 con los datos de tu red local:
   ```cpp
   const char *WIFI_SSID     = "Tu_SSID_WiFi";
   const char *WIFI_PASSWORD = "Tu_Contraseña_WiFi";
   ```
3. Conecta el ESP32-S3 a la PC mediante el **puerto UART/COM**.
4. Selecciona el puerto serie, la tarjeta `ESP32S3 Dev Module`, e inicia la subida (*Upload*).
5. Abre el Monitor Serie a **115200 baudios** para verificar la dirección IP asignada por tu enrutador.
6. Conecta tu teléfono o PC a la misma red WiFi e ingresa a `http://<DIRECCIÓN_IP_ESP32>`.
7. Desconecta la placa del PC y llévala al inversor. Conecta un cable estándar desde el **puerto NATIVE USB** del ESP32-S3 al puerto USB del inversor, y energiza la placa con una fuente externa de 5V conectada al pin `5V/VIN`.

---

## 🤖 Desarrollo y Personalización Asistido por IA

Este proyecto y la resolución del problema de comunicación de bajo nivel del bus USB ha sido diseñado e implementado con la asistencia de Inteligencia Artificial (específicamente **Claude**).

Dado que el código fuente está modularizado de forma limpia y comentado en su totalidad, puedes usar herramientas de IA generativa modernas como **Claude**, **ChatGPT** o **Gemini** (las cuales ofrecen excelentes resultados programando microcontroladores) para extender este software.

Algunas mejoras que puedes solicitar a una IA que programe sobre este código base:
*   **Manejo Inteligente de Relés:** Automatizar el encendido de relés inteligentes (por ejemplo, termos solares para calentar agua, calentadores de piscina, etc.) basándose en la potencia de generación solar excedente (`pv_power`) y el nivel de batería (`batt_soc`).
*   **Integración con Domótica:** Programar la exportación de los datos a Home Assistant vía comandos MQTT, o estructurar una API REST en JSON más robusta.
*   **Cambio Automático de Modos por Horario/Estación:** Crear rutinas lógicas que evalúen la hora actual para pasar a modo red o modo batería según la temporada del año.

---

## 📝 Código Fuente Completo (`qpigs_completo.ino`)

El código fuente listo para cargar se incluye a continuación:

```cpp
/*
 * MPP PIP 4024 (Voltronic/Axpert, protocolo PI30) via USB Host - ESP32-S3
 * ---------------------------------------------------------------------------
 * Características:
 *  1. Puerto USB Host nativo con el inversor (VID 0x0665 / PID 0x5161).
 *  2. Consulta la telemetría enviando el comando "QPIGS" cada 5 segundos.
 *  3. Calcula el CRC-16 especial de Voltronic y parsea los campos de datos.
 *  4. Sirve una interfaz web local para monitorear y realizar cambios de modo.
 *
 * Requisitos:
 * - ESP32-S3 alimentado por pin 5V con puerto USB nativo (GPIO19/20).
 * - Biblioteca "EspUsbHost" de tanakamasayuki.
 */

#include "EspUsbHost.h"
#include <WiFi.h>
#include <WebServer.h>

// ---------------- CONFIGURACION WIFI ----------------------------------
const char *WIFI_SSID     = "Flia";
const char *WIFI_PASSWORD = "TU_CONTRASEÑA_REAL"; // Cambia por tu contraseña real
// ------------------------------------------------------------------------

EspUsbHost usb;
WebServer server(80);

uint8_t hidInterfaceNumber = 0;
uint8_t hidDeviceAddress = ESP_USB_HOST_ANY_ADDRESS;
bool hidInterfaceKnown = false;

// ============================================================================
//  Tabla de CRC-16 del Protocolo Voltronic / PI30
// ============================================================================
static const uint16_t PI30_CRC_TABLE[16] = {
  0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
  0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
};

void pi30Crc(const uint8_t *data, size_t length, uint8_t &highOut, uint8_t &lowOut) {
  uint16_t crc = 0x0000;
  for (size_t i = 0; i < length; i++) {
    uint8_t b = data[i];
    uint8_t da = (uint8_t)((crc >> 8) >> 4);
    crc <<= 4;
    crc ^= PI30_CRC_TABLE[da ^ (b >> 4)];

    da = (uint8_t)((crc >> 8) >> 4);
    crc <<= 4;
    crc ^= PI30_CRC_TABLE[da ^ (b & 0x0F)];
  }

  uint8_t low = crc & 0xFF;
  uint8_t high = (crc >> 8) & 0xFF;

  // Los bytes de delimitación reservados no pueden transmitirse directamente en el CRC
  if (low == 0x28 || low == 0x0d || low == 0x0a) low += 1;
  if (high == 0x28 || high == 0x0d || high == 0x0a) high += 1;

  highOut = high;
  lowOut = low;
}

size_t buildPi30Command(const char *cmd, uint8_t *outBuf, size_t outBufSize) {
  size_t cmdLen = strlen(cmd);
  if (cmdLen + 3 > outBufSize) return 0;

  uint8_t high, low;
  pi30Crc((const uint8_t *)cmd, cmdLen, high, low);

  memcpy(outBuf, cmd, cmdLen);
  outBuf[cmdLen] = high;
  outBuf[cmdLen + 1] = low;
  outBuf[cmdLen + 2] = '\r';
  return cmdLen + 3;
}

// ============================================================================
//  Emisor de Reporte HID (Canal de Control EP0)
// ============================================================================
bool sendPi30Command(const char *cmd) {
  if (!hidInterfaceKnown) {
    addLog("WARN: interfaz HID desconocida, esperando conexión...");
    return false;
  }

  uint8_t packet[64];
  size_t packetLen = buildPi30Command(cmd, packet, sizeof(packet));
  if (packetLen == 0) return false;

  size_t paddedLen = ((packetLen + 7) / 8) * 8;
  uint8_t padded[64] = {0};
  memcpy(padded, packet, packetLen);

  bool allOk = true;
  for (size_t offset = 0; offset < paddedLen; offset += 8) {
    bool ok = usb.sendHIDReport(
        hidInterfaceNumber,
        ESP_USB_HOST_HID_REPORT_TYPE_OUTPUT,
        0, // Sin report ID
        padded + offset,
        8,
        hidDeviceAddress
    );
    if (!ok) allOk = false;
    delay(10);
  }
  return allOk;
}

// ============================================================================
//  Receptor de Datos HID
// ============================================================================
#define RX_BUF_SIZE 256
uint8_t rxBuffer[RX_BUF_SIZE];
size_t rxBufferLen = 0;
volatile bool rxComplete = false;

void resetRxBuffer() {
  rxBufferLen = 0;
  rxComplete = false;
}

void onHIDInput(const EspUsbHostHIDInput &input) {
  if (!hidInterfaceKnown) {
    hidInterfaceNumber = input.interfaceNumber;
    hidDeviceAddress   = input.address;
    hidInterfaceKnown  = true;
    addLog("HID Interfaz capturada: iface=" + String(hidInterfaceNumber) +
           " addr=" + String(hidDeviceAddress));
  }

  for (size_t i = 0; i < input.length && rxBufferLen < RX_BUF_SIZE; i++) {
    uint8_t b = input.data[i];
    rxBuffer[rxBufferLen++] = b;
    if (b == '\r') {
      rxComplete = true;
    }
  }
}

// ============================================================================
//  Manejo y Parseo de Estructura de Datos
// ============================================================================
struct InverterData {
  bool valid = false;
  unsigned long lastUpdateMs = 0;
  String rawResponse = "";

  float gridVoltage = 0;
  float gridFreq = 0;
  float outVoltage = 0;
  float outFreq = 0;
  int outApparentPower = 0;
  int outActivePower = 0;
  int outLoadPercent = 0;
  float busVoltage = 0;
  float batVoltage = 0;
  int batChargeCurrent = 0;
  int batCapacityPercent = 0;
  float pvVoltage = 0;
  int batDischargeCurrent = 0;

  // Campos adicionales del QPIGS extendido (índices 16 a 20)
  String deviceStatus = "";          
  float batVoltageOffset = 0;       
  float eepromVersion = 0;          
  int pvPower = 0;                  
  String deviceStatus2 = "";         
};

InverterData inv;

bool deviceConnected = false;
uint16_t lastVid = 0, lastPid = 0;
String lastManufacturer = "";
String lastProduct = "";

#define MAX_LOG_LINES 20
String logLines[MAX_LOG_LINES];
int logCount = 0;

void addLog(const String &line) {
  for (int i = MAX_LOG_LINES - 1; i > 0; i--) {
    logLines[i] = logLines[i - 1];
  }
  logLines[0] = line;
  if (logCount < MAX_LOG_LINES) logCount++;
  Serial.println(line);
}

bool parseQpigs(const String &raw) {
  if (raw.length() < 10 || raw[0] != '(') return false;

  String body = raw.substring(1, raw.length() - 3);

  const int MAX_FIELDS = 25;
  String fields[MAX_FIELDS];
  int fieldCount = 0;
  int start = 0;
  for (int i = 0; i <= (int)body.length() && fieldCount < MAX_FIELDS; i++) {
    if (i == (int)body.length() || body[i] == ' ') {
      if (i > start) {
        fields[fieldCount++] = body.substring(start, i);
      }
      start = i + 1;
    }
  }

  if (fieldCount < 16) return false;

  inv.gridVoltage        = fields[0].toFloat();
  inv.gridFreq           = fields[1].toFloat();
  inv.outVoltage          = fields[2].toFloat();
  inv.outFreq             = fields[3].toFloat();
  inv.outApparentPower    = fields[4].toInt();
  inv.outActivePower      = fields[5].toInt();
  inv.outLoadPercent      = fields[6].toInt();
  inv.busVoltage          = fields[7].toFloat();
  inv.batVoltage          = fields[8].toFloat();
  inv.batChargeCurrent    = fields[9].toInt();
  inv.batCapacityPercent  = fields[10].toInt();
  inv.pvVoltage           = fields[13].toFloat();
  inv.batDischargeCurrent = fields[15].toInt();

  // Campos adicionales (hasta 24 campos retornados por el PIP 4024)
  if (fieldCount > 16) inv.deviceStatus = fields[16];
  if (fieldCount > 17) inv.batVoltageOffset = fields[17].toFloat();
  if (fieldCount > 18) inv.eepromVersion = fields[18].toFloat();
  if (fieldCount > 19) inv.pvPower = fields[19].toInt();
  if (fieldCount > 20) inv.deviceStatus2 = fields[20];

  inv.valid = true;
  inv.lastUpdateMs = millis();
  inv.rawResponse = raw;
  return true;
}

// ============================================================================
//  Callbacks de Conexión USB
// ============================================================================
void onDeviceConnected(const EspUsbHostDeviceInfo &device) {
  deviceConnected = true;
  lastVid = device.vid;
  lastPid = device.pid;
  lastManufacturer = String(device.manufacturer);
  lastProduct = String(device.product);

  hidInterfaceNumber = 0;
  hidDeviceAddress   = device.address;
  hidInterfaceKnown  = true;

  addLog("=== Inversor Conectado: VID=0x" + String(device.vid, HEX) +
         " PID=0x" + String(device.pid, HEX) + " ===");
}

void onDeviceDisconnected(const EspUsbHostDeviceInfo &device) {
  deviceConnected = false;
  hidInterfaceKnown = false;
  inv.valid = false;
  addLog("=== Inversor Desconectado ===");
}

// ============================================================================
//  Lógica de Control de Consulta al Inversor
// ============================================================================
unsigned long lastPollMs = 0;
const unsigned long POLL_INTERVAL_MS = 5000;
unsigned long pollWaitingSinceMs = 0;
bool waitingResponse = false;
const unsigned long RESPONSE_TIMEOUT_MS = 2000;

String pendingCmd = "";
String lastCmdSent = "QPIGS";

void pollInverter() {
  if (!deviceConnected) return;

  resetRxBuffer();
  lastCmdSent = "QPIGS";
  bool ok = sendPi30Command("QPIGS");
  if (ok) {
    waitingResponse = true;
    pollWaitingSinceMs = millis();
  } else {
    addLog("ERROR: fallo al enviar QPIGS");
  }
}

void sendControlCommand(const String &cmd) {
  if (!deviceConnected) {
    addLog("Imposible enviar " + cmd + ": inversor desconectado");
    return;
  }
  resetRxBuffer();
  lastCmdSent = cmd;
  bool ok = sendPi30Command(cmd.c_str());
  if (ok) {
    waitingResponse = true;
    pollWaitingSinceMs = millis();
    addLog("Comando enviado: " + cmd);
  } else {
    addLog("ERROR: fallo al enviar " + cmd);
  }
}

void checkResponse() {
  if (!waitingResponse) return;

  if (rxComplete) {
    waitingResponse = false;
    String raw = "";
    for (size_t i = 0; i < rxBufferLen; i++) {
      raw += (char)rxBuffer[i];
    }
    if (lastCmdSent == "QPIGS") {
      if (parseQpigs(raw)) {
        addLog("QPIGS OK: Vbat=" + String(inv.batVoltage, 2) +
               "V Vpv=" + String(inv.pvVoltage, 1) +
               "V Carga=" + String(inv.outLoadPercent) + "%");
      } else {
        addLog("QPIGS: respuesta inválida (len=" + String(rxBufferLen) + ")");
      }
    } else {
      String r = raw;
      r.trim();
      addLog("Resp " + lastCmdSent + ": " + r);
      lastCmdSent = "QPIGS";
    }
    resetRxBuffer();
  } else if (millis() - pollWaitingSinceMs > RESPONSE_TIMEOUT_MS) {
    waitingResponse = false;
    addLog(lastCmdSent + ": timeout de respuesta");
    lastCmdSent = "QPIGS";
    resetRxBuffer();
  }
}

// ============================================================================
//  Handlers del Servidor Web
// ============================================================================
String fmtAge(unsigned long ms) {
  if (ms == 0) return "nunca";
  unsigned long ageSec = (millis() - ms) / 1000;
  return String(ageSec) + "s atrás";
}

void handleSetMode() {
  String m = server.arg("m");
  String cmd = "";
  if (m == "uti")        cmd = "POP00";
  else if (m == "solar") cmd = "POP01";
  else if (m == "sbu")   cmd = "POP02";

  if (cmd != "") {
    pendingCmd = cmd;
    addLog("Modo en cola: " + m + " (" + cmd + ")");
  }
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "OK");
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>";
  html += "<title>Monitor de Inversor ESP32</title>";
  html += "<style>";
  html += "body{font-family:system-ui,monospace;background:#111;color:#eee;padding:16px;max-width:680px;margin:auto;}";
  html += "h1{font-size:1.3em;color:#7fdc7f;}";
  html += ".ok{color:#7fdc7f;font-weight:bold;}";
  html += ".bad{color:#ff6b6b;font-weight:bold;}";
  html += ".box{background:#1e1e1e;border:1px solid #333;border-radius:8px;padding:14px;margin-bottom:12px;}";
  html += ".grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;}";
  html += ".metric{background:#252525;border-radius:6px;padding:10px;}";
  html += ".metric .label{font-size:0.75em;color:#999;}";
  html += ".metric .value{font-size:1.3em;color:#7fdc7f;}";
  html += ".log{font-size:0.8em;white-space:pre-wrap;word-break:break-all;color:#aaa;}";
  html += ".modes{display:flex;gap:8px;}";
  html += ".modebtn{flex:1;display:block;text-align:center;text-decoration:none;padding:14px 8px;border-radius:8px;background:#2a2a2a;border:1px solid #444;color:#eee;font-weight:bold;font-size:0.95em;}";
  html += ".modebtn:active{background:#3a3a3a;}";
  html += ".modebtn small{display:block;font-size:0.7em;color:#999;font-weight:normal;margin-top:3px;}";
  html += "</style></head><body>";

  html += "<h1>&#9889; Monitor de Inversor (PIP 4024)</h1>";

  // Estado USB
  html += "<div class='box'>";
  html += "<b>Conexión USB: </b>";
  if (deviceConnected) {
    html += "<span class='ok'>CONECTADO</span> (VID 0x" + String(lastVid, HEX) +
            " PID 0x" + String(lastPid, HEX) + ", " + lastProduct + ")";
  } else {
    html += "<span class='bad'>DESCONECTADO</span>";
  }
  html += "</div>";

  // Control de Prioridad
  html += "<div class='box'>";
  html += "<b>Cambiar Prioridad de Salida</b><br><br>";
  html += "<div class='modes'>";
  html += "<a class='modebtn' href='/setmode?m=sbu'>&#128267; Batería<small>SBU / POP02</small></a>";
  html += "<a class='modebtn' href='/setmode?m=solar'>&#9728; Solar<small>POP01</small></a>";
  html += "<a class='modebtn' href='/setmode?m=uti'>&#128268; Red<small>POP00</small></a>";
  html += "</div>";
  html += "</div>";

  // Telemetría
  html += "<div class='box'>";
  html += "<b>Datos del Inversor (QPIGS)</b><br><br>";
  if (inv.valid) {
    html += "<div class='grid'>";
    html += "<div class='metric'><div class='label'>Voltaje Batería</div><div class='value'>" + String(inv.batVoltage, 2) + " V</div></div>";
    html += "<div class='metric'><div class='label'>Capacidad Batería</div><div class='value'>" + String(inv.batCapacityPercent) + " %</div></div>";
    html += "<div class='metric'><div class='label'>Voltaje Solar (PV)</div><div class='value'>" + String(inv.pvVoltage, 1) + " V</div></div>";
    html += "<div class='metric'><div class='label'>Corriente Carga</div><div class='value'>" + String(inv.batChargeCurrent) + " A</div></div>";
    html += "<div class='metric'><div class='label'>Voltaje Salida AC</div><div class='value'>" + String(inv.outVoltage, 1) + " V</div></div>";
    html += "<div class='metric'><div class='label'>Carga de Salida</div><div class='value'>" + String(inv.outLoadPercent) + " %</div></div>";
    html += "<div class='metric'><div class='label'>Potencia Activa</div><div class='value'>" + String(inv.outActivePower) + " W</div></div>";
    html += "<div class='metric'><div class='label'>Voltaje de Red</div><div class='value'>" + String(inv.gridVoltage, 1) + " V</div></div>";
    html += "<div class='metric'><div class='label'>Potencia Solar</div><div class='value'>" + String(inv.pvPower) + " W</div></div>";
    html += "<div class='metric'><div class='label'>Estado Inversor</div><div class='value' style='font-size:0.9em;'>" + inv.deviceStatus + "</div></div>";
    html += "</div>";
    html += "<br><small>Última actualización: " + fmtAge(inv.lastUpdateMs) + "</small>";
  } else {
    html += "<span class='bad'>Sin datos válidos todavía</span>";
  }
  html += "</div>";

  // Diagnóstico Crudo
  html += "<div class='box'>";
  html += "<b>Diagnóstico Crudo (Campos QPIGS)</b><br><br>";
  if (inv.rawResponse.length() > 0) {
    String body = inv.rawResponse;
    if (body.length() > 0 && body[0] == '(') body = body.substring(1);
    if (body.length() > 3) body = body.substring(0, body.length() - 3);
    int campos = 0;
    int start = 0;
    String numerado = "";
    for (int i = 0; i <= (int)body.length(); i++) {
      if (i == (int)body.length() || body[i] == ' ') {
        if (i > start) {
          numerado += "[" + String(campos) + "] " + body.substring(start, i) + "\n";
          campos++;
        }
        start = i + 1;
      }
    }
    html += "<div class='log'>Total Campos parseados: <b>" + String(campos) + "</b></div><br>";
    html += "<div class='log'>" + numerado + "</div><br>";
    html += "<div class='log'>RAW Char/Hex (" + String(inv.rawResponse.length()) + " bytes):\n" + inv.rawResponse + "</div>";
  } else {
    html += "<span class='bad'>Sin trama recibida todavía</span>";
  }
  html += "</div>";

  // WiFi
  html += "<div class='box'>";
  html += "<b>WiFi Conectado:</b> '" + String(WIFI_SSID) + "' &mdash; <b>IP Local:</b> " + WiFi.localIP().toString();
  html += "</div>";

  // Logs
  html += "<div class='box'>";
  html += "<b>Logs del Sistema:</b><div class='log'>";
  if (logCount == 0) {
    html += "(sin registros)";
  } else {
    for (int i = 0; i < logCount; i++) html += logLines[i] + "\n";
  }
  html += "</div></div>";

  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

// ============================================================================
//  Setup y Bucle Principal
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("\n--- ESP32-S3 Native USB Voltronic Controller Inicializando ---");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a WiFi");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("==> WiFi Conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WARN: Falló la conexión WiFi. Verifica credenciales.");
  }

  server.on("/", handleRoot);
  server.on("/setmode", handleSetMode);
  server.begin();
  Serial.println("Servidor web escuchando en puerto 80");

  usb.onDeviceConnected(onDeviceConnected);
  usb.onDeviceDisconnected(onDeviceDisconnected);
  usb.onHIDInput(onHIDInput);

  if (!usb.begin()) {
    addLog("ERROR: usb.begin() falló: " + String(usb.lastErrorName()));
  } else {
    addLog("USB Host inicializado. Esperando inversor...");
  }
}

void loop() {
  server.handleClient();
  checkResponse();

  if (!waitingResponse) {
    if (pendingCmd != "") {
      String c = pendingCmd;
      pendingCmd = "";
      sendControlCommand(c);
    } else if (millis() - lastPollMs > POLL_INTERVAL_MS) {
      lastPollMs = millis();
      pollInverter();
    }
  }
}
```

---

## 🤝 Contribuciones y Licencia
Siéntete libre de clonar este repositorio, abrir problemas (Issues) o enviar Pull Requests para adaptar la lógica a otros inversores con protocolo PI30, o para integrar telemetría vía MQTT con asistentes domóticos.

Distribuido bajo la Licencia MIT.
