/*
 * MTT PIP 4024 (Voltronic/Axpert, protocolo PI30) via USB Host - ESP32-S3
 * ---------------------------------------------------------------------------
 * Este sketch:
 *  1. Habla USB Host nativo con el inversor (VID 0x0665 / PID 0x5161,
 *     confirmado en tu hardware).
 *  2. Cada pocos segundos manda el comando "QPIGS" (estado general:
 *     voltajes, corrientes, potencia, carga de bateria, etc.).
 *  3. Arma la respuesta completa (el inversor contesta en paquetes de a
 *     8 bytes via reportes HID), valida el CRC, y parsea los campos.
 *  4. Muestra todo en una pagina web servida por WiFi, sin necesitar
 *     estar al lado del inversor.
 *
 * HARDWARE (igual que los sketches anteriores):
 * - ESP32-S3-DevKitC-1 N16R8
 * - Alimentar la placa por el pin 5V/VIN con la fuente externa de 5V.
 * - Conectar el inversor (cable micro-USB a USB-A) al puerto USB NATIVO
 *   (USB-C) del ESP32-S3.
 * - El puerto "UART"/"COM" se usa solo para cargar el sketch.
 *
 * LIBRERIA NECESARIA:
 * - "EspUsbHost" de tanakamasayuki (ya la tenes instalada)
 *
 * *** ANTES DE CARGAR: completa tu password de WiFi mas abajo ***
 */

#include "EspUsbHost.h"
#include <WiFi.h>
#include <WebServer.h>

// ---------------- CONFIGURACION WIFI ----------------------------------
const char *WIFI_SSID     = "Your_WiFi_SSID";
const char *WIFI_PASSWORD = "Your_WiFi_Password";   // <-- poné tu contraseña real acá
// ------------------------------------------------------------------------

EspUsbHost usb;
WebServer server(80);

// Interfaz HID capturada del primer input (se llena en onHIDInput)
uint8_t hidInterfaceNumber = 0;
uint8_t hidDeviceAddress = ESP_USB_HOST_ANY_ADDRESS;
bool hidInterfaceKnown = false;

// ============================================================================
//  CRC-16 del protocolo Voltronic/PI30
// ============================================================================
// Tabla y algoritmo validados contra los valores de referencia conocidos:
//   "QPI"   -> CRC 0xBE 0xAC
//   "QPIGS" -> CRC 0xB7 0xA9

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

  // Los bytes reservados (0x28 '(' , 0x0D '\r', 0x0A '\n') no pueden
  // aparecer dentro del CRC: si coinciden, se incrementan en 1.
  if (low == 0x28 || low == 0x0d || low == 0x0a) low += 1;
  if (high == 0x28 || high == 0x0d || high == 0x0a) high += 1;

  highOut = high;
  lowOut = low;
}

// Arma el paquete completo: comando + CRC(2 bytes) + '\r'
size_t buildPi30Command(const char *cmd, uint8_t *outBuf, size_t outBufSize) {
  size_t cmdLen = strlen(cmd);
  if (cmdLen + 3 > outBufSize) return 0;  // no entra

  uint8_t high, low;
  pi30Crc((const uint8_t *)cmd, cmdLen, high, low);

  memcpy(outBuf, cmd, cmdLen);
  outBuf[cmdLen] = high;
  outBuf[cmdLen + 1] = low;
  outBuf[cmdLen + 2] = '\r';
  return cmdLen + 3;
}

// ============================================================================
//  Envio de comandos via reportes HID de 8 bytes
// ============================================================================
// El inversor recibe los comandos en paquetes de 8 bytes (igual que un
// reporte HID de teclado). Si el comando+CRC+\r mide menos de un multiplo
// de 8, se completa con ceros (0x00) hasta el siguiente multiplo de 8.

bool sendPi30Command(const char *cmd) {
  if (!hidInterfaceKnown) {
    addLog("WARN: interfaz HID todavia desconocida, esperando primer input...");
    return false;
  }

  uint8_t packet[64];
  size_t packetLen = buildPi30Command(cmd, packet, sizeof(packet));
  if (packetLen == 0) return false;

  // Redondear hacia arriba al siguiente multiplo de 8, relleno con 0x00
  size_t paddedLen = ((packetLen + 7) / 8) * 8;
  uint8_t padded[64] = {0};
  memcpy(padded, packet, packetLen);

  // Usamos sendHIDReport (control EP0, HID Set_Report) en vez de
  // sendVendorOutput (interrupt OUT endpoint) porque el Voltronic
  // solo tiene endpoint interrupt IN, no OUT. El control pipe
  // acepta Output Reports en todos los dispositivos HID estandar.
  bool allOk = true;
  for (size_t offset = 0; offset < paddedLen; offset += 8) {
    bool ok = usb.sendHIDReport(
        hidInterfaceNumber,
        ESP_USB_HOST_HID_REPORT_TYPE_OUTPUT,
        0,              // sin report ID (Voltronic no usa IDs)
        padded + offset,
        8,
        hidDeviceAddress
    );
    if (!ok) allOk = false;
    delay(10);  // respiro entre paquetes
  }
  return allOk;
}

