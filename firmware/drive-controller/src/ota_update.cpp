#include "ota_update.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

namespace {
constexpr size_t kDigestBytes = 32;
constexpr char kNamespace[] = "hotelbot-ota";
constexpr char kKeyName[] = "auth-key";

int hexDigit(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  return -1;
}

bool decodeDigest(const char* text, uint8_t (&bytes)[kDigestBytes]) {
  if (strlen(text) != 2 * sizeof(bytes)) return false;
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    const int high = hexDigit(text[2 * i]);
    const int low = hexDigit(text[2 * i + 1]);
    if (high < 0 || low < 0) return false;
    bytes[i] = static_cast<uint8_t>(16 * high + low);
  }
  return true;
}

bool equalDigest(const uint8_t* a, const uint8_t* b) {
  volatile uint8_t difference = 0;
  for (size_t i = 0; i < kDigestBytes; ++i) difference = difference | (a[i] ^ b[i]);
  return difference == 0;
}

bool authorize(httpd_req_t* request, uint32_t bootId, uint8_t (&digest)[kDigestBytes]) {
  char digestText[2 * kDigestBytes + 1], authText[sizeof(digestText)];
  uint8_t supplied[kDigestBytes], expected[kDigestBytes], key[kDigestBytes];
  if (httpd_req_get_hdr_value_str(request, "X-Firmware-SHA256", digestText, sizeof(digestText)) != ESP_OK ||
      httpd_req_get_hdr_value_str(request, "X-Update-Auth", authText, sizeof(authText)) != ESP_OK ||
      !decodeDigest(digestText, digest) || !decodeDigest(authText, supplied)) return false;

  Preferences nvs;
  if (!nvs.begin(kNamespace, true) || nvs.getBytes(kKeyName, key, sizeof(key)) != sizeof(key)) {
    return false;
  }
  char message[96];
  const int length = snprintf(message, sizeof(message), "%lu:%s", static_cast<unsigned long>(bootId), digestText);
  const int result = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, sizeof(key),
                                    reinterpret_cast<uint8_t*>(message), length, expected);
  mbedtls_platform_zeroize(key, sizeof(key));
  return result == 0 && equalDigest(supplied, expected);
}

bool writeImage(httpd_req_t* request, const esp_partition_t* partition, const uint8_t* expected) {
  esp_ota_handle_t handle;
  if (esp_ota_begin(partition, request->content_len, &handle) != ESP_OK) return false;

  // The HTTP server runs a single handler at a time. Keep the receive buffer off its task stack.
  static uint8_t block[1024];
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  bool ok = mbedtls_sha256_starts(&hash, 0) == 0;
  size_t remaining = request->content_len;
  const uint32_t startedAt = millis();
  while (remaining && ok) {
    const int count = httpd_req_recv(request, reinterpret_cast<char*>(block), std::min(remaining, sizeof(block)));
    ok = count > 0 && millis() - startedAt < 60'000 &&
         mbedtls_sha256_update(&hash, block, count) == 0 && esp_ota_write(handle, block, count) == ESP_OK;
    if (ok) remaining -= count;
  }
  uint8_t actual[kDigestBytes];
  ok = ok && mbedtls_sha256_finish(&hash, actual) == 0 && equalDigest(actual, expected);
  mbedtls_sha256_free(&hash);
  if (!ok) {
    esp_ota_abort(handle);
    return false;
  }
  return esp_ota_end(handle) == ESP_OK && esp_ota_set_boot_partition(partition) == ESP_OK;
}
}  // namespace

void handleOtaSerial(const char* command, bool idle) {
  Preferences nvs;
  if (!strcmp(command, "status")) {
    const bool hasKey = nvs.begin(kNamespace, true) && nvs.getBytesLength(kKeyName) == kDigestBytes;
    Serial.printf("OTA %s %s\n", hasKey ? "ready" : "no key", esp_ota_get_running_partition()->label);
    return;
  }
  if (!idle) {
    Serial.println("OTA stop drive before provisioning");
    return;
  }
  uint8_t key[kDigestBytes];
  if (strncmp(command, "key ", 4) || !decodeDigest(command + 4, key)) {
    Serial.println("OTA invalid command");
    return;
  }
  const bool saved = nvs.begin(kNamespace, false) && nvs.putBytes(kKeyName, key, sizeof(key)) == sizeof(key);
  mbedtls_platform_zeroize(key, sizeof(key));
  Serial.println(saved ? "OTA key saved" : "OTA could not save key");
}

esp_err_t handleOtaUpload(httpd_req_t* request, uint32_t bootId,
                          bool (*prepare)(), void (*finish)(bool)) {
  uint8_t digest[kDigestBytes];
  if (!authorize(request, bootId, digest)) {
    httpd_resp_set_status(request, "403 Forbidden");
    return httpd_resp_sendstr(request, "Update signature rejected.");
  }
  const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
  if (!partition || !request->content_len || request->content_len > partition->size) {
    httpd_resp_set_status(request, "400 Bad Request");
    return httpd_resp_sendstr(request, "Firmware image does not fit the OTA slot.");
  }
  if (!prepare()) {
    httpd_resp_set_status(request, "409 Conflict");
    return httpd_resp_sendstr(request, "Stop the wheels and wait for them to settle before uploading.");
  }

  const bool installed = writeImage(request, partition, digest);
  httpd_resp_set_status(request, installed ? "200 OK" : "400 Bad Request");
  const esp_err_t reply = httpd_resp_sendstr(request, installed ? "Firmware installed. Restarting." : "Update failed.");
  finish(installed);
  return reply;
}
