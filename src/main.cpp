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
#include <U8g2lib.h>
#include <Updater.h>

static const char *FIRMWARE_VERSION = "1.0.0";

// ---------------------------------------------------------------------------
// TLS certificate and private key used for the HTTPS server.  The pair is a
// long-lived self-signed certificate generated specifically for MiniLabBox.
// The certificate is embedded directly in firmware so the device can expose
// HTTPS endpoints without external provisioning.
// ---------------------------------------------------------------------------
static const char TLS_CERT[] PROGMEM = R"CERT(-----BEGIN CERTIFICATE-----
MIIDvTCCAqWgAwIBAgIUSPqBltAiDymlJeyrbqMDBZOYx74wDQYJKoZIhvcNAQEL
BQAwbjELMAkGA1UEBhMCRlIxDDAKBgNVBAgMA0lERjEOMAwGA1UEBwwFUGFyaXMx
EzARBgNVBAoMCk1pbmlMYWJCb3gxETAPBgNVBAsMCEZpcm13YXJlMRkwFwYDVQQD
DBBtaW5pbGFiYm94LmxvY2FsMB4XDTI1MDkxNTIxMzExNloXDTM1MDkxMzIxMzEx
NlowbjELMAkGA1UEBhMCRlIxDDAKBgNVBAgMA0lERjEOMAwGA1UEBwwFUGFyaXMx
EzARBgNVBAoMCk1pbmlMYWJCb3gxETAPBgNVBAsMCEZpcm13YXJlMRkwFwYDVQQD
DBBtaW5pbGFiYm94LmxvY2FsMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKC
AQEAuutBAPT65XT3nABJK42vnB7LV0ncpQxwP5hBiMRfvVCAW9XyeSPJdLs9avvh
TjZ53L5BSoqro8DY5enu9jlB7LgruuPFJIML4evf6o2ngPNorj7eEwzBaW8tRpvn
9ZFNgVD+F0tbg5Q2tYXjdFNxVnG7vy4r2bklkrhlYK+ze96OKXSwXNYVODBwBZp7
fsr1EC7qyRJZ6AGhK8YkavcY1d6hAys2KYzW/eBrGqXw2w+Vs6GWrJGeoRPfF+vw
BHjwbX1jIyrwW1/VklyydvREONqVZcya0BIIjJAPa85QZTSqgoyHr+zIMntLarx/
Y2uhQbigzxRUwAYCRvuDTDRgrQIDAQABo1MwUTAdBgNVHQ4EFgQUjNnmXYq/hgNd
x0pdlYkFN+qfw2MwHwYDVR0jBBgwFoAUjNnmXYq/hgNdx0pdlYkFN+qfw2MwDwYD
VR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAczkSSc2x6pAqoc3uhy6/
GwLQMJ9pY6qx1TSCMeZZie61P3YZkeqjGXhO+mtmlhUBZ7TeZjnANdPQ1Dy0Cq0o
HCV2zQE7Eq3NSEHIo1b47RQPo/AQxWXSmhgOL9/518inT6JctIdpvUxJJTsa8OjK
f75f/x/3fz3iR1PXZ0KwcGXsb9NDqcJ2qPhkC1Pd+vx7gph96ZDeQCvZvsQKtGcw
cbdYZukPnYYEZc/6Nad3EI8r4YgeCRq20SgoLygwpK6aDgle7QgJv92heRvgK5Mt
y4+lii26ry0WK0TtSOQqZYYsJIxJM/8ap6ehq8Oo1MhkhUKkZeWzCEKWcEpqdySh
6w==
-----END CERTIFICATE-----
)CERT";