// ============================================================================
//  Recepcion: el inversor contesta tambien en paquetes de 8 bytes.
//  Vamos acumulando hasta encontrar el '\r' final.
// ============================================================================

#define RX_BUF_SIZE 256
uint8_t rxBuffer[RX_BUF_SIZE];
size_t rxBufferLen = 0;
volatile bool rxComplete = false;

void resetRxBuffer() {
  rxBufferLen = 0;
  rxComplete = false;
}

// onHIDInput recibe TODOS los reportes HID sin filtrar por report ID.
// Usamos este en vez de onVendorInput porque el Voltronic responde con '('
// como primer byte (0x28), que no coincide con ningun report ID conocido,
// por lo que onVendorInput nunca dispara.
void onHIDInput(const EspUsbHostHIDInput &input) {
  // Capturar interfaz y direccion la primera vez que llegue algo
  if (!hidInterfaceKnown) {
    hidInterfaceNumber = input.interfaceNumber;
    hidDeviceAddress   = input.address;
    hidInterfaceKnown  = true;
    addLog("HID interfaz detectada: iface=" + String(hidInterfaceNumber) +
           " addr=" + String(hidDeviceAddress));
  }

  // Acumular bytes en el buffer de recepcion
  for (size_t i = 0; i < input.length && rxBufferLen < RX_BUF_SIZE; i++) {
    uint8_t b = input.data[i];
    rxBuffer[rxBufferLen++] = b;
    if (b == '\r') {
      rxComplete = true;
    }
  }
}

// ============================================================================
//  Estado del inversor (parseado de QPIGS)
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

  // Extended fields for Voltronic PIP 4024 (indices 16 to 20)
  String deviceStatus = "";          // Field 16 (binary flags)
  float batVoltageOffset = 0;       // Field 17
  float eepromVersion = 0;          // Field 18
  int pvPower = 0;                  // Field 19 (PV charging power W)
  String deviceStatus2 = "";         // Field 20 (binary flags)
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

// Parsea la respuesta cruda de QPIGS.
// Formato esperado (ejemplo real del protocolo PI30):
// (240.4 50.1 230.0 50.0 0000 0000 000 435 54.10 001 100 0027 0000 000.0 00.00 00000 00000101 00 00 00000 100<CRC><CR>
bool parseQpigs(const String &raw) {
  if (raw.length() < 10 || raw[0] != '(') return false;

  // Sacamos el '(' inicial y cortamos antes del CRC+CR (ultimos 3 bytes)
  String body = raw.substring(1, raw.length() - 3);

  // Separar por espacios
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

  if (fieldCount < 16) return false;  // respuesta incompleta

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
  // fields[11] = temperatura del heat sink del inversor (no usada por ahora)
  // fields[12] = corriente PV hacia bateria (no usada por ahora)
  inv.pvVoltage           = fields[13].toFloat();
  // fields[14] = battery_voltage_from_scc (no usada por ahora)
  inv.batDischargeCurrent = fields[15].toInt();

  // Campos adicionales (hasta 24 campos devueltos por algunos modelos como el PIP 4024)
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
//  Callbacks de conexion/desconexion USB
// ============================================================================

void onDeviceConnected(const EspUsbHostDeviceInfo &device) {
  deviceConnected = true;
  lastVid = device.vid;
  lastPid = device.pid;
  lastManufacturer = String(device.manufacturer);
  lastProduct = String(device.product);

  // El Voltronic tiene una sola interfaz HID (interfaz 0).
  // No esperamos onHIDInput para descubrirla porque el inversor
  // no envia datos hasta recibir un comando (protocolo request-response).
  hidInterfaceNumber = 0;
  hidDeviceAddress   = device.address;
  hidInterfaceKnown  = true;

  addLog("=== Inversor conectado: VID=0x" + String(device.vid, HEX) +
         " PID=0x" + String(device.pid, HEX) +
         " addr=" + String(device.address) + " ===");
}

void onDeviceDisconnected(const EspUsbHostDeviceInfo &device) {
  deviceConnected = false;
  hidInterfaceKnown = false;
  inv.valid = false;
  addLog("=== Inversor desconectado ===");
}

// ============================================================================
//  Polling periodico: pide QPIGS cada X segundos
// ============================================================================

unsigned long lastPollMs = 0;
const unsigned long POLL_INTERVAL_MS = 5000;  // cada 5 segundos
unsigned long pollWaitingSinceMs = 0;
bool waitingResponse = false;
const unsigned long RESPONSE_TIMEOUT_MS = 2000;

// Comando manual pendiente de enviar (ej. POP02 para cambiar de modo).
// Si esta vacio, el polling normal manda QPIGS.
String pendingCmd = "";
// Que comando esperamos respuesta ahora mismo ("QPIGS" o un comando de control).
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
    addLog("ERROR: fallo el envio de QPIGS");
  }
}

