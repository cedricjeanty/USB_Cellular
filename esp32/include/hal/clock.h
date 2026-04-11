#pragma once
// AirBridge Clock HAL — monotonic time + delay abstraction

#include <cstdint>

class IClock {
public:
    virtual ~IClock() = default;

    // Monotonic milliseconds since boot
    virtual uint32_t millis() = 0;

    // Blocking delay
    virtual void delay_ms(uint32_t ms) = 0;
};