static const char TLS_KEY[] PROGMEM = R"KEY(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC660EA9PrldPec
AEkrja+cHstXSdylDHA/mEGIxF+9UIBb1fJ5I8l0uz1q++FONnncvkFKiqujwNjl
6e72OUHsuCu648Ukgwvh69/qjaeA82iuPt4TDMFpby1Gm+f1kU2BUP4XS1uDlDa1
heN0U3FWcbu/LivZuSWSuGVgr7N73o4pdLBc1hU4MHAFmnt+yvUQLurJElnoAaEr
xiRq9xjV3qEDKzYpjNb94GsapfDbD5WzoZaskZ6hE98X6/AEePBtfWMjKvBbX9WS
XLJ29EQ42pVlzJrQEgiMkA9rzlBlNKqCjIev7Mgye0tqvH9ja6FBuKDPFFTABgJG
+4NMNGCtAgMBAAECggEAJvpKRJsRW7prrOgBWhfyZAWm5PWl0XQZzyUem1jJ1yY7
kgr4BGtiRdmKue264q6o9E9nOZZXqu7au/zvhABWHzkbg14eXNoH5w1jFNwDrzfy
3w0EjafmCCizIgt+UB7D8QC076Ia/AHy86DvGLGSy5ItcrU71yvM6j7SAxq4fI6F
kULCNRIO7nXPimQUogzHloZJIinafNpjsVf7qyXx+OO/YYi3TkwDhCl99J6PR92A
aGsuLj3A18uQIBpWACfgZXBdItaa3z9TZwVlCXCxRn/ViIkUZw5gwtgNl34jFJlo
RtPZiP/nNEPwj0N/Z6vHVmtBzcl1VcDjrI4DcB7lwQKBgQDrNt/rJce0raJdoxKa
d3fIrNk4O9tIF6EgPx/o4Gr649EZBQAmbaL23DzMkcuc9qsdrucjLWSXMc8sNApt
exEFIKyJTiWqQWGv3rw6jRT//Z8GEZT6Xa+2S/objvwGwvtPO7qTkDmbJG1+HkTA
HD5CI7DwhLdWqR89AwvUiH9ObQKBgQDLb9Gsnju2wGMSZSkPej/CbYlu3Q1llLK/
L6bC9FJajpD4dfphF79M8HF+gnlmPxuLK0Xkcv8YjhbixEolf5QWwMceOq9+qpos
y3GskYMqTWQ8hUxltsGdpnXFkJ0+7a69cYSbHZRiu9g4txOnHDh+YejLgML1m/BG
HJmCeDDzQQKBgE/+9L7TtYz0dMEl4gDY2stMRgBDEzv8lVcTQPYBxUCY1JeOxNNM
/Fy64I6ukzJKDj5lKsUi/hAR56Tf+h/r+AjnaOa1xkeWPvQCa7/6FYdOqZP1zNYt
oMH+KwzOX1apX7E93iGrrnveMsLu5nDz6hSycM4MRRJbKH2mmJJq7ektAoGBAJWd
KPDwdi4TE0mGCEqPt7B/6mEURTP9xe+BVf1uvdpHmyp/aaJaWqB0/KLzxeCCbPlO
29oFEMK4TPB9N6KYTwrkwAvlUQew5C4pePJXGcXUoPE5f1QWshIFR/wCPQL4vlgo
0kNZ37U1PPGJAvUVdh7MVu7DRZ5oDq8hfWxMhIOBAoGADQ7QRWfd2LI8E6fBV+KM
C6P8iHSHOpunL92yX9LsI01eoIJuazM6I5NKaquGl5xRhywDPTbJ5cVrf2BTom1l
oAETBCgzxCO/AYrebG18rXFBbR8uGVUiRHvxzhUiqyITY7/3/59dISVXkZZQ+kDf
xfuP17fzRNjzewM8mDmktjA=
-----END PRIVATE KEY-----
)KEY";

