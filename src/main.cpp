/*
 * MiniLabBox v2 firmware
 *
 * This firmware runs on an ESP8266 NodeMCU board and provides a flexible
 * hardware abstraction layer, web configuration portal and UDP broadcast
 * functionality for simple lab automation tasks.  Inputs and outputs can
 * be configured at runtime as ADC channels, external ADS1115 channels,
 * remote values broadcast over the network, AC sensors (ZMPT101B/ZMCT103C)
 * or simple DC measurements through a divider.  Outputs can be PWM for
 * driving 0–10 V converters or simple digital GPIO.  A web UI allows
 * editing of the configuration and live monitoring.
 *
 * Copyright (c) 2025 MiniLabBox Developers
 */

#include <Arduino.h>
#include <LittleFS.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_ADS1015.h>
#include <Adafruit_MCP4725.h>
#include <ESP8266mDNS.h>
#include <U8g2lib.h>
#include <Updater.h>
#include <memory>
#include <cstdlib>
#include <math.h>
#include <vector>

#include "build_version.h"
#include "virtual_lab/FunctionGenerator.h"
#include "virtual_lab/MathZone.h"
#include "virtual_lab/Multimeter.h"
#include "virtual_lab/Oscilloscope.h"
#include "virtual_lab/VirtualWorkspace.h"

static void logPrintf(const char *fmt, ...);
static void logMessage(const String &msg);

static bool logStorageReady = false;
static bool littleFsFormatAttempted = false;
static String firmwareVersion = "0.0.0";

static String formatFirmwareVersion() {
  String version = String(static_cast<unsigned>(BuildInfo::kFirmwareMajor));
  version += ".";
  version += String(static_cast<unsigned>(BuildInfo::kFirmwareMinor));
  version += ".";
  version += String(static_cast<unsigned long>(BuildInfo::kFirmwarePatch));
  if (BuildInfo::HasPreReleaseTag()) {
    version += "-";
    version += BuildInfo::kFirmwarePreReleaseTag;
  }
  if (BuildInfo::HasBuildMetadata()) {
    version += "+";
    version += BuildInfo::kFirmwareBuildMetadata;
  }
  return version;
}

static bool ensureLittleFsReady(bool allowFormat = false) {
  if (logStorageReady) {
    return true;
  }
  if (LittleFS.begin()) {
    logStorageReady = true;
    return true;
  }
  if (allowFormat && !littleFsFormatAttempted) {
    Serial.println("LittleFS mount failed, formatting...");
    LittleFS.format();
    littleFsFormatAttempted = true;
    if (LittleFS.begin()) {
      logStorageReady = true;
      return true;
    }
  }
  return false;
}

static void initFirmwareVersion() {
  firmwareVersion = formatFirmwareVersion();
  logPrintf("Firmware version initialised to %s (major=%u minor=%u patch=%lu)",
            firmwareVersion.c_str(),
            static_cast<unsigned>(BuildInfo::kFirmwareMajor),
            static_cast<unsigned>(BuildInfo::kFirmwareMinor),
            static_cast<unsigned long>(BuildInfo::kFirmwarePatch));
}

static const char *getFirmwareVersion() {
  return firmwareVersion.c_str();
}

static bool decodeWaveformShape(const String &name,
                                virtual_lab::WaveformShape &shape) {
  String value = name;
  value.toLowerCase();
  if (value == "dc" || value == "constant") {
    shape = virtual_lab::WaveformShape::DC;
    return true;
  }
  if (value == "sine" || value == "sin" || value == "sinus") {
    shape = virtual_lab::WaveformShape::Sine;
    return true;
  }
  if (value == "square" || value == "carre" || value == "rect") {
    shape = virtual_lab::WaveformShape::Square;
    return true;
  }
  if (value == "triangle") {
    shape = virtual_lab::WaveformShape::Triangle;
    return true;
  }
  if (value == "saw" || value == "sawtooth" || value == "dent") {
    shape = virtual_lab::WaveformShape::Sawtooth;
    return true;
  }
  if (value == "noise") {
    shape = virtual_lab::WaveformShape::Noise;
    return true;
  }
  return false;
}

static bool decodeMultimeterMode(const String &name,
                                 virtual_lab::MultimeterMode &mode) {
  String value = name;
  value.toLowerCase();
  if (value == "dc" || value == "volt" || value == "tension") {
    mode = virtual_lab::MultimeterMode::DC;
    return true;
  }
  if (value == "ac" || value == "rms") {
    mode = virtual_lab::MultimeterMode::AC_RMS;
    return true;
  }
  if (value == "min") {
    mode = virtual_lab::MultimeterMode::MIN;
    return true;
  }
  if (value == "max") {
    mode = virtual_lab::MultimeterMode::MAX;
    return true;
  }
  if (value == "avg" || value == "average" || value == "moyenne") {
    mode = virtual_lab::MultimeterMode::AVERAGE;
    return true;
  }
  if (value == "pp" || value == "peak" || value == "peak_to_peak") {
    mode = virtual_lab::MultimeterMode::PEAK_TO_PEAK;
    return true;
  }
  return false;
}

