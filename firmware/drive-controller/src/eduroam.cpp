#include "eduroam.h"
#include "eduroam_ca.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_eap_client.h>
#include <esp_wifi.h>
#include <mbedtls/platform_util.h>

namespace {
constexpr char kSsid[] = "eduroam";
constexpr char kRealm[] = "@utoronto.ca";
constexpr char kRadiusServer[] = "radius.wireless.utoronto.ca";
constexpr char kNamespace[] = "eduroam";
constexpr char kProfileKey[] = "profile";
constexpr uint32_t kRetryIntervalMs = 60'000;
bool started = false;
uint32_t lastAttempt = 0;

struct Credentials {
  char username[96];
  char password[128];
  ~Credentials() { mbedtls_platform_zeroize(this, sizeof(*this)); }
};

bool loadCredentials(Credentials& credentials) {
  Preferences nvs;
  return nvs.begin(kNamespace, true) &&
         nvs.getBytes(kProfileKey, &credentials, sizeof(credentials)) == sizeof(credentials) &&
         credentials.username[0] && credentials.password[0] &&
         credentials.username[sizeof(credentials.username) - 1] == 0 &&
         credentials.password[sizeof(credentials.password) - 1] == 0;
}

esp_err_t connectEnterprise(const Credentials& credentials) {
  wifi_config_t network{};
  memcpy(network.sta.ssid, kSsid, sizeof(kSsid));
  network.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
  network.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  network.sta.pmf_cfg.capable = true;

  const auto* user = reinterpret_cast<const uint8_t*>(credentials.username);
  const auto* password = reinterpret_cast<const uint8_t*>(credentials.password);
  const int userLength = strlen(credentials.username);
  const auto* ca = reinterpret_cast<const uint8_t*>(kEduroamCa);
  // The board has no trusted time source before joining. Keep CA and name verification enabled.
  const esp_err_t settings[] = {
    esp_wifi_set_config(WIFI_IF_STA, &network),
    esp_eap_client_use_default_cert_bundle(false),
    esp_eap_client_set_ca_cert(ca, sizeof(kEduroamCa)),
    esp_eap_client_set_domain_name(kRadiusServer),
    esp_eap_client_set_disable_time_check(true),
    esp_eap_client_set_eap_methods(ESP_EAP_TYPE_PEAP),
    esp_eap_client_set_identity(user, userLength),
    esp_eap_client_set_username(user, userLength),
    esp_eap_client_set_password(password, strlen(credentials.password)),
  };
  for (const auto result : settings) {
    if (result != ESP_OK) return result;
  }
  const esp_err_t enabled = esp_wifi_sta_enterprise_enable();
  return enabled == ESP_OK ? esp_wifi_connect() : enabled;
}
}  // namespace

void startEduroam() {
  Credentials credentials{};
  if (!loadCredentials(credentials)) return;
  WiFi.setSleep(false);
  if (!WiFi.mode(WIFI_AP_STA)) {
    Serial.println("EDUROAM could not start station mode");
    return;
  }
  WiFi.setAutoReconnect(false);
  const esp_err_t result = connectEnterprise(credentials);
  if (result != ESP_OK) {
    Serial.printf("EDUROAM setup failed (%s)\n", esp_err_to_name(result));
    WiFi.mode(WIFI_AP);
    return;
  }
  started = true;
  lastAttempt = millis();
  Serial.println("EDUROAM joining");
}

void serviceEduroam(uint32_t now) {
  if (!started || now - lastAttempt < kRetryIntervalMs) return;
  lastAttempt = now;
  const auto status = WiFi.status();
  // Scanning with the shared radio can interrupt the hotspot's control connection.
  if (status != WL_CONNECTED && status != WL_IDLE_STATUS && status != WL_CONNECT_FAILED &&
      WiFi.softAPgetStationNum() == 0) esp_wifi_connect();
}

void handleEduroamCommand(char* command, bool idle) {
  if (!strcmp(command, "status")) {
    Credentials credentials{};
    const bool saved = loadCredentials(credentials);
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("EDUROAM connected %s\n", WiFi.localIP().toString().c_str());
    } else if (started) {
      Serial.printf("EDUROAM disconnected (Wi-Fi status %d)\n", WiFi.status());
    } else {
      Serial.println(saved ? "EDUROAM profile saved; reboot to connect" : "EDUROAM no profile");
    }
    return;
  }
  if (!idle) {
    Serial.println("EDUROAM stop drive before provisioning");
    return;
  }
  Preferences nvs;
  if (!strcmp(command, "forget")) {
    const bool removed = nvs.begin(kNamespace, false) && (!nvs.isKey(kProfileKey) || nvs.remove(kProfileKey));
    Serial.println(removed ? "EDUROAM profile removed; reboot to disconnect" : "EDUROAM storage failed");
    return;
  }
  if (strncmp(command, "set ", 4)) {
    Serial.println("EDUROAM invalid command");
    return;
  }
  char* username = command + 4;
  char* separator = strchr(username, '\t');
  if (!separator) {
    Serial.println("EDUROAM invalid profile");
    return;
  }
  *separator = '\0';
  const char* password = separator + 1;
  const char* realm = strchr(username, '@');
  Credentials credentials{};
  if (!realm || realm == username || strcmp(realm, kRealm) || strchr(username, ' ') ||
      strlen(username) >= sizeof(credentials.username) ||
      !*password || strlen(password) >= sizeof(credentials.password)) {
    Serial.println("EDUROAM invalid profile");
    return;
  }
  strcpy(credentials.username, username);
  strcpy(credentials.password, password);
  const bool saved = nvs.begin(kNamespace, false) &&
                     nvs.putBytes(kProfileKey, &credentials, sizeof(credentials)) == sizeof(credentials);
  Serial.println(saved ? "EDUROAM profile saved; reboot to connect" : "EDUROAM storage failed");
}
