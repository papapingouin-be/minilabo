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
#include <ESPAsyncWebServer.h>
#include <ESPAsyncTCP.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_ADS1015.h>
#include <ESP8266mDNS.h>

// ---------------------------------------------------------------------------
// Simple logging facility.  Log messages are written to Serial and appended
// to a file on LittleFS.  The log file is truncated on each boot.  Content
// can be retrieved over HTTP for display in the UI.
// ---------------------------------------------------------------------------
static const char *LOG_PATH = "/log.txt";

static void initLogging() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed, formatting...");
    LittleFS.format();
    LittleFS.begin();
  }
  LittleFS.remove(LOG_PATH);
}

static void logMessage(const String &msg) {
  Serial.println(msg);
  File f = LittleFS.open(LOG_PATH, "a");
  if (f) {
    f.println(msg);
    f.close();
  }
}

static void logPrintf(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  logMessage(String(buf));
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
  OUTPUT_GPIO
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
  return OUTPUT_DISABLED;
}

// Convert an output type enumeration back to a string.
static String outputTypeToString(OutputType t) {
  switch (t) {
    case OUTPUT_PWM010: return "pwm010";
    case OUTPUT_GPIO:   return "gpio";
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
  float      scale;        // Gain for mapping physical value to duty (0–1023)
  float      offset;       // Offset added to duty after scaling
  bool       active;       // Whether this output is enabled
  float      value;        // Last commanded value
};

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
};

static Config config;            // Global configuration instance
static AsyncWebServer server(80); // HTTP server instance
static WiFiUDP udp;               // UDP object for broadcasting and listening
static Adafruit_ADS1115 ads;      // ADS1115 ADC instance

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

// Timing variables for input sampling and broadcast.  These are updated
// in loop() to ensure that tasks run at their configured intervals.
static uint32_t lastInputUpdate    = 0;
static uint32_t lastBroadcastUpdate = 0;

// Forward declarations
void loadConfig();
void saveConfig();
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
void parseConfigFromJson(const JsonDocument &doc);

// Utility to convert an ESP8266 pin string ("D2", "A0") to the
// numeric constant.  Returns -1 if the string is not recognised.
int parsePin(const String &p) {
  if (p.length() == 0) return -1;
  String s = p;
  s.toUpperCase();
  if (s == "A0") return A0;
  if (s.startsWith("D")) {
    int n = s.substring(1).toInt();
    switch (n) {
      case 0: return D0;
      case 1: return D1;
      case 2: return D2;
      case 3: return D3;
      case 4: return D4;
      case 5: return D5;
      case 6: return D6;
      case 7: return D7;
      case 8: return D8;
      case 9: return D9;
      case 10: return D10;
      default: return -1;
    }
  }
  return -1;
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

  // Default inputs: use meaningful channel names (IN1, IN2…) instead of generic "inputX".
  // Start with one ADC on A0 and leave remaining channels disabled.  Names can be changed via the UI.
  config.inputCount = 2;
  config.inputs[0] = {"IN1", INPUT_ADC, A0, -1, "", "", 1.0f, 0.0f, "", true, 0.0f};
  config.inputs[1] = {"IN2", INPUT_DISABLED, -1, -1, "", "", 1.0f, 0.0f, "", false, 0.0f};
  // Clear other potential entries with professional default names
  for (uint8_t i = 2; i < MAX_INPUTS; i++) {
    config.inputs[i] = {String("IN") + String(i+1), INPUT_DISABLED, -1, -1, "", "", 1.0f, 0.0f, "", false, 0.0f};
  }

  // Default output: one PWM on D2.  Additional outputs are disabled.  Use professional names (OUT1, OUT2…).
  config.outputCount = 1;
  config.outputs[0] = {"OUT1", OUTPUT_PWM010, D2, 2000, 1.0f, 0.0f, true, 0.0f};
  for (uint8_t i = 1; i < MAX_OUTPUTS; i++) {
    config.outputs[i] = {String("OUT") + String(i+1), OUTPUT_DISABLED, -1, 2000, 1.0f, 0.0f, false, 0.0f};
  }
}

