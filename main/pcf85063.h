#pragma once

#include <cstdint>
#include <ctime>

class PCF85063 {
public:
    PCF85063();
    ~PCF85063();

    // Initialize I2C and check if RTC is present
    bool begin();

    // Set time to RTC (call after NTP sync)
    bool setTime(time_t unixTime);

    // Get time from RTC
    // Returns 0 if RTC time is invalid or not set
    time_t getTime();

    // Check if RTC has valid time
    bool hasValidTime();

private:
    bool writeTimeRegisters(const struct tm *tm);
    bool readTimeRegisters(struct tm *tm);

    bool _initialized;
    static const uint8_t I2C_ADDR = 0x51;
};

extern PCF85063 g_rtc;