// ---------------------------------------------------------------------------
// Simple logging facility.  Log messages are written to Serial and appended
// to a file on LittleFS.  The log file is kept between boots and truncated
// only when it grows beyond a configured limit.  Content can be retrieved over
// HTTP for display in the UI.
// ---------------------------------------------------------------------------
static const char *LOG_PATH = "/log.txt";
static const char *USER_FILES_DIR = "/private";
static const char *SAMPLE_FILE_NAME = "sample.html";
static const char *SAMPLE_FILE_PATH = "/private/sample.html";
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
  if (LittleFS.exists(USER_FILES_DIR)) {
    return true;
  }
  if (!LittleFS.mkdir(USER_FILES_DIR)) {
    Serial.println("Failed to create private user directory");
    return false;
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
  if (cleaned.indexOf("//") != -1) return false;
  int start = 0;
  while (start < static_cast<int>(cleaned.length())) {
    int sep = cleaned.indexOf('/', start);
    int end = (sep == -1) ? cleaned.length() : sep;
    String segment = cleaned.substring(start, end);
    if (segment.length() == 0 || segment == "." || segment == "..") {
      return false;
    }
    start = (sep == -1) ? cleaned.length() : sep + 1;
  }
  if (!cleaned.equals(SAMPLE_FILE_NAME)) {
    return false;
  }
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

static bool enforceSampleFilePolicy() {
  if (!ensureUserDirectory()) {
    return false;
  }
  Dir dir = LittleFS.openDir(USER_FILES_DIR);
  while (dir.next()) {
    if (dir.isDirectory()) {
      continue;
    }
    String relative = toRelativeUserPath(dir.fileName());
    if (!relative.equals(SAMPLE_FILE_NAME)) {
      if (!LittleFS.remove(dir.fileName())) {
        Serial.printf("Failed to remove unexpected file %s from /private\n",
                      dir.fileName().c_str());
      }
    }
  }
  if (!LittleFS.exists(SAMPLE_FILE_PATH)) {
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
  Serial.printf("OLED initialised at 0x%02X using SDA=GPIO12 SCL=GPIO14\n",
                address);
  return true;
}

static void oledLog(const String &msg) {
  if (!oledLogging) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_5x7_tf);
  String shortMsg = msg;
  if (shortMsg.length() > 21) {
    shortMsg.remove(21);
  }
  oled.drawStr(0, 8, shortMsg.c_str());
  oled.sendBuffer();
}

static const size_t MAX_LOG_FILE_SIZE = 16 * 1024;  // 16 KB

static void initLogging() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed, formatting...");
    LittleFS.format();
    LittleFS.begin();
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
  File f = LittleFS.open(LOG_PATH, "a");
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
static const uint16_t PRIMARY_WEB_PORT = 443;
static AsyncWebServer server(PRIMARY_WEB_PORT);       // HTTPS server instance
static AsyncWebServer redirectServer(80);             // HTTP redirector
static WiFiUDP udp;               // UDP object for broadcasting and listening
static Adafruit_ADS1115 ads;      // ADS1115 ADC instance
static String configSetBody;      // buffer for incoming /api/config/set JSON
static String loginRequestBody;   // buffer for incoming /api/session/login JSON
static String outputSetBody;      // buffer for incoming /api/output/set JSON
static String fileSaveBody;       // buffer for incoming /api/files/save JSON

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
void parseConfigFromJson(const JsonDocument &doc);
String formatPin(uint16_t value);
void initialiseSecurity();
String generateSessionToken();
String buildSessionCookie(const String &value, bool expire);
bool extractSessionToken(AsyncWebServerRequest *req, String &tokenOut);
bool sessionTokenValid(const String &token, bool refreshActivity);
bool requireAuth(AsyncWebServerRequest *req);
void invalidateSession();
void triggerDiscovery();
void registerDiscoveredNode(const String &nodeId, const IPAddress &ip);
void sendDiscoveryResponse(const IPAddress &ip, uint16_t port);

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

String formatPin(uint16_t value) {
  char buf[5];
  snprintf(buf, sizeof(buf), "%04u", static_cast<unsigned int>(value % 10000));
  return String(buf);
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

  config.peerCount = 0;
  for (uint8_t i = 0; i < MAX_PEERS; i++) {
    config.peers[i] = {"", ""};
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
  DynamicJsonDocument doc(6144);
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
// modifying the global config.  Returns true on success.
bool saveConfig() {
  DynamicJsonDocument doc(6144);
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
    o["value"]  = oc.value;
  }
  doc["peerCount"] = config.peerCount;
  JsonArray peerArr = doc.createNestedArray("peers");
  for (uint8_t i = 0; i < config.peerCount && i < MAX_PEERS; i++) {
    JsonObject o = peerArr.createNestedObject();
    const PeerAuth &pa = config.peers[i];
    o["nodeId"] = pa.nodeId;
    o["pin"] = pa.pin;
  }
  // Write file
  File f = LittleFS.open("/config.json", "w");
  if (!f) {
    logMessage("Failed to open config for writing");
    return false;
  }
  if (serializeJson(doc, f) == 0) {
    logMessage("Failed to write config JSON");
    f.close();
    return false;
  }
  f.close();
  logMessage("Configuration saved");
  return true;
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
      if (o.containsKey("value")) {
        oc.value = o["value"].as<float>();
      } else {
        oc.value = 0.0f;
      }
      idx++;
    }
  }
  for (uint8_t i = idx; i < MAX_OUTPUTS; i++) {
    config.outputs[i] = {String("OUT") + String(i+1), OUTPUT_DISABLED, -1, 2000, 1.0f, 0.0f, false, 0.0f};
  }
  if (doc.containsKey("peers") && doc["peers"].is<JsonArray>()) {
    JsonArrayConst arr = doc["peers"].as<JsonArrayConst>();
    config.peerCount = min((uint8_t)arr.size(), MAX_PEERS);
    uint8_t pIdx = 0;
    for (JsonObjectConst o : arr) {
      if (pIdx >= MAX_PEERS) break;
      PeerAuth &pa = config.peers[pIdx];
      pa.nodeId = o.containsKey("nodeId") ? o["nodeId"].as<String>() : "";
      pa.pin = o.containsKey("pin") ? o["pin"].as<String>() : "";
      pIdx++;
    }
    for (uint8_t i = config.peerCount; i < MAX_PEERS; i++) {
      config.peers[i].nodeId = "";
      config.peers[i].pin = "";
    }
  } else {
    config.peerCount = 0;
    for (uint8_t i = 0; i < MAX_PEERS; i++) {
      config.peers[i].nodeId = "";
      config.peers[i].pin = "";
    }
  }
}