// Load configuration from LittleFS.  If loading fails (no file or parse
// error) the default configuration is applied and saved back to
// LittleFS.  JSON fields missing from the file fall back to defaults.
void loadConfig() {
  if (!LittleFS.begin()) {
    logMessage("LittleFS mount failed, formatting...");
    LittleFS.format();
    LittleFS.begin();
  }
  if (!LittleFS.exists("/config.json")) {
    logMessage("No config file found, applying defaults");
    setDefaultConfig();
    saveConfig();
    return;
  }
  File f = LittleFS.open("/config.json", "r");
  if (!f) {
    logMessage("Failed to open config file, applying defaults");
    setDefaultConfig();
    saveConfig();
    return;
  }
  size_t size = f.size();
  std::unique_ptr<char[]> buf(new char[size + 1]);
  f.readBytes(buf.get(), size);
  buf[size] = '\0';
  DynamicJsonDocument doc(4096);
  auto err = deserializeJson(doc, buf.get());
  if (err) {
    logMessage("Failed to parse config JSON, applying defaults");
    setDefaultConfig();
    saveConfig();
    return;
  }
  parseConfigFromJson(doc);
  f.close();
  logMessage("Configuration loaded");
}

// Save the current configuration to LittleFS.  If writing fails the
// operation is silently ignored.  Always call saveConfig() after
// modifying the global config.
void saveConfig() {
  DynamicJsonDocument doc(4096);
  // Populate JSON document
  doc["nodeId"] = config.nodeId;
  JsonObject wifiObj = doc.createNestedObject("wifi");
  wifiObj["mode"] = config.wifi.mode;
  wifiObj["ssid"] = config.wifi.ssid;
  wifiObj["pass"] = config.wifi.pass;
  JsonObject modObj = doc.createNestedObject("modules");
  modObj["ads1115"] = config.modules.ads1115;
  modObj["pwm010"]  = config.modules.pwm010;
  modObj["zmpt"]    = config.modules.zmpt;
  modObj["zmct"]    = config.modules.zmct;
  modObj["div"]     = config.modules.div;
  doc["inputCount"]  = config.inputCount;
  JsonArray inputsArr = doc.createNestedArray("inputs");
  for (uint8_t i = 0; i < config.inputCount && i < MAX_INPUTS; i++) {
    JsonObject o = inputsArr.createNestedObject();
    const InputConfig &ic = config.inputs[i];
    o["name"]       = ic.name;
    o["type"]       = inputTypeToString(ic.type);
    o["pin"]        = pinToString(ic.pin);
    o["adsChannel"] = ic.adsChannel;
    o["remoteNode"] = ic.remoteNode;
    o["remoteName"] = ic.remoteName;
    o["scale"]      = ic.scale;
    o["offset"]     = ic.offset;
    o["unit"]       = ic.unit;
    o["active"]     = ic.active;
  }
  doc["outputCount"] = config.outputCount;
  JsonArray outputsArr = doc.createNestedArray("outputs");
  for (uint8_t i = 0; i < config.outputCount && i < MAX_OUTPUTS; i++) {
    JsonObject o = outputsArr.createNestedObject();
    const OutputConfig &oc = config.outputs[i];
    o["name"]   = oc.name;
    o["type"]   = outputTypeToString(oc.type);
    o["pin"]    = pinToString(oc.pin);
    o["pwmFreq"] = oc.pwmFreq;
    o["scale"]  = oc.scale;
    o["offset"] = oc.offset;
    o["active"] = oc.active;
  }
  // Write file
  File f = LittleFS.open("/config.json", "w");
  if (!f) {
    logMessage("Failed to open config for writing");
    return;
  }
  serializeJson(doc, f);
  f.close();
  logMessage("Configuration saved");
}

