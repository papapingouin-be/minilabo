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
#include <cstring>
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
static const char *IO_CONFIG_FILE_PATH = "/private/io_config.json";
static const char *CONFIG_SAVE_LOG_PATH = "/private/sauvegardeconfig.log";
static const char *IO_CONFIG_BACKUP_FILE_PATH = "/private/io_config.bak";
static const char *IO_CONFIG_TEMP_FILE_PATH = "/private/io_config.tmp";
static const char *IO_CONFIG_BACKUP_STAGING_PATH = "/private/io_config.bak.tmp";
static const char *INTERFACE_CONFIG_FILE_PATH = "/private/interface_config.json";
static const char *INTERFACE_CONFIG_BACKUP_FILE_PATH = "/private/interface_config.bak";
static const char *INTERFACE_CONFIG_TEMP_FILE_PATH = "/private/interface_config.tmp";
static const char *INTERFACE_CONFIG_BACKUP_STAGING_PATH = "/private/interface_config.bak.tmp";
static const char *VIRTUAL_CONFIG_FILE_PATH = "/private/virtual_config.json";
static const char *VIRTUAL_CONFIG_BACKUP_FILE_PATH = "/private/virtual_config.bak";
static const char *VIRTUAL_CONFIG_TEMP_FILE_PATH = "/private/virtual_config.tmp";
static const char *VIRTUAL_CONFIG_BACKUP_STAGING_PATH = "/private/virtual_config.bak.tmp";
static const char *LEGACY_CONFIG_FILE_PATH = "/config.json";
static const uint32_t CONFIG_RECORD_MAGIC = 0x4D4C4243; // 'MLBC'
static const uint16_t CONFIG_RECORD_VERSION = 1;
static const size_t CONFIG_RECORD_HEADER_SIZE = 16; // bytes
static const size_t CONFIG_JSON_MIN_CAPACITY = 1024;
static const size_t CONFIG_JSON_SAFETY_MARGIN = 512;
static const size_t CONFIG_JSON_MAX_CAPACITY = 28672;

struct ConfigRecordMetadata {
  uint16_t version;
  uint16_t sections;
  uint32_t payloadLength;
  uint32_t checksum;
};

static uint32_t crc32ForBuffer(const uint8_t *data, size_t length);
static bool tryDecodeConfigRecord(const uint8_t *data,
                                  size_t length,
                                  ConfigRecordMetadata &meta,
                                  const char *label,
                                  bool &isRecord);

static size_t configJsonCapacityForPayload(size_t payloadSize) {
  size_t capacity = CONFIG_JSON_MIN_CAPACITY;
  if (payloadSize > 0) {
    size_t margin = payloadSize / 4;
    if (margin < CONFIG_JSON_SAFETY_MARGIN) {
      margin = CONFIG_JSON_SAFETY_MARGIN;
    }
    size_t desired = payloadSize + margin;
    if (desired > capacity) {
      capacity = desired;
    }
  }
  if (capacity > CONFIG_JSON_MAX_CAPACITY) {
    capacity = CONFIG_JSON_MAX_CAPACITY;
  }
  return capacity;
}

static size_t growConfigJsonCapacity(size_t current) {
  if (current >= CONFIG_JSON_MAX_CAPACITY) {
    return CONFIG_JSON_MAX_CAPACITY;
  }
  size_t next = current + current / 2;
  if (next < current + 1024) {
    next = current + 1024;
  }
  if (next > CONFIG_JSON_MAX_CAPACITY) {
    next = CONFIG_JSON_MAX_CAPACITY;
  }
  return next;
}
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

static void appendConfigSaveLog(const String &message) {
  if (!ensureUserDirectory()) {
    logMessage("Unable to open private directory for config save log");
    return;
  }
  File logFile = LittleFS.open(CONFIG_SAVE_LOG_PATH, "a");
  if (!logFile) {
    logPrintf("Failed to append config save log: %s", CONFIG_SAVE_LOG_PATH);
    return;
  }
  String line = String("[") + millis() + "] " + message;
  logFile.println(line);
  logFile.close();
}

