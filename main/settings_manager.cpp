#include "settings_manager.h"
#include <esp_log.h>

static const char *TAG = "Settings";

SettingsManager g_settings;

bool SettingsManager::begin() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %d", ret);
        return false;
    }
    ret = nvs_open("pjournal", NVS_READWRITE, &nvs_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %d", ret);
        return false;
    }
    return true;
}

std::string SettingsManager::get(const std::string &key) {
    if (!nvs_) return "";
    size_t len = 0;
    if (nvs_get_str(nvs_, key.c_str(), NULL, &len) != ESP_OK) return "";
    std::string val(len, '\0');
    esp_err_t ret = nvs_get_str(nvs_, key.c_str(), &val[0], &len);
    if (ret != ESP_OK) return "";
    val.resize(len > 0 ? len - 1 : 0);
    return val;
}

void SettingsManager::set(const std::string &key, const std::string &val) {
    if (!nvs_) return;
    nvs_set_str(nvs_, key.c_str(), val.c_str());
    nvs_commit(nvs_);
}

std::string SettingsManager::getString(const std::string &key, const std::string &def) {
    std::string v = get(key);
    return v.empty() ? def : v;
}

void SettingsManager::setString(const std::string &key, const std::string &val) {
    set(key, val);
}

void SettingsManager::erase(const std::string &key) {
    if (nvs_) nvs_erase_key(nvs_, key.c_str());
}

// Convenience
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
