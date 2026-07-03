#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <mbedtls/gcm.h>
#include <mbedtls/pkcs5.h>

#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr const char* WIFI1_SSID = "Telcel-07C4";
constexpr const char* WIFI1_PASSWORD = "6M8JNB19NA4";
constexpr const char* WIFI2_SSID = "Alan Daniel's IPhone";
constexpr const char* WIFI2_PASSWORD = "passwordalan";
constexpr const char* FW_VERSION = "0.1.1";
constexpr const char* UPDATE_MANIFEST_URL = "https://raw.githubusercontent.com/Sonny-bot-33/c3-supermini-blackvault/main/manifest.json";

constexpr const char* AP_SSID = "BlackVault";
constexpr const char* AP_PASSWORD = "korsob-mehvyp-wikRa9";
constexpr uint8_t AP_CHANNEL = 6;
constexpr uint8_t AP_MAX_CONNECTIONS = 1;
constexpr uint16_t HTTP_PORT = 80;
constexpr uint8_t LED_PIN = 8;
constexpr uint8_t BUZZER_PIN = 5;
constexpr size_t MAX_MESSAGE_LEN = 4096;
constexpr size_t PASSPHRASE_MAX_LEN = 128;
constexpr size_t MIN_PASSPHRASE_LEN = 8;
constexpr size_t SALT_LEN = 16;
constexpr size_t NONCE_LEN = 12;
constexpr size_t TAG_LEN = 16;
constexpr size_t RANDOM_PADDING_LEN = 32;
constexpr unsigned int PBKDF2_ITERATIONS = 120000;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr unsigned long NO_CLIENT_SLEEP_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long BUZZER_HALF_PERIOD_US = 160;

WebServer server(HTTP_PORT);
String statusMessage = "Booting";
unsigned long lastClientSeenMs = 0;
bool blackVaultStarted = false;

struct WifiCandidate {
  const char* ssid;
  const char* password;
};

const WifiCandidate WIFI_CANDIDATES[] = {
  {WIFI1_SSID, WIFI1_PASSWORD},
  {WIFI2_SSID, WIFI2_PASSWORD},
};

struct VaultResult {
  bool ok = false;
  String error;
  String filename;
  String mimeType;
  std::string data;
};

bool randomBytes(uint8_t* out, size_t len) {
  if (!out || len == 0) return false;
  esp_fill_random(out, len);
  return true;
}

void secureZero(void* ptr, size_t len) {
  if (!ptr || len == 0) return;
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (len--) {
    *p++ = 0;
  }
}

void ledOff() {
  digitalWrite(LED_PIN, LOW);
}

void buzzerOn() {
  digitalWrite(BUZZER_PIN, HIGH);
}

void buzzerOff() {
  digitalWrite(BUZZER_PIN, LOW);
}

void buzzForMs(unsigned long durationMs) {
  const unsigned long startUs = micros();
  const unsigned long durationUs = durationMs * 1000UL;
  while ((micros() - startUs) < durationUs) {
    buzzerOn();
    delayMicroseconds(BUZZER_HALF_PERIOD_US);
    buzzerOff();
    delayMicroseconds(BUZZER_HALF_PERIOD_US);
  }
  buzzerOff();
}

void playBootPattern() {
  buzzForMs(2000);
}

void playBlackVaultPattern() {
  buzzForMs(1000);
  delay(1000);
  buzzForMs(1000);
}

void playSleepPattern() {
  for (int i = 0; i < 3; ++i) {
    buzzForMs(1000);
    if (i < 2) {
      delay(1000);
    }
  }
}

String makeFilename() {
  uint8_t id[8];
  randomBytes(id, sizeof(id));
  char hex[17] = {0};
  for (size_t i = 0; i < sizeof(id); ++i) {
    snprintf(&hex[i * 2], 3, "%02x", id[i]);
  }
  return String("blackvault-") + hex + ".vlt";
}

bool deriveKey(const std::string& passphrase, const uint8_t* salt, size_t saltLen, uint8_t* outKey, size_t outKeyLen) {
  const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                               reinterpret_cast<const unsigned char*>(passphrase.data()),
                                               passphrase.size(), salt, saltLen, PBKDF2_ITERATIONS, outKeyLen, outKey);
  return rc == 0;
}

