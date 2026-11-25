#pragma once

#include <stdint.h>

void startEduroam();
void serviceEduroam(uint32_t now);
void handleEduroamCommand(char* command, bool idle);
