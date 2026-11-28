#pragma once

#include <esp_http_server.h>

void handleOtaSerial(const char* command, bool idle);
esp_err_t handleOtaUpload(httpd_req_t* request, uint32_t bootId,
                          bool (*prepare)(), void (*finish)(bool));