std::string buildPayload(const std::string& message, const std::string& createdAt) {
  JsonDocument doc;
  doc["v"] = 1;
  doc["ts"] = createdAt.c_str();
  doc["msg"] = message.c_str();

  uint8_t padding[RANDOM_PADDING_LEN];
  if (randomBytes(padding, sizeof(padding))) {
    char hex[(RANDOM_PADDING_LEN * 2) + 1];
    for (size_t i = 0; i < RANDOM_PADDING_LEN; ++i) {
      snprintf(&hex[i * 2], 3, "%02x", padding[i]);
    }
    doc["p"] = hex;
    secureZero(padding, sizeof(padding));
  }

  std::string payload;
  serializeJson(doc, payload);
  return payload;
}

VaultResult encryptToVlt(const std::string& message, const std::string& passphrase, const std::string& createdAt) {
  VaultResult result;
  if (message.empty()) {
    result.error = "Mensaje vacio";
    return result;
  }
  if (message.size() > MAX_MESSAGE_LEN) {
    result.error = "Mensaje demasiado largo";
    return result;
  }
  if (passphrase.size() < MIN_PASSPHRASE_LEN || passphrase.size() > PASSPHRASE_MAX_LEN) {
    result.error = "Passphrase invalida";
    return result;
  }

  std::vector<uint8_t> salt(SALT_LEN);
  std::vector<uint8_t> nonce(NONCE_LEN);
  std::vector<uint8_t> tag(TAG_LEN);
  if (!randomBytes(salt.data(), salt.size()) || !randomBytes(nonce.data(), nonce.size())) {
    result.error = "Sin aleatoriedad suficiente";
    return result;
  }

  const std::string payload = buildPayload(message, createdAt);
  std::vector<uint8_t> ciphertext(payload.size());

  uint8_t key[32] = {0};
  if (!deriveKey(passphrase, salt.data(), salt.size(), key, sizeof(key))) {
    result.error = "No se pudo derivar clave";
    return result;
  }

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, payload.size(), nonce.data(), nonce.size(), nullptr, 0,
                                   reinterpret_cast<const unsigned char*>(payload.data()), ciphertext.data(),
                                   tag.size(), tag.data());
  }
  mbedtls_gcm_free(&gcm);
  secureZero(key, sizeof(key));

  if (rc != 0) {
    result.error = "Fallo de cifrado";
    return result;
  }

  JsonDocument doc;
  doc["m"] = "BVLT";
  doc["v"] = 1;
  doc["k"] = "pbkdf2-sha256";
  doc["i"] = PBKDF2_ITERATIONS;
  doc["c"] = "aes-256-gcm";
  doc["s"] = base64::encode(salt.data(), salt.size()).c_str();
  doc["n"] = base64::encode(nonce.data(), nonce.size()).c_str();
  doc["t"] = base64::encode(tag.data(), tag.size()).c_str();
  doc["d"] = base64::encode(ciphertext.data(), ciphertext.size()).c_str();

  serializeJson(doc, result.data);
  result.ok = true;
  result.filename = makeFilename();
  result.mimeType = "application/octet-stream";
  return result;
}

String trimCopy(const String& s) {
  String out = s;
  out.trim();
  return out;
}

String buildCreatedAt(const String& date, const String& hour, const String& minute) {
  const String d = trimCopy(date);
  const String h = trimCopy(hour);
  const String m = trimCopy(minute);
  if (d.isEmpty() && h.isEmpty() && m.isEmpty()) {
    return "0000-00-00T00:00:00Z";
  }

  auto pad2 = [](String v) -> String {
    if (v.isEmpty()) return String("00");
    if (v.length() == 1) return String("0") + v;
    return v;
  };

  return (d.isEmpty() ? "0000-00-00" : d) + String("T") + pad2(h) + ":" + pad2(m) + ":00Z";
}

bool connectToKnownWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(150);

  for (const auto& candidate : WIFI_CANDIDATES) {
    Serial.printf("[wifi] intentando %s\n", candidate.ssid);
    WiFi.begin(candidate.ssid, candidate.password);
    const unsigned long start = millis();
    while (millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wifi] conectado a %s ip=%s\n", candidate.ssid, WiFi.localIP().toString().c_str());
        statusMessage = String("WiFi OK: ") + candidate.ssid;
        return true;
      }
      delay(250);
    }
    Serial.printf("[wifi] fallo %s status=%d\n", candidate.ssid, (int)WiFi.status());
    WiFi.disconnect(true, true);
    delay(150);
  }

  statusMessage = "Sin WiFi";
  return false;
}

