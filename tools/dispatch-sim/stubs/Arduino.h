#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstddef>
typedef uint8_t byte;
uint32_t millis();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
long random(long lo, long hi);
struct SimSerial {
    int  printf(const char* fmt, ...);
    void println(const char* s);
    void println();
    int  availableForWrite();
};
extern SimSerial Serial;
#include <string>
typedef std::string String;
