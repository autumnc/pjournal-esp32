#pragma once

#include <string>

class SettingsManager {
public:
    bool begin();
    std::string getString(const std::string &key, const std::string &def = "");
    void setString(const std::string &key, const std::string &val);
    void erase(const std::string &key);

    // Convenience accessors
    std::string flomoEmail();
    std::string flomoPassword();
    std::string flomoToken();
    std::string webdavUrl();
    std::string webdavUsername();
    std::string webdavPassword();
    std::string deepseekKey();
    std::string personalExperience();
    std::string personalHobbies();
    std::string wifiSsid();
    std::string wifiPassword();
    std::string timezone();
    std::string ntpServer();
    bool autoSave();

    void setFlomoEmail(const std::string &v);
    void setFlomoPassword(const std::string &v);
    void setFlomoToken(const std::string &v);
    void setWebdavUrl(const std::string &v);
    void setWebdavUsername(const std::string &v);
    void setWebdavPassword(const std::string &v);
    void setDeepseekKey(const std::string &v);
    void setPersonalExperience(const std::string &v);
    void setPersonalHobbies(const std::string &v);
    void setWifiSsid(const std::string &v);
    void setWifiPassword(const std::string &v);
    void setTimezone(const std::string &v);
    void setNtpServer(const std::string &v);

private:
    std::string get(const std::string &key);
    void set(const std::string &key, const std::string &val);
};

extern SettingsManager g_settings;