bool fetchManifest(JsonDocument& doc) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, UPDATE_MANIFEST_URL)) {
    Serial.println("[ota] no se pudo abrir manifest");
    return false;
  }
  http.setConnectTimeout(12000);
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ota] manifest code=%d\n", code);
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();
  const auto err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[ota] manifest invalido: %s\n", err.c_str());
    return false;
  }
  return true;
}

bool applyRemoteFirmware(const String& firmwareUrl) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, firmwareUrl)) {
    Serial.println("[ota] begin firmware fallo");
    return false;
  }
  http.setConnectTimeout(15000);
  http.setTimeout(60000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ota] firmware code=%d\n", code);
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  if (!Update.begin(contentLength > 0 ? contentLength : UPDATE_SIZE_UNKNOWN)) {
    Serial.println("[ota] Update.begin fallo");
    http.end();
    return false;
  }

  const size_t written = Update.writeStream(*stream);
  if (contentLength > 0 && written != static_cast<size_t>(contentLength)) {
    Serial.printf("[ota] escritura parcial %u/%d\n", (unsigned)written, contentLength);
    Update.abort();
    http.end();
    return false;
  }

  if (!Update.end()) {
    Serial.println("[ota] Update.end fallo");
    Update.printError(Serial);
    http.end();
    return false;
  }

  if (!Update.isFinished()) {
    Serial.println("[ota] update no terminado");
    http.end();
    return false;
  }

  http.end();
  return true;
}

void checkForOtaUpdate() {
  statusMessage = "Revisando OTA";
  if (!connectToKnownWifi()) {
    Serial.println("[ota] sin wifi, sigue BlackVault");
    return;
  }

  JsonDocument manifest;
  if (!fetchManifest(manifest)) {
    Serial.println("[ota] no se pudo leer manifest, sigue BlackVault");
    return;
  }

  const String remoteVersion = manifest["version"] | "";
  const String firmwareUrl = manifest["firmwareUrl"] | manifest["firmware_url"] | "";

  if (remoteVersion.isEmpty() || firmwareUrl.isEmpty()) {
    Serial.println("[ota] manifest incompleto, sigue BlackVault");
    statusMessage = "Manifest incompleto";
    return;
  }

  Serial.printf("[ota] actual=%s remota=%s\n", FW_VERSION, remoteVersion.c_str());
  if (remoteVersion == FW_VERSION) {
    Serial.println("[ota] sin actualizacion");
    statusMessage = "Sin actualizacion";
    return;
  }

  Serial.printf("[ota] actualizando desde %s\n", firmwareUrl.c_str());
  statusMessage = String("Actualizando a ") + remoteVersion;
  if (applyRemoteFirmware(firmwareUrl)) {
    Serial.println("[ota] actualizacion OK, reiniciando");
    delay(1000);
    ESP.restart();
  }

  Serial.println("[ota] actualizacion fallida, sigue BlackVault");
  statusMessage = "OTA fallida";
}

