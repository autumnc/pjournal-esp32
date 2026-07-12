#include "settings_manager.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <sys/stat.h>
#include <esp_log.h>

static const char *TAG = "Settings";
static const char *BASE_DIR = "/sdcard/settings";

SettingsManager g_settings;

static std::map<std::string, std::string> s_cache;

bool SettingsManager::begin() {
    mkdir(BASE_DIR, 0777);
    ESP_LOGI(TAG, "Settings directory: %s", BASE_DIR);
    return true;
}

std::string SettingsManager::get(const std::string &key) {
    auto it = s_cache.find(key);
    if (it != s_cache.end()) return it->second;
    std::string path = std::string(BASE_DIR) + "/" + key;
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return "";
    std::string val;
    char buf[256];
    int n;
    while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
        buf[n] = 0;
        val += buf;
    }
    fclose(f);
    s_cache[key] = val;
    return val;
}

void SettingsManager::set(const std::string &key, const std::string &val) {
    mkdir(BASE_DIR, 0777);
    std::string path = std::string(BASE_DIR) + "/" + key;
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to write %s", path.c_str());
        return;
    }
    fwrite(val.data(), 1, val.size(), f);
    fclose(f);
    s_cache[key] = val;
}

std::string SettingsManager::getString(const std::string &key, const std::string &def) {
    std::string v = get(key);
    return v.empty() ? def : v;
}

void SettingsManager::setString(const std::string &key, const std::string &val) {
    set(key, val);
}

void SettingsManager::erase(const std::string &key) {
    s_cache.erase(key);
    std::string path = std::string(BASE_DIR) + "/" + key;
    remove(path.c_str());
}

// Convenience accessors
std::string SettingsManager::flomoEmail() { return get("flomo_email"); }
std::string SettingsManager::flomoPassword() { return get("flomo_pass"); }
std::string SettingsManager::flomoToken() { return get("flomo_token"); }
std::string SettingsManager::webdavUrl() { return get("webdav_url"); }
std::string SettingsManager::webdavUsername() { return get("webdav_user"); }
std::string SettingsManager::webdavPassword() { return get("webdav_pass"); }
std::string SettingsManager::deepseekKey() { return get("deepseek_key"); }
std::string SettingsManager::personalExperience() { return get("personal_exp"); }
std::string SettingsManager::personalHobbies() { return get("personal_hob"); }
std::string SettingsManager::wifiSsid() { return get("wifi_ssid"); }
std::string SettingsManager::wifiPassword() { return get("wifi_pass"); }

void SettingsManager::setFlomoEmail(const std::string &v) { set("flomo_email", v); }
void SettingsManager::setFlomoPassword(const std::string &v) { set("flomo_pass", v); }
void SettingsManager::setFlomoToken(const std::string &v) { set("flomo_token", v); }
void SettingsManager::setWebdavUrl(const std::string &v) { set("webdav_url", v); }
void SettingsManager::setWebdavUsername(const std::string &v) { set("webdav_user", v); }
void SettingsManager::setWebdavPassword(const std::string &v) { set("webdav_pass", v); }
void SettingsManager::setDeepseekKey(const std::string &v) { set("deepseek_key", v); }
void SettingsManager::setPersonalExperience(const std::string &v) { set("personal_exp", v); }
void SettingsManager::setPersonalHobbies(const std::string &v) { set("personal_hob", v); }
void SettingsManager::setWifiSsid(const std::string &v) { set("wifi_ssid", v); }
void SettingsManager::setWifiPassword(const std::string &v) { set("wifi_pass", v); }
std::string SettingsManager::timezone() { return get("timezone"); }
std::string SettingsManager::ntpServer() { return get("ntp_server"); }
void SettingsManager::setTimezone(const std::string &v) { set("timezone", v); }
void SettingsManager::setNtpServer(const std::string &v) { set("ntp_server", v); }
