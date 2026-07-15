#include "pcf85063.h"
#include "user_config.h"
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

static const char *TAG = "PCF85063";
PCF85063 g_rtc;

// Register addresses
#define REG_CTRL1      0x00
#define REG_CTRL2      0x01
#define REG_SC         0x04  // Seconds (0-59) + OS flag in bit 7
#define REG_MN         0x05  // Minutes (0-59)
#define REG_HR         0x06  // Hours (0-23)
#define REG_DY         0x07  // Day of month (1-31)
#define REG_DW         0x08  // Day of week (0-6)
#define REG_MO         0x09  // Month (1-12)
#define REG_YR         0x0A  // Year (0-99)

#define OSCILLATOR_STOPPED_FLAG 0x80  // Bit 7 of REG_SC

static i2c_master_bus_handle_t s_i2c_bus = nullptr;
static i2c_master_dev_handle_t s_rtc_dev = nullptr;

PCF85063::PCF85063() : _initialized(false) {
}

PCF85063::~PCF85063() {
}

bool PCF85063::begin() {
    if (_initialized) return true;

    // Configure I2C master bus
    i2c_master_bus_config_t i2c_bus_config = {};
    i2c_bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_bus_config.i2c_port = -1;  // Auto-select port
    i2c_bus_config.scl_io_num = ESP32_I2C_SCL_PIN;
    i2c_bus_config.sda_io_num = ESP32_I2C_SDA_PIN;
    i2c_bus_config.glitch_ignore_cnt = 7;
    i2c_bus_config.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&i2c_bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %d", err);
        return false;
    }

    // Add RTC device
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = I2C_ADDR;
    dev_cfg.scl_speed_hz = 100000;

    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_rtc_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCF85063 not found on I2C bus");
        return false;
    }

    // Ensure oscillator is running: clear STOP bit and set 24H mode
    uint8_t ctrl1 = 0;
    uint8_t ctrl1_reg = REG_CTRL1;
    if (i2c_master_transmit(s_rtc_dev, &ctrl1_reg, 1, 100) == ESP_OK &&
        i2c_master_receive(s_rtc_dev, &ctrl1, 1, 100) == ESP_OK) {
        uint8_t newCtrl1 = ctrl1;
        newCtrl1 &= ~(1 << 5);  // Clear STOP bit
        newCtrl1 &= ~(1 << 1);  // Clear 12H bit → 24H mode
        if (newCtrl1 != ctrl1) {
            uint8_t writeBuf[2] = {REG_CTRL1, newCtrl1};
            i2c_master_transmit(s_rtc_dev, writeBuf, 2, 100);
            ESP_LOGI(TAG, "Control_1 configured: 0x%02x → 0x%02x", ctrl1, newCtrl1);
        }
    }

    // Initialize Control_2 to known state
    uint8_t ctrl2Buf[2] = {REG_CTRL2, 0x00};
    i2c_master_transmit(s_rtc_dev, ctrl2Buf, 2, 100);

    _initialized = true;
    ESP_LOGI(TAG, "PCF85063 initialized successfully");

    // Log current register values for debugging
    uint8_t regs[7];
    uint8_t regAddr = REG_SC;
    if (i2c_master_transmit(s_rtc_dev, &regAddr, 1, 100) == ESP_OK &&
        i2c_master_receive(s_rtc_dev, regs, 7, 100) == ESP_OK) {
        ESP_LOGI(TAG, "RTC registers: SC=0x%02x MN=0x%02x HR=0x%02x DY=0x%02x DW=0x%02x MO=0x%02x YR=0x%02x",
                 regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6]);
    }

    return true;
}