String pageHtml() {
  return R"HTML(<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>BlackVault</title><style>
body{font-family:system-ui,sans-serif;background:#101114;color:#ececec;padding:20px;max-width:720px;margin:0 auto}
h1{font-size:1.5rem;margin-bottom:18px}
.panel{background:#17191d;border:1px solid #2d3138;border-radius:16px;padding:16px;box-shadow:0 8px 24px rgba(0,0,0,.18)}
.row{display:flex;gap:10px;flex-wrap:wrap}.field{flex:1;min-width:120px}
label{display:block;font-size:.86rem;color:#b8bec7;margin-bottom:6px}
textarea,input{width:100%;box-sizing:border-box;margin:0 0 12px 0;padding:12px;border-radius:12px;border:1px solid #3a3f46;background:#20242a;color:#c7ccd3}
textarea{min-height:220px;color:#a9b0b8;resize:vertical}
button{padding:12px 16px;margin-right:8px;border:0;border-radius:12px;background:#d9dde3;color:#111;font-weight:700}
.ghost{background:#2a2f36;color:#e6e6e6}#msg{margin-top:12px;white-space:pre-wrap;color:#c9d1d9}
</style></head><body><h1>BlackVault</h1><div class='panel'><div class='row'><div class='field'><label for='entry_date'>Fecha</label><input id='entry_date' type='date' placeholder='0000-00-00'></div><div class='field'><label for='entry_hour'>Hora</label><input id='entry_hour' type='number' min='0' max='23' placeholder='00'></div><div class='field'><label for='entry_minute'>Minuto</label><input id='entry_minute' type='number' min='0' max='59' placeholder='00'></div></div><label for='message'>Mensaje</label><textarea id='message' rows='10' maxlength='4096' placeholder='Escribe aquí'></textarea><label for='passphrase'>Passphrase</label><input id='passphrase' type='password' maxlength='128' autocomplete='off' placeholder='Passphrase'><div><button onclick='saveNote()'>Guardar y descargar</button><button class='ghost' onclick='clearForm()'>Limpiar</button></div><div id='msg'></div></div><script>
function pad2(n){return String(n).padStart(2,'0');}
function seedDateTime(){const now=new Date();document.getElementById('entry_date').value=`${now.getFullYear()}-${pad2(now.getMonth()+1)}-${pad2(now.getDate())}`;document.getElementById('entry_hour').value=pad2(now.getHours());document.getElementById('entry_minute').value=pad2(now.getMinutes());}
async function saveNote(){const body={message:document.getElementById('message').value,passphrase:document.getElementById('passphrase').value,entry_date:document.getElementById('entry_date').value,entry_hour:document.getElementById('entry_hour').value,entry_minute:document.getElementById('entry_minute').value};const r=await fetch('/api/vault/download',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});if(!r.ok){const j=await r.json().catch(()=>({error:'Error desconocido'}));document.getElementById('msg').textContent=j.error||'Error';return;}const blob=await r.blob();const cd=r.headers.get('Content-Disposition')||'';let filename='blackvault.vlt';const m=cd.match(/filename="([^"]+)"/);if(m)filename=m[1];const url=URL.createObjectURL(blob);const a=document.createElement('a');a.href=url;a.download=filename;document.body.appendChild(a);a.click();a.remove();URL.revokeObjectURL(url);document.getElementById('msg').textContent='Archivo cifrado descargado';}
function clearForm(){document.getElementById('message').value='';document.getElementById('passphrase').value='';document.getElementById('msg').textContent='';seedDateTime();}
document.addEventListener('DOMContentLoaded',seedDateTime);
</script></body></html>)HTML";
}

void markClientActivity() {
  lastClientSeenMs = millis();
}

void handleRoot() {
  markClientActivity();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", pageHtml());
}

void handleStatus() {
  markClientActivity();
  JsonDocument doc;
  doc["ok"] = true;
  doc["status"] = statusMessage;
  String payload;
  serializeJson(doc, payload);
  server.send(200, "application/json", payload);
}

void handleDownload() {
  markClientActivity();
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"Body vacio\"}");
    return;
  }

  JsonDocument req;
  if (deserializeJson(req, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"JSON invalido\"}");
    return;
  }

  const std::string message = (const char*)(req["message"] | "");
  const std::string passphrase = (const char*)(req["passphrase"] | "");
  const String createdAt = buildCreatedAt(req["entry_date"] | "", req["entry_hour"] | "", req["entry_minute"] | "");

  const VaultResult result = encryptToVlt(message, passphrase, createdAt.c_str());
  if (!result.ok) {
    statusMessage = result.error;
    JsonDocument err;
    err["error"] = result.error;
    String payload;
    serializeJson(err, payload);
    server.send(400, "application/json", payload);
    return;
  }

  statusMessage = "Archivo generado";
  server.sendHeader("Content-Type", result.mimeType);
  server.sendHeader("Content-Disposition", String("attachment; filename=\"") + result.filename + "\"");
  server.send(200, result.mimeType, String(result.data.c_str()));
}

void enterDeepSleep() {
  Serial.println("[sleep] entrando en deep sleep");
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  buzzerOff();
  ledOff();
  esp_sleep_enable_timer_wakeup(10ULL * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

void maybeSleepForInactivity() {
  if (!blackVaultStarted) return;
  if (lastClientSeenMs == 0) return;
  if ((millis() - lastClientSeenMs) < NO_CLIENT_SLEEP_MS) return;
  playSleepPattern();
  enterDeepSleep();
}

void appSetup() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  ledOff();
  buzzerOff();

  Serial.begin(115200);
  delay(300);

  playBootPattern();
  Serial.println("BlackVault SuperMini boot");

  checkForOtaUpdate();

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/vault/status", HTTP_GET, handleStatus);
  server.on("/api/vault/download", HTTP_POST, handleDownload);
  server.begin();

  lastClientSeenMs = millis();
  blackVaultStarted = true;
  playBlackVaultPattern();

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void appLoop() {
  server.handleClient();
  maybeSleepForInactivity();
}

}  // namespace

void setup() { appSetup(); }

void loop() { appLoop(); }