// Parse a configuration from a JSON document.  This helper reads
// optional fields safely, falling back to existing values when keys
// are missing.  Unknown types are ignored.  String keys are case-
// insensitive for the type fields.
void parseConfigFromJson(const JsonDocument &doc) {
  // nodeId
  if (doc.containsKey("nodeId")) {
    config.nodeId = doc["nodeId"].as<String>();
  }
  // wifi
  if (doc.containsKey("wifi")) {
    JsonObjectConst w = doc["wifi"].as<JsonObjectConst>();
    if (w.containsKey("mode")) config.wifi.mode = w["mode"].as<String>();
    if (w.containsKey("ssid")) config.wifi.ssid = w["ssid"].as<String>();
    if (w.containsKey("pass")) config.wifi.pass = w["pass"].as<String>();
  }
  // modules
  if (doc.containsKey("modules")) {
    JsonObjectConst m = doc["modules"].as<JsonObjectConst>();
    config.modules.ads1115 = m["ads1115"].as<bool>();
    config.modules.pwm010  = m["pwm010"].as<bool>();
    config.modules.zmpt    = m["zmpt"].as<bool>();
    config.modules.zmct    = m["zmct"].as<bool>();
    config.modules.div     = m["div"].as<bool>();
  }
  // inputs
  uint8_t idx = 0;
  if (doc.containsKey("inputs") && doc["inputs"].is<JsonArray>()) {
    JsonArrayConst arr = doc["inputs"].as<JsonArrayConst>();
    config.inputCount = min((uint8_t)arr.size(), MAX_INPUTS);
    for (JsonObjectConst o : arr) {
      if (idx >= MAX_INPUTS) break;
      InputConfig &ic = config.inputs[idx];
      if (o.containsKey("name")) ic.name = o["name"].as<String>();
      if (o.containsKey("type")) ic.type = parseInputType(o["type"].as<String>());
      if (o.containsKey("pin"))  ic.pin = parsePin(o["pin"].as<String>());
      if (o.containsKey("adsChannel")) ic.adsChannel = o["adsChannel"].as<int>();
      if (o.containsKey("remoteNode")) ic.remoteNode = o["remoteNode"].as<String>();
      if (o.containsKey("remoteName")) ic.remoteName = o["remoteName"].as<String>();
      if (o.containsKey("scale")) ic.scale = o["scale"].as<float>();
      if (o.containsKey("offset")) ic.offset = o["offset"].as<float>();
      if (o.containsKey("unit")) ic.unit = o["unit"].as<String>();
      if (o.containsKey("active")) ic.active = o["active"].as<bool>();
      idx++;
    }
  }
  // fill remaining inputs as disabled
  for (uint8_t i = idx; i < MAX_INPUTS; i++) {
    // Use professional names for unspecified inputs
    config.inputs[i] = {String("IN") + String(i+1), INPUT_DISABLED, -1, -1, "", "", 1.0f, 0.0f, "", false, 0.0f};
  }
  // outputs
  idx = 0;
  if (doc.containsKey("outputs") && doc["outputs"].is<JsonArray>()) {
    JsonArrayConst arr = doc["outputs"].as<JsonArrayConst>();
    config.outputCount = min((uint8_t)arr.size(), MAX_OUTPUTS);
    for (JsonObjectConst o : arr) {
      if (idx >= MAX_OUTPUTS) break;
      OutputConfig &oc = config.outputs[idx];
      if (o.containsKey("name")) oc.name = o["name"].as<String>();
      if (o.containsKey("type")) oc.type = parseOutputType(o["type"].as<String>());
      if (o.containsKey("pin"))  oc.pin = parsePin(o["pin"].as<String>());
      if (o.containsKey("pwmFreq")) oc.pwmFreq = o["pwmFreq"].as<int>();
      if (o.containsKey("scale")) oc.scale = o["scale"].as<float>();
      if (o.containsKey("offset")) oc.offset = o["offset"].as<float>();
      if (o.containsKey("active")) oc.active = o["active"].as<bool>();
      oc.value = 0.0f;
      idx++;
    }
  }
  for (uint8_t i = idx; i < MAX_OUTPUTS; i++) {
    config.outputs[i] = {String("OUT") + String(i+1), OUTPUT_DISABLED, -1, 2000, 1.0f, 0.0f, false, 0.0f};
  }
}

