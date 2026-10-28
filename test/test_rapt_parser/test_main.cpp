#include <unity.h>

#include "rapt_parser.h"

namespace
{
void writeBigEndian16(uint8_t* destination, uint16_t value)
{
    destination[0] = static_cast<uint8_t>(value >> 8);
    destination[1] = static_cast<uint8_t>(value);
}

void writeBigEndianFloat(uint8_t* destination, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    destination[0] = static_cast<uint8_t>(bits >> 24);
    destination[1] = static_cast<uint8_t>(bits >> 16);
    destination[2] = static_cast<uint8_t>(bits >> 8);
    destination[3] = static_cast<uint8_t>(bits);
}

void testVersion2Measurements()
{
    uint8_t data[25] = {};
    data[0] = 0x52;
    data[1] = 0x41;
    data[2] = 'P';
    data[3] = 'T';
    data[4] = 2;
    writeBigEndian16(data + 11, static_cast<uint16_t>((293.15f) * 128.0f));
    writeBigEndianFloat(data + 13, 1050.0f);
    writeBigEndian16(data + 23, 80 * 256);

    RaptSample sample = {};
    TEST_ASSERT_TRUE(parseRaptAdvertisement(data, sizeof(data), sample));
    TEST_ASSERT_EQUAL_UINT8(2, sample.version);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 20.0f, sample.temperature);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.05f, sample.gravity);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, sample.batteryPercent);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.02f, sample.batteryVolts);
}

void testVersion1Measurements()
{
    uint8_t data[25] = {};
    data[0] = 0x52;
    data[1] = 0x41;
    data[2] = 'P';
    data[3] = 'T';
    data[4] = 1;
    for (uint8_t i = 0; i < 6; ++i)
    {
        data[5 + i] = i + 1;
    }
    writeBigEndian16(data + 11, static_cast<uint16_t>((293.15f) * 128.0f));
    writeBigEndianFloat(data + 13, 1005.0f);
    writeBigEndian16(data + 23, 50 * 256);

    RaptSample sample = {};
    TEST_ASSERT_TRUE(parseRaptAdvertisement(data, sizeof(data), sample));
    TEST_ASSERT_EQUAL_UINT8(1, sample.version);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.005f, sample.gravity);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, sample.batteryPercent);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.75f, sample.batteryVolts);
}

void testRejectsWrongLengthAndCompanyId()
{
    uint8_t data[25] = {};
    RaptSample sample = {};
    data[0] = 0x52;
    data[1] = 0x41;
    data[2] = 'P';
    data[3] = 'T';
    data[4] = 2;
    TEST_ASSERT_FALSE(parseRaptAdvertisement(data, sizeof(data) - 1, sample));
    data[0] = 0;
    TEST_ASSERT_FALSE(parseRaptAdvertisement(data, sizeof(data), sample));
}

void testRejectsUnavailableMeasurementSentinels()
{
    uint8_t data[25] = {};
    data[0] = 0x52;
    data[1] = 0x41;
    data[2] = 'P';
    data[3] = 'T';
    data[4] = 2;
    writeBigEndian16(data + 11, 0xffff);
    writeBigEndian16(data + 23, 100 * 256);

    RaptSample sample = {};
    TEST_ASSERT_FALSE(parseRaptAdvertisement(data, sizeof(data), sample));

    writeBigEndian16(data + 11, 30000);
    writeBigEndian16(data + 23, 0xffff);
    TEST_ASSERT_FALSE(parseRaptAdvertisement(data, sizeof(data), sample));
}
} // namespace

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(testVersion2Measurements);
    RUN_TEST(testVersion1Measurements);
    RUN_TEST(testRejectsWrongLengthAndCompanyId);
    RUN_TEST(testRejectsUnavailableMeasurementSentinels);
    UNITY_END();
}

void loop()
{
}