static bool writeTextFile(const char *path, const String &content) {
  File f = LittleFS.open(path, "w");
  if (!f) {
    logPrintf("Failed to open %s for writing", path);
    return false;
  }
  size_t written = f.write(reinterpret_cast<const uint8_t *>(content.c_str()),
                           content.length());
  f.close();
  if (written != content.length()) {
    logPrintf("Short write when saving %s (%u/%u bytes)", path,
              static_cast<unsigned>(written),
              static_cast<unsigned>(content.length()));
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

static String summariseLogDetail(JsonVariantConst detail) {
  if (detail.isNull()) {
    return String();
  }
  String output;
  if (detail.is<JsonObjectConst>() || detail.is<JsonArrayConst>()) {
    serializeJson(detail, output);
  } else {
    output = detail.as<String>();
  }
  static const size_t kMaxLogDetailLength = 512;
  if (output.length() > kMaxLogDetailLength) {
    output.remove(kMaxLogDetailLength);
    output += F("...");
  }
  return output;
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
static const uint8_t MAX_METER_CHANNELS = 6;

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

struct MeterChannelConfig {
  String id;
  String name;
  String label;
  String input;
  String unit;
  String symbol;
  bool enabled;
  float scale;
  float offset;
  bool hasRangeMin;
  float rangeMin;
  bool hasRangeMax;
  float rangeMax;
  uint8_t bits;
};

struct VirtualMultimeterConfig {
  uint8_t channelCount;
  MeterChannelConfig channels[MAX_METER_CHANNELS];
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
  VirtualMultimeterConfig virtualMultimeter;
  uint8_t       peerCount;   // Number of stored peer PINs
  PeerAuth      peers[MAX_PEERS];
};

static const uint8_t CONFIG_SECTION_INTERFACE = 0x01;
static const uint8_t CONFIG_SECTION_MODULES   = 0x02;
static const uint8_t CONFIG_SECTION_IO        = 0x04;
static const uint8_t CONFIG_SECTION_VIRTUAL   = 0x08;
static const uint8_t CONFIG_SECTION_PEERS     = 0x10;
static const uint8_t CONFIG_SECTION_ALL = CONFIG_SECTION_INTERFACE |
                                         CONFIG_SECTION_MODULES   |
                                         CONFIG_SECTION_IO        |
                                         CONFIG_SECTION_VIRTUAL   |
                                         CONFIG_SECTION_PEERS;

static Config config;            // Global configuration instance
static const uint16_t HTTP_PORT = 80;
static ESP8266WebServer primaryHttpServer(HTTP_PORT);              // HTTP server instance
static WiFiUDP udp;               // UDP object for broadcasting and listening
static Adafruit_ADS1115 ads;      // ADS1115 ADC instance
static Adafruit_MCP4725 mcp4725Drivers[MAX_OUTPUTS];
static bool mcp4725Ready[MAX_OUTPUTS] = {false};
static uint8_t mcp4725Addresses[MAX_OUTPUTS] = {0};

// Forward references required by the IO configuration helpers defined below.
int parsePin(const String &p);
static String describePinValue(int pin);
static String describeOptionalInt(int value);
static uint8_t parseI2cAddress(const String &s);
static String formatI2cAddress(uint8_t address);

// ---------------------------------------------------------------------------
// Helper utilities for robust IO configuration parsing.
// These helpers ensure that configuration documents are always expanded
// into a consistent in-memory representation with exhaustive logging.
// ---------------------------------------------------------------------------

static InputConfig makeDefaultInputSlot(uint8_t index) {
  InputConfig ic;
  ic.name = String("IN") + String(index + 1);
  ic.type = INPUT_DISABLED;
  ic.pin = -1;
  ic.adsChannel = -1;
  ic.remoteNode = "";
  ic.remoteName = "";
  ic.scale = 1.0f;
  ic.offset = 0.0f;
  ic.unit = "";
  ic.active = false;
  ic.value = NAN;
  return ic;
}

static OutputConfig makeDefaultOutputSlot(uint8_t index) {
  OutputConfig oc;
  oc.name = String("OUT") + String(index + 1);
  oc.type = OUTPUT_DISABLED;
  oc.pin = -1;
  oc.pwmFreq = 2000;
  oc.i2cAddress = 0x60;
  oc.scale = 1.0f;
  oc.offset = 0.0f;
  oc.active = false;
  oc.value = 0.0f;
  return oc;
}

static void resetInputSlots(Config &cfg) {
  cfg.inputCount = 0;
  for (uint8_t i = 0; i < MAX_INPUTS; ++i) {
    cfg.inputs[i] = makeDefaultInputSlot(i);
  }
}

static void resetOutputSlots(Config &cfg) {
  cfg.outputCount = 0;
  for (uint8_t i = 0; i < MAX_OUTPUTS; ++i) {
    cfg.outputs[i] = makeDefaultOutputSlot(i);
  }
}

static bool hasDuplicateName(const std::vector<String> &names,
                             const String &candidate) {
  for (const String &value : names) {
    if (value == candidate) {
      return true;
    }
  }
  return false;
}

static void logParsedInput(const InputConfig &ic, const String &entryTag) {
  String tag = entryTag;
  if (tag.length() == 0) {
    tag = ic.name;
  }
  logPrintf(
      "Input %s => name=%s type=%s pin=%s adsChannel=%s remote=%s/%s scale=%s offset=%s unit=%s active=%s",
      tag.c_str(), ic.name.c_str(), inputTypeToString(ic.type).c_str(),
      describePinValue(ic.pin).c_str(),
      describeOptionalInt(ic.adsChannel).c_str(), ic.remoteNode.c_str(),
      ic.remoteName.c_str(), String(ic.scale, 4).c_str(),
      String(ic.offset, 4).c_str(), ic.unit.c_str(),
      ic.active ? "true" : "false");
}

static void logParsedOutput(const OutputConfig &oc, const String &entryTag) {
  String tag = entryTag;
  if (tag.length() == 0) {
    tag = oc.name;
  }
  logPrintf(
      "Output %s => name=%s type=%s pin=%s pwm=%s addr=%s scale=%s offset=%s active=%s value=%s",
      tag.c_str(), oc.name.c_str(), outputTypeToString(oc.type).c_str(),
      describePinValue(oc.pin).c_str(),
      describeOptionalInt(oc.pwmFreq).c_str(),
      formatI2cAddress(oc.i2cAddress).c_str(), String(oc.scale, 4).c_str(),
      String(oc.offset, 4).c_str(), oc.active ? "true" : "false",
      String(oc.value, 4).c_str());
}

static bool parsePinVariant(JsonVariantConst variant, int &pinOut, String &rawOut) {
  if (variant.is<const char *>()) {
    rawOut = variant.as<String>();
  } else if (variant.is<int>() || variant.is<long>() || variant.is<unsigned long>() ||
             variant.is<unsigned int>()) {
    rawOut = String(variant.as<long>());
  } else {
    return false;
  }
  rawOut.trim();
  int parsed = parsePin(rawOut);
  if (parsed == -1) {
    return false;
  }
  pinOut = parsed;
  return true;
}

static bool parseI2cAddressVariant(JsonVariantConst variant,
                                   uint8_t &addressOut,
                                   String &rawOut) {
  if (variant.is<const char *>()) {
    rawOut = variant.as<String>();
  } else if (variant.is<int>() || variant.is<long>() || variant.is<unsigned long>() ||
             variant.is<unsigned int>()) {
    rawOut = String(variant.as<long>(), 10);
  } else {
    return false;
  }
  rawOut.trim();
  addressOut = parseI2cAddress(rawOut);
  return true;
}

static bool populateInputFromObject(JsonObjectConst obj,
                                    InputConfig &ic,
                                    const String &entryTag) {
  String fallbackName = ic.name;
  String tag = entryTag.length() > 0 ? entryTag : fallbackName;

  if (obj.containsKey("name")) {
    String provided = obj["name"].as<String>();
    provided.trim();
    if (provided.length() > 0) {
      ic.name = provided;
    } else {
      logPrintf("Input %s provided empty name; using %s", tag.c_str(),
                fallbackName.c_str());
      ic.name = fallbackName;
    }
  } else if (entryTag.length() > 0 && !entryTag.startsWith("#")) {
    ic.name = entryTag;
    logPrintf("Input %s missing 'name', using map key '%s'", tag.c_str(),
              entryTag.c_str());
  } else {
    logPrintf("Input %s missing 'name', using default %s", tag.c_str(),
              fallbackName.c_str());
  }

  if (obj.containsKey("type")) {
    String typeStr = obj["type"].as<String>();
    typeStr.trim();
    InputType parsed = parseInputType(typeStr);
    ic.type = parsed;
    String lower = typeStr;
    lower.toLowerCase();
    if (parsed == INPUT_DISABLED && lower.length() > 0 && lower != "disabled") {
      logPrintf("Input %s has unsupported type '%s', defaulting to disabled",
                ic.name.c_str(), typeStr.c_str());
    }
  } else {
    logPrintf("Input %s missing type, defaulting to disabled",
              ic.name.c_str());
    ic.type = INPUT_DISABLED;
  }

  if (obj.containsKey("pin")) {
    String rawPin;
    int parsedPin = -1;
    if (!parsePinVariant(obj["pin"], parsedPin, rawPin)) {
      logPrintf("Input %s has invalid pin specification '%s'", ic.name.c_str(),
                rawPin.c_str());
    } else {
      ic.pin = parsedPin;
    }
  } else if (ic.type == INPUT_ADC) {
    ic.pin = A0;
    logPrintf("Input %s missing pin for ADC type, defaulting to A0",
              ic.name.c_str());
  } else if (ic.type == INPUT_DIV || ic.type == INPUT_ZMPT ||
             ic.type == INPUT_ZMCT) {
    logPrintf("Input %s missing pin for %s sensor", ic.name.c_str(),
              inputTypeToString(ic.type).c_str());
  }

  if (obj.containsKey("adsChannel")) {
    int channel = obj["adsChannel"].as<int>();
    ic.adsChannel = channel;
    if (channel < 0 || channel > 3) {
      logPrintf("Input %s has out-of-range adsChannel %d (expected 0-3)",
                ic.name.c_str(), channel);
    }
  } else if (ic.type == INPUT_ADS1115) {
    logPrintf("Input %s missing 'adsChannel' for ADS1115", ic.name.c_str());
  }

  if (obj.containsKey("remoteNode")) {
    ic.remoteNode = obj["remoteNode"].as<String>();
    ic.remoteNode.trim();
  }
  if (obj.containsKey("remoteName")) {
    ic.remoteName = obj["remoteName"].as<String>();
    ic.remoteName.trim();
  }
  if (ic.type == INPUT_REMOTE) {
    if (ic.remoteNode.length() == 0) {
      logPrintf("Input %s of type remote missing 'remoteNode'", ic.name.c_str());
    }
    if (ic.remoteName.length() == 0) {
      ic.remoteName = ic.name;
      logPrintf("Input %s of type remote missing 'remoteName', using %s",
                ic.name.c_str(), ic.name.c_str());
    }
  }

  ic.scale = obj.containsKey("scale") ? obj["scale"].as<float>() : 1.0f;
  ic.offset = obj.containsKey("offset") ? obj["offset"].as<float>() : 0.0f;
  ic.unit = obj.containsKey("unit") ? obj["unit"].as<String>() : String();
  ic.unit.trim();
  bool hadActive = obj.containsKey("active");
  ic.active = hadActive ? obj["active"].as<bool>() : (ic.type != INPUT_DISABLED);
  if (!hadActive && ic.type != INPUT_DISABLED) {
    logPrintf("Input %s missing 'active', defaulting to true", ic.name.c_str());
  }
  if (obj.containsKey("value")) {
    ic.value = obj["value"].as<float>();
  }

  logParsedInput(ic, tag);
  return true;
}

static bool populateOutputFromObject(JsonObjectConst obj,
                                     OutputConfig &oc,
                                     const String &entryTag) {
  String fallbackName = oc.name;
  String tag = entryTag.length() > 0 ? entryTag : fallbackName;

  if (obj.containsKey("name")) {
    String provided = obj["name"].as<String>();
    provided.trim();
    if (provided.length() > 0) {
      oc.name = provided;
    } else {
      logPrintf("Output %s provided empty name; using %s", tag.c_str(),
                fallbackName.c_str());
      oc.name = fallbackName;
    }
  } else if (entryTag.length() > 0 && !entryTag.startsWith("#")) {
    oc.name = entryTag;
    logPrintf("Output %s missing 'name', using map key '%s'", tag.c_str(),
              entryTag.c_str());
  } else {
    logPrintf("Output %s missing 'name', using default %s", tag.c_str(),
              fallbackName.c_str());
  }

  if (obj.containsKey("type")) {
    String typeStr = obj["type"].as<String>();
    typeStr.trim();
    OutputType parsed = parseOutputType(typeStr);
    oc.type = parsed;
    String lower = typeStr;
    lower.toLowerCase();
    if (parsed == OUTPUT_DISABLED && lower.length() > 0 && lower != "disabled") {
      logPrintf("Output %s has unsupported type '%s', defaulting to disabled",
                oc.name.c_str(), typeStr.c_str());
    }
  } else {
    logPrintf("Output %s missing type, defaulting to disabled",
              oc.name.c_str());
    oc.type = OUTPUT_DISABLED;
  }

  if (obj.containsKey("pin")) {
    String rawPin;
    int parsedPin = -1;
    if (!parsePinVariant(obj["pin"], parsedPin, rawPin)) {
      logPrintf("Output %s has invalid pin specification '%s'", oc.name.c_str(),
                rawPin.c_str());
    } else {
      oc.pin = parsedPin;
    }
  } else if (oc.type == OUTPUT_PWM010 || oc.type == OUTPUT_GPIO) {
    logPrintf("Output %s missing pin for %s", oc.name.c_str(),
              outputTypeToString(oc.type).c_str());
  }

  oc.pwmFreq = obj.containsKey("pwmFreq") ? obj["pwmFreq"].as<int>() : oc.pwmFreq;

  if (obj.containsKey("i2cAddress")) {
    String rawAddr;
    uint8_t parsedAddr = oc.i2cAddress;
    if (!parseI2cAddressVariant(obj["i2cAddress"], parsedAddr, rawAddr)) {
      logPrintf("Output %s has invalid i2cAddress specification '%s'",
                oc.name.c_str(), rawAddr.c_str());
    } else {
      oc.i2cAddress = parsedAddr;
    }
  } else if (oc.type == OUTPUT_MCP4725) {
    logPrintf("Output %s missing i2cAddress for MCP4725", oc.name.c_str());
  }

  oc.scale = obj.containsKey("scale") ? obj["scale"].as<float>() : 1.0f;
  oc.offset = obj.containsKey("offset") ? obj["offset"].as<float>() : 0.0f;
  bool hadActive = obj.containsKey("active");
  oc.active = hadActive ? obj["active"].as<bool>() : (oc.type != OUTPUT_DISABLED);
  if (!hadActive && oc.type != OUTPUT_DISABLED) {
    logPrintf("Output %s missing 'active', defaulting to true", oc.name.c_str());
  }
  oc.value = obj.containsKey("value") ? obj["value"].as<float>() : oc.value;

  logParsedOutput(oc, tag);
  return true;
}

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
    size_t nestedCapacity = configJsonCapacityForPayload(trimmed.length());
    DynamicJsonDocument nested(nestedCapacity);
    if (nestedCapacity > 0 && nested.capacity() == 0) {
      logPrintf("Failed to allocate %u bytes to parse %s JSON string",
                static_cast<unsigned>(nestedCapacity), contextLabel);
      return false;
    }
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

static bool decodeInputs(JsonVariantConst inputsVar,
                         std::vector<InputConfig> &parsed) {
  std::vector<String> seenNames;
  bool processed = false;
  auto handleArray = [&](JsonArrayConst arr) {
    processed = true;
    size_t total = arr.size();
    if (total > MAX_INPUTS) {
      logPrintf(
          "Input array provides %u entries but only %u are supported; extra entries will be ignored",
          static_cast<unsigned>(total), static_cast<unsigned>(MAX_INPUTS));
    }
    uint16_t index = 0;
    for (JsonVariantConst entry : arr) {
      if (parsed.size() >= MAX_INPUTS) {
        logPrintf("Ignoring additional input definitions beyond %u entries",
                  static_cast<unsigned>(MAX_INPUTS));
        break;
      }
      JsonObjectConst entryObj = entry.as<JsonObjectConst>();
      if (entryObj.isNull()) {
        logPrintf("Input entry %u is not an object; skipping",
                  static_cast<unsigned>(index));
        ++index;
        continue;
      }
      InputConfig candidate = makeDefaultInputSlot(parsed.size());
      String entryTag = String("#") + String(index);
      populateInputFromObject(entryObj, candidate, entryTag);
      if (hasDuplicateName(seenNames, candidate.name)) {
        logPrintf("Duplicate input name '%s' encountered; ignoring entry %s",
                  candidate.name.c_str(), entryTag.c_str());
      } else {
        parsed.push_back(candidate);
        seenNames.push_back(candidate.name);
      }
      ++index;
    }
  };
  auto handleObject = [&](JsonObjectConst obj) {
    processed = true;
    size_t total = obj.size();
    if (total > MAX_INPUTS) {
      logPrintf(
          "Input map provides %u entries but only %u are supported; extra entries will be ignored",
          static_cast<unsigned>(total), static_cast<unsigned>(MAX_INPUTS));
    }
    for (JsonPairConst kv : obj) {
      if (parsed.size() >= MAX_INPUTS) {
        logPrintf("Ignoring additional input definitions beyond %u entries",
                  static_cast<unsigned>(MAX_INPUTS));
        break;
      }
      JsonVariantConst value = kv.value();
      JsonObjectConst entryObj = value.as<JsonObjectConst>();
      if (entryObj.isNull()) {
        logPrintf("Input entry '%s' is not an object; skipping",
                  kv.key().c_str());
        continue;
      }
      InputConfig candidate = makeDefaultInputSlot(parsed.size());
      String entryTag = String(kv.key().c_str());
      populateInputFromObject(entryObj, candidate, entryTag);
      if (hasDuplicateName(seenNames, candidate.name)) {
        logPrintf("Duplicate input name '%s' encountered; ignoring entry '%s'",
                  candidate.name.c_str(), entryTag.c_str());
      } else {
        parsed.push_back(candidate);
        seenNames.push_back(candidate.name);
      }
    }
  };

  if (!inputsVar.isNull()) {
    JsonArrayConst arr = inputsVar.as<JsonArrayConst>();
    if (!arr.isNull()) {
      handleArray(arr);
    } else {
      JsonObjectConst obj = inputsVar.as<JsonObjectConst>();
      if (!obj.isNull()) {
        handleObject(obj);
      }
    }
    if (!processed && inputsVar.is<const char *>()) {
      processed = parseJsonContainerString(inputsVar.as<String>(), handleArray,
                                          handleObject, "input configuration");
    }
    if (!processed) {
      String serialized;
      serializeJson(inputsVar, serialized);
      if (serialized.length() > 0) {
        processed = parseJsonContainerString(serialized, handleArray,
                                            handleObject,
                                            "input configuration");
      }
    }
  }
  return processed;
}

static bool decodeOutputs(JsonVariantConst outputsVar,
                          std::vector<OutputConfig> &parsed) {
  std::vector<String> seenNames;
  bool processed = false;
  auto handleArray = [&](JsonArrayConst arr) {
    processed = true;
    size_t total = arr.size();
    if (total > MAX_OUTPUTS) {
      logPrintf(
          "Output array provides %u entries but only %u are supported; extra entries will be ignored",
          static_cast<unsigned>(total), static_cast<unsigned>(MAX_OUTPUTS));
    }
    uint16_t index = 0;
    for (JsonVariantConst entry : arr) {
      if (parsed.size() >= MAX_OUTPUTS) {
        logPrintf("Ignoring additional output definitions beyond %u entries",
                  static_cast<unsigned>(MAX_OUTPUTS));
        break;
      }
      JsonObjectConst entryObj = entry.as<JsonObjectConst>();
      if (entryObj.isNull()) {
        logPrintf("Output entry %u is not an object; skipping",
                  static_cast<unsigned>(index));
        ++index;
        continue;
      }
      OutputConfig candidate = makeDefaultOutputSlot(parsed.size());
      String entryTag = String("#") + String(index);
      populateOutputFromObject(entryObj, candidate, entryTag);
      if (hasDuplicateName(seenNames, candidate.name)) {
        logPrintf("Duplicate output name '%s' encountered; ignoring entry %s",
                  candidate.name.c_str(), entryTag.c_str());
      } else {
        parsed.push_back(candidate);
        seenNames.push_back(candidate.name);
      }
      ++index;
    }
  };
  auto handleObject = [&](JsonObjectConst obj) {
    processed = true;
    size_t total = obj.size();
    if (total > MAX_OUTPUTS) {
      logPrintf(
          "Output map provides %u entries but only %u are supported; extra entries will be ignored",
          static_cast<unsigned>(total), static_cast<unsigned>(MAX_OUTPUTS));
    }
    for (JsonPairConst kv : obj) {
      if (parsed.size() >= MAX_OUTPUTS) {
        logPrintf("Ignoring additional output definitions beyond %u entries",
                  static_cast<unsigned>(MAX_OUTPUTS));
        break;
      }
      JsonVariantConst value = kv.value();
      JsonObjectConst entryObj = value.as<JsonObjectConst>();
      if (entryObj.isNull()) {
        logPrintf("Output entry '%s' is not an object; skipping",
                  kv.key().c_str());
        continue;
      }
      OutputConfig candidate = makeDefaultOutputSlot(parsed.size());
      String entryTag = String(kv.key().c_str());
      populateOutputFromObject(entryObj, candidate, entryTag);
      if (hasDuplicateName(seenNames, candidate.name)) {
        logPrintf("Duplicate output name '%s' encountered; ignoring entry '%s'",
                  candidate.name.c_str(), entryTag.c_str());
      } else {
        parsed.push_back(candidate);
        seenNames.push_back(candidate.name);
      }
    }
  };

  if (!outputsVar.isNull()) {
    JsonArrayConst arr = outputsVar.as<JsonArrayConst>();
    if (!arr.isNull()) {
      handleArray(arr);
    } else {
      JsonObjectConst obj = outputsVar.as<JsonObjectConst>();
      if (!obj.isNull()) {
        handleObject(obj);
      }
    }
    if (!processed && outputsVar.is<const char *>()) {
      processed = parseJsonContainerString(outputsVar.as<String>(),
                                          handleArray, handleObject,
                                          "output configuration");
    }
    if (!processed) {
      String serialized;
      serializeJson(outputsVar, serialized);
      if (serialized.length() > 0) {
        processed = parseJsonContainerString(serialized, handleArray,
                                            handleObject,
                                            "output configuration");
      }
    }
  }
  return processed;
}

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
bool saveIoConfig();
bool saveInterfaceConfig();
bool saveVirtualConfig();
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
String pinToString(int pin);
static const InputConfig *findInputByName(const Config &cfg, const String &name);
static const OutputConfig *findOutputByName(const Config &cfg, const String &name);
static String diffInputConfig(const InputConfig &before, const InputConfig &after);
static String diffOutputConfig(const OutputConfig &before, const OutputConfig &after);
static const char *describeJsonType(const JsonVariantConst &value);
static void logIoDelta(const Config &before, const Config &after);
static bool verifyConfigStored(const Config &expected, String &errorDetail);

static MeterChannelConfig makeEmptyMeterChannel() {
  return {"", "", "", "", "", "", false, 1.0f, 0.0f, false, 0.0f, false,
          0.0f, 10};
}

static MeterChannelConfig makeDefaultMeterChannel() {
  return {"", "", "", "", "", "", true, 1.0f, 0.0f, false, 0.0f, false,
          0.0f, 10};
}

static void clearVirtualMultimeterConfig(VirtualMultimeterConfig &cfg) {
  cfg.channelCount = 0;
  for (uint8_t i = 0; i < MAX_METER_CHANNELS; ++i) {
    cfg.channels[i] = makeEmptyMeterChannel();
  }
}

static void parseMeterChannelEntry(JsonObjectConst obj,
                                   VirtualMultimeterConfig &target,
                                   int indexHint,
                                   const String &entryLabel) {
  if (target.channelCount >= MAX_METER_CHANNELS) {
    return;
  }
  MeterChannelConfig &mc = target.channels[target.channelCount];
  mc = makeDefaultMeterChannel();
  auto fallbackId = [&](void) {
    if (entryLabel.length() > 0) {
      return entryLabel;
    }
    if (indexHint >= 0) {
      return String("meter") + String(indexHint + 1);
    }
    return String("meter") + String(target.channelCount + 1);
  };
  if (obj.containsKey("id")) {
    mc.id = obj["id"].as<String>();
    mc.id.trim();
  }
  if (mc.id.length() == 0) {
    mc.id = fallbackId();
  }
  if (obj.containsKey("name")) {
    mc.name = obj["name"].as<String>();
    mc.name.trim();
  }
  if (mc.name.length() == 0) {
    mc.name = mc.id;
  }
  if (obj.containsKey("label")) {
    mc.label = obj["label"].as<String>();
    mc.label.trim();
  }
  if (mc.label.length() == 0) {
    mc.label = mc.name;
  }
  if (obj.containsKey("input")) {
    mc.input = obj["input"].as<String>();
    mc.input.trim();
  }
  if (obj.containsKey("unit")) {
    mc.unit = obj["unit"].as<String>();
    mc.unit.trim();
  }
  if (obj.containsKey("symbol")) {
    mc.symbol = obj["symbol"].as<String>();
    mc.symbol.trim();
  }
  mc.enabled = obj.containsKey("enabled") ? obj["enabled"].as<bool>() : true;
  mc.scale = obj.containsKey("scale") ? obj["scale"].as<float>() : 1.0f;
  if (isnan(mc.scale)) mc.scale = 1.0f;
  mc.offset = obj.containsKey("offset") ? obj["offset"].as<float>() : 0.0f;
  if (isnan(mc.offset)) mc.offset = 0.0f;
  if (obj.containsKey("rangeMin") && !obj["rangeMin"].isNull()) {
    float value = obj["rangeMin"].as<float>();
    if (isnan(value)) {
      mc.hasRangeMin = false;
      mc.rangeMin = 0.0f;
    } else {
      mc.hasRangeMin = true;
      mc.rangeMin = value;
    }
  } else {
    mc.hasRangeMin = false;
    mc.rangeMin = 0.0f;
  }
  if (obj.containsKey("rangeMax") && !obj["rangeMax"].isNull()) {
    float value = obj["rangeMax"].as<float>();
    if (isnan(value)) {
      mc.hasRangeMax = false;
      mc.rangeMax = 0.0f;
    } else {
      mc.hasRangeMax = true;
      mc.rangeMax = value;
    }
  } else {
    mc.hasRangeMax = false;
    mc.rangeMax = 0.0f;
  }
  int bitsValue = obj.containsKey("bits") ? obj["bits"].as<int>() : 10;
  if (bitsValue < 1) bitsValue = 1;
  if (bitsValue > 32) bitsValue = 32;
  mc.bits = static_cast<uint8_t>(bitsValue);
  target.channelCount++;
}

static void parseMeterChannelArray(JsonArrayConst arr,
                                   VirtualMultimeterConfig &target) {
  uint8_t index = 0;
  for (JsonVariantConst entry : arr) {
    JsonObjectConst obj = entry.as<JsonObjectConst>();
    if (obj.isNull()) {
      index++;
      continue;
    }
    parseMeterChannelEntry(obj, target, index, String());
    index++;
  }
}

static void parseMeterChannelObject(JsonObjectConst obj,
                                    VirtualMultimeterConfig &target) {
  JsonArrayConst channelsArr = obj["channels"].as<JsonArrayConst>();
  if (!channelsArr.isNull()) {
    parseMeterChannelArray(channelsArr, target);
    return;
  }
  JsonObjectConst channelsObj = obj["channels"].as<JsonObjectConst>();
  if (!channelsObj.isNull()) {
    int index = 0;
    for (JsonPairConst kv : channelsObj) {
      if (target.channelCount >= MAX_METER_CHANNELS) {
        break;
      }
      JsonObjectConst entryObj = kv.value().as<JsonObjectConst>();
      if (entryObj.isNull()) {
        index++;
        continue;
      }
      parseMeterChannelEntry(entryObj, target, index,
                             String(kv.key().c_str()));
      index++;
    }
  }
}

static void parseVirtualMultimeterVariant(JsonVariantConst meterVar,
                                          VirtualMultimeterConfig &target) {
  clearVirtualMultimeterConfig(target);
  if (meterVar.isNull()) {
    return;
  }
  JsonArrayConst arr = meterVar.as<JsonArrayConst>();
  if (!arr.isNull()) {
    parseMeterChannelArray(arr, target);
    return;
  }
  JsonObjectConst obj = meterVar.as<JsonObjectConst>();
  if (!obj.isNull()) {
    parseMeterChannelObject(obj, target);
  }
}

static void populateVirtualMultimeterJson(JsonObject obj,
                                          const VirtualMultimeterConfig &cfg) {
  obj["channelCount"] = cfg.channelCount;
  JsonArray arr = obj.createNestedArray("channels");
  for (uint8_t i = 0; i < cfg.channelCount && i < MAX_METER_CHANNELS; ++i) {
    const MeterChannelConfig &mc = cfg.channels[i];
    JsonObject entry = arr.createNestedObject();
    entry["id"] = mc.id;
    entry["name"] = mc.name;
    entry["label"] = mc.label;
    entry["input"] = mc.input;
    entry["unit"] = mc.unit;
    entry["symbol"] = mc.symbol;
    entry["enabled"] = mc.enabled;
    entry["scale"] = mc.scale;
    entry["offset"] = mc.offset;
    if (mc.hasRangeMin) {
      entry["rangeMin"] = mc.rangeMin;
    } else {
      entry["rangeMin"] = nullptr;
    }
    if (mc.hasRangeMax) {
      entry["rangeMax"] = mc.rangeMax;
    } else {
      entry["rangeMax"] = nullptr;
    }
    entry["bits"] = mc.bits;
  }
}

void parseConfigFromJson(const JsonDocument &doc,
                         Config &target,
                         const Config *previous,
                         bool logIoChanges,
                         uint8_t sections = CONFIG_SECTION_ALL) {
  if (sections & CONFIG_SECTION_INTERFACE) {
    if (doc.containsKey("nodeId")) {
      target.nodeId = doc["nodeId"].as<String>();
    }
    if (doc.containsKey("wifi")) {
      JsonObjectConst w = doc["wifi"].as<JsonObjectConst>();
      if (!w.isNull()) {
        if (w.containsKey("mode")) target.wifi.mode = w["mode"].as<String>();
        if (w.containsKey("ssid")) target.wifi.ssid = w["ssid"].as<String>();
        if (w.containsKey("pass")) target.wifi.pass = w["pass"].as<String>();
      }
    }
  }
  if (sections & CONFIG_SECTION_MODULES) {
    if (doc.containsKey("modules")) {
      JsonObjectConst m = doc["modules"].as<JsonObjectConst>();
      if (!m.isNull()) {
        if (m.containsKey("ads1115")) target.modules.ads1115 = m["ads1115"].as<bool>();
        if (m.containsKey("pwm010")) target.modules.pwm010 = m["pwm010"].as<bool>();
        if (m.containsKey("zmpt")) target.modules.zmpt = m["zmpt"].as<bool>();
        if (m.containsKey("zmct")) target.modules.zmct = m["zmct"].as<bool>();
        if (m.containsKey("div")) target.modules.div = m["div"].as<bool>();
        if (m.containsKey("mcp4725")) target.modules.mcp4725 = m["mcp4725"].as<bool>();
      }
    }
  }

  if (sections & CONFIG_SECTION_IO) {
    resetInputSlots(target);
    resetOutputSlots(target);
  }
  if ((sections & CONFIG_SECTION_VIRTUAL) && !previous) {
    clearVirtualMultimeterConfig(target.virtualMultimeter);
  }

  if (sections & CONFIG_SECTION_IO) {
    std::vector<InputConfig> parsedInputs;
    JsonVariantConst inputsVar = doc["inputs"];
    if (!inputsVar.isNull()) {
      if (!decodeInputs(inputsVar, parsedInputs)) {
        logPrintf("Input configuration malformed: expected array or object but found %s",
                  describeJsonType(inputsVar));
        logMessage("Input configuration malformed: expected array or object");
      }
    } else {
      logPrintf("Configuration JSON missing 'inputs'; keeping defaults");
    }

    if (!parsedInputs.empty()) {
      uint8_t applied = static_cast<uint8_t>(parsedInputs.size());
      if (applied > MAX_INPUTS) {
        applied = MAX_INPUTS;
      }
      for (uint8_t i = 0; i < applied; ++i) {
        target.inputs[i] = parsedInputs[i];
      }
      target.inputCount = applied;
    } else {
      target.inputCount = 0;
    }
    logPrintf("Applied %u input configuration entries",
              static_cast<unsigned>(target.inputCount));

    std::vector<OutputConfig> parsedOutputs;
    JsonVariantConst outputsVar = doc["outputs"];
    if (!outputsVar.isNull()) {
      if (!decodeOutputs(outputsVar, parsedOutputs)) {
        logPrintf("Output configuration malformed: expected array or object but found %s",
                  describeJsonType(outputsVar));
        logMessage("Output configuration malformed: expected array or object");
      }
    } else {
      logPrintf("Configuration JSON missing 'outputs'; keeping defaults");
    }

    if (!parsedOutputs.empty()) {
      uint8_t applied = static_cast<uint8_t>(parsedOutputs.size());
      if (applied > MAX_OUTPUTS) {
        applied = MAX_OUTPUTS;
      }
      for (uint8_t i = 0; i < applied; ++i) {
        target.outputs[i] = parsedOutputs[i];
      }
      target.outputCount = applied;
    } else {
      target.outputCount = 0;
    }
    logPrintf("Applied %u output configuration entries",
              static_cast<unsigned>(target.outputCount));
  }

  if (sections & CONFIG_SECTION_VIRTUAL) {
    JsonVariantConst meterVar = doc["virtualMultimeter"];
    if (!meterVar.isNull()) {
      parseVirtualMultimeterVariant(meterVar, target.virtualMultimeter);
    } else if (previous) {
      target.virtualMultimeter = previous->virtualMultimeter;
    } else {
      clearVirtualMultimeterConfig(target.virtualMultimeter);
    }
  }

  if (sections & CONFIG_SECTION_PEERS) {
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
    if (!peersVar.isNull()) {
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

  if ((sections & CONFIG_SECTION_IO) && logIoChanges && previous) {
    logIoDelta(*previous, target);
  }
}

void parseConfigFromJson(const JsonDocument &doc,
                         Config &target,
                         uint8_t sections = CONFIG_SECTION_ALL) {
  parseConfigFromJson(doc, target, nullptr, false, sections);
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

static void logJsonParseFailure(const char *context,
                                const String &payload,
                                size_t capacity,
                                DeserializationError err) {
  logPrintf("%s JSON parse failed (%u bytes, capacity=%u): %s", context,
            static_cast<unsigned>(payload.length()),
            static_cast<unsigned>(capacity), err.c_str());
  if (payload.length() == 0) {
    return;
  }
  size_t previewLen = payload.length();
  if (previewLen > 160) {
    previewLen = 160;
  }
  String preview = payload.substring(0, previewLen);
  preview.replace("\r", "\\r");
  preview.replace("\n", "\\n");
  if (payload.length() > previewLen) {
    preview += "…";
  }
  logPrintf("%s JSON preview (%u chars): %s", context,
            static_cast<unsigned>(preview.length()), preview.c_str());
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
                               uint8_t sections = CONFIG_SECTION_ALL,
                               bool includeRuntimeFields = false) {
  doc.clear();
  if (sections & CONFIG_SECTION_INTERFACE) {
    doc["nodeId"] = config.nodeId;
    if (includeRuntimeFields) {
      doc["fwVersion"] = getFirmwareVersion();
    }
    JsonObject wifiObj = doc.createNestedObject("wifi");
    wifiObj["mode"] = config.wifi.mode;
    wifiObj["ssid"] = config.wifi.ssid;
    wifiObj["pass"] = config.wifi.pass;
  }
  if (sections & CONFIG_SECTION_MODULES) {
    JsonObject modObj = doc.createNestedObject("modules");
    modObj["ads1115"] = config.modules.ads1115;
    modObj["pwm010"] = config.modules.pwm010;
    modObj["zmpt"] = config.modules.zmpt;
    modObj["zmct"] = config.modules.zmct;
    modObj["div"] = config.modules.div;
    modObj["mcp4725"] = config.modules.mcp4725;
  }
  if (sections & CONFIG_SECTION_IO) {
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
  }
  if (sections & CONFIG_SECTION_VIRTUAL) {
    JsonObject meterObj = doc.createNestedObject("virtualMultimeter");
    populateVirtualMultimeterJson(meterObj, config.virtualMultimeter);
  }
  if (sections & CONFIG_SECTION_PEERS) {
    doc["peerCount"] = config.peerCount;
    JsonArray peersArr = doc.createNestedArray("peers");
    for (uint8_t i = 0; i < config.peerCount && i < MAX_PEERS; i++) {
      JsonObject o = peersArr.createNestedObject();
      o["nodeId"] = config.peers[i].nodeId;
      o["pin"] = config.peers[i].pin;
    }
  }
}

static bool buildConfigJsonPayload(String &payload,
                                   uint8_t sections,
                                   bool includeRuntimeFields,
                                   const char *label,
                                   bool logPayload,
                                   void (*mutator)(JsonDocument &) = nullptr) {
  const char *effectiveLabel = label ? label : "Config";
  size_t docCapacity = configJsonCapacityForPayload(payload.length());
  if (docCapacity == 0) {
    docCapacity = configJsonCapacityForPayload(0);
  }
  while (true) {
    DynamicJsonDocument doc(docCapacity);
    if (docCapacity > 0 && doc.capacity() == 0) {
      logPrintf("Failed to allocate %u bytes for %s config JSON",
                static_cast<unsigned>(docCapacity), effectiveLabel);
      if (docCapacity <= CONFIG_JSON_MIN_CAPACITY) {
        return false;
      }
      size_t nextCapacity = docCapacity / 2;
      if (nextCapacity < CONFIG_JSON_MIN_CAPACITY) {
        nextCapacity = CONFIG_JSON_MIN_CAPACITY;
      }
      if (nextCapacity == docCapacity) {
        return false;
      }
      docCapacity = nextCapacity;
      continue;
    }
    populateConfigJson(doc, sections, includeRuntimeFields);
    if (mutator) {
      mutator(doc);
    }
    if (doc.overflowed()) {
      if (docCapacity >= CONFIG_JSON_MAX_CAPACITY) {
        logPrintf("%s config JSON exceeded maximum capacity (%u bytes)",
                  effectiveLabel, static_cast<unsigned>(docCapacity));
        return false;
      }
      size_t nextCapacity = growConfigJsonCapacity(docCapacity);
      if (nextCapacity == docCapacity) {
        logPrintf("%s config JSON could not grow beyond %u bytes",
                  effectiveLabel, static_cast<unsigned>(docCapacity));
        return false;
      }
      logPrintf("%s config JSON overflowed %u bytes; retrying with %u",
                effectiveLabel, static_cast<unsigned>(docCapacity),
                static_cast<unsigned>(nextCapacity));
      docCapacity = nextCapacity;
      continue;
    }
    if (logPayload) {
      logConfigJson(effectiveLabel, doc);
    }
    payload.clear();
    if (serializeJson(doc, payload) == 0) {
      logMessage(String(effectiveLabel) + " config JSON encode failed");
      return false;
    }
    return true;
  }
}

static void appendIoConfigMetadata(JsonDocument &doc) {
  JsonObject limits = doc.createNestedObject("limits");
  limits["maxInputs"] = MAX_INPUTS;
  limits["maxOutputs"] = MAX_OUTPUTS;
  JsonObject metadata = doc.createNestedObject("metadata");
  metadata["nodeId"] = config.nodeId;
  metadata["fwVersion"] = getFirmwareVersion();
}

static bool loadConfigFromPath(const char *path,
                               const char *label,
                               uint8_t sections) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    logPrintf("Failed to open %s config file: %s", label, path);
    return false;
  }
  size_t size = f.size();
  std::unique_ptr<uint8_t[]> raw(new uint8_t[size > 0 ? size : 1]);
  size_t read = f.read(raw.get(), size);
  f.close();
  if (read > size) {
    read = size;
  }
  logPrintf("Reading configuration from %s (%u/%u bytes)", path,
            static_cast<unsigned>(read), static_cast<unsigned>(size));
  if (read != size) {
    logPrintf("Warning: short read on %s (expected %u bytes, got %u)", path,
              static_cast<unsigned>(size), static_cast<unsigned>(read));
  }
  ConfigRecordMetadata meta;
  bool isRecord = false;
  if (!tryDecodeConfigRecord(raw.get(), read, meta, label, isRecord)) {
    return false;
  }

  const uint8_t *jsonPtr = raw.get();
  size_t jsonLength = read;
  if (isRecord) {
    const uint8_t *payloadPtr = raw.get() + CONFIG_RECORD_HEADER_SIZE;
    jsonPtr = payloadPtr;
    jsonLength = meta.payloadLength;
    uint32_t actualChecksum = crc32ForBuffer(jsonPtr, jsonLength);
    if (actualChecksum != meta.checksum) {
      logPrintf(
          "%s config record checksum mismatch: stored=%08X computed=%08X",
          label, static_cast<unsigned>(meta.checksum),
          static_cast<unsigned>(actualChecksum));
      return false;
    }
    logPrintf(
        "%s config record metadata: version=%u sections=0x%04X payload=%u checksum=%08X",
        label, static_cast<unsigned>(meta.version),
        static_cast<unsigned>(meta.sections),
        static_cast<unsigned>(meta.payloadLength),
        static_cast<unsigned>(meta.checksum));
  }

  std::unique_ptr<char[]> buf(new char[jsonLength + 1]);
  if (jsonLength > 0) {
    memcpy(buf.get(), jsonPtr, jsonLength);
  }
  buf[jsonLength] = '\0';

  size_t docCapacity = configJsonCapacityForPayload(jsonLength);
  while (true) {
    DynamicJsonDocument doc(docCapacity);
    DeserializationError err = deserializeJson(doc, buf.get(), jsonLength);
    if (err == DeserializationError::NoMemory &&
        docCapacity < CONFIG_JSON_MAX_CAPACITY) {
      size_t nextCapacity = growConfigJsonCapacity(docCapacity);
      if (nextCapacity == docCapacity) {
        String context = String(label) + " config";
        String payload(buf.get());
        logJsonParseFailure(context.c_str(), payload, docCapacity, err);
        return false;
      }
      logPrintf("%s config JSON exceeded %u bytes while loading; retrying with %u",
                label, static_cast<unsigned>(docCapacity),
                static_cast<unsigned>(nextCapacity));
      docCapacity = nextCapacity;
      continue;
    }
    if (err) {
      String context = String(label) + " config";
      String payload(buf.get());
      logJsonParseFailure(context.c_str(), payload, docCapacity, err);
      return false;
    }
    parseConfigFromJson(doc, config, sections);
    logConfigSummary(label);
    logConfigJson(label, doc);
    logMessage(String("Configuration loaded from ") + path);
    return true;
  }
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

  // Reset IO slots to safe defaults.  Names are pre-filled so indexes remain
  // stable when the UI creates or removes channels.
  resetInputSlots(config);
  resetOutputSlots(config);

  config.virtualMultimeter.channelCount = 0;
  for (uint8_t i = 0; i < MAX_METER_CHANNELS; ++i) {
    config.virtualMultimeter.channels[i] = {"", "", "", "", "", "", false, 1.0f, 0.0f, false, 0.0f, false, 0.0f, 10};
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

  setDefaultConfig();

  bool interfaceLoaded = false;
  bool interfaceNeedsRewrite = false;
  if (LittleFS.exists(INTERFACE_CONFIG_FILE_PATH)) {
    if (loadConfigFromPath(INTERFACE_CONFIG_FILE_PATH, "interface",
                           CONFIG_SECTION_INTERFACE | CONFIG_SECTION_PEERS)) {
      interfaceLoaded = true;
    } else {
      logMessage("Primary interface configuration load failed");
    }
  }
  if (!interfaceLoaded && LittleFS.exists(INTERFACE_CONFIG_BACKUP_FILE_PATH)) {
    logMessage("Attempting to load interface configuration from backup");
    if (loadConfigFromPath(INTERFACE_CONFIG_BACKUP_FILE_PATH,
                           "interface backup",
                           CONFIG_SECTION_INTERFACE | CONFIG_SECTION_PEERS)) {
      interfaceLoaded = true;
      interfaceNeedsRewrite = true;
    } else {
      logMessage("Interface backup configuration load failed");
    }
  }

  bool virtualLoaded = false;
  bool virtualNeedsRewrite = false;
  if (LittleFS.exists(VIRTUAL_CONFIG_FILE_PATH)) {
    if (loadConfigFromPath(VIRTUAL_CONFIG_FILE_PATH, "virtual",
                           CONFIG_SECTION_VIRTUAL)) {
      virtualLoaded = true;
    } else {
      logMessage("Primary virtual configuration load failed");
    }
  }
  if (!virtualLoaded && LittleFS.exists(VIRTUAL_CONFIG_BACKUP_FILE_PATH)) {
    logMessage("Attempting to load virtual configuration from backup");
    if (loadConfigFromPath(VIRTUAL_CONFIG_BACKUP_FILE_PATH,
                           "virtual backup",
                           CONFIG_SECTION_VIRTUAL)) {
      virtualLoaded = true;
      virtualNeedsRewrite = true;
    } else {
      logMessage("Virtual backup configuration load failed");
    }
  }

  bool ioLoaded = false;
  bool ioNeedsRewrite = false;
  uint8_t ioSections = CONFIG_SECTION_MODULES | CONFIG_SECTION_IO;
  if (!interfaceLoaded) {
    ioSections |= CONFIG_SECTION_INTERFACE | CONFIG_SECTION_PEERS;
  }
  if (!virtualLoaded) {
    ioSections |= CONFIG_SECTION_VIRTUAL;
  }
  if (LittleFS.exists(IO_CONFIG_FILE_PATH)) {
    if (loadConfigFromPath(IO_CONFIG_FILE_PATH, "IO primary", ioSections)) {
      ioLoaded = true;
      if (!interfaceLoaded &&
          (ioSections & (CONFIG_SECTION_INTERFACE | CONFIG_SECTION_PEERS))) {
        interfaceLoaded = true;
        interfaceNeedsRewrite = true;
      }
      if (!virtualLoaded && (ioSections & CONFIG_SECTION_VIRTUAL)) {
        virtualLoaded = true;
        virtualNeedsRewrite = true;
      }
      if (ioSections & (CONFIG_SECTION_INTERFACE | CONFIG_SECTION_PEERS |
                        CONFIG_SECTION_VIRTUAL)) {
        ioNeedsRewrite = true;
      }
    } else {
      logMessage("Primary IO configuration load failed");
    }
  }
  if (!ioLoaded && LittleFS.exists(IO_CONFIG_BACKUP_FILE_PATH)) {
    logMessage("Attempting to load IO configuration from backup");
    if (loadConfigFromPath(IO_CONFIG_BACKUP_FILE_PATH, "IO backup",
                           ioSections)) {
      ioLoaded = true;
      ioNeedsRewrite = true;
      if (!interfaceLoaded &&
          (ioSections & (CONFIG_SECTION_INTERFACE | CONFIG_SECTION_PEERS))) {
        interfaceLoaded = true;
        interfaceNeedsRewrite = true;
      }
      if (!virtualLoaded && (ioSections & CONFIG_SECTION_VIRTUAL)) {
        virtualLoaded = true;
        virtualNeedsRewrite = true;
      }
    } else {
      logMessage("IO backup configuration load failed");
    }
  }

  bool legacyUsed = false;
  if ((!ioLoaded || !interfaceLoaded || !virtualLoaded) &&
      LittleFS.exists(LEGACY_CONFIG_FILE_PATH)) {
    logMessage("Migrating legacy configuration from /config.json");
    if (loadConfigFromPath(LEGACY_CONFIG_FILE_PATH, "legacy",
                           CONFIG_SECTION_ALL)) {
      ioLoaded = true;
      interfaceLoaded = true;
      virtualLoaded = true;
      ioNeedsRewrite = true;
      interfaceNeedsRewrite = true;
      virtualNeedsRewrite = true;
      legacyUsed = true;
    } else {
      logMessage("Legacy configuration parse failed");
    }
  }

  if (!ioLoaded) {
    logMessage("No IO configuration found; applying defaults");
  }
  if (!interfaceLoaded) {
    logMessage("No interface configuration found; applying defaults");
  }
  if (!virtualLoaded) {
    logMessage("No virtual configuration found; applying defaults");
  }

  if (legacyUsed) {
    LittleFS.remove(LEGACY_CONFIG_FILE_PATH);
  }

  if (ioLoaded && ioNeedsRewrite) {
    if (!saveIoConfig()) {
      logMessage("Failed to rewrite IO configuration");
    }
  } else if (!ioLoaded) {
    if (!saveIoConfig()) {
      logMessage("Failed to save default IO configuration");
    }
  }

  if (interfaceLoaded && interfaceNeedsRewrite) {
    if (!saveInterfaceConfig()) {
      logMessage("Failed to rewrite interface configuration");
    }
  } else if (!interfaceLoaded) {
    if (!saveInterfaceConfig()) {
      logMessage("Failed to save default interface configuration");
    }
  }

  if (virtualLoaded && virtualNeedsRewrite) {
    if (!saveVirtualConfig()) {
      logMessage("Failed to rewrite virtual configuration");
    }
  } else if (!virtualLoaded) {
    if (!saveVirtualConfig()) {
      logMessage("Failed to save default virtual configuration");
    }
  }
}

// Save configuration sections to LittleFS.  Each helper persists a subset
// of the global configuration to its dedicated JSON file inside /private.
// Call saveIoConfig(), saveInterfaceConfig() or saveVirtualConfig() after
// modifying the respective portion of the global config.
static uint32_t crc32ForBuffer(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint32_t>(data[i]);
    for (int bit = 0; bit < 8; ++bit) {
      if (crc & 1u) {
        crc = (crc >> 1) ^ 0xEDB88320u;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

static uint16_t readUint16Le(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

static uint32_t readUint32Le(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

static bool writeUint16Le(File &f, uint16_t value) {
  uint8_t buf[2];
  buf[0] = static_cast<uint8_t>(value & 0xFFu);
  buf[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  return f.write(buf, sizeof(buf)) == sizeof(buf);
}

static bool writeUint32Le(File &f, uint32_t value) {
  uint8_t buf[4];
  buf[0] = static_cast<uint8_t>(value & 0xFFu);
  buf[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  buf[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  buf[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
  return f.write(buf, sizeof(buf)) == sizeof(buf);
}

static bool tryDecodeConfigRecord(const uint8_t *data,
                                  size_t length,
                                  ConfigRecordMetadata &meta,
                                  const char *label,
                                  bool &isRecord) {
  isRecord = false;
  if (length < CONFIG_RECORD_HEADER_SIZE) {
    return true; // Not enough bytes to be a record; treat as plain JSON
  }
  uint32_t magic = readUint32Le(data);
  if (magic != CONFIG_RECORD_MAGIC) {
    return true; // Legacy JSON payload
  }
  isRecord = true;
  meta.version = readUint16Le(data + 4);
  meta.sections = readUint16Le(data + 6);
  meta.payloadLength = readUint32Le(data + 8);
  meta.checksum = readUint32Le(data + 12);
  if (meta.version != CONFIG_RECORD_VERSION) {
    logPrintf("Unsupported %s config record version %u", label,
              static_cast<unsigned>(meta.version));
    return false;
  }
  if (meta.payloadLength > (length - CONFIG_RECORD_HEADER_SIZE)) {
    logPrintf(
        "%s config record claims %u bytes but only %u available; refusing", label,
        static_cast<unsigned>(meta.payloadLength),
        static_cast<unsigned>(length - CONFIG_RECORD_HEADER_SIZE));
    return false;
  }
  return true;
}

static bool saveJsonConfig(uint8_t sections,
                           const char *path,
                           const char *label,
                           bool logToFile = false) {
  logConfigSummary(label);
  String payload;
  if (!buildConfigJsonPayload(payload, sections, false, label, false)) {
    return false;
  }
  if (!ensureUserDirectory()) {
    logMessage(String(label) + " config directory unavailable");
    return false;
  }
  if (!writeTextFile(path, payload)) {
    logPrintf("Failed to write %s configuration to %s", label, path);
    if (logToFile) {
      appendConfigSaveLog(String("Erreur lors de l'écriture de ") + path);
    }
    return false;
  }
  if (logToFile) {
    appendConfigSaveLog(String("Configuration ") + label + " enregistrée dans " + path);
  }
  logMessage(String(label) + " configuration saved");
  return true;
}

bool saveIoConfig() {
  return saveJsonConfig(CONFIG_SECTION_MODULES | CONFIG_SECTION_IO,
                       IO_CONFIG_FILE_PATH, "IO", true);
}

bool saveInterfaceConfig() {
  return saveJsonConfig(CONFIG_SECTION_INTERFACE | CONFIG_SECTION_PEERS,
                       INTERFACE_CONFIG_FILE_PATH, "Interface");
}

bool saveVirtualConfig() {
  return saveJsonConfig(CONFIG_SECTION_VIRTUAL,
                       VIRTUAL_CONFIG_FILE_PATH, "Virtual");
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

static bool verifyConfigStored(const Config &expected,
                               const char *path,
                               uint8_t sections,
                               String &errorDetail) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    errorDetail = String("open failed: ") + path;
    return false;
  }
  size_t size = f.size();
  std::unique_ptr<uint8_t[]> raw(new uint8_t[size > 0 ? size : 1]);
  size_t read = f.read(raw.get(), size);
  f.close();
  ConfigRecordMetadata meta;
  bool isRecord = false;
  if (!tryDecodeConfigRecord(raw.get(), read, meta, path, isRecord)) {
    errorDetail = "record_header_invalid";
    return false;
  }

  const uint8_t *jsonPtr = raw.get();
  size_t jsonLength = read;
  if (isRecord) {
    jsonPtr = raw.get() + CONFIG_RECORD_HEADER_SIZE;
    jsonLength = meta.payloadLength;
    uint32_t actualChecksum = crc32ForBuffer(jsonPtr, jsonLength);
    if (actualChecksum != meta.checksum) {
      errorDetail = "checksum_mismatch";
      logPrintf(
          "%s verification checksum mismatch: stored=%08X computed=%08X", path,
          static_cast<unsigned>(meta.checksum),
          static_cast<unsigned>(actualChecksum));
      return false;
    }
  }

  std::unique_ptr<char[]> buf(new char[jsonLength + 1]);
  if (jsonLength > 0) {
    memcpy(buf.get(), jsonPtr, jsonLength);
  }
  buf[jsonLength] = '\0';

  size_t docCapacity = configJsonCapacityForPayload(jsonLength);
  while (true) {
    DynamicJsonDocument doc(docCapacity);
    DeserializationError err = deserializeJson(doc, buf.get(), jsonLength);
    if (err == DeserializationError::NoMemory &&
        docCapacity < CONFIG_JSON_MAX_CAPACITY) {
      size_t nextCapacity = growConfigJsonCapacity(docCapacity);
      if (nextCapacity == docCapacity) {
        errorDetail = String("json parse failed: ") + err.c_str();
        String context = String(path) + " verify";
        String payload(buf.get());
        logJsonParseFailure(context.c_str(), payload, docCapacity, err);
        return false;
      }
      logPrintf("Verification JSON for %s exceeded %u bytes; retrying with %u",
                path, static_cast<unsigned>(docCapacity),
                static_cast<unsigned>(nextCapacity));
      docCapacity = nextCapacity;
      continue;
    }
    if (err) {
      errorDetail = String("json parse failed: ") + err.c_str();
      String context = String(path) + " verify";
      String payload(buf.get());
      logJsonParseFailure(context.c_str(), payload, docCapacity, err);
      return false;
    }
    Config reloaded = expected;
    parseConfigFromJson(doc, reloaded, sections);
    if (sections & CONFIG_SECTION_MODULES) {
      if (expected.modules.ads1115 != reloaded.modules.ads1115 ||
          expected.modules.pwm010 != reloaded.modules.pwm010 ||
          expected.modules.zmpt != reloaded.modules.zmpt ||
          expected.modules.zmct != reloaded.modules.zmct ||
          expected.modules.div != reloaded.modules.div ||
          expected.modules.mcp4725 != reloaded.modules.mcp4725) {
        errorDetail = "module_flags_mismatch";
        return false;
      }
    }
    if (sections & CONFIG_SECTION_IO) {
      if (!inputsMatch(expected, reloaded, errorDetail)) {
        return false;
      }
      if (!outputsMatch(expected, reloaded, errorDetail)) {
        return false;
      }
    }
    if (sections & CONFIG_SECTION_INTERFACE) {
      if (expected.nodeId != reloaded.nodeId) {
        errorDetail = "nodeId mismatch";
        return false;
      }
      if (expected.wifi.mode != reloaded.wifi.mode ||
          expected.wifi.ssid != reloaded.wifi.ssid ||
          expected.wifi.pass != reloaded.wifi.pass) {
        errorDetail = "wifi mismatch";
        return false;
      }
    }
    if (sections & CONFIG_SECTION_PEERS) {
      if (expected.peerCount != reloaded.peerCount) {
        errorDetail = "peerCount mismatch";
        return false;
      }
      for (uint8_t i = 0; i < expected.peerCount && i < MAX_PEERS; ++i) {
        if (expected.peers[i].nodeId != reloaded.peers[i].nodeId ||
            expected.peers[i].pin != reloaded.peers[i].pin) {
          errorDetail = "peer mismatch";
          return false;
        }
      }
    }
    if (sections & CONFIG_SECTION_VIRTUAL) {
      if (expected.virtualMultimeter.channelCount !=
          reloaded.virtualMultimeter.channelCount) {
        errorDetail = "virtualMultimeter mismatch";
        return false;
      }
    }
    logPrintf("Configuration verification succeeded for %s", path);
    return true;
  }
}

template <typename ServerT>
static void handleIoConfigSetRequest(ServerT *srv, const String &body) {
  appendConfigSaveLog("--- Début de sauvegarde de configuration IO ---");
  if (body.length() == 0) {
    appendConfigSaveLog("Erreur : corps de requête vide");
    srv->send(400, "application/json", R"({"error":"No body"})");
    return;
  }
  appendConfigSaveLog(String("Requête reçue (") + body.length() + " octets)");
  size_t docCapacity = configJsonCapacityForPayload(body.length());
  if (docCapacity == 0) {
    docCapacity = configJsonCapacityForPayload(0);
  }
  DynamicJsonDocument doc(docCapacity);
  DeserializationError parseErr = deserializeJson(doc, body);
  if (parseErr == DeserializationError::NoMemory) {
    appendConfigSaveLog("Erreur : charge utile trop volumineuse");
    srv->send(400, "application/json",
              R"({"error":"payload_too_large"})");
    return;
  }
  if (parseErr) {
    appendConfigSaveLog(String("Erreur de parsing JSON: ") + parseErr.c_str());
    DynamicJsonDocument errDoc(256);
    errDoc["error"] = "invalid_json";
    errDoc["detail"] = parseErr.c_str();
    String errPayload;
    serializeJson(errDoc, errPayload);
    srv->send(400, "application/json", errPayload);
    return;
  }
  if (doc.overflowed()) {
    appendConfigSaveLog("Erreur : mémoire insuffisante pour le JSON reçu");
    srv->send(400, "application/json",
              R"({"error":"json_overflow"})");
    return;
  }
  appendConfigSaveLog("JSON analysé avec succès");
  Config previousConfig = config;
  parseConfigFromJson(doc, config, &previousConfig, false,
                      CONFIG_SECTION_MODULES | CONFIG_SECTION_IO);
  appendConfigSaveLog("Configuration appliquée en mémoire");
  if (!saveIoConfig()) {
    appendConfigSaveLog("Erreur : écriture du fichier io_config.json");
    config = previousConfig;
    srv->send(500, "application/json",
              R"({"error":"save_failed"})");
    return;
  }
  appendConfigSaveLog("Sauvegarde terminée avec succès");
  DynamicJsonDocument respDoc(256);
  respDoc["status"] = "ok";
  respDoc["message"] = "Fichier enregistré.";
  respDoc["requiresReboot"] = true;
  String payload;
  serializeJson(respDoc, payload);
  srv->send(200, "application/json", payload);
  appendConfigSaveLog("Réponse envoyée au client");
  appendConfigSaveLog("--- Fin de sauvegarde de configuration IO ---");
}

template <typename ServerT>
static void handleInterfaceConfigSetRequest(ServerT *srv, const String &body) {
  if (body.length() == 0) {
    srv->send(400, "application/json", R"({"error":"No body"})");
    return;
  }
  size_t docCapacity = configJsonCapacityForPayload(body.length());
  while (true) {
    DynamicJsonDocument doc(docCapacity);
    DeserializationError parseErr = deserializeJson(doc, body);
    if (parseErr == DeserializationError::NoMemory &&
        docCapacity < CONFIG_JSON_MAX_CAPACITY) {
      size_t nextCapacity = growConfigJsonCapacity(docCapacity);
      if (nextCapacity == docCapacity) {
        logJsonParseFailure("Interface configuration", body, docCapacity,
                            parseErr);
        DynamicJsonDocument errDoc(256);
        errDoc["error"] = "invalid_json";
        errDoc["detail"] = parseErr.c_str();
        errDoc["bytes"] = static_cast<uint32_t>(body.length());
        errDoc["capacity"] = static_cast<uint32_t>(docCapacity);
        errDoc["hint"] = "payload_too_large";
        String errPayload;
        serializeJson(errDoc, errPayload);
        srv->send(400, "application/json", errPayload);
        return;
      }
      logPrintf(
          "Interface configuration JSON parse exceeded %u bytes; retrying with %u",
          static_cast<unsigned>(docCapacity),
          static_cast<unsigned>(nextCapacity));
      docCapacity = nextCapacity;
      continue;
    }
    if (parseErr) {
      logJsonParseFailure("Interface configuration", body, docCapacity,
                          parseErr);
      DynamicJsonDocument errDoc(256);
      errDoc["error"] = "invalid_json";
      errDoc["detail"] = parseErr.c_str();
      errDoc["bytes"] = static_cast<uint32_t>(body.length());
      errDoc["capacity"] = static_cast<uint32_t>(docCapacity);
      if (parseErr == DeserializationError::NoMemory) {
        errDoc["hint"] = "payload_too_large";
      }
      String errPayload;
      serializeJson(errDoc, errPayload);
      srv->send(400, "application/json", errPayload);
      return;
    }
    logPrintf("Interface configuration update received (%u bytes)",
              static_cast<unsigned>(body.length()));
    logConfigJson("Received interface", doc);
    Config previousConfig = config;
    parseConfigFromJson(doc, config, &previousConfig, false,
                        CONFIG_SECTION_INTERFACE | CONFIG_SECTION_PEERS);
    if (!saveInterfaceConfig()) {
      config = previousConfig;
      logMessage(
          "Interface configuration update failed to save; changes reverted");
      srv->send(500, "application/json", R"({"error":"save_failed"})");
      return;
    }
    String verifyError;
    if (!verifyConfigStored(config, INTERFACE_CONFIG_FILE_PATH,
                            CONFIG_SECTION_INTERFACE | CONFIG_SECTION_PEERS,
                            verifyError)) {
      logPrintf("Interface configuration verification failed: %s",
                verifyError.c_str());
      config = previousConfig;
      if (!saveInterfaceConfig()) {
        logMessage(
            "Failed to restore interface configuration after verification failure");
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
    logConfigSummary("Applied interface");
    logMessage("Interface configuration update saved; rebooting");
    DynamicJsonDocument respDoc(1024);
    respDoc["status"] = "ok";
    respDoc["verified"] = true;
    respDoc["nodeId"] = config.nodeId;
    JsonObject wifiObj = respDoc.createNestedObject("wifi");
    wifiObj["mode"] = config.wifi.mode;
    wifiObj["ssid"] = config.wifi.ssid;
    wifiObj["pass"] = config.wifi.pass;
    JsonArray peersArr = respDoc.createNestedArray("peers");
    for (uint8_t i = 0; i < config.peerCount && i < MAX_PEERS; ++i) {
      JsonObject peer = peersArr.createNestedObject();
      peer["nodeId"] = config.peers[i].nodeId;
      peer["pin"] = config.peers[i].pin;
    }
    String payload;
    serializeJson(respDoc, payload);
    srv->send(200, "application/json", payload);
    delay(100);
    ESP.restart();
    return;
  }
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
    String payload;
    if (!buildConfigJsonPayload(payload, CONFIG_SECTION_ALL, true,
                                "API config get", false)) {
      srv->send(500, "application/json",
                R"({"error":"encode_failed"})");
      return;
    }
    srv->send(200, "application/json", payload);
  });

  server.on("/api/config/io/get", HTTP_GET, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String payload;
    if (!buildConfigJsonPayload(payload,
                                CONFIG_SECTION_MODULES | CONFIG_SECTION_IO,
                                false, "API IO config get", false,
                                appendIoConfigMetadata)) {
      srv->send(500, "application/json",
                R"({"error":"encode_failed"})");
      return;
    }
    srv->send(200, "application/json", payload);
  });

  server.on("/api/config/io/set", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    handleIoConfigSetRequest(srv, body);
  });

  server.on("/api/config/set", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    handleIoConfigSetRequest(srv, body);
  });

  server.on("/api/config/interface/get", HTTP_GET, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String payload;
    if (!buildConfigJsonPayload(
            payload, CONFIG_SECTION_INTERFACE | CONFIG_SECTION_PEERS, true,
            "API interface config get", false)) {
      srv->send(500, "application/json",
                R"({"error":"encode_failed"})");
      return;
    }
    srv->send(200, "application/json", payload);
  });

  server.on("/api/config/interface/set", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    handleInterfaceConfigSetRequest(srv, body);
  });

  server.on("/api/config/virtual-multimeter", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"No body"})");
      return;
    }
    size_t docCapacity = configJsonCapacityForPayload(body.length());
    if (docCapacity == 0) {
      docCapacity = configJsonCapacityForPayload(0);
    }
    while (true) {
      DynamicJsonDocument doc(docCapacity);
      if (docCapacity > 0 && doc.capacity() == 0) {
        srv->send(500, "application/json",
                  R"({"error":"alloc_failed"})");
        return;
      }
      DeserializationError parseErr = deserializeJson(doc, body);
      if (parseErr == DeserializationError::NoMemory &&
          docCapacity < CONFIG_JSON_MAX_CAPACITY) {
        size_t nextCapacity = growConfigJsonCapacity(docCapacity);
        if (nextCapacity == docCapacity) {
          srv->send(400, "application/json",
                    R"({"error":"Invalid JSON"})");
          return;
        }
        docCapacity = nextCapacity;
        continue;
      }
      if (parseErr) {
        srv->send(400, "application/json",
                  R"({"error":"Invalid JSON"})");
        return;
      }
      JsonVariantConst channelsVariant;
      if (doc.containsKey("channels")) {
        channelsVariant = doc["channels"];
      } else if (doc.containsKey("virtualMultimeter")) {
        channelsVariant = doc["virtualMultimeter"];
      } else {
        channelsVariant = doc.as<JsonVariantConst>();
      }
      VirtualMultimeterConfig newConfig;
      clearVirtualMultimeterConfig(newConfig);
      bool parsedChannels = false;
      if (!channelsVariant.isNull()) {
        JsonArrayConst arr = channelsVariant.as<JsonArrayConst>();
        if (!arr.isNull()) {
          parseMeterChannelArray(arr, newConfig);
          parsedChannels = true;
        } else {
          JsonObjectConst obj = channelsVariant.as<JsonObjectConst>();
          if (!obj.isNull()) {
            parseMeterChannelObject(obj, newConfig);
            parsedChannels = true;
          }
        }
      } else {
        parsedChannels = true;
      }
      if (!parsedChannels) {
        srv->send(400, "application/json",
                  R"({"error":"invalid_channels"})");
        return;
      }
      Config previousConfig = config;
      config.virtualMultimeter = newConfig;
      if (!saveVirtualConfig()) {
        config.virtualMultimeter = previousConfig.virtualMultimeter;
        srv->send(500, "application/json",
                  R"({"error":"save_failed"})");
        return;
      }
      String verifyError;
      if (!verifyConfigStored(config, VIRTUAL_CONFIG_FILE_PATH,
                              CONFIG_SECTION_VIRTUAL, verifyError)) {
        logPrintf("Virtual multimeter verification failed: %s",
                  verifyError.c_str());
        config.virtualMultimeter = previousConfig.virtualMultimeter;
        if (!saveVirtualConfig()) {
          logMessage(
              "Failed to restore virtual configuration after verification failure");
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
      logPrintf("Virtual multimeter configuration updated (%u channels)",
                static_cast<unsigned>(config.virtualMultimeter.channelCount));
      DynamicJsonDocument resp(1024);
      resp["status"] = "ok";
      JsonObject applied = resp.createNestedObject("applied");
      populateVirtualMultimeterJson(applied, config.virtualMultimeter);
      String payload;
      serializeJson(resp, payload);
      srv->send(200, "application/json", payload);
      return;
    }
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
    saveIoConfig();
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
    saveInterfaceConfig();
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

  server.on("/api/logs/append", HTTP_POST, [&server]() {
    auto *srv = &server;
    if (!requireAuth(srv)) return;
    String body = srv->hasArg("plain") ? srv->arg("plain") : String();
    if (body.length() == 0) {
      srv->send(400, "application/json", R"({"error":"empty_body"})");
      return;
    }
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
      DynamicJsonDocument errDoc(192);
      errDoc["error"] = "invalid_json";
      errDoc["detail"] = err.c_str();
      String payload;
      serializeJson(errDoc, payload);
      srv->send(400, "application/json", payload);
      return;
    }
    String source = doc["source"].as<String>();
    if (source.length() == 0) {
      source = F("client");
    }
    String eventType = doc["event"].as<String>();
    if (eventType.length() == 0) {
      eventType = F("message");
    }
    String message = doc["message"].as<String>();
    uint32_t step = doc["step"] | 0;
    JsonVariantConst detailVar = doc["detail"];
    String detailText = summariseLogDetail(detailVar);
    JsonVariantConst sessionVar = doc["session"];
    String sessionKind;
    String sessionTitle;
    if (!sessionVar.isNull()) {
      sessionKind = sessionVar["kind"].as<String>();
      sessionTitle = sessionVar["title"].as<String>();
    }
    String line;
    line.reserve(128 + detailText.length());
    line += '[';
    line += source;
    if (sessionKind.length() > 0) {
      line += '/';
      line += sessionKind;
    }
    line += "] ";
    if (sessionTitle.length() > 0) {
      line += sessionTitle;
      if (step > 0) {
        line += F(" - etape ");
        line += step;
      }
      line += F(" : ");
    } else if (step > 0) {
      line += F("Etape ");
      line += step;
      line += F(" : ");
    }
    if (message.length() > 0) {
      line += message;
    } else {
      line += F("evenement ");
      line += eventType;
    }
    if (detailText.length() > 0) {
      line += F(" | ");
      line += detailText;
    }
    logMessage(line);
    srv->send(200, "application/json", R"({"status":"ok"})");
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