static bool parseVariableBindings(JsonVariantConst variant,
                                  std::vector<virtual_lab::VariableBinding> &out,
                                  String &error) {
  out.clear();
  if (variant.isNull()) {
    return true;
  }
  if (!variant.is<JsonArrayConst>()) {
    error = "bindings_not_array";
    return false;
  }
  for (JsonObjectConst binding : variant.as<JsonArrayConst>()) {
    String variable = binding["variable"].as<String>();
    if (variable.length() == 0) {
      error = "binding_missing_variable";
      return false;
    }
    String signalId = binding["signal"].as<String>();
    if (signalId.length() == 0) {
      signalId = variable;
    }
    virtual_lab::VariableBinding vb;
    vb.variable = variable;
    vb.signalId = signalId;
    out.push_back(vb);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Simple logging facility.  Log messages are written to Serial and appended
// to a file on LittleFS.  The log file is kept between boots and truncated
// only when it grows beyond a configured limit.  Content can be retrieved over
// HTTP for display in the UI.
// ---------------------------------------------------------------------------
static const char *LOG_PATH = "/log.txt";
static const char *USER_FILES_DIR = "/private";
static const char *SAMPLE_FILE_PATH = "/private/sample.html";
static const char *CONFIG_FILE_PATH = "/private/io_config.json";
static const char *CONFIG_BACKUP_FILE_PATH = "/private/io_config.bak";
static const char *CONFIG_TEMP_FILE_PATH = "/private/io_config.tmp";
static const char *CONFIG_BACKUP_STAGING_PATH = "/private/io_config.bak.tmp";
static const char *LEGACY_CONFIG_FILE_PATH = "/config.json";
static const size_t CONFIG_JSON_CAPACITY = 9216;
static const char SAMPLE_FILE_CONTENT[] = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="utf-8">
  <title>MiniLabBox – sample</title>
</head>
<body>
  <h1>MiniLabBox</h1>
  <p>Ce fichier <code>sample.html</code> est stocké dans le répertoire <code>/private</code>.</p>
  <p>Modifiez son contenu depuis l'éditeur de fichiers pour tester l'interface.</p>
</body>
</html>
)rawliteral";

static bool ensureUserDirectory() {
  if (!ensureLittleFsReady(false)) {
    logMessage("LittleFS unavailable, cannot ensure private directory");
    return false;
  }
  if (LittleFS.exists(USER_FILES_DIR)) {
    return true;
  }
  if (!LittleFS.mkdir(USER_FILES_DIR)) {
    logMessage("Failed to create private user directory");
    return false;
  }
  return true;
}

static bool isValidUserFileName(const String &name) {
  if (name.length() == 0 || name.length() > 64) {
    return false;
  }
  if (name.equals(".") || name.equals("..")) {
    return false;
  }
  if (name.charAt(0) == '.') {
    return false;
  }
  for (size_t i = 0; i < name.length(); ++i) {
    char c = name.charAt(i);
    if (!(isAlphaNumeric(c) || c == '.' || c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

static bool sanitizeClientRelativePath(const String &clientPath,
                                      String &relativePath) {
  if (clientPath.length() == 0) return false;
  String cleaned = clientPath;
  cleaned.replace('\\', '/');
  while (cleaned.startsWith("/")) {
    cleaned.remove(0, 1);
  }
  cleaned.trim();
  if (cleaned.length() == 0) return false;
  if (cleaned.indexOf('/') != -1) return false;
  if (!isValidUserFileName(cleaned)) return false;
  relativePath = cleaned;
  return true;
}

static bool resolveUserPath(const String &clientPath, String &fsPath,
                            String *relativeOut = nullptr) {
  String relative;
  if (!sanitizeClientRelativePath(clientPath, relative)) {
    return false;
  }
  if (relativeOut) {
    *relativeOut = relative;
  }
  fsPath = String(USER_FILES_DIR) + "/" + relative;
  return true;
}

static String toRelativeUserPath(const String &fsPath) {
  String prefix = String(USER_FILES_DIR) + "/";
  if (fsPath.startsWith(prefix)) {
    return fsPath.substring(prefix.length());
  }
  String prefixNoSlash = prefix;
  if (prefixNoSlash.startsWith("/")) {
    prefixNoSlash.remove(0, 1);
  }
  if (fsPath.startsWith(prefixNoSlash)) {
    return fsPath.substring(prefixNoSlash.length());
  }
  String dirNoSlash = String(USER_FILES_DIR);
  if (dirNoSlash.startsWith("/")) {
    dirNoSlash.remove(0, 1);
  }
  if (fsPath == dirNoSlash || fsPath == String(USER_FILES_DIR)) {
    return "";
  }
  return fsPath;
}

static bool ensureUserStorageReady() {
  if (!ensureUserDirectory()) {
    return false;
  }
  Dir dir = LittleFS.openDir(USER_FILES_DIR);
  bool hasFile = false;
  while (dir.next()) {
    if (dir.isDirectory()) {
      continue;
    }
    hasFile = true;
    break;
  }
  if (!hasFile) {
    File f = LittleFS.open(SAMPLE_FILE_PATH, "w");
    if (!f) {
      Serial.println("Failed to create sample.html in /private");
      return false;
    }
    f.print(SAMPLE_FILE_CONTENT);
    f.close();
  }
  return true;
}

// ---------------------------------------------------------------------------
// OLED display support.  During boot we render log messages to the attached
// OLED so that early errors are visible without a serial console.  Once the
// configuration is modified via the web API, OLED logging is disabled to free
// the screen for application use.
// ---------------------------------------------------------------------------
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(
    U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ 14, /* data=*/ 12);
static bool oledLogging = true;
static const size_t OLED_STATUS_LINE_COUNT = 3;
static const size_t OLED_LOG_LINE_COUNT = 3;
static const size_t OLED_MAX_LINE_CHARS = 21;
static const uint8_t OLED_STATUS_TOP_MARGIN = 12;
static const uint8_t OLED_STATUS_LINE_HEIGHT = 14;
static const uint8_t OLED_LOG_LINE_HEIGHT = 8;
static const uint8_t OLED_LOG_BOTTOM_MARGIN = 2;
static String oledStatusLines[OLED_STATUS_LINE_COUNT];
static String oledLogBuffer[OLED_LOG_LINE_COUNT];

static void clearOledStatusLines() {
  for (size_t i = 0; i < OLED_STATUS_LINE_COUNT; ++i) {
    oledStatusLines[i] = "";
  }
}

static void clearOledLogBuffer() {
  for (size_t i = 0; i < OLED_LOG_LINE_COUNT; ++i) {
    oledLogBuffer[i] = "";
  }
}

static String normaliseOledText(const String &text) {
  String normalised = text;
  normalised.replace('\r', ' ');
  normalised.replace('\n', ' ');
  normalised.trim();
  if (normalised.length() > OLED_MAX_LINE_CHARS) {
    normalised.remove(OLED_MAX_LINE_CHARS);
  }
  return normalised;
}

static void renderOledStatusAndLog() {
  if (!oledLogging) return;

  oled.clearBuffer();

  oled.setFont(u8g2_font_7x14B_tr);
  for (size_t i = 0; i < OLED_STATUS_LINE_COUNT; ++i) {
    if (oledStatusLines[i].length() == 0) {
      continue;
    }
    uint8_t y = OLED_STATUS_TOP_MARGIN + (i * OLED_STATUS_LINE_HEIGHT);
    oled.drawStr(0, y, oledStatusLines[i].c_str());
  }

  oled.setFont(u8g2_font_5x7_tf);
  for (size_t i = 0; i < OLED_LOG_LINE_COUNT; ++i) {
    if (oledLogBuffer[i].length() == 0) {
      continue;
    }
    uint8_t y = 64 - OLED_LOG_BOTTOM_MARGIN -
                ((OLED_LOG_LINE_COUNT - 1 - i) * OLED_LOG_LINE_HEIGHT);
    oled.drawStr(0, y, oledLogBuffer[i].c_str());
  }

  oled.sendBuffer();
  oled.setFont(u8g2_font_5x7_tf);
}

static void setOledStatusLine(size_t index, const String &text,
                              bool refreshDisplay = true) {
  if (!oledLogging || index >= OLED_STATUS_LINE_COUNT) {
    return;
  }
  oledStatusLines[index] = normaliseOledText(text);
  if (refreshDisplay) {
    renderOledStatusAndLog();
  }
}

static void setOledStatusLines(const String &line0,
                               const String &line1,
                               const String &line2) {
  if (!oledLogging) {
    return;
  }
  setOledStatusLine(0, line0, false);
  setOledStatusLine(1, line1, false);
  setOledStatusLine(2, line2, false);
  renderOledStatusAndLog();
}

static bool initOled() {
  Serial.println("Initialising OLED...");
  // Ensure the I2C bus is initialised before interacting with the OLED.
  // The manufacturer wiring uses SDA on GPIO12 (D6) and SCL on GPIO14 (D5),
  // so the firmware must configure the same pins instead of the default D2/D1
  // pair.  Using the documented wiring matches the standalone example
  // provided with the display.
  Wire.begin(12, 14);  // SDA = GPIO12 (D6), SCL = GPIO14 (D5)

  // Some modules use address 0x3C while others use 0x3D.  Probe both
  // addresses and pick the first one that responds.  This avoids false
  // "not detected" errors when a different address is used or the OLED
  // is a little slow to power up.
  uint8_t address = 0x3C;
  bool found = false;
  Wire.beginTransmission(address);
  if (Wire.endTransmission() == 0) {
    found = true;
  } else {
    Wire.beginTransmission(0x3D);
    if (Wire.endTransmission() == 0) {
      address = 0x3D;
      found = true;
    }
  }

  if (!found) {
    Serial.println("OLED not detected on I2C bus");
    oledLogging = false;
    return false;
  }

  // U8G2 expects the 8-bit I2C address (shifted left by one).
  oled.setI2CAddress(address << 1);
  oled.begin();
  oled.clearBuffer();
  oled.setFont(u8g2_font_5x7_tf);
  oled.sendBuffer();
  clearOledStatusLines();
  clearOledLogBuffer();
  Serial.printf("OLED initialised at 0x%02X using SDA=GPIO12 SCL=GPIO14\n",
                address);
  return true;
}

static void oledLog(const String &msg) {
  if (!oledLogging || OLED_LOG_LINE_COUNT == 0) return;
  String shortMsg = normaliseOledText(msg);

  size_t insertIndex = 0;
  while (insertIndex < OLED_LOG_LINE_COUNT &&
         oledLogBuffer[insertIndex].length() > 0) {
    insertIndex++;
  }

  if (insertIndex < OLED_LOG_LINE_COUNT) {
    oledLogBuffer[insertIndex] = shortMsg;
  } else {
    for (size_t i = 1; i < OLED_LOG_LINE_COUNT; ++i) {
      oledLogBuffer[i - 1] = oledLogBuffer[i];
    }
    oledLogBuffer[OLED_LOG_LINE_COUNT - 1] = shortMsg;
  }
  renderOledStatusAndLog();
}

static const size_t MAX_LOG_FILE_SIZE = 16 * 1024;  // 16 KB

static void initLogging() {
  if (!ensureLittleFsReady(true)) {
    Serial.println("LittleFS unavailable, file logging disabled");
    return;
  }
  if (LittleFS.exists(LOG_PATH)) {
    File f = LittleFS.open(LOG_PATH, "r");
    if (f && f.size() > MAX_LOG_FILE_SIZE) {
      f.close();
      LittleFS.remove(LOG_PATH);
    } else if (f) {
      f.close();
    }
  }
}

static void logMessage(const String &msg) {
  Serial.println(msg);
  if (!ensureLittleFsReady(false)) {
    oledLog(msg);
    return;
  }
  File f = LittleFS.open(LOG_PATH, "a");
  if (!f) {
    f = LittleFS.open(LOG_PATH, "w");
  }
  if (f) {
    f.print('[');
    f.print(millis());
    f.print("] ");
    f.println(msg);
    f.close();
  }
  oledLog(msg);
}

static void logPrintf(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  logMessage(String(buf));
}

static bool otaUploadAuthorized = false;
static bool otaUploadInProgress = false;
static bool otaUploadSuccess = false;
static size_t otaUploadSize = 0;
static String otaLastError;

static String otaErrorMessage() {
#if defined(ARDUINO_ARCH_ESP8266)
  return Update.getErrorString();
#else
  return String("update_error");
#endif
}

// Maximum number of inputs and outputs supported.  Adjust to suit your
// application.  Keeping these small reduces memory usage on the ESP8266.
static const uint8_t MAX_INPUTS  = 4;
static const uint8_t MAX_OUTPUTS = 2;

// UDP broadcast port for exchanging measurements between nodes.  All
// MiniLabBox nodes should listen and transmit on this port.
static const uint16_t BROADCAST_PORT = 3333;

// Update intervals (in milliseconds).  These values govern how often
// inputs are sampled and broadcast.  Reducing them increases CPU
// utilisation.  Feel free to tune to your needs.
static const uint32_t INPUT_UPDATE_INTERVAL    = 50;    // 20 Hz updates
static const uint32_t BROADCAST_UPDATE_INTERVAL = 500;  // 2 Hz broadcast

// Enumerations describing supported input and output types.  These strings
// are used in the JSON configuration file and the web UI.
enum InputType {
  INPUT_DISABLED = 0,
  INPUT_ADC,
  INPUT_ADS1115,
  INPUT_REMOTE,
  INPUT_ZMPT,
  INPUT_ZMCT,
  INPUT_DIV
};

enum OutputType {
  OUTPUT_DISABLED = 0,
  OUTPUT_PWM010,
  OUTPUT_GPIO,
  OUTPUT_MCP4725
};

// Convert a type string from JSON/UI into the internal enumeration.  If
// unknown, INPUT_DISABLED is returned.
static InputType parseInputType(const String &s) {
  String t = s;
  t.toLowerCase();
  if (t == "adc")       return INPUT_ADC;
  if (t == "ads1115")   return INPUT_ADS1115;
  if (t == "remote")    return INPUT_REMOTE;
  if (t == "zmpt")      return INPUT_ZMPT;
  if (t == "zmct")      return INPUT_ZMCT;
  if (t == "div")       return INPUT_DIV;
  return INPUT_DISABLED;
}

// Convert an input type enumeration back to its string form for JSON
// serialisation and UI display.
static String inputTypeToString(InputType t) {
  switch (t) {
    case INPUT_ADC:     return "adc";
    case INPUT_ADS1115: return "ads1115";
    case INPUT_REMOTE:  return "remote";
    case INPUT_ZMPT:    return "zmpt";
    case INPUT_ZMCT:    return "zmct";
    case INPUT_DIV:     return "div";
    default:            return "disabled";
  }
}

// Convert a type string into an output type enumeration.
static OutputType parseOutputType(const String &s) {
  String t = s;
  t.toLowerCase();
  if (t == "pwm010") return OUTPUT_PWM010;
  if (t == "gpio")   return OUTPUT_GPIO;
  if (t == "mcp4725") return OUTPUT_MCP4725;
  return OUTPUT_DISABLED;
}

// Convert an output type enumeration back to a string.
static String outputTypeToString(OutputType t) {
  switch (t) {
    case OUTPUT_PWM010: return "pwm010";
    case OUTPUT_GPIO:   return "gpio";
    case OUTPUT_MCP4725: return "mcp4725";
    default:            return "disabled";
  }
}

// Structure describing a single logical input channel.  Each input has a
// name, type and several optional parameters depending on its type.  The
// scale and offset parameters allow calibration of analog measurements.
struct InputConfig {
  String     name;         // Human readable identifier (e.g. "input1")
  InputType  type;         // Which kind of input this represents
  int        pin;          // ESP8266 pin number for ADC/DIV types
  int        adsChannel;   // ADS1115 channel number (0–3)
  String     remoteNode;   // Node ID for remote input
  String     remoteName;   // Input name on remote node
  float      scale;        // Multiplier applied to the raw reading
  float      offset;       // Offset added after scaling
  String     unit;         // Unit string for display (e.g. "V", "A")
  bool       active;       // Whether this input is enabled
  float      value;        // Last measured value (populated at runtime)
};

// Structure describing a single logical output channel.  Each output can
// drive a PWM converter or simple GPIO pin.  Scale and offset can be
// used to map a desired physical output value into a duty cycle.
struct OutputConfig {
  String     name;         // Human readable identifier (e.g. "output1")
  OutputType type;         // Which kind of output this represents
  int        pin;          // ESP8266 pin number for PWM/GPIO
  int        pwmFreq;      // PWM frequency in Hz (only global frequency is used)
  uint8_t    i2cAddress;   // I2C address for MCP4725 outputs
  float      scale;        // Gain for mapping physical value to duty (0–1023)
  float      offset;       // Offset added to duty after scaling
  bool       active;       // Whether this output is enabled
  float      value;        // Last commanded value
};

// Peer security information.  Each remote MiniLabBox can have an associated
// PIN so that authenticated API calls can be made from scripts if required.
struct PeerAuth {
  String nodeId;
  String pin;
};

static const uint8_t MAX_PEERS = 16;

// Wi-Fi configuration structure.  Contains the mode (AP or STA) and
// credentials for station mode.  For AP mode the SSID is derived from
// the node ID and the password may be blank (open network).
struct WifiConfig {
  String mode;  // "AP" or "STA"
  String ssid;
  String pass;
};

// Module configuration flags.  You can enable or disable optional
// hardware modules here.  When a module is disabled, the firmware will
// not attempt to initialise or read it, saving memory and time.
struct ModulesConfig {
  bool ads1115;
  bool pwm010;
  bool zmpt;
  bool zmct;
  bool div;
  bool mcp4725;
};

// Top level configuration.  This object is loaded from and saved to
// LittleFS.  Changing any of these values via the web UI and saving
// triggers a reboot so the new configuration can be applied.
struct Config {
  String        nodeId;      // Unique node identifier used in broadcast
  WifiConfig    wifi;        // Wi-Fi settings
  ModulesConfig modules;     // Module enable flags
  uint8_t       inputCount;  // Number of configured inputs
  InputConfig   inputs[MAX_INPUTS];
  uint8_t       outputCount; // Number of configured outputs
  OutputConfig  outputs[MAX_OUTPUTS];
  uint8_t       peerCount;   // Number of stored peer PINs
  PeerAuth      peers[MAX_PEERS];
};

static Config config;            // Global configuration instance
static const uint16_t HTTP_PORT = 80;
static ESP8266WebServer primaryHttpServer(HTTP_PORT);              // HTTP server instance
static WiFiUDP udp;               // UDP object for broadcasting and listening
static Adafruit_ADS1115 ads;      // ADS1115 ADC instance
static Adafruit_MCP4725 mcp4725Drivers[MAX_OUTPUTS];
static bool mcp4725Ready[MAX_OUTPUTS] = {false};
static uint8_t mcp4725Addresses[MAX_OUTPUTS] = {0};

// Remote value cache.  When a broadcast packet is received from another
// node, each measurement is stored in this table.  Inputs configured
// as type remote look up their value here.
struct RemoteValue {
  String   nodeId;
  String   inputName;
  float    value;
  uint32_t timestamp;
};
static const int MAX_REMOTE_VALUES = 16;
static RemoteValue remoteValues[MAX_REMOTE_VALUES];
static int remoteCount = 0;

struct DiscoveredNode {
  String nodeId;
  IPAddress ip;
  uint32_t lastSeen;
};

static const int MAX_DISCOVERED_NODES = 24;
static DiscoveredNode discoveredNodes[MAX_DISCOVERED_NODES];
static uint8_t discoveredCount = 0;
static const uint32_t DISCOVERY_TIMEOUT_MS = 60000UL;
static uint32_t lastDiscoveryRequest = 0;
static const uint32_t DISCOVERY_REQUEST_INTERVAL = 5000UL;

static String sessionPin;
static String sessionToken;
static uint32_t sessionIssuedAt = 0;
static uint32_t sessionLastActivity = 0;
static const uint32_t SESSION_TIMEOUT_MS = 30UL * 60UL * 1000UL; // 30 minutes
static const char *SESSION_COOKIE_NAME = "MLBSESSION";

// Timing variables for input sampling and broadcast.  These are updated
// in loop() to ensure that tasks run at their configured intervals.
static uint32_t lastInputUpdate    = 0;
static uint32_t lastBroadcastUpdate = 0;

// Forward declarations
void loadConfig();
bool saveConfig();
void setDefaultConfig();
void setupWiFi();
void setupSensors();
void setupServer();
void updateInputs();
void updateOutputs();
void processUdp();
void sendBroadcast();
void updateRemoteValue(const String &nodeId, const String &inputName, float val);
float getRemoteValue(const String &nodeId, const String &inputName);
int parsePin(const String &p);
String pinToString(int pin);
static const InputConfig *findInputByName(const Config &cfg, const String &name);
static const OutputConfig *findOutputByName(const Config &cfg, const String &name);
static String describePinValue(int pin);
static String describeOptionalInt(int value);
static String diffInputConfig(const InputConfig &before, const InputConfig &after);
static String diffOutputConfig(const OutputConfig &before, const OutputConfig &after);
static uint8_t parseI2cAddress(const String &s);
static String formatI2cAddress(uint8_t address);
static const char *describeJsonType(const JsonVariantConst &value);
static void logIoDelta(const Config &before, const Config &after);
static bool verifyConfigStored(const Config &expected, String &errorDetail);

template <typename ArrayHandler, typename ObjectHandler>
static bool parseJsonContainerString(const String &rawValue,
                                     ArrayHandler &&handleArray,
                                     ObjectHandler &&handleObject,
                                     const char *contextLabel) {
  if (rawValue.length() == 0) {
    return false;
  }
  String toParse = rawValue;
  for (uint8_t depth = 0; depth < 3; ++depth) {
    String trimmed = toParse;
    trimmed.trim();
    if (trimmed.length() == 0) {
      return false;
    }
    DynamicJsonDocument nested(CONFIG_JSON_CAPACITY);
    DeserializationError err = deserializeJson(nested, trimmed);
    if (err) {
      logPrintf("Failed to parse %s JSON string: %s", contextLabel,
                err.c_str());
      return false;
    }
    JsonArrayConst arr = nested.as<JsonArrayConst>();
    if (!arr.isNull()) {
      handleArray(arr);
      return true;
    }
    JsonObjectConst obj = nested.as<JsonObjectConst>();
    if (!obj.isNull()) {
      handleObject(obj);
      return true;
    }
    if (nested.is<const char *>()) {
      toParse = nested.as<String>();
      continue;
    }
    break;
  }
  return false;
}

void parseConfigFromJson(const JsonDocument &doc,
                         Config &target,
                         const Config *previous,
                         bool logIoChanges) {
  if (doc.containsKey("nodeId")) {
    target.nodeId = doc["nodeId"].as<String>();
  }
  if (doc.containsKey("wifi")) {
    JsonObjectConst w = doc["wifi"].as<JsonObjectConst>();
    if (w.containsKey("mode")) target.wifi.mode = w["mode"].as<String>();
    if (w.containsKey("ssid")) target.wifi.ssid = w["ssid"].as<String>();
    if (w.containsKey("pass")) target.wifi.pass = w["pass"].as<String>();
  }
  if (doc.containsKey("modules")) {
    JsonObjectConst m = doc["modules"].as<JsonObjectConst>();
    target.modules.ads1115 = m["ads1115"].as<bool>();
    target.modules.pwm010  = m["pwm010"].as<bool>();
    target.modules.zmpt    = m["zmpt"].as<bool>();
    target.modules.zmct    = m["zmct"].as<bool>();
    target.modules.div     = m["div"].as<bool>();
    target.modules.mcp4725 = m["mcp4725"].as<bool>();
  }
  for (uint8_t i = 0; i < MAX_INPUTS; i++) {
    target.inputs[i] = {String("IN") + String(i + 1), INPUT_DISABLED, -1, -1,
                        "", "", 1.0f, 0.0f, "", false, NAN};
  }
  target.inputCount = 0;
  const Config *diffSource = (logIoChanges && previous) ? previous : nullptr;
  JsonVariantConst inputsVar = doc["inputs"];
  auto parseInputEntry = [&](JsonObjectConst o, const String &entryLabel,
                             int indexForLog) {
    if (target.inputCount >= MAX_INPUTS) {
      return;
    }
    InputConfig &ic = target.inputs[target.inputCount];
    String defaultName = ic.name;
    String entryTag;
    if (indexForLog >= 0) {
      entryTag = String("#") + String(indexForLog);
    } else if (entryLabel.length() > 0) {
      entryTag = String("\"") + entryLabel + "\"";
    } else {
      entryTag = String("#") + String(target.inputCount);
    }
    if (o.containsKey("name")) {
      ic.name = o["name"].as<String>();
    } else if (entryLabel.length() > 0) {
      ic.name = entryLabel;
      logPrintf("Input entry %s missing 'name', using key %s",
                entryTag.c_str(), entryLabel.c_str());
    } else {
      logPrintf("Input entry %s missing 'name', keeping default %s",
                entryTag.c_str(), defaultName.c_str());
      ic.name = defaultName;
    }
    if (o.containsKey("type")) {
      String typeStr = o["type"].as<String>();
      InputType parsedType = parseInputType(typeStr);
      ic.type = parsedType;
      String lower = typeStr;
      lower.toLowerCase();
      if (parsedType == INPUT_DISABLED && lower.length() > 0 &&
          lower != "disabled") {
        logPrintf("Input %s has unsupported type '%s', defaulting to disabled",
                  ic.name.c_str(), typeStr.c_str());
      }
    } else {
      logPrintf("Input %s missing type, defaulting to disabled",
                ic.name.c_str());
      ic.type = INPUT_DISABLED;
    }
    if (o.containsKey("pin")) {
      String pinStr = o["pin"].as<String>();
      int parsedPin = parsePin(pinStr);
      if (parsedPin == -1) {
        logPrintf("Input %s has invalid pin '%s'; leaving unassigned",
                  ic.name.c_str(), pinStr.c_str());
      } else {
        ic.pin = parsedPin;
      }
    } else if (ic.type == INPUT_ADC && ic.pin == -1) {
      logPrintf("Input %s missing pin, defaulting to A0", ic.name.c_str());
      ic.pin = A0;
    }
    if (o.containsKey("adsChannel")) {
      int channel = o["adsChannel"].as<int>();
      ic.adsChannel = channel;
      if (channel < 0 || channel > 3) {
        logPrintf("Input %s has out-of-range adsChannel %d (expected 0-3)",
                  ic.name.c_str(), channel);
      }
    }
    if (o.containsKey("remoteNode"))
      ic.remoteNode = o["remoteNode"].as<String>();
    if (o.containsKey("remoteName"))
      ic.remoteName = o["remoteName"].as<String>();
    if (o.containsKey("scale")) ic.scale = o["scale"].as<float>();
    if (o.containsKey("offset")) ic.offset = o["offset"].as<float>();
    if (o.containsKey("unit")) ic.unit = o["unit"].as<String>();
    if (o.containsKey("active")) ic.active = o["active"].as<bool>();
    if (diffSource) {
      const InputConfig *previousEntry = findInputByName(*diffSource, ic.name);
      if (!previousEntry) {
        String typeStr = inputTypeToString(ic.type);
        String pinStr = describePinValue(ic.pin);
        String channelStr = describeOptionalInt(ic.adsChannel);
        logPrintf("Input added: %s (type=%s, pin=%s, adsChannel=%s, active=%s)",
                  ic.name.c_str(), typeStr.c_str(), pinStr.c_str(),
                  channelStr.c_str(), ic.active ? "true" : "false");
      } else {
        String diff = diffInputConfig(*previousEntry, ic);
        if (diff.length() > 0) {
          logPrintf("Input updated: %s {%s}", ic.name.c_str(), diff.c_str());
        }
      }
    }
    target.inputCount++;
  };
  auto handleInputArray = [&](JsonArrayConst arr) {
    size_t totalInputs = arr.size();
    if (totalInputs > MAX_INPUTS) {
      logPrintf(
          "Configuration provides %u inputs but only %u are supported; ignoring extras",
          static_cast<unsigned>(totalInputs),
          static_cast<unsigned>(MAX_INPUTS));
    }
    uint8_t desired = static_cast<uint8_t>(totalInputs);
    if (desired > MAX_INPUTS) {
      desired = MAX_INPUTS;
    }
    for (uint8_t idx = 0; idx < desired; ++idx) {
      JsonVariantConst entry = arr[idx];
      JsonObjectConst entryObj = entry.as<JsonObjectConst>();
      if (entryObj.isNull()) {
        logPrintf("Input entry %u is not an object; skipping",
                  static_cast<unsigned>(idx));
        continue;
      }
      parseInputEntry(entryObj, String(), idx);
    }
  };
  auto handleInputObject = [&](JsonObjectConst obj) {
    size_t totalInputs = obj.size();
    if (totalInputs > MAX_INPUTS) {
      logPrintf(
          "Input map provides %u entries but only %u are supported; ignoring extras",
          static_cast<unsigned>(totalInputs),
          static_cast<unsigned>(MAX_INPUTS));
    }
    uint8_t processed = 0;
    for (JsonPairConst kv : obj) {
      if (target.inputCount >= MAX_INPUTS) {
        break;
      }
      JsonVariantConst value = kv.value();
      JsonObjectConst valueObj = value.as<JsonObjectConst>();
      if (valueObj.isNull()) {
        logPrintf("Input entry '%s' is not an object; skipping",
                  kv.key().c_str());
        continue;
      }
      parseInputEntry(valueObj, String(kv.key().c_str()), -1);
      processed++;
    }
    if (processed == 0) {
      logMessage("Input configuration object contained no usable entries");
    }
  };
  if (doc.containsKey("inputs")) {
    bool parsedInputs = false;
    JsonArrayConst arr = inputsVar.as<JsonArrayConst>();
    if (!arr.isNull()) {
      handleInputArray(arr);
      parsedInputs = true;
    } else {
      JsonObjectConst obj = inputsVar.as<JsonObjectConst>();
      if (!obj.isNull()) {
        handleInputObject(obj);
        parsedInputs = true;
      }
    }
    if (!parsedInputs) {
      bool parsedFromString = false;
      if (inputsVar.is<const char *>()) {
        parsedFromString =
            parseJsonContainerString(inputsVar.as<String>(), handleInputArray,
                                     handleInputObject, "input configuration");
      }
      if (!parsedFromString) {
        String serialized;
        serializeJson(inputsVar, serialized);
        if (serialized.length() > 0) {
          parsedFromString = parseJsonContainerString(
              serialized, handleInputArray, handleInputObject,
              "input configuration");
        }
      }
      parsedInputs = parsedFromString;
    }
    if (!parsedInputs) {
      logPrintf("Input configuration malformed: expected array but found %s",
                describeJsonType(inputsVar));
      logMessage("Input configuration malformed: expected array");
    }
  }
  if (diffSource) {
    for (uint8_t i = 0; i < diffSource->inputCount && i < MAX_INPUTS; i++) {
      const InputConfig &prevInput = diffSource->inputs[i];
      if (!findInputByName(target, prevInput.name)) {
        logPrintf("Input removed: %s", prevInput.name.c_str());
      }
    }
  }
  for (uint8_t i = 0; i < MAX_OUTPUTS; i++) {
    target.outputs[i] = {String("OUT") + String(i + 1), OUTPUT_DISABLED, -1,
                         2000, 0x60, 1.0f, 0.0f, false, 0.0f};
  }
  target.outputCount = 0;
  JsonVariantConst outputsVar = doc["outputs"];
  auto parseOutputEntry = [&](JsonObjectConst o, const String &entryLabel,
                              int indexForLog) {
    if (target.outputCount >= MAX_OUTPUTS) {
      return;
    }
    OutputConfig &oc = target.outputs[target.outputCount];
    String defaultName = oc.name;
    String entryTag;
    if (indexForLog >= 0) {
      entryTag = String("#") + String(indexForLog);
    } else if (entryLabel.length() > 0) {
      entryTag = String("\"") + entryLabel + "\"";
    } else {
      entryTag = String("#") + String(target.outputCount);
    }
    if (o.containsKey("name")) {
      oc.name = o["name"].as<String>();
    } else if (entryLabel.length() > 0) {
      oc.name = entryLabel;
      logPrintf("Output entry %s missing 'name', using key %s",
                entryTag.c_str(), entryLabel.c_str());
    } else {
      logPrintf("Output entry %s missing 'name', keeping default %s",
                entryTag.c_str(), defaultName.c_str());
      oc.name = defaultName;
    }
    if (o.containsKey("type")) {
      String typeStr = o["type"].as<String>();
      OutputType parsedType = parseOutputType(typeStr);
      oc.type = parsedType;
      String lower = typeStr;
      lower.toLowerCase();
      if (parsedType == OUTPUT_DISABLED && lower.length() > 0 &&
          lower != "disabled") {
        logPrintf(
            "Output %s has unsupported type '%s', defaulting to disabled",
            oc.name.c_str(), typeStr.c_str());
      }
    } else {
      logPrintf("Output %s missing type, defaulting to disabled",
                oc.name.c_str());
      oc.type = OUTPUT_DISABLED;
    }
    if (o.containsKey("pin")) {
      String pinStr = o["pin"].as<String>();
      int parsedPin = parsePin(pinStr);
      if (parsedPin == -1) {
        logPrintf("Output %s has invalid pin '%s'; leaving unassigned",
                  oc.name.c_str(), pinStr.c_str());
      } else {
        oc.pin = parsedPin;
      }
    }
    if (o.containsKey("pwmFreq")) oc.pwmFreq = o["pwmFreq"].as<int>();
    if (o.containsKey("i2cAddress"))
      oc.i2cAddress = parseI2cAddress(o["i2cAddress"].as<String>());
    if (o.containsKey("scale")) oc.scale = o["scale"].as<float>();
    if (o.containsKey("offset")) oc.offset = o["offset"].as<float>();
    if (o.containsKey("active")) oc.active = o["active"].as<bool>();
    if (o.containsKey("value")) {
      oc.value = o["value"].as<float>();
    } else {
      oc.value = 0.0f;
    }
    if (diffSource) {
      const OutputConfig *previousEntry = findOutputByName(*diffSource, oc.name);
      if (!previousEntry) {
        String typeStr = outputTypeToString(oc.type);
        String pinStr = describePinValue(oc.pin);
        String addrStr = formatI2cAddress(oc.i2cAddress);
        logPrintf("Output added: %s (type=%s, pin=%s, pwm=%d, addr=%s, active=%s)",
                  oc.name.c_str(), typeStr.c_str(), pinStr.c_str(), oc.pwmFreq,
                  addrStr.c_str(), oc.active ? "true" : "false");
      } else {
        String diff = diffOutputConfig(*previousEntry, oc);
        if (diff.length() > 0) {
          logPrintf("Output updated: %s {%s}", oc.name.c_str(), diff.c_str());
        }
      }
    }
    target.outputCount++;
  };
  auto handleOutputArray = [&](JsonArrayConst arr) {
    size_t totalOutputs = arr.size();
    if (totalOutputs > MAX_OUTPUTS) {
      logPrintf(
          "Configuration provides %u outputs but only %u are supported; ignoring extras",
          static_cast<unsigned>(totalOutputs),
          static_cast<unsigned>(MAX_OUTPUTS));
    }
    uint8_t desired = static_cast<uint8_t>(totalOutputs);
    if (desired > MAX_OUTPUTS) {
      desired = MAX_OUTPUTS;
    }
    for (uint8_t idx = 0; idx < desired; ++idx) {
      JsonVariantConst entry = arr[idx];
      JsonObjectConst entryObj = entry.as<JsonObjectConst>();
      if (entryObj.isNull()) {
        logPrintf("Output entry %u is not an object; skipping",
                  static_cast<unsigned>(idx));
        continue;
      }
      parseOutputEntry(entryObj, String(), idx);
    }
  };
  auto handleOutputObject = [&](JsonObjectConst obj) {
    size_t totalOutputs = obj.size();
    if (totalOutputs > MAX_OUTPUTS) {
      logPrintf(
          "Output map provides %u entries but only %u are supported; ignoring extras",
          static_cast<unsigned>(totalOutputs),
          static_cast<unsigned>(MAX_OUTPUTS));
    }
    uint8_t processed = 0;
    for (JsonPairConst kv : obj) {
      if (target.outputCount >= MAX_OUTPUTS) {
        break;
      }
      JsonVariantConst value = kv.value();
      JsonObjectConst valueObj = value.as<JsonObjectConst>();
      if (valueObj.isNull()) {
        logPrintf("Output entry '%s' is not an object; skipping",
                  kv.key().c_str());
        continue;
      }
      parseOutputEntry(valueObj, String(kv.key().c_str()), -1);
      processed++;
    }
    if (processed == 0) {
      logMessage("Output configuration object contained no usable entries");
    }
  };
  if (doc.containsKey("outputs")) {
    bool parsedOutputs = false;
    JsonArrayConst arr = outputsVar.as<JsonArrayConst>();
    if (!arr.isNull()) {
      handleOutputArray(arr);
      parsedOutputs = true;
    } else {
      JsonObjectConst obj = outputsVar.as<JsonObjectConst>();
      if (!obj.isNull()) {
        handleOutputObject(obj);
        parsedOutputs = true;
      }
    }
    if (!parsedOutputs) {
      bool parsedFromString = false;
      if (outputsVar.is<const char *>()) {
        parsedFromString = parseJsonContainerString(outputsVar.as<String>(),
                                                   handleOutputArray,
                                                   handleOutputObject,
                                                   "output configuration");
      }
      if (!parsedFromString) {
        String serialized;
        serializeJson(outputsVar, serialized);
        if (serialized.length() > 0) {
          parsedFromString = parseJsonContainerString(
              serialized, handleOutputArray, handleOutputObject,
              "output configuration");
        }
      }
      parsedOutputs = parsedFromString;
    }
    if (!parsedOutputs) {
      logPrintf("Output configuration malformed: expected array but found %s",
                describeJsonType(outputsVar));
      logMessage("Output configuration malformed: expected array");
    }
  }
  if (diffSource) {
    for (uint8_t i = 0; i < diffSource->outputCount && i < MAX_OUTPUTS; i++) {
      const OutputConfig &prevOutput = diffSource->outputs[i];
      if (!findOutputByName(target, prevOutput.name)) {
        logPrintf("Output removed: %s", prevOutput.name.c_str());
      }
    }
  }
  target.peerCount = 0;
  JsonVariantConst peersVar = doc["peers"];
  auto parsePeerEntry = [&](JsonObjectConst o, const String &entryLabel,
                            int indexForLog) {
    if (target.peerCount >= MAX_PEERS) {
      return;
    }
    PeerAuth &pa = target.peers[target.peerCount];
    if (o.containsKey("nodeId")) {
      pa.nodeId = o["nodeId"].as<String>();
    } else if (entryLabel.length() > 0) {
      pa.nodeId = entryLabel;
    } else {
      pa.nodeId = "";
    }
    if (pa.nodeId.length() == 0) {
      if (entryLabel.length() > 0) {
        logPrintf("Peer entry '%s' missing nodeId", entryLabel.c_str());
      } else if (indexForLog >= 0) {
        logPrintf("Peer entry %u missing nodeId",
                  static_cast<unsigned>(indexForLog));
      } else {
        logPrintf("Peer entry #%u missing nodeId",
                  static_cast<unsigned>(target.peerCount));
      }
    }
    if (o.containsKey("pin")) {
      pa.pin = o["pin"].as<String>();
    } else {
      pa.pin = "";
    }
    if (pa.pin.length() == 0) {
      if (entryLabel.length() > 0) {
        logPrintf("Peer entry '%s' missing pin", entryLabel.c_str());
      } else if (indexForLog >= 0) {
        logPrintf("Peer entry %u missing pin",
                  static_cast<unsigned>(indexForLog));
      } else {
        logPrintf("Peer entry #%u missing pin",
                  static_cast<unsigned>(target.peerCount));
      }
    }
    target.peerCount++;
  };
  auto handlePeerArray = [&](JsonArrayConst arr) {
    size_t totalPeers = arr.size();
    if (totalPeers > MAX_PEERS) {
      logPrintf(
          "Configuration provides %u peers but only %u are supported; ignoring extras",
          static_cast<unsigned>(totalPeers),
          static_cast<unsigned>(MAX_PEERS));
    }
    uint8_t desired = static_cast<uint8_t>(totalPeers);
    if (desired > MAX_PEERS) desired = MAX_PEERS;
    for (uint8_t idx = 0; idx < desired; ++idx) {
      JsonVariantConst entry = arr[idx];
      JsonObjectConst entryObj = entry.as<JsonObjectConst>();
      if (entryObj.isNull()) {
        logPrintf("Peer entry %u is not an object; skipping",
                  static_cast<unsigned>(idx));
        continue;
      }
      parsePeerEntry(entryObj, String(), idx);
    }
  };
  auto handlePeerObject = [&](JsonObjectConst obj) {
    size_t totalPeers = obj.size();
    if (totalPeers > MAX_PEERS) {
      logPrintf(
          "Peer map provides %u entries but only %u are supported; ignoring extras",
          static_cast<unsigned>(totalPeers),
          static_cast<unsigned>(MAX_PEERS));
    }
    for (JsonPairConst kv : obj) {
      if (target.peerCount >= MAX_PEERS) {
        break;
      }
      JsonVariantConst value = kv.value();
      JsonObjectConst valueObj = value.as<JsonObjectConst>();
      if (valueObj.isNull()) {
        logPrintf("Peer entry '%s' is not an object; skipping",
                  kv.key().c_str());
        continue;
      }
      parsePeerEntry(valueObj, String(kv.key().c_str()), -1);
    }
  };
  if (doc.containsKey("peers")) {
    bool parsedPeers = false;
    JsonArrayConst arr = peersVar.as<JsonArrayConst>();
    if (!arr.isNull()) {
      handlePeerArray(arr);
      parsedPeers = true;
    } else {
      JsonObjectConst obj = peersVar.as<JsonObjectConst>();
      if (!obj.isNull()) {
        handlePeerObject(obj);
        parsedPeers = true;
      }
    }
    if (!parsedPeers) {
      bool parsedFromString = false;
      if (peersVar.is<const char *>()) {
        parsedFromString = parseJsonContainerString(peersVar.as<String>(),
                                                   handlePeerArray,
                                                   handlePeerObject,
                                                   "peer configuration");
      }
      if (!parsedFromString) {
        String serialized;
        serializeJson(peersVar, serialized);
        if (serialized.length() > 0) {
          parsedFromString = parseJsonContainerString(
              serialized, handlePeerArray, handlePeerObject,
              "peer configuration");
        }
      }
      parsedPeers = parsedFromString;
    }
    if (!parsedPeers) {
      logPrintf("Peer configuration malformed: expected array but found %s",
                describeJsonType(peersVar));
      logMessage("Peer configuration malformed: expected array");
      target.peerCount = 0;
    }
  }
  for (uint8_t i = target.peerCount; i < MAX_PEERS; i++) {
    target.peers[i].nodeId = "";
    target.peers[i].pin = "";
  }
}

void parseConfigFromJson(const JsonDocument &doc, Config &target) {
  parseConfigFromJson(doc, target, nullptr, false);
}



int parsePin(const String &pinString) {
  String trimmed = pinString;
  trimmed.trim();
  if (trimmed.length() == 0) {
    return -1;
  }

  String upper = trimmed;
  upper.toUpperCase();

  struct PinName {
    const char *name;
    int value;
  };

  static const PinName kNamedPins[] = {
      {"D0", D0},   {"D1", D1},   {"D2", D2},   {"D3", D3},
      {"D4", D4},   {"D5", D5},   {"D6", D6},   {"D7", D7},
      {"D8", D8},   {"D9", D9},   {"D10", D10}, {"A0", A0},
  };

  for (const auto &entry : kNamedPins) {
    if (upper == entry.name) {
      return entry.value;
    }
  }

  if (upper.startsWith("GPIO")) {
    const char *start = upper.c_str() + 4;
    if (*start != '\0') {
      char *end = nullptr;
      long value = strtol(start, &end, 10);
      if (end != start && *end == '\0') {
        return static_cast<int>(value);
      }
    }
    return -1;
  }

  char *end = nullptr;
  long value = strtol(trimmed.c_str(), &end, 0);
  if (end == trimmed.c_str() || *end != '\0') {
    return -1;
  }
  return static_cast<int>(value);
}

// Utility to convert a numeric pin back to a string.  Only common
// NodeMCU pin names are supported.  Unknown pins are returned as
// hexadecimal strings.
String pinToString(int pin) {
  switch (pin) {
    case D0:  return "D0";
    case D1:  return "D1";
    case D2:  return "D2";
    case D3:  return "D3";
    case D4:  return "D4";
    case D5:  return "D5";
    case D6:  return "D6";
    case D7:  return "D7";
    case D8:  return "D8";
    case D9:  return "D9";
    case D10: return "D10";
    case A0:  return "A0";
    default:  return String(pin);
  }
}

String formatPin(uint16_t value) {
  char buf[5];
  snprintf(buf, sizeof(buf), "%04u", static_cast<unsigned int>(value % 10000));
  return String(buf);
}

static uint8_t parseI2cAddress(const String &s) {
  String trimmed = s;
  trimmed.trim();
  if (trimmed.length() == 0) {
    return 0x60;
  }
  char *endPtr = nullptr;
  long value = strtol(trimmed.c_str(), &endPtr, 0);
  if (endPtr == trimmed.c_str()) {
    return 0x60;
  }
  if (value < 0 || value > 0x7F) {
    return 0x60;
  }
  return static_cast<uint8_t>(value);
}

static String formatI2cAddress(uint8_t address) {
  char buf[7];
  snprintf(buf, sizeof(buf), "0x%02X", static_cast<unsigned int>(address & 0x7F));
  return String(buf);
}

static const char *describeJsonType(const JsonVariantConst &value) {
  if (value.isNull()) return "null";
  if (value.is<JsonArrayConst>()) return "array";
  if (value.is<JsonObjectConst>()) return "object";
  if (value.is<const char *>()) return "string";
  if (value.is<bool>()) return "boolean";
  if (value.is<float>()) return "float";
  if (value.is<long>() || value.is<int>()) return "integer";
  if (value.is<unsigned long>() || value.is<unsigned int>()) return "unsigned";
  return "unknown";
}

static void logConfigSummary(const char *prefix) {
  logPrintf(
      "%s config summary: nodeId=%s, wifi.mode=%s, ssid=%s, inputs=%u, outputs=%u, peers=%u",
      prefix, config.nodeId.c_str(), config.wifi.mode.c_str(),
      config.wifi.ssid.c_str(), static_cast<unsigned>(config.inputCount),
      static_cast<unsigned>(config.outputCount),
      static_cast<unsigned>(config.peerCount));
}

static void logConfigJson(const char *context, JsonDocument &doc) {
  JsonObject wifiObj = doc["wifi"].as<JsonObject>();
  bool hadPass = false;
  String passCopy;
  if (!wifiObj.isNull() && wifiObj.containsKey("pass")) {
    passCopy = wifiObj["pass"].as<String>();
    wifiObj["pass"] = "***";
    hadPass = true;
  }
  String payload;
  serializeJson(doc, payload);
  logPrintf("%s config JSON: %s", context, payload.c_str());
  if (hadPass) {
    wifiObj["pass"] = passCopy;
  }
}

static void populateConfigJson(JsonDocument &doc,
                               bool includeRuntimeFields = false) {
  doc.clear();
  doc["nodeId"] = config.nodeId;
  if (includeRuntimeFields) {
    doc["fwVersion"] = getFirmwareVersion();
  }
  JsonObject wifiObj = doc.createNestedObject("wifi");
  wifiObj["mode"] = config.wifi.mode;
  wifiObj["ssid"] = config.wifi.ssid;
  wifiObj["pass"] = config.wifi.pass;
  JsonObject modObj = doc.createNestedObject("modules");
  modObj["ads1115"] = config.modules.ads1115;
  modObj["pwm010"] = config.modules.pwm010;
  modObj["zmpt"] = config.modules.zmpt;
  modObj["zmct"] = config.modules.zmct;
  modObj["div"] = config.modules.div;
  modObj["mcp4725"] = config.modules.mcp4725;
  doc["inputCount"] = config.inputCount;
  JsonArray inputsArr = doc.createNestedArray("inputs");
  for (uint8_t i = 0; i < config.inputCount && i < MAX_INPUTS; i++) {
    JsonObject o = inputsArr.createNestedObject();
    const InputConfig &ic = config.inputs[i];
    o["name"] = ic.name;
    o["type"] = inputTypeToString(ic.type);
    o["pin"] = pinToString(ic.pin);
    o["adsChannel"] = ic.adsChannel;
    o["remoteNode"] = ic.remoteNode;
    o["remoteName"] = ic.remoteName;
    o["scale"] = ic.scale;
    o["offset"] = ic.offset;
    o["unit"] = ic.unit;
    o["active"] = ic.active;
  }
  doc["outputCount"] = config.outputCount;
  JsonArray outputsArr = doc.createNestedArray("outputs");
  for (uint8_t i = 0; i < config.outputCount && i < MAX_OUTPUTS; i++) {
    JsonObject o = outputsArr.createNestedObject();
    const OutputConfig &oc = config.outputs[i];
    o["name"] = oc.name;
    o["type"] = outputTypeToString(oc.type);
    o["pin"] = pinToString(oc.pin);
    o["pwmFreq"] = oc.pwmFreq;
    o["i2cAddress"] = formatI2cAddress(oc.i2cAddress);
    o["scale"] = oc.scale;
    o["offset"] = oc.offset;
    o["active"] = oc.active;
    o["value"] = oc.value;
  }
  doc["peerCount"] = config.peerCount;
  JsonArray peersArr = doc.createNestedArray("peers");
  for (uint8_t i = 0; i < config.peerCount && i < MAX_PEERS; i++) {
    JsonObject o = peersArr.createNestedObject();
    o["nodeId"] = config.peers[i].nodeId;
    o["pin"] = config.peers[i].pin;
  }
}

static bool loadConfigFromPath(const char *path, const char *label) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    logPrintf("Failed to open %s config file: %s", label, path);
    return false;
  }
  size_t size = f.size();
  std::unique_ptr<char[]> buf(new char[size + 1]);
  size_t read = f.readBytes(buf.get(), size);
  f.close();
  if (read > size) {
    read = size;
  }
  buf[read] = '\0';
  logPrintf("Reading configuration from %s (%u/%u bytes)", path,
            static_cast<unsigned>(read), static_cast<unsigned>(size));
  if (read != size) {
    logPrintf("Warning: short read on %s (expected %u bytes, got %u)", path,
              static_cast<unsigned>(size), static_cast<unsigned>(read));
  }
  DynamicJsonDocument doc(CONFIG_JSON_CAPACITY);
  DeserializationError err = deserializeJson(doc, buf.get(), read);
  if (err) {
    logPrintf("Failed to parse %s config JSON: %s", label, err.c_str());
    return false;
  }
  parseConfigFromJson(doc, config);
  logConfigSummary(label);
  logConfigJson(label, doc);
  logMessage(String("Configuration loaded from ") + path);
  return true;
}

// Default configuration used when no config file is present.  The
// default Node ID is derived from the MAC address.  The system starts
// in access point mode to allow initial configuration via the web UI.
void setDefaultConfig() {
  // Derive a node ID from the last 4 characters of the MAC address
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String suffix = mac.substring(mac.length() - 4);
  config.nodeId = String("minilabox") + suffix;

  // Default to AP mode with SSID based on the node ID
  config.wifi.mode = "AP";
  config.wifi.ssid = config.nodeId;
  config.wifi.pass = "";

  // Disable optional modules by default.  They can be enabled via the
  // configuration page.
  config.modules.ads1115 = false;
  config.modules.pwm010  = false;
  config.modules.zmpt    = false;
  config.modules.zmct    = false;
  config.modules.div     = false;
  config.modules.mcp4725 = false;

  // Start with no inputs configured.  Populate the backing array with disabled
  // placeholders so indexes remain valid until the UI adds channels.
  config.inputCount = 0;
  for (uint8_t i = 0; i < MAX_INPUTS; i++) {
    config.inputs[i] = {String("IN") + String(i + 1), INPUT_DISABLED, -1, -1, "", "", 1.0f, 0.0f, "", false, NAN};
  }

  // Likewise start with no outputs.  Disabled placeholders keep a predictable
  // naming scheme that can be reused when the user creates new channels.
  config.outputCount = 0;
  for (uint8_t i = 0; i < MAX_OUTPUTS; i++) {
    config.outputs[i] = {String("OUT") + String(i + 1), OUTPUT_DISABLED, -1, 2000, 0x60, 1.0f, 0.0f, false, 0.0f};
  }

  config.peerCount = 0;
  for (uint8_t i = 0; i < MAX_PEERS; i++) {
    config.peers[i] = {"", ""};
  }
}

// Load configuration from LittleFS.  If loading fails (no file or parse
// error) the default configuration is applied and saved back to
// LittleFS.  JSON fields missing from the file fall back to defaults.
void loadConfig() {
  if (!ensureLittleFsReady(false)) {
    logMessage("LittleFS unavailable, applying defaults");
    setDefaultConfig();
    return;
  }
  if (!ensureUserDirectory()) {
    logMessage("Failed to ensure private directory for config");
    setDefaultConfig();
    return;
  }
  bool loaded = false;
  if (LittleFS.exists(CONFIG_FILE_PATH)) {
    if (loadConfigFromPath(CONFIG_FILE_PATH, "primary")) {
      loaded = true;
    } else {
      logMessage("Primary configuration load failed");
    }
  }
  if (!loaded && LittleFS.exists(CONFIG_BACKUP_FILE_PATH)) {
    logMessage("Attempting to load configuration from backup");
    if (loadConfigFromPath(CONFIG_BACKUP_FILE_PATH, "backup")) {
      loaded = true;
      if (!saveConfig()) {
        logMessage("Failed to rewrite configuration after restoring backup");
      }
    } else {
      logMessage("Backup configuration load failed");
    }
  }
  if (!loaded && LittleFS.exists(LEGACY_CONFIG_FILE_PATH)) {
    logMessage("Migrating legacy configuration from /config.json");
    if (loadConfigFromPath(LEGACY_CONFIG_FILE_PATH, "legacy")) {
      loaded = true;
      if (saveConfig()) {
        LittleFS.remove(LEGACY_CONFIG_FILE_PATH);
        logMessage("Legacy configuration migrated to /private/io_config.json");
      } else {
        logMessage("Failed to save migrated configuration");
      }
    } else {
      logMessage("Legacy config parse failed, applying defaults");
    }
  }
  if (!loaded) {
    logMessage("No valid configuration found, applying defaults");
    setDefaultConfig();
    if (!saveConfig()) {
      logMessage("Failed to save default configuration");
    }
  }
}

// Save the current configuration to LittleFS.  If writing fails the
// operation is silently ignored.  Always call saveConfig() after
// modifying the global config.  Returns true on success.
static bool removeFileIfExists(const char *path) {
  if (!LittleFS.exists(path)) {
    return true;
  }
  if (!LittleFS.remove(path)) {
    logPrintf("Failed to remove %s", path);
    return false;
  }
  return true;
}

static bool writeStringToFile(const char *path, const String &payload) {
  File f = LittleFS.open(path, "w");
  if (!f) {
    logPrintf("Failed to open %s for writing", path);
    return false;
  }
  size_t written = f.print(payload);
  f.flush();
  f.close();
  if (written != payload.length()) {
    logPrintf("Short write when saving %s: expected %u bytes, wrote %u", path,
              static_cast<unsigned>(payload.length()),
              static_cast<unsigned>(written));
    LittleFS.remove(path);
    return false;
  }
  return true;
}

static bool copyFileToPath(const char *sourcePath, const char *destPath) {
  File src = LittleFS.open(sourcePath, "r");
  if (!src) {
    logPrintf("Failed to open %s for reading", sourcePath);
    return false;
  }
  File dst = LittleFS.open(destPath, "w");
  if (!dst) {
    logPrintf("Failed to open %s for writing", destPath);
    src.close();
    return false;
  }
  uint8_t buffer[256];
  while (src.available()) {
    size_t read = src.read(buffer, sizeof(buffer));
    if (read == 0) {
      break;
    }
    size_t written = dst.write(buffer, read);
    if (written != read) {
      logPrintf("Short write while copying %s to %s", sourcePath, destPath);
      src.close();
      dst.close();
      LittleFS.remove(destPath);
      return false;
    }
  }
  dst.flush();
  src.close();
  dst.close();
  return true;
}

bool saveConfig() {
  DynamicJsonDocument doc(CONFIG_JSON_CAPACITY);
  populateConfigJson(doc);
  logConfigSummary("Saving");
  logConfigJson("Saving", doc);
  String payload;
  if (serializeJson(doc, payload) == 0) {
    logMessage("Failed to encode config JSON");
    return false;
  }
  if (!ensureUserDirectory()) {
    logMessage("Failed to ensure private directory for config");
    return false;
  }
  if (!removeFileIfExists(CONFIG_TEMP_FILE_PATH)) {
    return false;
  }
  if (!writeStringToFile(CONFIG_TEMP_FILE_PATH, payload)) {
    return false;
  }
  if (!removeFileIfExists(CONFIG_BACKUP_STAGING_PATH)) {
    LittleFS.remove(CONFIG_TEMP_FILE_PATH);
    return false;
  }
  bool hadExistingConfig = LittleFS.exists(CONFIG_FILE_PATH);
  if (hadExistingConfig) {
    if (!LittleFS.rename(CONFIG_FILE_PATH, CONFIG_BACKUP_STAGING_PATH)) {
      logPrintf("Failed to stage existing configuration %s",
                CONFIG_FILE_PATH);
      LittleFS.remove(CONFIG_TEMP_FILE_PATH);
      return false;
    }
    logPrintf("Existing configuration moved to staging %s",
              CONFIG_BACKUP_STAGING_PATH);
  }
  if (!LittleFS.rename(CONFIG_TEMP_FILE_PATH, CONFIG_FILE_PATH)) {
    logPrintf("Failed to commit configuration file %s", CONFIG_FILE_PATH);
    LittleFS.remove(CONFIG_TEMP_FILE_PATH);
    if (hadExistingConfig) {
      if (!LittleFS.rename(CONFIG_BACKUP_STAGING_PATH, CONFIG_FILE_PATH)) {
        logPrintf("Failed to restore configuration from staging %s",
                  CONFIG_BACKUP_STAGING_PATH);
      }
    }
    return false;
  }
  if (!writeStringToFile(CONFIG_BACKUP_FILE_PATH, payload)) {
    logPrintf("Failed to refresh backup config file %s", CONFIG_BACKUP_FILE_PATH);
    if (hadExistingConfig) {
      if (!removeFileIfExists(CONFIG_FILE_PATH)) {
        logPrintf("Unable to remove partially written config %s", CONFIG_FILE_PATH);
      }
      if (!LittleFS.rename(CONFIG_BACKUP_STAGING_PATH, CONFIG_FILE_PATH)) {
        logPrintf("Failed to restore configuration from staging %s",
                  CONFIG_BACKUP_STAGING_PATH);
      } else {
        logPrintf("Configuration restored from staging after backup failure");
        copyFileToPath(CONFIG_FILE_PATH, CONFIG_BACKUP_FILE_PATH);
      }
    } else {
      removeFileIfExists(CONFIG_FILE_PATH);
    }
    return false;
  }
  if (hadExistingConfig) {
    removeFileIfExists(CONFIG_BACKUP_STAGING_PATH);
  }
  logPrintf("Configuration saved to %s (%u bytes)", CONFIG_FILE_PATH,
            static_cast<unsigned>(payload.length()));
  logMessage("Configuration saved");
  return true;
}

static const InputConfig *findInputByName(const Config &cfg, const String &name) {
  for (uint8_t i = 0; i < cfg.inputCount && i < MAX_INPUTS; i++) {
    if (cfg.inputs[i].name == name) {
      return &cfg.inputs[i];
    }
  }
  return nullptr;
}

static const OutputConfig *findOutputByName(const Config &cfg, const String &name) {
  for (uint8_t i = 0; i < cfg.outputCount && i < MAX_OUTPUTS; i++) {
    if (cfg.outputs[i].name == name) {
      return &cfg.outputs[i];
    }
  }
  return nullptr;
}

static bool floatsDiffer(float a, float b) {
  bool aNan = isnan(a);
  bool bNan = isnan(b);
  if (aNan && bNan) {
    return false;
  }
  if (aNan != bNan) {
    return true;
  }
  return fabsf(a - b) > 0.0001f;
}

static String describeStringValue(const String &value) {
  if (value.length() == 0) {
    return String("(empty)");
  }
  return value;
}

static String describePinValue(int pin) {
  if (pin < 0) {
    return String("-");
  }
  return pinToString(pin);
}

static String describeOptionalInt(int value) {
  if (value < 0) {
    return String("-");
  }
  return String(value);
}

static String describeFloatValue(float value) {
  if (isnan(value)) {
    return String("nan");
  }
  return String(value, 4);
}

static void appendDiff(String &diff,
                       const char *field,
                       const String &before,
                       const String &after) {
  if (diff.length() > 0) {
    diff += ", ";
  }
  diff += String(field) + ": " + before + " -> " + after;
}

static String diffInputConfig(const InputConfig &before, const InputConfig &after) {
  String diff;
  if (before.type != after.type) {
    appendDiff(diff, "type", inputTypeToString(before.type),
               inputTypeToString(after.type));
  }
  if (before.pin != after.pin) {
    appendDiff(diff, "pin", describePinValue(before.pin),
               describePinValue(after.pin));
  }
  if (before.adsChannel != after.adsChannel) {
    appendDiff(diff, "adsChannel", describeOptionalInt(before.adsChannel),
               describeOptionalInt(after.adsChannel));
  }
  if (before.remoteNode != after.remoteNode) {
    appendDiff(diff, "remoteNode", describeStringValue(before.remoteNode),
               describeStringValue(after.remoteNode));
  }
  if (before.remoteName != after.remoteName) {
    appendDiff(diff, "remoteName", describeStringValue(before.remoteName),
               describeStringValue(after.remoteName));
  }
  if (floatsDiffer(before.scale, after.scale)) {
    appendDiff(diff, "scale", describeFloatValue(before.scale),
               describeFloatValue(after.scale));
  }
  if (floatsDiffer(before.offset, after.offset)) {
    appendDiff(diff, "offset", describeFloatValue(before.offset),
               describeFloatValue(after.offset));
  }
  if (before.unit != after.unit) {
    appendDiff(diff, "unit", describeStringValue(before.unit),
               describeStringValue(after.unit));
  }
  if (before.active != after.active) {
    appendDiff(diff, "active", before.active ? "true" : "false",
               after.active ? "true" : "false");
  }
  return diff;
}

static String diffOutputConfig(const OutputConfig &before, const OutputConfig &after) {
  String diff;
  if (before.type != after.type) {
    appendDiff(diff, "type", outputTypeToString(before.type),
               outputTypeToString(after.type));
  }
  if (before.pin != after.pin) {
    appendDiff(diff, "pin", describePinValue(before.pin),
               describePinValue(after.pin));
  }
  if (before.pwmFreq != after.pwmFreq) {
    appendDiff(diff, "pwmFreq", String(before.pwmFreq), String(after.pwmFreq));
  }
  if (before.i2cAddress != after.i2cAddress) {
    appendDiff(diff, "i2cAddress", formatI2cAddress(before.i2cAddress),
               formatI2cAddress(after.i2cAddress));
  }
  if (floatsDiffer(before.scale, after.scale)) {
    appendDiff(diff, "scale", describeFloatValue(before.scale),
               describeFloatValue(after.scale));
  }
  if (floatsDiffer(before.offset, after.offset)) {
    appendDiff(diff, "offset", describeFloatValue(before.offset),
               describeFloatValue(after.offset));
  }
  if (before.active != after.active) {
    appendDiff(diff, "active", before.active ? "true" : "false",
               after.active ? "true" : "false");
  }
  if (floatsDiffer(before.value, after.value)) {
    appendDiff(diff, "value", describeFloatValue(before.value),
               describeFloatValue(after.value));
  }
  return diff;
}

static void logIoDelta(const Config &before, const Config &after) {
  for (uint8_t i = 0; i < after.inputCount && i < MAX_INPUTS; ++i) {
    const InputConfig &ic = after.inputs[i];
    const InputConfig *previousEntry = findInputByName(before, ic.name);
    if (!previousEntry) {
      String typeStr = inputTypeToString(ic.type);
      String pinStr = describePinValue(ic.pin);
      String channelStr = describeOptionalInt(ic.adsChannel);
      logPrintf("Input added: %s (type=%s, pin=%s, adsChannel=%s, active=%s)",
                ic.name.c_str(), typeStr.c_str(), pinStr.c_str(),
                channelStr.c_str(), ic.active ? "true" : "false");
    } else {
      String diff = diffInputConfig(*previousEntry, ic);
      if (diff.length() > 0) {
        logPrintf("Input updated: %s {%s}", ic.name.c_str(), diff.c_str());
      }
    }
  }
  for (uint8_t i = 0; i < before.inputCount && i < MAX_INPUTS; ++i) {
    const InputConfig &prevInput = before.inputs[i];
    if (!findInputByName(after, prevInput.name)) {
      logPrintf("Input removed: %s", prevInput.name.c_str());
    }
  }

  for (uint8_t i = 0; i < after.outputCount && i < MAX_OUTPUTS; ++i) {
    const OutputConfig &oc = after.outputs[i];
    const OutputConfig *previousEntry = findOutputByName(before, oc.name);
    if (!previousEntry) {
      String typeStr = outputTypeToString(oc.type);
      String pinStr = describePinValue(oc.pin);
      String addrStr = formatI2cAddress(oc.i2cAddress);
      logPrintf("Output added: %s (type=%s, pin=%s, pwm=%d, addr=%s, active=%s)",
                oc.name.c_str(), typeStr.c_str(), pinStr.c_str(), oc.pwmFreq,
                addrStr.c_str(), oc.active ? "true" : "false");
    } else {
      String diff = diffOutputConfig(*previousEntry, oc);
      if (diff.length() > 0) {
        logPrintf("Output updated: %s {%s}", oc.name.c_str(), diff.c_str());
      }
    }
  }
  for (uint8_t i = 0; i < before.outputCount && i < MAX_OUTPUTS; ++i) {
    const OutputConfig &prevOutput = before.outputs[i];
    if (!findOutputByName(after, prevOutput.name)) {
      logPrintf("Output removed: %s", prevOutput.name.c_str());
    }
  }
}

static bool inputsMatch(const Config &expected,
                        const Config &actual,
                        String &errorDetail) {
  if (expected.inputCount != actual.inputCount) {
    errorDetail =
        String("inputCount mismatch: ") + expected.inputCount + " != " +
        actual.inputCount;
    return false;
  }
  for (uint8_t i = 0; i < expected.inputCount && i < MAX_INPUTS; ++i) {
    const InputConfig &ic = expected.inputs[i];
    const InputConfig *actualEntry = findInputByName(actual, ic.name);
    if (!actualEntry) {
      errorDetail = String("input missing: ") + ic.name;
      return false;
    }
    String diff = diffInputConfig(ic, *actualEntry);
    if (diff.length() > 0) {
      errorDetail = String("input mismatch ") + ic.name + " {" + diff + "}";
      return false;
    }
  }
  for (uint8_t i = 0; i < actual.inputCount && i < MAX_INPUTS; ++i) {
    const InputConfig &ic = actual.inputs[i];
    if (!findInputByName(expected, ic.name)) {
      errorDetail = String("unexpected input: ") + ic.name;
      return false;
    }
  }
  return true;
}

static bool outputsMatch(const Config &expected,
                         const Config &actual,
                         String &errorDetail) {
  if (expected.outputCount != actual.outputCount) {
    errorDetail =
        String("outputCount mismatch: ") + expected.outputCount + " != " +
        actual.outputCount;
    return false;
  }
  for (uint8_t i = 0; i < expected.outputCount && i < MAX_OUTPUTS; ++i) {
    const OutputConfig &oc = expected.outputs[i];
    const OutputConfig *actualEntry = findOutputByName(actual, oc.name);
    if (!actualEntry) {
      errorDetail = String("output missing: ") + oc.name;
      return false;
    }
    String diff = diffOutputConfig(oc, *actualEntry);
    if (diff.length() > 0) {
      errorDetail = String("output mismatch ") + oc.name + " {" + diff + "}";
      return false;
    }
  }
  for (uint8_t i = 0; i < actual.outputCount && i < MAX_OUTPUTS; ++i) {
    const OutputConfig &oc = actual.outputs[i];
    if (!findOutputByName(expected, oc.name)) {
      errorDetail = String("unexpected output: ") + oc.name;
      return false;
    }
  }
  return true;
}

static bool verifyConfigStored(const Config &expected, String &errorDetail) {
  File f = LittleFS.open(CONFIG_FILE_PATH, "r");
  if (!f) {
    errorDetail = String("open failed: ") + CONFIG_FILE_PATH;
    return false;
  }
  size_t size = f.size();
  std::unique_ptr<char[]> buf(new char[size + 1]);
  size_t read = f.readBytes(buf.get(), size);
  f.close();
  buf[read] = '\0';

  DynamicJsonDocument doc(CONFIG_JSON_CAPACITY);
  DeserializationError err = deserializeJson(doc, buf.get());
  if (err) {
    errorDetail = String("json parse failed: ") + err.c_str();
    return false;
  }
  Config reloaded = expected;
  parseConfigFromJson(doc, reloaded);
  if (!inputsMatch(expected, reloaded, errorDetail)) {
    return false;
  }
  if (!outputsMatch(expected, reloaded, errorDetail)) {
    return false;
  }
  logPrintf("Configuration verification succeeded (%u inputs, %u outputs)",
            static_cast<unsigned>(expected.inputCount),
            static_cast<unsigned>(expected.outputCount));
  return true;
}

// Parse a configuration from a JSON document.  This helper reads
// optional fields safely, falling back to existing values when keys
// are missing.  Unknown types are ignored.  String keys are case-
// insensitive for the type fields.


void invalidateSession() {
  sessionToken = "";
  sessionIssuedAt = 0;
  sessionLastActivity = 0;
}

void initialiseSecurity() {
  invalidateSession();
  uint16_t rawPin = static_cast<uint16_t>(random(0, 10000));
  sessionPin = formatPin(rawPin);
  setOledStatusLine(2, String("PIN: ") + sessionPin);
  logPrintf("Session PIN generated: %s", sessionPin.c_str());
}

static void updateOledStatusSummary() {
  if (!oledLogging) return;
  IPAddress ip = (WiFi.getMode() == WIFI_STA) ? WiFi.localIP() : WiFi.softAPIP();
  setOledStatusLines(config.nodeId, ip.toString(), String("PIN: ") + sessionPin);
}

String generateSessionToken() {
  char buf[33];
  for (int i = 0; i < 16; i++) {
    uint8_t val = static_cast<uint8_t>(random(0, 256));
    snprintf(&buf[i * 2], 3, "%02x", val);
  }
  buf[32] = '\0';
  return String(buf);
}

String buildSessionCookie(const String &value, bool expire) {
  String cookie = String(SESSION_COOKIE_NAME) + "=" + value + "; Path=/";
  if (expire) {
    cookie += "; Max-Age=0";
  }
  cookie += "; HttpOnly";
  cookie += "; SameSite=Strict";
  return cookie;
}

template <typename ServerT>
bool extractSessionToken(ServerT *server, String &tokenOut) {
  tokenOut = "";
  if (server->hasHeader("Cookie")) {
    String cookie = server->header("Cookie");
    int start = 0;
    int cookieLen = static_cast<int>(cookie.length());
    while (start < cookieLen) {
      int end = cookie.indexOf(';', start);
      if (end == -1) end = cookieLen;
      String pair = cookie.substring(start, end);
      pair.trim();
      String prefix = String(SESSION_COOKIE_NAME) + "=";
      if (pair.startsWith(prefix)) {
        tokenOut = pair.substring(prefix.length());
        tokenOut.trim();
        break;
      }
      start = end + 1;
    }
  }
  if (tokenOut.length() == 0 && server->hasHeader("X-Session-Token")) {
    tokenOut = server->header("X-Session-Token");
    tokenOut.trim();
  }
  if (tokenOut.length() == 0 && server->hasHeader("Authorization")) {
    String auth = server->header("Authorization");
    const String bearer = "Bearer ";
    if (auth.startsWith(bearer)) {
      tokenOut = auth.substring(bearer.length());
      tokenOut.trim();
    }
  }
  return tokenOut.length() > 0;
}

bool sessionTokenValid(const String &token, bool refreshActivity) {
  if (sessionToken.length() == 0 || token.length() == 0) {
    return false;
  }
  if (token != sessionToken) {
    return false;
  }
  if (sessionIssuedAt == 0) {
    return false;
  }
  uint32_t reference = sessionLastActivity ? sessionLastActivity : sessionIssuedAt;
  uint32_t now = millis();
  if (SESSION_TIMEOUT_MS > 0 && (now - reference) > SESSION_TIMEOUT_MS) {
    invalidateSession();
    return false;
  }
  if (refreshActivity) {
    sessionLastActivity = now;
  }
  return true;
}

template <typename ServerT>
bool requireAuth(ServerT *server) {
  String token;
  if (!extractSessionToken(server, token)) {
    server->send(401, "application/json", R"({"error":"unauthorized"})");
    return false;
  }
  if (!sessionTokenValid(token, true)) {
    server->send(401, "application/json", R"({"error":"unauthorized"})");
    return false;
  }
  return true;
}

void registerDiscoveredNode(const String &nodeId, const IPAddress &ip) {
  if (nodeId.length() == 0 || nodeId == config.nodeId) {
    return;
  }
  uint32_t now = millis();
  for (uint8_t i = 0; i < discoveredCount; i++) {
    if (discoveredNodes[i].nodeId == nodeId) {
      discoveredNodes[i].ip = ip;
      discoveredNodes[i].lastSeen = now;
      return;
    }
  }
  if (discoveredCount < MAX_DISCOVERED_NODES) {
    discoveredNodes[discoveredCount].nodeId = nodeId;
    discoveredNodes[discoveredCount].ip = ip;
    discoveredNodes[discoveredCount].lastSeen = now;
    discoveredCount++;
    return;
  }
  uint8_t oldest = 0;
  uint32_t oldestTs = discoveredNodes[0].lastSeen;
  for (uint8_t i = 1; i < MAX_DISCOVERED_NODES; i++) {
    if (discoveredNodes[i].lastSeen < oldestTs) {
      oldest = i;
      oldestTs = discoveredNodes[i].lastSeen;
    }
  }
  discoveredNodes[oldest].nodeId = nodeId;
  discoveredNodes[oldest].ip = ip;
  discoveredNodes[oldest].lastSeen = now;
}

void sendDiscoveryResponse(const IPAddress &ip, uint16_t port) {
  DynamicJsonDocument doc(256);
  doc["cmd"] = "discover_reply";
  doc["node"] = config.nodeId;
  IPAddress localIp = (WiFi.getMode() == WIFI_STA) ? WiFi.localIP() : WiFi.softAPIP();
  doc["ip"] = localIp.toString();
  doc["fw"] = getFirmwareVersion();
  String payload;
  serializeJson(doc, payload);
  udp.beginPacket(ip, port);
  udp.write(reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
  udp.endPacket();
}

void triggerDiscovery() {
  uint32_t now = millis();
  if (now - lastDiscoveryRequest < DISCOVERY_REQUEST_INTERVAL) {
    return;
  }
  lastDiscoveryRequest = now;
  DynamicJsonDocument doc(192);
  doc["cmd"] = "discover";
  doc["from"] = config.nodeId;
  String payload;
  serializeJson(doc, payload);
  udp.beginPacket(IPAddress(255, 255, 255, 255), BROADCAST_PORT);
  udp.write(reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
  udp.endPacket();
}

// Initialise Wi-Fi according to the configuration.  If STA mode is
// requested but connection fails within a timeout, the ESP8266 falls
// back to AP mode.  Once connected, an mDNS hostname is published
// allowing access via nodeId.local on the same network.
void setupWiFi() {
  WiFi.mode(WIFI_OFF);
  delay(100);
  if (config.wifi.mode.equalsIgnoreCase("STA")) {
    logPrintf("Connecting to SSID '%s'...", config.wifi.ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.wifi.ssid.c_str(), config.wifi.pass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      logMessage("WiFi connected");
    } else {
      // Connection failed.  Start our own AP so the user can reconfigure
      // the device, but keep the configured STA credentials so that a
      // reboot will retry connecting to the desired network.
      logMessage("Failed to connect, starting AP");
      WiFi.mode(WIFI_AP);
      WiFi.softAP(config.nodeId.c_str());
    }
  } else {
    logMessage("Starting in Access Point mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(config.nodeId.c_str());
  }
  IPAddress ip = (WiFi.getMode() == WIFI_STA) ? WiFi.localIP() : WiFi.softAPIP();
  logPrintf("IP address: %s", ip.toString().c_str());
  if (WiFi.status() == WL_CONNECTED) {
    logPrintf("WiFi RSSI: %d dBm", WiFi.RSSI());
  }
  IPAddress gateway = WiFi.gatewayIP();
  logPrintf("Gateway: %s", gateway.toString().c_str());
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient testClient;
    if (testClient.connect("8.8.8.8", 53)) {
      logMessage("Internet connectivity OK");
      testClient.stop();
    } else {
      logMessage("Internet connectivity test failed");
    }
  }
  // Start mDNS if possible
  if (MDNS.begin(config.nodeId.c_str())) {
    logMessage("mDNS responder started");
    MDNS.addService("http", "tcp", HTTP_PORT);
  }
}

// Initialise any optional sensors based on the module flags.  Only
// modules explicitly enabled via config will be initialised.  PWM
// frequency is set globally according to the first active output.
void setupSensors() {
  if (config.modules.ads1115) {
    ads.begin();
    ads.setGain(GAIN_ONE); // ±4.096 V range
  }
  // Configure PWM if any outputs are PWM010 type.  ESP8266 uses a single
  // PWM frequency for all channels so we take the frequency of the first
  // enabled PWM output.
  for (uint8_t i = 0; i < config.outputCount; i++) {
    const OutputConfig &oc = config.outputs[i];
    if (oc.active && oc.type == OUTPUT_PWM010) {
      analogWriteRange(1023);
      analogWriteFreq(oc.pwmFreq);
      break;
    }
  }
  for (uint8_t i = 0; i < MAX_OUTPUTS; i++) {
    mcp4725Ready[i] = false;
    mcp4725Addresses[i] = 0;
  }
  if (config.modules.mcp4725) {
    for (uint8_t i = 0; i < config.outputCount && i < MAX_OUTPUTS; i++) {
      const OutputConfig &oc = config.outputs[i];
      if (!oc.active || oc.type != OUTPUT_MCP4725) {
        continue;
      }
      uint8_t addr = oc.i2cAddress ? oc.i2cAddress : 0x60;
      if (mcp4725Drivers[i].begin(addr)) {
        mcp4725Ready[i] = true;
        mcp4725Addresses[i] = addr;
      } else {
        logPrintf("MCP4725 init failed on output %u (addr %s)",
                  static_cast<unsigned>(i),
                  formatI2cAddress(addr).c_str());
        mcp4725Ready[i] = false;
      }
    }
  }
  // No initialisation required for ZMPT/ZMCT/Div; they use analogRead.
}

// Update sensor readings.  Called periodically from loop().  For each
// configured and active input, the appropriate measurement is taken
// depending on the input type.  Values are stored in the InputConfig
// structure for later retrieval and broadcast.
void updateInputs() {
  // Collect a timestamp for this update if needed in the future
  for (uint8_t i = 0; i < config.inputCount && i < MAX_INPUTS; i++) {
    InputConfig &ic = config.inputs[i];
    if (!ic.active || ic.type == INPUT_DISABLED) {
      ic.value = NAN;
      continue;
    }
    switch (ic.type) {
      case INPUT_ADC: {
        // Internal ADC (A0).  10-bit resolution.  Raw value 0–1023.
        if (ic.pin == A0) {
          int raw = analogRead(A0);
          float v = raw;
          ic.value = ic.scale * v + ic.offset;
        } else {
          ic.value = NAN;
        }
        break;
      }
      case INPUT_ADS1115: {
        // External ADS1115 channel.  16-bit resolution (signed).  Use
        // raw code directly; scaling can be applied via ic.scale.
        if (config.modules.ads1115 && ic.adsChannel >= 0 && ic.adsChannel < 4) {
          int16_t raw = ads.readADC_SingleEnded(ic.adsChannel);
          float v = raw;
          ic.value = ic.scale * v + ic.offset;
        } else {
          ic.value = NAN;
        }
        break;
      }
      case INPUT_REMOTE: {
        // Look up a remote value.  If the value has not been
        // received recently, NAN will be returned.
        float rv = getRemoteValue(ic.remoteNode, ic.remoteName);
        ic.value = ic.scale * rv + ic.offset;
        break;
      }
      case INPUT_ZMPT:
      case INPUT_ZMCT:
      case INPUT_DIV: {
        // For AC (ZMPT/ZMCT) and DC divider inputs, we read the
        // analog pin multiple times to compute a representative value.
        // AC sensors use RMS of the AC component; DC uses the average
        // raw code.  We assume that analogRead resolution is 10 bits.
        if (ic.pin >= 0) {
          const int samples = 32;
          float sumSq = 0.0f;
          float sum   = 0.0f;
          for (int k = 0; k < samples; k++) {
            int raw = 0;
            if (ic.pin == A0) raw = analogRead(A0);
            else raw = analogRead(ic.pin);
            sum += raw;
            float diff = raw - 512.0f; // centre on mid-scale (assuming 10-bit ADC)
            sumSq += diff * diff;
            delayMicroseconds(200); // small delay for sampling
          }
          if (ic.type == INPUT_DIV) {
            // DC measurement: use average raw code
            float avg = sum / samples;
            ic.value = ic.scale * avg + ic.offset;
          } else {
            // AC measurement: RMS of AC component
            float meanSq = sumSq / samples;
            float rmsRaw = sqrtf(meanSq);
            ic.value = ic.scale * rmsRaw + ic.offset;
          }
        } else {
          ic.value = NAN;
        }
        break;
      }
      default:
        ic.value = NAN;
        break;
    }
  }
}

// Update outputs.  Called when an output value changes.  Maps a
// physical value into a PWM duty cycle or digital level.  The actual
// value is stored in the OutputConfig for status reporting.
void updateOutputs() {
  for (uint8_t i = 0; i < config.outputCount && i < MAX_OUTPUTS; i++) {
    OutputConfig &oc = config.outputs[i];
    if (!oc.active || oc.type == OUTPUT_DISABLED) continue;
    if (oc.type == OUTPUT_PWM010) {
      // Calculate duty cycle from physical value.  Value is assumed to
      // be in volts for 0–10 V converters.  The scale and offset
      // fields allow calibration of the 0–10 V module.  The result
      // should be clamped to the 0–1023 range used by analogWrite.
      float duty = oc.value * oc.scale + oc.offset;
      if (duty < 0.0f) duty = 0.0f;
      if (duty > 1023.0f) duty = 1023.0f;
      analogWrite(oc.pin, (uint32_t)duty);
    } else if (oc.type == OUTPUT_GPIO) {
      // Digital output: treat any value > 0 as logic HIGH
      int lvl = (oc.value > 0.5f) ? HIGH : LOW;
      pinMode(oc.pin, OUTPUT);
      digitalWrite(oc.pin, lvl);
    } else if (oc.type == OUTPUT_MCP4725) {
      if (!config.modules.mcp4725) {
        continue;
      }
      uint8_t index = i;
      uint8_t addr = oc.i2cAddress ? oc.i2cAddress : 0x60;
      if (!mcp4725Ready[index] || mcp4725Addresses[index] != addr) {
        if (mcp4725Drivers[index].begin(addr)) {
          mcp4725Ready[index] = true;
          mcp4725Addresses[index] = addr;
        } else {
          mcp4725Ready[index] = false;
          logPrintf("MCP4725 write skipped on output %u (addr %s)",
                    static_cast<unsigned>(index),
                    formatI2cAddress(addr).c_str());
          continue;
        }
      }
      float code = oc.value * oc.scale + oc.offset;
      if (code < 0.0f) code = 0.0f;
      if (code > 4095.0f) code = 4095.0f;
      mcp4725Drivers[index].setVoltage(static_cast<uint16_t>(code), false);
    }
  }
}

// Update or insert a value in the remote cache.  Called when a
// broadcast packet is received from another node.  Up to
// MAX_REMOTE_VALUES distinct remote measurements are stored.  If the
// cache is full new values overwrite the oldest entry.
void updateRemoteValue(const String &nodeId, const String &inputName, float val) {
  // First try to find an existing entry
  for (int i = 0; i < remoteCount; i++) {
    if (remoteValues[i].nodeId == nodeId && remoteValues[i].inputName == inputName) {
      remoteValues[i].value = val;
      remoteValues[i].timestamp = millis();
      return;
    }
  }
  // Insert new entry
  if (remoteCount < MAX_REMOTE_VALUES) {
    remoteValues[remoteCount].nodeId = nodeId;
    remoteValues[remoteCount].inputName = inputName;
    remoteValues[remoteCount].value = val;
    remoteValues[remoteCount].timestamp = millis();
    remoteCount++;
  } else {
    // Overwrite the oldest entry
    int oldestIdx = 0;
    uint32_t oldestTs = remoteValues[0].timestamp;
    for (int i = 1; i < MAX_REMOTE_VALUES; i++) {
      if (remoteValues[i].timestamp < oldestTs) {
        oldestTs = remoteValues[i].timestamp;
        oldestIdx = i;
      }
    }
    remoteValues[oldestIdx].nodeId = nodeId;
    remoteValues[oldestIdx].inputName = inputName;
    remoteValues[oldestIdx].value = val;
    remoteValues[oldestIdx].timestamp = millis();
  }
}

// Retrieve a remote value from the cache.  If the value has not been
// updated within the last 5 seconds (missing broadcast) it is
// considered stale and NAN is returned.
float getRemoteValue(const String &nodeId, const String &inputName) {
  uint32_t now = millis();
  for (int i = 0; i < remoteCount; i++) {
    if (remoteValues[i].nodeId == nodeId && remoteValues[i].inputName == inputName) {
      if (now - remoteValues[i].timestamp < 5000UL) {
        return remoteValues[i].value;
      } else {
        return NAN;
      }
    }
  }
  return NAN;
}

// Read any pending UDP packets and update the remote cache.  Each
// packet is expected to contain a JSON object with the fields
// "node" and "inputs".  The latter contains key/value pairs for
// each measurement.  Packets originating from this node are ignored.
void processUdp() {
  int packetSize = udp.parsePacket();
  while (packetSize > 0) {
    if (packetSize > 0 && packetSize < 1024) {
      std::unique_ptr<char[]> buf(new char[packetSize + 1]);
      int len = udp.read(buf.get(), packetSize);
      if (len > 0) {
        buf[len] = '\0';
        DynamicJsonDocument doc(1024);
        auto err = deserializeJson(doc, buf.get());
        if (!err) {
          IPAddress senderIp = udp.remoteIP();
          uint16_t senderPort = udp.remotePort();
          if (doc.containsKey("cmd")) {
            String cmd = doc["cmd"].as<String>();
            if (cmd == "discover") {
              String fromId = doc["from"].as<String>();
              if (fromId.length() > 0) {
                registerDiscoveredNode(fromId, senderIp);
              }
              if (fromId != config.nodeId) {
                sendDiscoveryResponse(senderIp, senderPort);
              }
            } else if (cmd == "discover_reply") {
              String nodeId = doc["node"].as<String>();
              if (nodeId.length() > 0) {
                registerDiscoveredNode(nodeId, senderIp);
              }
            }
          }

          String remoteId = doc["node"].as<String>();
          // Ignore our own broadcasts
          if (remoteId.length() > 0 && remoteId != config.nodeId) {
            registerDiscoveredNode(remoteId, senderIp);
            if (doc.containsKey("inputs") && doc["inputs"].is<JsonObject>()) {
              JsonObject inObj = doc["inputs"].as<JsonObject>();
              for (JsonPair kv : inObj) {
                String inName = kv.key().c_str();
                float inVal = kv.value().as<float>();
                updateRemoteValue(remoteId, inName, inVal);
              }
            }
            // Optionally handle remote outputs (not implemented)
          }
        }
      }
    }
    packetSize = udp.parsePacket();
  }
}

// Broadcast local measurements to the network.  A JSON object is
// transmitted containing the node ID, a timestamp and all enabled
// input values.  Broadcast interval is controlled by
// BROADCAST_UPDATE_INTERVAL.
void sendBroadcast() {
  unsigned long now = millis();
  if (now - lastBroadcastUpdate < BROADCAST_UPDATE_INTERVAL) return;
  lastBroadcastUpdate = now;
  DynamicJsonDocument doc(512);
  doc["node"] = config.nodeId;
  doc["ts"]   = now;
  JsonObject inObj = doc.createNestedObject("inputs");
  for (uint8_t i = 0; i < config.inputCount && i < MAX_INPUTS; i++) {
    const InputConfig &ic = config.inputs[i];
    if (ic.active && !isnan(ic.value)) {
      inObj[ic.name] = ic.value;
    }
  }
  String payload;
  serializeJson(doc, payload);
  udp.beginPacket(IPAddress(255,255,255,255), BROADCAST_PORT);
  udp.write(reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length());
  udp.endPacket();
}

// Set up the HTTP server and API routes.  Static content is served
// directly from LittleFS.  Configuration and value endpoints return
// JSON.  POST requests allow modifying configuration and outputs.
template <typename ServerT>
void registerRoutes(ServerT &server) {
  server.on("/api/session/login", HTTP_POST, [&server]() {
    auto *srv = &server;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    String pin = doc["pin"].as<String>();
    pin.trim();
    if (pin != sessionPin) {
      srv->sendHeader("Set-Cookie", buildSessionCookie("", true));
      srv->send(401, "application/json", R"({"error":"invalid_pin"})");
      return;
    }
    sessionToken = generateSessionToken();
    sessionIssuedAt = millis();
    sessionLastActivity = sessionIssuedAt;
    DynamicJsonDocument respDoc(256);
    respDoc["status"] = "ok";
    respDoc["token"] = sessionToken;
    String payload;
    serializeJson(respDoc, payload);
    srv->sendHeader("Set-Cookie",
                    buildSessionCookie(sessionToken, false));
    srv->send(200, "application/json", payload);
  });

  server.on("/api/session/logout", HTTP_POST, [&server]() {
    auto *srv = &server;
    invalidateSession();
    srv->sendHeader("Set-Cookie", buildSessionCookie("", true));
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/session/status", HTTP_GET, [&server]() {
    auto *srv = &server;
    String token;
    if (!extractSessionToken(srv, token) || !sessionTokenValid(token, true)) {
      srv->send(401, "application/json", R"({"status":"invalid"})");
      return;
    }
    uint32_t now = millis();
    uint32_t reference = sessionLastActivity ? sessionLastActivity : sessionIssuedAt;
    uint32_t remaining = (SESSION_TIMEOUT_MS > 0)
                             ? (SESSION_TIMEOUT_MS - (now - reference))
                             : 0;
    DynamicJsonDocument doc(256);
    doc["status"] = "ok";
    doc["expiresIn"] = static_cast<uint32_t>(remaining);
    String payload;
    serializeJson(doc, payload);
    srv->send(200, "application/json", payload);
  });

  server.on("/", HTTP_GET, [&server]() {
    auto *srv = &server;
    File f = LittleFS.open("/index.html", "r");
    if (!f) {
      srv->send(404, "text/plain", "Not found");
      return;
    }
    srv->streamFile(f, "text/html");
    f.close();
  });

  server.on("/api/config/get", HTTP_GET, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    DynamicJsonDocument doc(CONFIG_JSON_CAPACITY);
    populateConfigJson(doc, true);
    String payload;
    serializeJson(doc, payload);
    srv->send(200, "application/json", payload);
  });

  server.on("/api/config/set", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(CONFIG_JSON_CAPACITY);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    logPrintf("Configuration update received (%u bytes)",
              static_cast<unsigned>(body.length()));
    logConfigJson("Received", doc);
    Config previousConfig = config;
    parseConfigFromJson(doc, config, &previousConfig, false);
    if (!saveConfig()) {
      config = previousConfig;
      logMessage("Configuration update failed to save; changes reverted");
      srv->send(500, "application/json", R"({"error":"save_failed"})");
      return;
    }
    String verifyError;
    if (!verifyConfigStored(config, verifyError)) {
      logPrintf("Configuration verification failed: %s",
                verifyError.c_str());
      config = previousConfig;
      if (!saveConfig()) {
        logMessage("Failed to restore configuration after verification failure");
      }
      DynamicJsonDocument errDoc(256);
      errDoc["error"] = "verify_failed";
      if (verifyError.length() > 0) {
        errDoc["detail"] = verifyError;
      }
      String errPayload;
      serializeJson(errDoc, errPayload);
      srv->send(500, "application/json", errPayload);
      return;
    }
    logIoDelta(previousConfig, config);
    logConfigSummary("Applied");
    logMessage("Configuration update saved; rebooting");
    DynamicJsonDocument respDoc(2048);
    respDoc["status"] = "ok";
    respDoc["verified"] = true;
    JsonObject applied = respDoc.createNestedObject("applied");
    JsonArray inputsArr = applied.createNestedArray("inputs");
    for (uint8_t i = 0; i < config.inputCount && i < MAX_INPUTS; i++) {
      JsonObject out = inputsArr.createNestedObject();
      const InputConfig &ic = config.inputs[i];
      out["name"] = ic.name;
      out["type"] = inputTypeToString(ic.type);
      if (ic.type == INPUT_ADC || ic.type == INPUT_DIV || ic.type == INPUT_ZMPT ||
          ic.type == INPUT_ZMCT) {
        out["pin"] = pinToString(ic.pin);
      }
      if (ic.type == INPUT_ADS1115) {
        out["adsChannel"] = ic.adsChannel;
      }
      if (ic.type == INPUT_REMOTE) {
        out["remoteNode"] = ic.remoteNode;
        out["remoteName"] = ic.remoteName;
      }
      out["scale"] = ic.scale;
      out["offset"] = ic.offset;
      out["unit"] = ic.unit;
      out["active"] = ic.active;
    }
    JsonArray outputsArr = applied.createNestedArray("outputs");
    for (uint8_t i = 0; i < config.outputCount && i < MAX_OUTPUTS; i++) {
      JsonObject out = outputsArr.createNestedObject();
      const OutputConfig &oc = config.outputs[i];
      out["name"] = oc.name;
      out["type"] = outputTypeToString(oc.type);
      if (oc.type == OUTPUT_PWM010 || oc.type == OUTPUT_GPIO) {
        out["pin"] = pinToString(oc.pin);
      }
      if (oc.type == OUTPUT_PWM010) {
        out["pwmFreq"] = oc.pwmFreq;
      }
      if (oc.type == OUTPUT_MCP4725) {
        out["i2cAddress"] = formatI2cAddress(oc.i2cAddress);
      }
      out["scale"] = oc.scale;
      out["offset"] = oc.offset;
      out["active"] = oc.active;
    }
    String payload;
    serializeJson(respDoc, payload);
    srv->send(200, "application/json", payload);
    delay(100);
    ESP.restart();
  });

  server.on("/api/reboot", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    srv->send(200, "application/json", R"({"status":"rebooting"})");
    delay(100);
    ESP.restart();
  });

  server.on(
      "/api/ota", HTTP_POST,
      [&server]() {
        auto *srv = &server;
        if (!requireAuth(srv)) {
          otaUploadAuthorized = false;
          otaUploadInProgress = false;
          return;
        }
        if (!otaUploadInProgress) {
          logMessage("OTA finalize requested but no upload in progress");
          srv->send(400, "application/json", R"({"error":"no_upload"})");
          return;
        }
        if (!otaUploadSuccess) {
          DynamicJsonDocument doc(256);
          doc["error"] = otaLastError.length() ? otaLastError : "ota_failed";
          String payload;
          serializeJson(doc, payload);
          srv->send(500, "application/json", payload);
          logPrintf("OTA update failed: %s",
                    otaLastError.length() ? otaLastError.c_str() : "unknown");
        } else {
          DynamicJsonDocument doc(128);
          doc["status"] = "ok";
          doc["size"] = static_cast<uint32_t>(otaUploadSize);
          String payload;
          serializeJson(doc, payload);
          srv->send(200, "application/json", payload);
          logPrintf("OTA update applied (%u bytes), rebooting",
                    static_cast<unsigned>(otaUploadSize));
          delay(100);
          ESP.restart();
        }
        otaUploadAuthorized = false;
        otaUploadInProgress = false;
      },
      [&server]() {
        auto *srv = &server;
        HTTPUpload &upload = srv->upload();
        switch (upload.status) {
          case UPLOAD_FILE_START: {
            otaUploadAuthorized = false;
            otaUploadInProgress = false;
            otaUploadSuccess = false;
            otaUploadSize = 0;
            otaLastError = "";
            String token;
            if (!extractSessionToken(srv, token) ||
                !sessionTokenValid(token, true)) {
              otaLastError = "unauthorized";
              logMessage("OTA upload rejected: unauthorized session");
              return;
            }
            otaUploadAuthorized = true;
            otaUploadInProgress = true;
            logPrintf("OTA upload started: %s", upload.filename.c_str());
            size_t sketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
            if (!Update.begin(sketchSpace)) {
              otaLastError = String("begin_failed: ") + otaErrorMessage();
              logPrintf("OTA begin failed: %s", otaLastError.c_str());
              Update.printError(Serial);
            }
            break;
          }
          case UPLOAD_FILE_WRITE: {
            if (!otaUploadAuthorized || !Update.isRunning()) {
              return;
            }
            if (Update.write(upload.buf, upload.currentSize) !=
                upload.currentSize) {
              otaLastError = String("write_failed: ") + otaErrorMessage();
              logPrintf("OTA write failed at %u bytes: %s",
                        static_cast<unsigned>(otaUploadSize),
                        otaLastError.c_str());
              Update.printError(Serial);
              Update.end();
            } else {
              otaUploadSize = upload.totalSize;
            }
            break;
          }
          case UPLOAD_FILE_END: {
            if (!otaUploadAuthorized) {
              return;
            }
            if (!Update.isRunning()) {
              if (otaLastError.length() == 0) {
                otaLastError = "not_running";
              }
              logMessage("OTA upload ended but updater was not running");
              return;
            }
            if (Update.end(true)) {
              otaUploadSuccess = true;
              otaUploadSize = upload.totalSize;
              logPrintf("OTA upload finished: %u bytes",
                        static_cast<unsigned>(otaUploadSize));
            } else {
              otaLastError = String("finalize_failed: ") + otaErrorMessage();
              logPrintf("OTA finalize failed: %s", otaLastError.c_str());
              Update.printError(Serial);
            }
            break;
          }
          case UPLOAD_FILE_ABORTED: {
            otaLastError = "aborted";
            logMessage("OTA upload aborted by client");
            if (Update.isRunning()) {
              Update.end();
            }
            break;
          }
          default:
            break;
        }
      });

  server.on("/api/output/set", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    bool updated = false;
    if (doc.containsKey("outputs") && doc["outputs"].is<JsonArray>()) {
      JsonArray arr = doc["outputs"].as<JsonArray>();
      for (JsonObject o : arr) {
        String name = o["name"].as<String>();
        float value = o["value"].as<float>();
        for (uint8_t i = 0; i < config.outputCount && i < MAX_OUTPUTS; i++) {
          OutputConfig &oc = config.outputs[i];
          if (oc.name == name) {
            oc.value = value;
            oc.active = true;
            updated = true;
          }
        }
      }
    } else if (doc.containsKey("name")) {
      String name = doc["name"].as<String>();
      float value = doc["value"].as<float>();
      for (uint8_t i = 0; i < config.outputCount && i < MAX_OUTPUTS; i++) {
        OutputConfig &oc = config.outputs[i];
        if (oc.name == name) {
          oc.value = value;
          oc.active = true;
          updated = true;
          break;
        }
      }
    }
    if (!updated) {
      srv->send(404, "application/json", R"({"error":"Unknown output"})");
      return;
    }
    updateOutputs();
    saveConfig();
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/inputs", HTTP_GET, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.createNestedArray("inputs");
    for (uint8_t i = 0; i < config.inputCount && i < MAX_INPUTS; i++) {
      const InputConfig &ic = config.inputs[i];
      if (!ic.active || ic.type == INPUT_DISABLED) continue;
      JsonObject o = arr.createNestedObject();
      o["name"] = ic.name;
      o["value"] = ic.value;
      o["unit"] = ic.unit;
      o["timestamp"] = millis();
    }
    String payload;
    serializeJson(doc, payload);
    srv->send(200, "application/json", payload);
  });

  server.on("/api/outputs", HTTP_GET, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.createNestedArray("outputs");
    for (uint8_t i = 0; i < config.outputCount && i < MAX_OUTPUTS; i++) {
      const OutputConfig &oc = config.outputs[i];
      JsonObject o = arr.createNestedObject();
      o["name"] = oc.name;
      o["value"] = oc.value;
      o["active"] = oc.active;
    }
    String payload;
    serializeJson(doc, payload);
    srv->send(200, "application/json", payload);
  });

  server.on("/api/discovery", HTTP_GET, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    triggerDiscovery();
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.createNestedArray("nodes");
    uint32_t now = millis();
    for (uint8_t i = 0; i < discoveredCount; i++) {
      if (now - discoveredNodes[i].lastSeen > DISCOVERY_TIMEOUT_MS) {
        continue;
      }
      JsonObject o = arr.createNestedObject();
      o["nodeId"] = discoveredNodes[i].nodeId;
      o["ip"] = discoveredNodes[i].ip.toString();
      o["ageMs"] = static_cast<uint32_t>(now - discoveredNodes[i].lastSeen);
    }
    String payload;
    serializeJson(doc, payload);
    srv->send(200, "application/json", payload);
  });

  server.on("/api/peers/set", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    if (!doc.containsKey("peers") || !doc["peers"].is<JsonArray>()) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    JsonArray arr = doc["peers"].as<JsonArray>();
    config.peerCount = min<uint8_t>(arr.size(), MAX_PEERS);
    uint8_t idx = 0;
    for (JsonObject o : arr) {
      if (idx >= MAX_PEERS) break;
      config.peers[idx].nodeId = o["nodeId"].as<String>();
      config.peers[idx].pin = o["pin"].as<String>();
      idx++;
    }
    saveConfig();
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/remote", HTTP_GET, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    DynamicJsonDocument doc(1024);
    JsonObject obj = doc.to<JsonObject>();
    for (int i = 0; i < remoteCount; i++) {
      String key = remoteValues[i].nodeId + ":" + remoteValues[i].inputName;
      obj[key] = remoteValues[i].value;
    }
    String payload;
    serializeJson(doc, payload);
    srv->send(200, "application/json", payload);
  });

  server.on("/api/logs", HTTP_GET, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    if (!LittleFS.exists(LOG_PATH)) {
      srv->send(404, "text/plain", "No log");
      return;
    }
    File f = LittleFS.open(LOG_PATH, "r");
    if (!f) {
      srv->send(500, "text/plain", "Failed to open log");
      return;
    }
    srv->streamFile(f, "text/plain");
    f.close();
  });

  server.on("/api/files/list", HTTP_GET, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    if (!ensureUserStorageReady()) {
      srv->send(500, "application/json", R"({"error":"storage unavailable"})");
      return;
    }
    DynamicJsonDocument doc(512);
    JsonArray arr = doc.createNestedArray("files");
    Dir dir = LittleFS.openDir(USER_FILES_DIR);
    while (dir.next()) {
      if (dir.isDirectory()) continue;
      String relative = toRelativeUserPath(dir.fileName());
      if (relative.length() == 0) continue;
      JsonObject o = arr.createNestedObject();
      o["name"] = relative;
      o["size"] = dir.fileSize();
    }
    String payload;
    serializeJson(doc, payload);
    srv->send(200, "application/json", payload);
  });

  server.on("/api/files/get", HTTP_GET, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    if (!srv->hasArg("path")) {
      srv->send(400, "application/json", R"({"error":"missing path"})");
      return;
    }
    if (!ensureUserStorageReady()) {
      srv->send(500, "application/json", R"({"error":"storage unavailable"})");
      return;
    }
    String clientPath = srv->arg("path");
    String fsPath;
    if (!resolveUserPath(clientPath, fsPath)) {
      srv->send(400, "application/json", R"({"error":"invalid path"})");
      return;
    }
    if (!LittleFS.exists(fsPath)) {
      srv->send(404, "application/json", R"({"error":"not found"})");
      return;
    }
    File f = LittleFS.open(fsPath, "r");
    if (!f) {
      srv->send(500, "application/json", R"({"error":"open failed"})");
      return;
    }
    srv->streamFile(f, "text/html");
    f.close();
  });

  server.on("/api/files/save", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    if (!ensureUserStorageReady()) {
      srv->send(500, "application/json", R"({"error":"storage unavailable"})");
      return;
    }
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    String clientPath = doc["path"].as<String>();
    String fsPath;
    if (!resolveUserPath(clientPath, fsPath)) {
      srv->send(400, "application/json", R"({"error":"invalid path"})");
      return;
    }
    String content = doc["content"].as<String>();
    File f = LittleFS.open(fsPath, "w");
    if (!f) {
      srv->send(500, "application/json", R"({"error":"open failed"})");
      return;
    }
    f.print(content);
    f.close();
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/files/create", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    if (!ensureUserStorageReady()) {
      srv->send(500, "application/json", R"({"error":"storage unavailable"})");
      return;
    }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    String clientPath = doc["path"].as<String>();
    String fsPath;
    if (!resolveUserPath(clientPath, fsPath)) {
      srv->send(400, "application/json", R"({"error":"invalid path"})");
      return;
    }
    if (LittleFS.exists(fsPath)) {
      srv->send(409, "application/json", R"({"error":"exists"})");
      return;
    }
    String content = doc["content"].as<String>();
    File f = LittleFS.open(fsPath, "w");
    if (!f) {
      srv->send(500, "application/json", R"({"error":"create failed"})");
      return;
    }
    if (content.length() > 0) {
      f.print(content);
    }
    f.close();
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/files/rename", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    if (!ensureUserStorageReady()) {
      srv->send(500, "application/json", R"({"error":"storage unavailable"})");
      return;
    }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    String fromClient = doc["from"].as<String>();
    String toClient = doc["to"].as<String>();
    String fromPath;
    String toPath;
    if (!resolveUserPath(fromClient, fromPath) ||
        !resolveUserPath(toClient, toPath)) {
      srv->send(400, "application/json", R"({"error":"invalid path"})");
      return;
    }
    if (fromPath == toPath) {
      srv->send(200, "application/json", R"({"status":"ok"})");
      return;
    }
    if (!LittleFS.exists(fromPath)) {
      srv->send(404, "application/json", R"({"error":"not found"})");
      return;
    }
    if (LittleFS.exists(toPath)) {
      srv->send(409, "application/json", R"({"error":"exists"})");
      return;
    }
    if (!LittleFS.rename(fromPath, toPath)) {
      srv->send(500, "application/json", R"({"error":"rename failed"})");
      return;
    }
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/files/delete", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    if (!ensureUserStorageReady()) {
      srv->send(500, "application/json", R"({"error":"storage unavailable"})");
      return;
    }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    String clientPath = doc["path"].as<String>();
    String fsPath;
    if (!resolveUserPath(clientPath, fsPath)) {
      srv->send(400, "application/json", R"({"error":"invalid path"})");
      return;
    }
    if (!LittleFS.exists(fsPath)) {
      srv->send(404, "application/json", R"({"error":"not found"})");
      return;
    }
    if (!LittleFS.remove(fsPath)) {
      srv->send(500, "application/json", R"({"error":"delete failed"})");
      return;
    }
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/virtual/workspace", HTTP_GET, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    DynamicJsonDocument doc(8192);
    virtual_lab::VirtualWorkspace::Instance().populateSummaryJson(doc);
    String payload;
    serializeJson(doc, payload);
    srv->send(200, "application/json", payload);
  });

  server.on("/api/virtual/function-generator/output", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    virtual_lab::FunctionGeneratorOutputConfig config;
    config.id = doc["id"].as<String>();
    config.name = doc["name"].as<String>();
    config.units = doc["units"].as<String>();
    config.enabled = doc.containsKey("enabled") ? doc["enabled"].as<bool>() : true;
    virtual_lab::WaveformSettings settings;
    settings.amplitude = doc.containsKey("amplitude") ? doc["amplitude"].as<float>() : 1.0f;
    settings.offset = doc.containsKey("offset") ? doc["offset"].as<float>() : 0.0f;
    settings.frequency = doc.containsKey("frequency") ? doc["frequency"].as<float>() : 1.0f;
    settings.phase = doc.containsKey("phase") ? doc["phase"].as<float>() : 0.0f;
    settings.dutyCycle = doc.containsKey("dutyCycle") ? doc["dutyCycle"].as<float>() : 0.5f;
    String shape = doc["shape"].as<String>();
    if (shape.length() > 0) {
      if (!decodeWaveformShape(shape, settings.shape)) {
        srv->send(400, "application/json", R"({"error":"invalid_shape"})");
        return;
      }
    }
    config.settings = settings;
    String error;
    if (!virtual_lab::VirtualWorkspace::Instance().functionGenerator().configureOutput(
            config, error)) {
      DynamicJsonDocument resp(256);
      resp["error"] = error;
      String payload;
      serializeJson(resp, payload);
      srv->send(400, "application/json", payload);
      return;
    }
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/virtual/function-generator/output/remove", HTTP_POST,
            [&server]() {
              auto *srv = &server;
              if (!requireAuth(srv)) return;
              String body = srv->hasArg("plain") ? srv->arg("plain") : String();
              if (body.length() == 0) {
                srv->send(400, "application/json", R"({"error":"No body"})");
                return;
              }
              DynamicJsonDocument doc(256);
              if (deserializeJson(doc, body)) {
                srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
                return;
              }
              String id = doc["id"].as<String>();
              if (!virtual_lab::VirtualWorkspace::Instance().functionGenerator().removeOutput(id)) {
                srv->send(404, "application/json", R"({"error":"not found"})");
                return;
              }
              srv->send(200, "application/json", R"({"status":"ok"})");
            });

  server.on("/api/virtual/oscilloscope/trace", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    virtual_lab::OscilloscopeTraceConfig config;
    config.id = doc["id"].as<String>();
    config.signalId = doc["signalId"].as<String>();
    config.label = doc.containsKey("label") ? doc["label"].as<String>() : config.id;
    config.enabled = doc.containsKey("enabled") ? doc["enabled"].as<bool>() : true;
    if (!virtual_lab::VirtualWorkspace::Instance().oscilloscope().configureTrace(config)) {
      srv->send(400, "application/json", R"({"error":"invalid_trace"})");
      return;
    }
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/virtual/oscilloscope/trace/remove", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    String id = doc["id"].as<String>();
    if (!virtual_lab::VirtualWorkspace::Instance().oscilloscope().removeTrace(id)) {
      srv->send(404, "application/json", R"({"error":"not found"})");
      return;
    }
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/virtual/oscilloscope/capture", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    virtual_lab::OscilloscopeCaptureRequest request;
    request.startTime = doc.containsKey("startTime") ? doc["startTime"].as<float>() : 0.0f;
    request.sampleRate = doc.containsKey("sampleRate") ? doc["sampleRate"].as<float>() : 1000.0f;
    request.sampleCount = doc.containsKey("sampleCount") ? doc["sampleCount"].as<uint32_t>() : 512U;
    virtual_lab::OscilloscopeCaptureResult capture;
    String error;
    if (!virtual_lab::VirtualWorkspace::Instance().oscilloscope().capture(request, capture, error)) {
      DynamicJsonDocument resp(256);
      resp["error"] = error;
      String payload;
      serializeJson(resp, payload);
      srv->send(400, "application/json", payload);
      return;
    }
    DynamicJsonDocument resp(16384);
    JsonObject root = resp.to<JsonObject>();
    root["sampleRate"] = capture.sampleRate;
    JsonArray traces = root.createNestedArray("traces");
    for (const auto &trace : capture.traces) {
      JsonObject traceObj = traces.createNestedObject();
      traceObj["id"] = trace.id;
      traceObj["label"] = trace.label;
      traceObj["enabled"] = trace.enabled;
      JsonArray samples = traceObj.createNestedArray("samples");
      for (float value : trace.samples) {
        samples.add(value);
      }
    }
    String payload;
    serializeJson(resp, payload);
    srv->send(200, "application/json", payload);
  });

  server.on("/api/virtual/multimeter/input", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    virtual_lab::MultimeterInputConfig config;
    config.id = doc["id"].as<String>();
    config.signalId = doc["signalId"].as<String>();
    config.label = doc.containsKey("label") ? doc["label"].as<String>() : config.id;
    config.enabled = doc.containsKey("enabled") ? doc["enabled"].as<bool>() : true;
    if (!virtual_lab::VirtualWorkspace::Instance().multimeter().configureInput(config)) {
      srv->send(400, "application/json", R"({"error":"invalid_input"})");
      return;
    }
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/virtual/multimeter/input/remove", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    String id = doc["id"].as<String>();
    if (!virtual_lab::VirtualWorkspace::Instance().multimeter().removeInput(id)) {
      srv->send(404, "application/json", R"({"error":"not found"})");
      return;
    }
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/virtual/multimeter/measure", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    virtual_lab::MultimeterMeasurementRequest request;
    request.inputId = doc["inputId"].as<String>();
    if (doc.containsKey("mode")) {
      String mode = doc["mode"].as<String>();
      if (!decodeMultimeterMode(mode, request.mode)) {
        srv->send(400, "application/json", R"({"error":"invalid_mode"})");
        return;
      }
    }
    request.startTime = doc.containsKey("startTime") ? doc["startTime"].as<float>() : 0.0f;
    request.sampleRate = doc.containsKey("sampleRate") ? doc["sampleRate"].as<float>() : 500.0f;
    request.sampleCount = doc.containsKey("sampleCount") ? doc["sampleCount"].as<uint32_t>() : 128U;
    virtual_lab::MultimeterMeasurementResult result;
    String error;
    if (!virtual_lab::VirtualWorkspace::Instance().multimeter().measure(request, result, error)) {
      DynamicJsonDocument resp(256);
      resp["error"] = error;
      String payload;
      serializeJson(resp, payload);
      srv->send(400, "application/json", payload);
      return;
    }
    DynamicJsonDocument resp(512);
    JsonObject root = resp.to<JsonObject>();
    root["inputId"] = result.inputId;
    switch (result.mode) {
      case virtual_lab::MultimeterMode::DC:
        root["mode"] = "dc";
        break;
      case virtual_lab::MultimeterMode::AC_RMS:
        root["mode"] = "ac_rms";
        break;
      case virtual_lab::MultimeterMode::MIN:
        root["mode"] = "min";
        break;
      case virtual_lab::MultimeterMode::MAX:
        root["mode"] = "max";
        break;
      case virtual_lab::MultimeterMode::AVERAGE:
        root["mode"] = "average";
        break;
      case virtual_lab::MultimeterMode::PEAK_TO_PEAK:
        root["mode"] = "peak_to_peak";
        break;
    }
    root["value"] = result.value;
    root["min"] = result.minValue;
    root["max"] = result.maxValue;
    String payload;
    serializeJson(resp, payload);
    srv->send(200, "application/json", payload);
  });

  server.on("/api/virtual/math/expression", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(1536);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    virtual_lab::MathExpressionConfig config;
    config.id = doc["id"].as<String>();
    config.name = doc.containsKey("name") ? doc["name"].as<String>() : config.id;
    config.expression = doc["expression"].as<String>();
    config.units = doc.containsKey("units") ? doc["units"].as<String>() : String();
    String error;
    if (!parseVariableBindings(doc["bindings"], config.bindings, error)) {
      DynamicJsonDocument resp(256);
      resp["error"] = error;
      String payload;
      serializeJson(resp, payload);
      srv->send(400, "application/json", payload);
      return;
    }
    if (!virtual_lab::VirtualWorkspace::Instance().mathZone().defineExpression(config, error)) {
      DynamicJsonDocument resp(256);
      resp["error"] = error;
      String payload;
      serializeJson(resp, payload);
      srv->send(400, "application/json", payload);
      return;
    }
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/virtual/math/remove", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      srv->send(400, "application/json", R"({"error":"Invalid JSON"})");
      return;
    }
    String id = doc["id"].as<String>();
    if (!virtual_lab::VirtualWorkspace::Instance().mathZone().removeExpression(id)) {
      srv->send(404, "application/json", R"({"error":"not found"})");
      return;
    }
    srv->send(200, "application/json", R"({"status":"ok"})");
  });

  server.on("/api/virtual/help", HTTP_GET, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    DynamicJsonDocument doc(4096);
    JsonArray entries = doc.createNestedArray("entries");
    for (const auto &entry : virtual_lab::VirtualWorkspace::Instance().helpMenu().entries()) {
      JsonObject obj = entries.createNestedObject();
      obj["key"] = entry.key;
      obj["title"] = entry.title;
      obj["text"] = entry.text;
    }
    String payload;
    serializeJson(doc, payload);
    srv->send(200, "application/json", payload);
  });

  server.serveStatic("/", LittleFS, "/");
  server.onNotFound([&server]() {
    server.send(404, "text/plain", "Not found");
  });
}

