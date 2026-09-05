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
    std::string polishPrompt();
    std::string personalExperience();
    std::string personalHobbies();
    std::string wifiSsid();
    std::string wifiPassword();
    std::string timezone();
    std::string ntpServer();
    std::string xiaozhiOtaUrl();
    std::string clientId();
    bool autoSave();
    bool autoSleep();
    bool sleepScreen();
    bool markdownRender();
    bool firstLineIndent();
    bool versionHistory();
    bool recoveryDraft();
    int fontSize();
    std::string appMode();  // "journal"(个人日记) 或 "quick"(快捷编辑)
    std::string homeView();  // "week"(周视图) 或 "month"(月视图)
    std::string inputMode();  // "normal"(正常) 或 "typewriter"(打字机)
    std::string clickChineseMode();  // "key"(每键一声) / "count"(上屏按字数) / "single"(上屏单声)
    int typingClickVolume();  // 0..100,打字机音效音量
    std::string typingClickTimbre();  // 7 种音色 key,见 typing_click.cpp TIMBRES

    void setFlomoEmail(const std::string &v);
    void setFlomoPassword(const std::string &v);
    void setFlomoToken(const std::string &v);
    void setWebdavUrl(const std::string &v);
    void setWebdavUsername(const std::string &v);
    void setWebdavPassword(const std::string &v);
    void setDeepseekKey(const std::string &v);
    void setPolishPrompt(const std::string &v);
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