bool PCF85063::writeTimeRegisters(const struct tm *tm) {
    if (!_initialized || !s_rtc_dev) return false;

    // Convert BCD format
    auto toBcd = [](uint8_t val) -> uint8_t {
        return ((val / 10) << 4) | (val % 10);
    };

    uint8_t buf[8];
    buf[0] = REG_SC;  // Start register address
    buf[1] = toBcd(tm->tm_sec);  // Seconds
    buf[2] = toBcd(tm->tm_min);  // Minutes
    buf[3] = toBcd(tm->tm_hour); // Hours
    buf[4] = toBcd(tm->tm_mday);      // Day of month → reg 0x07
    buf[5] = toBcd(tm->tm_wday + 1);  // Day of week → reg 0x08
    buf[6] = toBcd(tm->tm_mon + 1);  // Month (1-12)
    buf[7] = toBcd(tm->tm_year % 100);  // Year (0-99)

    esp_err_t err = i2c_master_transmit(s_rtc_dev, buf, 8, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write time to RTC: %d", err);
        return false;
    }

    // Read back and verify
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t regAddr = REG_SC;
    uint8_t verify[7];
    if (i2c_master_transmit(s_rtc_dev, &regAddr, 1, 100) == ESP_OK &&
        i2c_master_receive(s_rtc_dev, verify, 7, 100) == ESP_OK) {
        ESP_LOGI(TAG, "RTC write verify: wrote SC=0x%02x MN=0x%02x HR=0x%02x DY=0x%02x DW=0x%02x MO=0x%02x YR=0x%02x, read back SC=0x%02x MN=0x%02x HR=0x%02x DY=0x%02x DW=0x%02x MO=0x%02x YR=0x%02x",
                 buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
                 verify[0], verify[1], verify[2], verify[3], verify[4], verify[5], verify[6]);
    }

    ESP_LOGI(TAG, "RTC time set successfully");
    return true;
}

bool PCF85063::readTimeRegisters(struct tm *tm) {
    if (!_initialized || !s_rtc_dev) return false;

    // Write register address
    uint8_t reg = REG_SC;
    esp_err_t err = i2c_master_transmit(s_rtc_dev, &reg, 1, 100);
    if (err != ESP_OK) return false;

    // Read time registers
    uint8_t buf[7];
    err = i2c_master_receive(s_rtc_dev, buf, 7, 100);
    if (err != ESP_OK) return false;

    // Check oscillator stopped flag
    if (buf[0] & OSCILLATOR_STOPPED_FLAG) {
        ESP_LOGW(TAG, "RTC oscillator was stopped, clearing OS flag");
        // Clear the OS flag by writing 0 to bit 7 of seconds register
        uint8_t clearBuf[2] = {REG_SC, static_cast<uint8_t>(buf[0] & 0x7F)};
        i2c_master_transmit(s_rtc_dev, clearBuf, 2, 100);
        // Continue to use the time values (they may still be valid)
    }

    // Convert from BCD
    auto fromBcd = [](uint8_t val) -> uint8_t {
        return (val & 0x0F) + ((val >> 4) * 10);
    };

    uint8_t sec   = fromBcd(buf[0] & 0x7F);
    uint8_t min   = fromBcd(buf[1] & 0x7F);
    uint8_t hour  = fromBcd(buf[2] & 0x3F);
    uint8_t mday  = fromBcd(buf[3] & 0x3F);
    uint8_t wday  = fromBcd(buf[4] & 0x07);
    uint8_t mon   = fromBcd(buf[5] & 0x1F);
    uint16_t year = fromBcd(buf[6]) + 2000;

    // Validate time fields
    if (year < 2024 || year > 2099 ||
        mon < 1 || mon > 12 ||
        mday < 1 || mday > 31 ||
        hour > 23 || min > 59 || sec > 59) {
        ESP_LOGW(TAG, "RTC time values out of range: %04d-%02d-%02d %02d:%02d:%02d",
                 year, mon, mday, hour, min, sec);
        return false;
    }

    tm->tm_sec  = sec;
    tm->tm_min  = min;
    tm->tm_hour = hour;
    tm->tm_mday = mday;
    tm->tm_mon  = mon - 1;
    tm->tm_year = year - 1900;
    tm->tm_wday = wday;

    return true;
}

bool PCF85063::setTime(time_t unixTime) {
    struct tm *tm = localtime(&unixTime);
    if (!tm) return false;

    return writeTimeRegisters(tm);
}

time_t PCF85063::getTime() {
    struct tm tm = {};
    if (!readTimeRegisters(&tm)) return 0;

    // Set tm_isdst to -1 to let mktime determine DST
    tm.tm_isdst = -1;

    time_t t = mktime(&tm);
    return t;
}

bool PCF85063::hasValidTime() {
    struct tm tm;
    return readTimeRegisters(&tm);
}