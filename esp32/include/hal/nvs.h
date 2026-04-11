#pragma once
// AirBridge NVS HAL — key-value storage abstraction
// Flattened API: no open/close handles. ESP32 impl wraps them internally.

#include <cstdint>
#include <cstddef>

class INvs {
public:
    virtual ~INvs() = default;

    virtual bool get_str(const char* ns, const char* key, char* out, size_t sz) = 0;
    virtual bool set_str(const char* ns, const char* key, const char* val) = 0;
    virtual bool get_u8(const char* ns, const char* key, uint8_t* out) = 0;
    virtual bool set_u8(const char* ns, const char* key, uint8_t val) = 0;
    virtual bool get_i32(const char* ns, const char* key, int32_t* out) = 0;
    virtual bool set_i32(const char* ns, const char* key, int32_t val) = 0;
    virtual bool get_u32(const char* ns, const char* key, uint32_t* out) = 0;
    virtual bool set_u32(const char* ns, const char* key, uint32_t val) = 0;
    virtual void erase_key(const char* ns, const char* key) = 0;
};