// Initialise Wi-Fi according to the configuration.  If STA mode is
// requested but connection fails within a timeout, the ESP8266 falls
// back to AP mode.  Once connected, an mDNS hostname is published
// allowing access via http://nodeId.local/ on the same network.
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
      logMessage("Failed to connect, starting AP");
      WiFi.mode(WIFI_AP);
      WiFi.softAP(config.nodeId.c_str(), config.wifi.pass.c_str());
      config.wifi.mode = "AP";
      config.wifi.ssid = config.nodeId;
      config.wifi.pass = "";
      saveConfig();
    }
  } else {
    logMessage("Starting in Access Point mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(config.nodeId.c_str(), config.wifi.pass.c_str());
  }
  IPAddress ip = (WiFi.getMode() == WIFI_STA) ? WiFi.localIP() : WiFi.softAPIP();
  logPrintf("IP address: %s", ip.toString().c_str());
  // Start mDNS if possible
  if (MDNS.begin(config.nodeId.c_str())) {
    logMessage("mDNS responder started");
    MDNS.addService("http", "tcp", 80);
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
          String remoteId = doc["node"].as<String>();
          // Ignore our own broadcasts
          if (remoteId.length() > 0 && remoteId != config.nodeId) {
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
void setupServer() {
  // Serve the root page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/index.html", "text/html");
  });
  // Serve other static files (config.html, example.html, etc.)
  server.serveStatic("/", LittleFS, "/");

  // API: return current configuration
  server.on("/api/config/get", HTTP_GET, [](AsyncWebServerRequest *req) {
    DynamicJsonDocument doc(2048);
    doc["nodeId"] = config.nodeId;
    JsonObject wifiObj = doc.createNestedObject("wifi");
    wifiObj["mode"] = config.wifi.mode;
    wifiObj["ssid"] = config.wifi.ssid;
    wifiObj["pass"] = config.wifi.pass;
    JsonObject modObj = doc.createNestedObject("modules");
    modObj["ads1115"] = config.modules.ads1115;
    modObj["pwm010"]  = config.modules.pwm010;
    modObj["zmpt"]    = config.modules.zmpt;
    modObj["zmct"]    = config.modules.zmct;
    modObj["div"]     = config.modules.div;
    doc["inputCount"] = config.inputCount;
    JsonArray inArr = doc.createNestedArray("inputs");
    for (uint8_t i = 0; i < config.inputCount && i < MAX_INPUTS; i++) {
      JsonObject o = inArr.createNestedObject();
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
    JsonArray outArr = doc.createNestedArray("outputs");
    for (uint8_t i = 0; i < config.outputCount && i < MAX_OUTPUTS; i++) {
      JsonObject o = outArr.createNestedObject();
      const OutputConfig &oc = config.outputs[i];
      o["name"] = oc.name;
      o["type"] = outputTypeToString(oc.type);
      o["pin"] = pinToString(oc.pin);
      o["pwmFreq"] = oc.pwmFreq;
      o["scale"] = oc.scale;
      o["offset"] = oc.offset;
      o["active"] = oc.active;
    }
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
  });

  // API: set configuration.  Expects a JSON body.  After saving
  // configuration a reboot is performed to apply changes.
  server.on("/api/config/set", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("plain", true)) {
      req->send(400, "application/json", "{\"error\":\"No body\"}");
      return;
    }
    String body = req->getParam("plain", true)->value();
    DynamicJsonDocument doc(4096);
    auto err = deserializeJson(doc, body);
    if (err) {
      req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }
    // Update global config from JSON
    parseConfigFromJson(doc);
    saveConfig();
    req->send(200, "application/json", "{\"status\":\"ok\"}");
    // Delay slightly to ensure response is sent before rebooting
    delay(200);
    ESP.restart();
  });

  // API: return current input values
  server.on("/api/inputs", HTTP_GET, [](AsyncWebServerRequest *req) {
    DynamicJsonDocument doc(1024);
    JsonObject obj = doc.to<JsonObject>();
    for (uint8_t i = 0; i < config.inputCount && i < MAX_INPUTS; i++) {
      const InputConfig &ic = config.inputs[i];
      if (ic.active) {
        obj[ic.name] = isnan(ic.value) ? 0 : ic.value;
      }
    }
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
  });

  // API: return current output values
  server.on("/api/outputs", HTTP_GET, [](AsyncWebServerRequest *req) {
    DynamicJsonDocument doc(1024);
    JsonObject obj = doc.to<JsonObject>();
    for (uint8_t i = 0; i < config.outputCount && i < MAX_OUTPUTS; i++) {
      const OutputConfig &oc = config.outputs[i];
      if (oc.active) {
        obj[oc.name] = oc.value;
      }
    }
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
  });

  // API: set an output value.  Expects JSON {"name":"output1","value":x}
  server.on("/api/output/set", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("plain", true)) {
      req->send(400, "application/json", "{\"error\":\"No body\"}");
      return;
    }
    String body = req->getParam("plain", true)->value();
    DynamicJsonDocument doc(512);
    auto err = deserializeJson(doc, body);
    if (err) {
      req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }
    String name = doc["name"].as<String>();
    float val = doc["value"].as<float>();
    bool found = false;
    for (uint8_t i = 0; i < config.outputCount && i < MAX_OUTPUTS; i++) {
      if (config.outputs[i].name == name) {
        config.outputs[i].value = val;
        found = true;
        break;
      }
    }
    if (!found) {
      req->send(404, "application/json", "{\"error\":\"Unknown output\"}");
      return;
    }
    updateOutputs();
    req->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // API: return remote values
  server.on("/api/remote", HTTP_GET, [](AsyncWebServerRequest *req) {
    DynamicJsonDocument doc(1024);
    JsonObject obj = doc.to<JsonObject>();
    for (int i = 0; i < remoteCount; i++) {
      String key = remoteValues[i].nodeId + ":" + remoteValues[i].inputName;
      obj[key] = remoteValues[i].value;
    }
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
  });

  // API: return system logs
  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!LittleFS.exists(LOG_PATH)) {
      req->send(404, "text/plain", "No log");
      return;
    }
    File f = LittleFS.open(LOG_PATH, "r");
    if (!f) {
      req->send(500, "text/plain", "Failed to open log");
      return;
    }
    String content = f.readString();
    f.close();
    req->send(200, "text/plain", content);
  });

  // Start the server
  server.begin();
}

// Arduino setup entry point.  Serial is initialised for debug output.
// Configuration is loaded or initialised.  Wi-Fi, sensors and the
// server are then configured.  UDP listener starts on the broadcast
// port.  The initial set of outputs is applied.
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  initLogging();
  logMessage("MiniLabBox v2 starting...");
  loadConfig();
  setupWiFi();
  // Start UDP listener
  udp.begin(BROADCAST_PORT);
  setupSensors();
  setupServer();
  // Initialise output pins to their default state
  updateOutputs();
  lastInputUpdate = millis();
  lastBroadcastUpdate = millis();
}

// Main loop.  Inputs are sampled on a schedule.  Broadcasts are sent
// periodically.  Incoming UDP packets are processed continuously.  The
// web server runs asynchronously and does not need servicing here.
void loop() {
  unsigned long now = millis();
  if (now - lastInputUpdate >= INPUT_UPDATE_INTERVAL) {
    lastInputUpdate = now;
    updateInputs();
  }
  processUdp();
  sendBroadcast();
  // Sleep briefly to yield to background tasks
  delay(5);
}