void invalidateSession() {
  sessionToken = "";
  sessionIssuedAt = 0;
  sessionLastActivity = 0;
}

void initialiseSecurity() {
  invalidateSession();
  uint16_t rawPin = static_cast<uint16_t>(random(0, 10000));
  sessionPin = formatPin(rawPin);
  logPrintf("Session PIN generated: %s", sessionPin.c_str());
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
  cookie += "; Secure";
  cookie += "; SameSite=Strict";
  return cookie;
}

bool extractSessionToken(AsyncWebServerRequest *req, String &tokenOut) {
  tokenOut = "";
  if (req->hasHeader("Cookie")) {
    String cookie = req->header("Cookie");
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
  if (tokenOut.length() == 0 && req->hasHeader("X-Session-Token")) {
    tokenOut = req->header("X-Session-Token");
    tokenOut.trim();
  }
  if (tokenOut.length() == 0 && req->hasHeader("Authorization")) {
    String auth = req->header("Authorization");
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

bool requireAuth(AsyncWebServerRequest *req) {
  String token;
  if (!extractSessionToken(req, token)) {
    req->send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return false;
  }
  if (!sessionTokenValid(token, true)) {
    req->send(401, "application/json", "{\"error\":\"unauthorized\"}");
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
  doc["fw"] = FIRMWARE_VERSION;
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
// allowing access via https://nodeId.local/ (with HTTP redirect support)
// on the same network.
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
  // Start mDNS if possible
  if (MDNS.begin(config.nodeId.c_str())) {
    logMessage("mDNS responder started");
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("https", "tcp", PRIMARY_WEB_PORT);
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

// Helper used by HTTP POST handlers to accumulate request body data.  The
// buffer is reset on the first chunk and each byte is appended so JSON payloads
// are available once AsyncWebServer invokes the main handler.
static void appendRequestBodyChunk(String &buffer, const uint8_t *data,
                                   size_t len, size_t index, size_t total) {
  if (index == 0) {
    buffer = "";
    if (total > 0 && total < 65536) {
      buffer.reserve(total);
    }
  }
  for (size_t i = 0; i < len; i++) {
    buffer += static_cast<char>(data[i]);
  }
}

// Set up the HTTP server and API routes.  Static content is served
// directly from LittleFS.  Configuration and value endpoints return
// JSON.  POST requests allow modifying configuration and outputs.
void setupServer() {
  // Session endpoints (login, logout, status)
  server.on(
      "/api/session/login", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        if (req->contentLength() == 0) {
          loginRequestBody = "";
          req->send(400, "application/json", "{\"error\":\"No body\"}");
          return;
        }
        String body = loginRequestBody;
        loginRequestBody = "";
        if (body.length() == 0) {
          req->send(400, "application/json", "{\"error\":\"No body\"}");
          return;
        }
        DynamicJsonDocument doc(256);
        if (deserializeJson(doc, body)) {
          req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
          return;
        }
        String pin = doc["pin"].as<String>();
        pin.trim();
        if (pin != sessionPin) {
          AsyncWebServerResponse *res = req->beginResponse(
              401, "application/json", "{\"error\":\"invalid_pin\"}");
          String cookie = buildSessionCookie("", true);
          res->addHeader("Set-Cookie", cookie);
          req->send(res);
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
        AsyncWebServerResponse *res =
            req->beginResponse(200, "application/json", payload);
        String cookie = buildSessionCookie(sessionToken, false);
        res->addHeader("Set-Cookie", cookie);
        req->send(res);
      },
      NULL,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
         size_t total) {
        appendRequestBodyChunk(loginRequestBody, data, len, index, total);
      });

  server.on("/api/session/logout", HTTP_POST, [](AsyncWebServerRequest *req) {
    invalidateSession();
    AsyncWebServerResponse *res =
        req->beginResponse(200, "application/json", "{\"status\":\"ok\"}");
    String cookie = buildSessionCookie("", true);
    res->addHeader("Set-Cookie", cookie);
    req->send(res);
  });

  server.on("/api/session/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    String token;
    if (!extractSessionToken(req, token) || !sessionTokenValid(token, true)) {
      req->send(401, "application/json", "{\"status\":\"invalid\"}");
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
    req->send(200, "application/json", payload);
  });

  // Serve the root page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/index.html", "text/html");
  });
  // Serve other static files (config.html, example.html, etc.)
  server.serveStatic("/", LittleFS, "/");

  // API: return current configuration
  server.on("/api/config/get", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    DynamicJsonDocument doc(2048);
    doc["nodeId"] = config.nodeId;
    doc["fwVersion"] = FIRMWARE_VERSION;
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
    doc["peerCount"] = config.peerCount;
    JsonArray peerArr = doc.createNestedArray("peers");
    for (uint8_t i = 0; i < config.peerCount && i < MAX_PEERS; i++) {
      JsonObject o = peerArr.createNestedObject();
      o["nodeId"] = config.peers[i].nodeId;
      o["pin"] = config.peers[i].pin;
    }
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
  });

  // API: set configuration.  Expects a JSON body.  After saving
  // configuration a reboot is performed to apply changes.
  server.on(
      "/api/config/set", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        if (!requireAuth(req)) {
          configSetBody = "";
          return;
        }
        if (configSetBody.length() == 0) {
          logMessage("Config set request missing body");
          req->send(400, "application/json", "{\"error\":\"No body\"}");
          return;
        }
        DynamicJsonDocument doc(4096);
        auto err = deserializeJson(doc, configSetBody);
        configSetBody = "";
        if (err) {
          logPrintf("Config set JSON parse error: %s", err.c_str());
          req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
          return;
        }
        // Update global config from JSON
        parseConfigFromJson(doc);
        if (!saveConfig()) {
          req->send(500, "application/json", "{\"error\":\"Save failed\"}");
          return;
        }
        oledLogging = false;
        req->send(200, "application/json", "{\"status\":\"ok\"}");
        // Delay slightly to ensure response is sent before rebooting
        delay(200);
        ESP.restart();
      },
      NULL,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
         size_t total) {
        String token;
        if (!extractSessionToken(req, token) || !sessionTokenValid(token, false)) {
          return;
        }
        appendRequestBodyChunk(configSetBody, data, len, index, total);
      });

  // API: reboot device on demand
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    req->send(200, "application/json", "{\"status\":\"ok\"}");
    delay(200);
    ESP.restart();
  });

  // API: OTA firmware update
  server.on(
      "/api/ota", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        if (!requireAuth(req)) return;
        bool success = !Update.hasError();
        req->send(success ? 200 : 500, "application/json",
                  success ? "{\"status\":\"ok\"}"
                          : "{\"status\":\"fail\"}");
        if (success) {
          delay(200);
          ESP.restart();
        }
      },
      [](AsyncWebServerRequest *req, String filename, size_t index,
         uint8_t *data, size_t len, bool final) {
        String token;
        if (!extractSessionToken(req, token) || !sessionTokenValid(token, false)) {
          return;
        }
        if (!index) {
          logPrintf("OTA update: %s", filename.c_str());
          if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
            Update.printError(Serial);
          }
        }
        if (Update.write(data, len) != len) {
          Update.printError(Serial);
        }
        if (final) {
          if (Update.end(true)) {
            logMessage("Update successful");
          } else {
            Update.printError(Serial);
          }
        }
      });

  // API: return current input values
  server.on("/api/inputs", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
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
    if (!requireAuth(req)) return;
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

  server.on("/api/discovery", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    triggerDiscovery();
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.to<JsonArray>();
    uint32_t now = millis();
    for (uint8_t i = 0; i < discoveredCount; i++) {
      if (now - discoveredNodes[i].lastSeen > DISCOVERY_TIMEOUT_MS) {
        continue;
      }
      JsonObject o = arr.createNestedObject();
      o["nodeId"] = discoveredNodes[i].nodeId;
      o["ip"] = discoveredNodes[i].ip.toString();
      o["ageMs"] = static_cast<uint32_t>(now - discoveredNodes[i].lastSeen);
      bool hasPin = false;
      for (uint8_t p = 0; p < config.peerCount && p < MAX_PEERS; p++) {
        if (config.peers[p].nodeId == discoveredNodes[i].nodeId &&
            config.peers[p].pin.length() > 0) {
          hasPin = true;
          break;
        }
      }
      o["hasPin"] = hasPin;
    }
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
  });

  // API: set an output value.  Expects JSON {"name":"output1","value":x}
  server.on(
      "/api/output/set", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        if (!requireAuth(req)) {
          outputSetBody = "";
          return;
        }
        if (req->contentLength() == 0) {
          outputSetBody = "";
          req->send(400, "application/json", "{\"error\":\"No body\"}");
          return;
        }
        String body = outputSetBody;
        outputSetBody = "";
        if (body.length() == 0) {
          req->send(400, "application/json", "{\"error\":\"No body\"}");
          return;
        }
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
          req->send(404, "application/json",
                    "{\"error\":\"Unknown output\"}");
          return;
        }
        updateOutputs();
        saveConfig();
        req->send(200, "application/json", "{\"status\":\"ok\"}");
      },
      NULL,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
         size_t total) {
        appendRequestBodyChunk(outputSetBody, data, len, index, total);
      });

  // API: return remote values
  server.on("/api/remote", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
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
    if (!requireAuth(req)) return;
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

  // API: simple file browser (LittleFS)
  server.on("/api/files/list", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    if (!enforceSampleFilePolicy()) {
      req->send(500, "application/json", "{\"error\":\"private directory\"}");
      return;
    }
    DynamicJsonDocument doc(256);
    JsonArray arr = doc.to<JsonArray>();
    if (LittleFS.exists(SAMPLE_FILE_PATH)) {
      arr.add(SAMPLE_FILE_NAME);
    }
    String resp;
    serializeJson(arr, resp);
    req->send(200, "application/json", resp);
  });

  server.on("/api/files/get", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    if (!req->hasParam("path")) {
      req->send(400, "text/plain", "missing path");
      return;
    }
    if (!enforceSampleFilePolicy()) {
      req->send(500, "text/plain", "storage unavailable");
      return;
    }
    String clientPath = req->getParam("path")->value();
    String fsPath;
    if (!resolveUserPath(clientPath, fsPath)) {
      req->send(400, "text/plain", "invalid path");
      return;
    }
    if (!LittleFS.exists(fsPath)) {
      req->send(404, "text/plain", "not found");
      return;
    }
    File f = LittleFS.open(fsPath, "r");
    if (!f) {
      req->send(500, "text/plain", "open failed");
      return;
    }
    String content = f.readString();
    f.close();
    req->send(200, "text/plain", content);
  });

  server.on(
      "/api/files/save", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        if (!requireAuth(req)) {
          fileSaveBody = "";
          return;
        }
        if (req->contentLength() == 0) {
          fileSaveBody = "";
          req->send(400, "application/json", "{\"error\":\"No body\"}");
          return;
        }
        if (!enforceSampleFilePolicy()) {
          fileSaveBody = "";
          req->send(500, "application/json",
                    "{\"error\":\"storage unavailable\"}");
          return;
        }
        String body = fileSaveBody;
        fileSaveBody = "";
        if (body.length() == 0) {
          req->send(400, "application/json", "{\"error\":\"No body\"}");
          return;
        }
        DynamicJsonDocument doc(8192);
        if (deserializeJson(doc, body)) {
          req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
          return;
        }
        String clientPath = doc["path"].as<String>();
        String fsPath;
        if (!resolveUserPath(clientPath, fsPath)) {
          req->send(400, "application/json", "{\"error\":\"invalid path\"}");
          return;
        }
        String content = doc["content"].as<String>();
        File f = LittleFS.open(fsPath, "w");
        if (!f) {
          req->send(500, "application/json", "{\"error\":\"open failed\"}");
          return;
        }
        f.print(content);
        f.close();
        req->send(200, "application/json", "{\"status\":\"ok\"}");
      },
      NULL,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
         size_t total) {
        appendRequestBodyChunk(fileSaveBody, data, len, index, total);
      });

  server.on("/api/files/create", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    if (!enforceSampleFilePolicy()) {
      req->send(500, "application/json", "{\"error\":\"storage unavailable\"}");
      return;
    }
    req->send(403, "application/json", "{\"error\":\"forbidden\"}");
  });

  server.on("/api/files/rename", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    if (!enforceSampleFilePolicy()) {
      req->send(500, "application/json", "{\"error\":\"storage unavailable\"}");
      return;
    }
    req->send(403, "application/json", "{\"error\":\"forbidden\"}");
  });

  server.on("/api/files/delete", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    if (!enforceSampleFilePolicy()) {
      req->send(500, "application/json", "{\"error\":\"storage unavailable\"}");
      return;
    }
    req->send(403, "application/json", "{\"error\":\"forbidden\"}");
  });

  // Start HTTPS server and HTTP redirector
  bool httpsStarted = server.beginSecure(TLS_CERT, TLS_KEY, nullptr);
  if (!httpsStarted) {
    logMessage("Failed to start HTTPS server");
    return;
  }
  logPrintf("HTTPS server started on port %u", PRIMARY_WEB_PORT);

  redirectServer.on("/", HTTP_ANY, [](AsyncWebServerRequest *req) {
    String host = req->host();
    if (!host.length()) {
      IPAddress ip = (WiFi.getMode() == WIFI_STA) ? WiFi.localIP() : WiFi.softAPIP();
      host = ip.toString();
    }
    req->redirect(String("https://") + host + "/");
  });
  redirectServer.onNotFound([](AsyncWebServerRequest *req) {
    String host = req->host();
    if (!host.length()) {
      IPAddress ip = (WiFi.getMode() == WIFI_STA) ? WiFi.localIP() : WiFi.softAPIP();
      host = ip.toString();
    }
    String target = String("https://") + host + req->url();
    req->redirect(target);
  });
  redirectServer.begin();
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
  randomSeed(analogRead(A0) ^ micros() ^ ESP.getCycleCount());
  initialiseSecurity();
  if (!ensureUserDirectory()) {
    logMessage("Failed to ensure private directory /private");
  } else if (!enforceSampleFilePolicy()) {
    logMessage("Failed to initialise /private/sample.html");
  }
  logMessage("MiniLabBox v2 starting...");
  if (!oledOk) {
    logMessage("OLED not detected (check wiring on GPIO12/GPIO14)");
  }
  loadConfig();
  setupWiFi();
  // Start UDP listener
  udp.begin(BROADCAST_PORT);
  setupSensors();
  setupServer();
  triggerDiscovery();
  // After network and services are up, show basic status on the OLED
  // so users immediately know the node ID and how to reach it.  This
  // reuses the boot logging screen and will remain until another log
  // message is printed or configuration changes disable OLED logging.
  if (oledLogging) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_7x14B_tr);
    oled.drawStr(0, 16, config.nodeId.c_str());
    IPAddress ip = (WiFi.getMode() == WIFI_STA) ? WiFi.localIP() : WiFi.softAPIP();
    oled.drawStr(0, 32, ip.toString().c_str());
    String pinLine = String("PIN: ") + sessionPin;
    oled.drawStr(0, 48, pinLine.c_str());
    oled.sendBuffer();
    oled.setFont(u8g2_font_5x7_tf);
  }
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