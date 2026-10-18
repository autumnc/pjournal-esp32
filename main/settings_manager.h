#pragma once

#include <string>

class SettingsManager {
public:
    bool begin();
    std::string getString(const std::string &key, const std::string &def = "");
    void setString(const std::string &key, const std::string &val);
    void erase(const std::string &key);
    // 清空内存缓存: 退出 U 盘模式后调用, 让电脑改过的设置文件重新从 SD 读取。
    void invalidateCache();

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
    std::string voiceAsrService();  // "xiaozhi" 或 "baidu"
    std::string baiduAsrApiKey();
    std::string baiduAsrSecretKey();
    std::string clientId();
    bool autoSave();
    bool autoSleep();
    bool sleepScreen();
    bool markdownRender();
    bool firstLineIndent();
    bool versionHistory();
    bool recoveryDraft();
    bool verticalReferenceLine();
    int fontSize();
    std::string appMode();  // "journal"(个人日记)、"quick"(快捷编辑) 或 "file"(文件编辑)
    std::string homeView();  // "week"(周视图) 或 "month"(月视图)
    std::string inputMode();  // "normal"(正常) 或 "typewriter"(打字机)
    std::string imeFuzzy();  // comma/space separated fuzzy pinyin options
    std::string imePredictMode();  // "always" / "space" / "off"
    bool imeSentence();  // 长全拼串的整句候选
    bool imeDocContext();  // 正文已出现的词在小范围加分
    bool imeCandidateHighlight();  // 候选页内左右键高亮选择
    std::string letterSelKey();  // 字母选字触发键: tab/lctrl/lwin/lalt/ralt/rwin/rctrl/off
    bool imeDebug();  // hidden: log IME candidate scoring details
    std::string editorOrientation();  // "horizontal"(横排) 或 "vertical"(竖排)
    std::string verticalReferenceLineStyle();  // "solid"(实线) / "dash"(虚线) / "dot"(点状虚线)
    bool typingClickEnabled();  // 编辑器中是否播放打字音效(与输入模式无关)
    std::string clickChineseMode();  // "key"(每键一声) / "count"(上屏按字数) / "single"(上屏单声)
    int typingClickVolume();  // 0..100,打字机音效音量
    std::string typingClickTimbre();  // 18 种轴体采样音色 key,见 click_samples.cpp k_sets

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