// Envia un comando de control (POP, PCP, etc.) y queda esperando ACK/NAK.
void sendControlCommand(const String &cmd) {
  if (!deviceConnected) {
    addLog("No se puede enviar " + cmd + ": sin inversor");
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
    addLog("ERROR: fallo el envio de " + cmd);
  }
}

void checkResponse() {
  if (!waitingResponse) return;

  if (rxComplete) {
    waitingResponse = false;
    String raw = "";
    for (size_t i = 0; i < rxBufferLen; i++) {
      char c = (char)rxBuffer[i];
      raw += c;
    }
    if (lastCmdSent == "QPIGS") {
      bool ok = parseQpigs(raw);
      if (ok) {
        addLog("QPIGS OK: Vbat=" + String(inv.batVoltage, 2) +
               "V Vpv=" + String(inv.pvVoltage, 1) +
               "V Carga=" + String(inv.outLoadPercent) + "%");
      } else {
        addLog("QPIGS: respuesta invalida o incompleta (len=" + String(rxBufferLen) + ")");
      }
    } else {
      // Respuesta a un comando de control: tipicamente (ACK o (NAK
      String r = raw;
      r.trim();
      addLog("Resp " + lastCmdSent + ": " + r);
      lastCmdSent = "QPIGS";  // volver al polling normal
    }
    resetRxBuffer();
  } else if (millis() - pollWaitingSinceMs > RESPONSE_TIMEOUT_MS) {
    waitingResponse = false;
    addLog(lastCmdSent + ": timeout esperando respuesta del inversor");
    lastCmdSent = "QPIGS";
    resetRxBuffer();
  }
}

// ============================================================================
//  Pagina web
// ============================================================================

String fmtAge(unsigned long ms) {
  if (ms == 0) return "nunca";
  unsigned long ageSec = (millis() - ms) / 1000;
  return String(ageSec) + "s atras";
}

