#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

struct RaptSample
{
    float temperature;
    float gravity;
    float batteryPercent;
    float batteryVolts;
    uint8_t version;
};

inline uint16_t readRaptBigEndian16(const uint8_t* data)
{
    return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

inline uint32_t readRaptBigEndian32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

inline float readRaptBigEndianFloat(const uint8_t* data)
{
    const uint32_t bits = readRaptBigEndian32(data);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline float raptBatteryPercentToVolts(float percent)
{
    return 3.3f + (percent / 100.0f) * 0.9f;
}

inline bool parseRaptAdvertisement(const uint8_t* data, size_t length, RaptSample& sample)
{
    // NimBLE includes company ID 0x4152 ("RA"); the parser payload starts with "PT".
    if (data == nullptr || length != 25 ||
        data[0] != 0x52 || data[1] != 0x41 ||
        data[2] != 'P' || data[3] != 'T')
    {
        return false;
    }

    const uint8_t version = data[4];
    if (version != 1 && version != 2)
    {
        return false;
    }

    sample.version = version;
    sample.batteryPercent = 0.0f;

    size_t temperatureOffset;
    size_t gravityOffset;
    size_t batteryOffset;
    if (version == 1)
    {
        temperatureOffset = 11;
        gravityOffset = 13;
        batteryOffset = 23;
    }
    else
    {
        temperatureOffset = 11;
        gravityOffset = 13;
        batteryOffset = 23;
    }

    const uint16_t rawTemperature = readRaptBigEndian16(data + temperatureOffset);
    const uint16_t rawBattery = readRaptBigEndian16(data + batteryOffset);
    if (rawTemperature == 0xffff || rawBattery == 0xffff)
    {
        return false;
    }

    const float rawGravity = readRaptBigEndianFloat(data + gravityOffset);
    sample.temperature = rawTemperature / 128.0f - 273.15f;
    sample.gravity = rawGravity / 1000.0f;
    sample.batteryPercent = std::round(rawBattery / 256.0f);
    sample.batteryVolts = raptBatteryPercentToVolts(sample.batteryPercent);

    return std::isfinite(sample.temperature) &&
           std::isfinite(sample.gravity) &&
           std::isfinite(sample.batteryPercent) &&
           std::isfinite(sample.batteryVolts);
}