void setupServer() {
  registerRoutes(primaryHttpServer);
  primaryHttpServer.begin();
  logPrintf("HTTP server started on port %u", HTTP_PORT);
}

static void diagnosticHttp() {
  logMessage("=== Diagnostic HTTP ===");
  logPrintf("Heap free: %u bytes", static_cast<unsigned>(ESP.getFreeHeap()));
  logPrintf("Max free block: %u bytes",
            static_cast<unsigned>(ESP.getMaxFreeBlockSize()));
  logPrintf("HTTP server listening on port %u", HTTP_PORT);
  IPAddress ip = (WiFi.getMode() == WIFI_STA) ? WiFi.localIP() : WiFi.softAPIP();
  logPrintf("Local IP: %s", ip.toString().c_str());
  logPrintf("WiFi mode: %d", static_cast<int>(WiFi.getMode()));
}
// Arduino setup entry point.  Serial is initialised for debug output.
// Configuration is loaded or initialised.  Wi-Fi, sensors and the
// server are then configured.  UDP listener starts on the broadcast
// port.  The initial set of outputs is applied.
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  bool oledOk = initOled();
  initLogging();
  initFirmwareVersion();
  randomSeed(analogRead(A0) ^ micros() ^ ESP.getCycleCount());
  initialiseSecurity();
  if (!ensureUserDirectory()) {
    logMessage("Failed to ensure private directory /private");
  } else if (!ensureUserStorageReady()) {
    logMessage("Failed to initialise /private/sample.html");
  }
  logPrintf("MiniLabBox v2 starting (FW %s)", getFirmwareVersion());
  if (!oledOk) {
    logMessage("OLED not detected (check wiring on GPIO12/GPIO14)");
  }
  loadConfig();
  setupWiFi();
  // Start UDP listener
  udp.begin(BROADCAST_PORT);
  setupSensors();
  setupServer();
  {
    auto &workspace = virtual_lab::VirtualWorkspace::Instance();
    auto ref5 = std::make_shared<virtual_lab::ConstantSignal>(
        "REF5", String("Référence 5 V"), 5.0f);
    ref5->setUnits("V");
    workspace.registerSignal(ref5);
    auto ref12 = std::make_shared<virtual_lab::ConstantSignal>(
        "REF12", String("Référence 12 V"), 12.0f);
    ref12->setUnits("V");
    workspace.registerSignal(ref12);
    auto gnd = std::make_shared<virtual_lab::ConstantSignal>(
        "GND", String("Masse virtuelle"), 0.0f);
    gnd->setUnits("V");
    workspace.registerSignal(gnd);
  }
  diagnosticHttp();
  triggerDiscovery();
  // After network and services are up, show basic status on the OLED
  // so users immediately know the node ID and how to reach it.  This
  // reuses the boot logging screen and will remain until another log
  // message is printed or configuration changes disable OLED logging.
  updateOledStatusSummary();
  // Initialise output pins to their default state
  updateOutputs();
  lastInputUpdate = millis();
  lastBroadcastUpdate = millis();
}

// Main loop.  Inputs are sampled on a schedule.  Broadcasts are sent
// periodically.  Incoming UDP packets are processed continuously.  The HTTP
// server is serviced on each pass so the web interface remains responsive.
void loop() {
  unsigned long now = millis();
  if (now - lastInputUpdate >= INPUT_UPDATE_INTERVAL) {
    lastInputUpdate = now;
    updateInputs();
  }
  processUdp();
  sendBroadcast();
  static uint32_t lastMemCheck = 0;
  if (now - lastMemCheck > 10000UL) {
    lastMemCheck = now;
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 2048) {
      logPrintf("ALERT: heap critically low (%u bytes)", static_cast<unsigned>(freeHeap));
    }
  }
  primaryHttpServer.handleClient();
  yield();
  // Sleep briefly to yield to background tasks
  delay(5);
}
