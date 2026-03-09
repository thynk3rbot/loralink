#ifndef DEBUG_MACROS_H
#define DEBUG_MACROS_H

#include <Arduino.h>

// Uncomment to enable debug logging
#define DEBUG_MODE

#ifdef DEBUG_MODE
#define LOG_PRINT(...) Serial.print(__VA_ARGS__)
#define LOG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define LOG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define LOG_PRINT(...)
#define LOG_PRINTLN(...)
#define LOG_PRINTF(...)
#endif

#endif // DEBUG_MACROS_H
