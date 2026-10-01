#pragma once
#include <cstdint>
#include <cstddef>
class TwoWire {
public:
    void    beginTransmission(uint8_t addr);
    size_t  write(uint8_t b);
    uint8_t endTransmission();
};
extern TwoWire Wire, Wire1;