// Handler: cambiar modo de salida del inversor (comando POP de PI30).
//   POP00 = Utility first (Red primero)
//   POP01 = Solar first   (Solar primero)
//   POP02 = SBU           (Solar/Battery/Utility)
void handleSetMode() {
  String m = server.arg("m");
  String cmd = "";
  if (m == "uti")        cmd = "POP00";
  else if (m == "solar") cmd = "POP01";
  else if (m == "sbu")   cmd = "POP02";

  if (cmd != "") {
    // Encolar como pendiente; el loop lo manda en cuanto el canal este libre.
    pendingCmd = cmd;
    addLog("Solicitud de modo: " + m + " (" + cmd + ")");
  }
  // Redirigir de vuelta a la pagina principal.
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "OK");
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>";
  html += "<title>MTT PIP 4024</title>";
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

  html += "<h1>&#9889; MTT PIP 4024</h1>";

  // Estado USB
  html += "<div class='box'>";
  html += "<b>Conexion USB: </b>";
  if (deviceConnected) {
    html += "<span class='ok'>CONECTADO</span> (VID 0x" + String(lastVid, HEX) +
            " PID 0x" + String(lastPid, HEX) + ", " + lastProduct + ")";
  } else {
    html += "<span class='bad'>SIN INVERSOR</span>";
  }
  html += "</div>";

  // Botones de cambio de modo (PRUEBA de comandos de escritura por USB)
  html += "<div class='box'>";
  html += "<b>Modo del inversor (prueba escritura)</b><br><br>";
  html += "<div class='modes'>";
  html += "<a class='modebtn' href='/setmode?m=sbu'>&#128267; Bateria<small>SBU / POP02</small></a>";
  html += "<a class='modebtn' href='/setmode?m=solar'>&#9728; Solar<small>POP01</small></a>";
  html += "<a class='modebtn' href='/setmode?m=uti'>&#128268; Red<small>POP00</small></a>";
  html += "</div>";
  html += "</div>";

  // Datos del inversor
  html += "<div class='box'>";
  html += "<b>Datos del inversor (QPIGS)</b><br><br>";
  if (inv.valid) {
    html += "<div class='grid'>";

    html += "<div class='metric'><div class='label'>Bateria</div><div class='value'>" +
            String(inv.batVoltage, 2) + " V</div></div>";

    html += "<div class='metric'><div class='label'>Capacidad bateria</div><div class='value'>" +
            String(inv.batCapacityPercent) + " %</div></div>";

    html += "<div class='metric'><div class='label'>Panel solar (PV)</div><div class='value'>" +
            String(inv.pvVoltage, 1) + " V</div></div>";

    html += "<div class='metric'><div class='label'>Corriente carga bat.</div><div class='value'>" +
            String(inv.batChargeCurrent) + " A</div></div>";

    html += "<div class='metric'><div class='label'>Salida (AC out)</div><div class='value'>" +
            String(inv.outVoltage, 1) + " V</div></div>";

    html += "<div class='metric'><div class='label'>Carga (load)</div><div class='value'>" +
            String(inv.outLoadPercent) + " %</div></div>";

    html += "<div class='metric'><div class='label'>Potencia activa</div><div class='value'>" +
            String(inv.outActivePower) + " W</div></div>";

    html += "<div class='metric'><div class='label'>Red (grid in)</div><div class='value'>" +
            String(inv.gridVoltage, 1) + " V</div></div>";

    html += "<div class='metric'><div class='label'>Potencia solar (PV)</div><div class='value'>" +
            String(inv.pvPower) + " W</div></div>";

    html += "<div class='metric'><div class='label'>Estado inversor</div><div class='value' style='font-size:0.9em;'>" +
            inv.deviceStatus + "</div></div>";

    html += "</div>";  // grid
    html += "<br><small>Ultima actualizacion: " + fmtAge(inv.lastUpdateMs) + "</small>";
  } else {
    html += "<span class='bad'>Sin datos validos todavia</span>";
  }
  html += "</div>";

  // === DIAGNOSTICO: respuesta QPIGS cruda + conteo de campos ===
  html += "<div class='box'>";
  html += "<b>Respuesta QPIGS cruda (diagnostico)</b><br><br>";
  if (inv.rawResponse.length() > 0) {
    // Contar campos: tomar entre '(' y antes del CRC+CR, separar por espacios
    String body = inv.rawResponse;
    if (body.length() > 0 && body[0] == '(') body = body.substring(1);
    // quitar los ultimos 3 bytes (CRC + CR) si estan
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
    html += "<div class='log'>Total de campos: <b>" + String(campos) + "</b></div><br>";
    html += "<div class='log'>" + numerado + "</div><br>";
    html += "<div class='log'>RAW (" + String(inv.rawResponse.length()) + " bytes):\n" +
            inv.rawResponse + "</div>";
  } else {
    html += "<span class='bad'>Sin respuesta cruda todavia</span>";
  }
  html += "</div>";

  // WiFi
  html += "<div class='box'>";
  html += "<b>WiFi:</b> '" + String(WIFI_SSID) + "' &mdash; IP: " + WiFi.localIP().toString();
  html += "</div>";

  // Log
  html += "<div class='box'>";
  html += "<b>Eventos recientes:</b><div class='log'>";
  if (logCount == 0) {
    html += "(sin eventos todavia)";
  } else {
    for (int i = 0; i < logCount; i++) html += logLines[i] + "\n";
  }
  html += "</div></div>";

  html += "<div class='box'><small>Se actualiza sola cada 5s. Consulta QPIGS cada " +
          String(POLL_INTERVAL_MS / 1000) + "s.</small></div>";

  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

// ============================================================================
//  setup / loop
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("############################################################");
  Serial.println("# MTT PIP 4024 - USB Host + WiFi - protocolo PI30 (QPIGS)");
  Serial.println("############################################################");

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
    Serial.print("==> IP del ESP32 (anotala): ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("ERROR: no se pudo conectar al WiFi. Revisa SSID/password.");
  }

  server.on("/", handleRoot);
  server.on("/setmode", handleSetMode);
  server.begin();
  Serial.println("Servidor web iniciado en puerto 80.");

  usb.onDeviceConnected(onDeviceConnected);
  usb.onDeviceDisconnected(onDeviceDisconnected);
  usb.onHIDInput(onHIDInput);  // recibe todos los reportes HID (reemplaza onVendorInput)

  if (!usb.begin()) {
    addLog("ERROR: usb.begin() fallo: " + String(usb.lastErrorName()));
  } else {
    addLog("USB Host inicializado. Esperando inversor...");
  }
}

void loop() {
  server.handleClient();
  checkResponse();

  if (!waitingResponse) {
    // Prioridad: si hay un comando de control pendiente, enviarlo primero.
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
