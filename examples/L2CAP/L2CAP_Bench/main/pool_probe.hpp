#pragma once
#include <cJSON.h>

// Flush only outside the timed transfer and native Bluetooth callbacks.
void benchEmitPoolFailures(void (*emit)(const char*, cJSON*));
