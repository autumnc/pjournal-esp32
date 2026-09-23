#include "IME.h"
#include "yong_pinyin.h"
#include "seg_table.h"
#include "trad_table.h"
#include "kaomoji_table.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <esp_log.h>
#include <esp_timer.h>
#include <sys/stat.h>

static const char *IME_TAG = "IME";
static const int64_t IME_PERF_SLOW_US = 12000;

#if PJOURNAL_IME_PERF_LOG
#define IME_PERF_NOW() esp_timer_get_time()
#else
#define IME_PERF_NOW() 0
#endif

// 用户词典持久化到 SD 卡(与设置同目录)。NVS 分区仅 24KB 且写入失败会静默
// 丢失/启动时整区擦除,改用 SD 文件后容量无上限,重启与重刷固件均保留。
static const char *USERDICT_DYNAMIC_PATH = "/sdcard/settings/userdict.txt";
static const char *USERDICT_FIXED_PATH = "/sdcard/settings/userdict_fixed.txt";
static const char *USERPREDICT_PATH = "/sdcard/settings/userpredict.txt";
static const char *ENGLISHDICT_PATH = "/sdcard/settings/englishdict.txt";
static const size_t USERDICT_FIXED_LIMIT = 500;
static const size_t USERDICT_DYNAMIC_LIMIT = 1000;
static const size_t USERPREDICT_LIMIT = 500;
static const size_t ENGLISHDICT_LIMIT = 10000;
static const int IME_KEY_UP = 0x80;
static const int IME_KEY_DOWN = 0x81;
static const int IME_KEY_LEFT = 0x82;
static const int IME_KEY_RIGHT = 0x83;
#if PJOURNAL_IME_FAST_LOOKUP
static const int IME_SEG_TABLE_MIN_LEN = 2;
static const int IME_PHRASE_PREFIX_MIN_LEN = 2;
static const int IME_USER_INITIAL_MIN_LEN = 2;
static const int IME_DICT_INITIAL_MIN_LEN = 2;
static const int IME_MAX_PHRASE_GROUP_SCAN = 900;
static const int IME_MAX_INITIAL_GROUP_SCAN = 700;
static const int IME_MAX_SHORT_INITIAL_GROUP_SCAN = 160;
static const int IME_MAX_MEDIUM_INITIAL_GROUP_SCAN = 360;
static const int IME_MAX_LONG_INITIAL_GROUP_SCAN = 220;
static const int IME_MAX_SHORTHAND_GROUP_SCAN = 900;
static const int IME_MAX_SINGLE_RECORD_SCAN = 120;
static const int IME_MAX_SHORT_CONSONANT_SINGLE_SCAN = 18;
static const int IME_MAX_PARTIAL_RECORD_SCAN = 180;
static const int IME_MAX_INITIAL_COLLECT = 72;
static const size_t IME_FAST_CANDIDATE_LIMIT = 80;
static const size_t IME_SHORT_CONSONANT_CANDIDATE_LIMIT = 12;
static const size_t IME_PARTIAL_PINYIN_CANDIDATE_LIMIT = 12;
#else
static const int IME_SEG_TABLE_MIN_LEN = 1;
static const int IME_PHRASE_PREFIX_MIN_LEN = 1;
static const int IME_USER_INITIAL_MIN_LEN = 1;
static const int IME_DICT_INITIAL_MIN_LEN = 1;
static const int IME_MAX_PHRASE_GROUP_SCAN = 5000;
static const int IME_MAX_INITIAL_GROUP_SCAN = 60000;
static const int IME_MAX_SHORT_INITIAL_GROUP_SCAN = 60000;
static const int IME_MAX_MEDIUM_INITIAL_GROUP_SCAN = 60000;
static const int IME_MAX_LONG_INITIAL_GROUP_SCAN = 60000;
static const int IME_MAX_SHORTHAND_GROUP_SCAN = 60000;
static const int IME_MAX_SINGLE_RECORD_SCAN = 1000000;
static const int IME_MAX_SHORT_CONSONANT_SINGLE_SCAN = 1000000;
static const int IME_MAX_PARTIAL_RECORD_SCAN = 1000000;
static const int IME_MAX_INITIAL_COLLECT = 300;
static const size_t IME_FAST_CANDIDATE_LIMIT = 300;
static const size_t IME_SHORT_CONSONANT_CANDIDATE_LIMIT = 300;
static const size_t IME_PARTIAL_PINYIN_CANDIDATE_LIMIT = 300;
#endif

static const char *BUILTIN_ENGLISH_WORDS[] = {
    "about", "after", "again", "also", "android", "api", "app", "apple",
    "backup", "because", "before", "between", "build", "cache", "calendar",
    "change", "cloud", "code", "commit", "config", "content", "context",
    "data", "debug", "device", "document", "editor", "email", "error",
    "event", "export", "feature", "file", "filter", "firmware", "flash",
    "format", "function", "github", "hello", "history", "image", "import",
    "input", "issue", "journal", "keyboard", "local", "manager", "markdown",
    "memory", "message", "network", "note", "openai", "output", "password",
    "plugin", "project", "prompt", "python", "release", "request", "screen",
    "search", "setting", "storage", "sync", "system", "task", "today",
    "token", "update", "upload", "user", "version", "voice", "wifi", "word",
    "work", "write"
};

struct BuiltinPredictEntry {
    const char *key;
    const char *candidates[9];
};

static const BuiltinPredictEntry BUILTIN_PREDICT[] = {
    {"我", {"们", "的", "也", "想", "是", "在", "会", "要", nullptr}},
    {"你", {"好", "们", "的", "也", "是", "在", "要", "看", nullptr}},
    {"他", {"们", "的", "也", "是", "在", "说", "会", "要", nullptr}},
    {"她", {"们", "的", "也", "是", "在", "说", "会", "要", nullptr}},
    {"它", {"们", "的", "是", "在", "会", "也", "有", "就", nullptr}},
    {"这", {"个", "样", "里", "些", "是", "种", "么", "次", nullptr}},
    {"那", {"个", "样", "里", "些", "是", "么", "种", "次", nullptr}},
    {"不", {"是", "会", "能", "要", "用", "知道", "过", "太", nullptr}},
    {"没", {"有", "事", "关系", "办法", "必要", "问题", "想到", "看到", nullptr}},
    {"有", {"点", "些", "时候", "一个", "没有", "可能", "什么", "问题", nullptr}},
    {"可", {"以", "能", "是", "爱", "惜", "见", "用", "怕", nullptr}},
    {"会", {"有", "不会", "觉得", "看到", "出现", "影响", "变成", "继续", nullptr}},
    {"想", {"到", "要", "了", "起来", "一下", "办法", "清楚", "知道", nullptr}},
    {"要", {"是", "不要", "把", "做", "看", "写", "用", "说", nullptr}},
    {"在", {"这里", "一起", "里面", "这个", "那边", "做", "看", "写", nullptr}},
    {"就", {"是", "会", "可以", "这样", "不用", "好了", "知道", "开始", nullptr}},
    {"都", {"是", "可以", "有", "没有", "会", "要", "在", "能", nullptr}},
    {"很", {"多", "好", "快", "难", "重要", "舒服", "清楚", "简单", nullptr}},
    {"太", {"多", "好了", "难", "晚", "快", "慢", "重要", "麻烦", nullptr}},
    {"好", {"的", "了", "像", "看", "用", "一点", "起来", "多", nullptr}},
    {"大", {"概", "家", "部分", "概是", "多数", "小", "概念", "量", nullptr}},
    {"小", {"心", "事", "时候", "朋友", "问题", "工具", "一点", "结", nullptr}},
    {"中", {"国", "文", "心", "午", "间", "断", "央", "年", nullptr}},
    {"国", {"内", "外", "家", "语", "际", "人", "民", "庆", nullptr}},
    {"今", {"天", "晚", "年", "后", "日", "早", "下午", "上午", nullptr}},
    {"明", {"天", "白", "年", "确", "显", "亮", "日", "早", nullptr}},
    {"昨", {"天", "晚", "日", "年", "夜", "天下午", "天晚上", "天上午", nullptr}},
    {"时", {"候", "间", "候", "刻", "常", "不时", "而", "差", nullptr}},
    {"日", {"记", "常", "期", "子", "程", "本", "后", "落", nullptr}},
    {"工", {"作", "具", "程", "资", "厂", "位", "人", "业", nullptr}},
    {"学", {"习", "校", "会", "生", "到", "术", "问", "院", nullptr}},
    {"写", {"下", "完", "一下", "出来", "进去", "好", "作", "入", nullptr}},
    {"看", {"看", "到", "一下", "起来", "见", "完", "过", "法", nullptr}},
    {"做", {"好", "完", "到", "一下", "出来", "法", "成", "过", nullptr}},
    {"用", {"来", "了", "一下", "起来", "户", "法", "得", "处", nullptr}},
    {"说", {"明", "了", "一下", "起来", "法", "到", "完", "不定", nullptr}},
    {"输", {"入", "出", "法", "错", "给", "送", "赢", "血", nullptr}},
    {"入", {"法", "口", "门", "手", "职", "睡", "选", "库", nullptr}},
    {"候", {"选", "补", "鸟", "车", "机", "诊", "审", "场", nullptr}},
    {"选", {"择", "项", "中", "候", "取", "词", "出", "举", nullptr}},
    {"词", {"组", "库", "语", "典", "频", "条", "汇", "义", nullptr}},
    {"字", {"符", "体", "词", "段", "节", "数", "幕", "形", nullptr}},
    {"功", {"能", "课", "夫", "耗", "率", "德", "效", "成", nullptr}},
    {"能", {"够", "不能", "力", "用", "看到", "实现", "支持", "继续", nullptr}},
    {"支", {"持", "付", "架", "线", "出", "撑", "配", "点", nullptr}},
    {"修", {"改", "复", "正", "饰", "订", "炼", "理", "行", nullptr}},
    {"优", {"化", "先", "点", "雅", "势", "秀", "惠", "良", nullptr}},
    {"问", {"题", "一下", "候", "问", "答", "号", "清楚", "出来", nullptr}},
    {"题", {"目", "外", "材", "库", "型", "解", "名", "意", nullptr}},
    {"拼", {"音", "写", "起来", "出来", "错", "一下", "读", "接", nullptr}},
    {"码", {"表", "字", "长", "本", "率", "序", "位", "元", nullptr}},
    {"固", {"件", "定", "化", "有", "态", "守", "执", "然", nullptr}},
    {"蓝", {"牙", "色", "图", "本", "屏", "牙键盘", "牙连接", "牙设备", nullptr}},
    {"键", {"盘", "入", "值", "位", "帽", "盘连接", "盘电量", "盘输入", nullptr}},
    {"屏", {"幕", "显", "保", "蔽", "幕显示", "幕刷新", "幕亮度", "幕内容", nullptr}},
    {"设", {"置", "备", "计", "定", "成", "为", "法", "想", nullptr}},
    {"同", {"步", "时", "意", "样", "步失败", "步完成", "步文件", "步数据", nullptr}},
    {"语", {"音", "句", "义", "文", "音输入", "音识别", "音转写", "音文件", nullptr}},
    {"识", {"别", "字", "破", "别结果", "别失败", "别文本", "别内容", nullptr}},
    {"刷", {"机", "新", "写", "卡", "机包", "机命令", "机失败", "机成功", nullptr}},
    {"启", {"动", "用", "发", "示", "动慢", "动界面", "动失败", "动完成", nullptr}},
    {"卡", {"顿", "住", "片", "死", "顿问题", "顿程度", "顿原因", "顿日志", nullptr}},
    {"日", {"记", "常", "期", "子", "程", "本", "后", "落", nullptr}},
    {"笔", {"记", "画", "者", "录", "记本", "记内容", "记文件", "记同步", nullptr}},
    {"灵", {"感", "活", "魂", "敏", "感记录", "感片段", "感整理", "感来源", nullptr}},
    {"大", {"概", "家", "纲", "部分", "多数", "小", "概念", "量", nullptr}},
    {"任", {"务", "何", "凭", "性", "务列表", "务管理", "务完成", "务同步", nullptr}},
    {"待", {"办", "会", "处理", "确定", "续", "命", "机", "选", nullptr}},
    {"保", {"存", "持", "护", "留", "证", "密", "险", "守", nullptr}},
    {"搜", {"索", "寻", "到", "一下", "索结果", "索内容", "索文件", "索词", nullptr}},
};

static std::string pinyinJoinedCode(const std::vector<ime::PinyinToken> &tokens) {
    std::string out;
    for (auto &t : tokens) out += t.text;
    return out;
}

static bool pinyinSegmentsMatch(const std::vector<ime::PinyinToken> &typed,
                                const std::vector<std::string> &entry) {
    if (typed.empty() || typed.size() > entry.size()) return false;
    for (size_t i = 0; i < typed.size(); i++) {
        if (typed[i].text.size() > entry[i].size()) return false;
        if (strncmp(typed[i].text.c_str(), entry[i].c_str(), typed[i].text.size()) != 0)
            return false;
    }
    return true;
}

static std::vector<std::string> splitSyllableText(const char *text) {
    std::vector<std::string> out;
    std::string cur;
    for (const char *p = text; *p; p++) {
        if (*p == ' ' || *p == '\'') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::string pinyinInitialCodeCompat(const std::string &code) {
    std::string init = ime::PinyinEngine::initialCode(code);
    if (!init.empty()) return init;
    const char *wc = code.c_str();
    int cl = (int)code.length();
    char fallback[13]; int o = 0;
    for (int i = 0; i < cl && o < 12; ) {
        if (strchr("aeiouv", wc[i])) {
            while (i < cl && strchr("aeiouvngr", wc[i]) && o < 12)
                fallback[o++] = wc[i++];
            continue;
        }
        if (i + 1 < cl && (wc[i] == 'z' || wc[i] == 'c' || wc[i] == 's') && wc[i + 1] == 'h') {
            fallback[o++] = wc[i];
            fallback[o++] = wc[i + 1];
            i += 2;
        } else {
            fallback[o++] = wc[i++];
        }
        while (i < cl && strchr("aeiouv", wc[i])) i++;
        if (i < cl && strchr("ngr", wc[i])) {
            int j = i;
            while (j < cl && strchr("ngr", wc[j])) j++;
            if (j >= cl || !strchr("aeiouv", wc[j])) i = j;
        }
    }
    fallback[o] = 0;
    return fallback;
}

static bool pinyinInitialStartsWithCompat(const char *wc, int cl, const char *typed, int typedLen) {
    int o = 0;
    auto pushInit = [&](char ch) -> bool {
        if (o < typedLen && ch != typed[o]) return false;
        o++;
        return true;
    };
    for (int i = 0; i < cl && o < 12; ) {
        if (strchr("aeiouv", wc[i])) {
            while (i < cl && strchr("aeiouvngr", wc[i]) && o < 12) {
                if (!pushInit(wc[i++])) return false;
                if (o >= typedLen) return true;
            }
            continue;
        }
        if (i + 1 < cl && (wc[i] == 'z' || wc[i] == 'c' || wc[i] == 's') && wc[i + 1] == 'h') {
            if (!pushInit(wc[i])) return false;
            if (o >= typedLen) return true;
            if (!pushInit(wc[i + 1])) return false;
            i += 2;
        } else {
            if (!pushInit(wc[i++])) return false;
        }
        if (o >= typedLen) return true;
        while (i < cl && strchr("aeiouv", wc[i])) i++;
        if (i < cl && strchr("ngr", wc[i])) {
            int j = i;
            while (j < cl && strchr("ngr", wc[j])) j++;
            if (j >= cl || !strchr("aeiouv", wc[j])) i = j;
        }
    }
    return o >= typedLen;
}

static int userCandidateScore(const std::string &entryCode, int count, int typedLen) {
    int score = std::min(count, 2000) * 8;
    if ((int)entryCode.length() == typedLen) score += 100000;
    else score += std::max(0, 64 - ((int)entryCode.length() - typedLen));
    return score;
}

static bool pinyinCodeEqualsAny(const std::string &matchedCode, const std::string &primaryCode,
                                const std::vector<std::string> &aliasCodes) {
    if (matchedCode == primaryCode) return true;
    for (auto &aliasCode : aliasCodes) {
        if (matchedCode == aliasCode) return true;
    }
    return false;
}

static int phraseCandidateScore(const std::string &word, int candLen, int typedLen, int syllableCount) {
    int score = std::min(candLen, typedLen) * 1000;
    if (candLen == typedLen) score += 100000;
    else if (candLen > typedLen) score -= std::min(8000, (candLen - typedLen) * 1200);
    int chars = (int)(word.length() / 3);
    if (syllableCount > 0) score += std::max(0, 64 - std::abs(chars - syllableCount) * 16);
    if (chars >= 2) score += 8;
    return score;
}

static int initialPhraseCandidateScoreFromLength(int initLen, int typedLen,
                                                 const std::string &word) {
    if (initLen < typedLen) return -1;
    int score = 0;
    if (initLen == typedLen) score += 100000;
    else score += std::max(0, 80 - (initLen - typedLen) * 16);
    int chars = (int)(word.length() / 3);
    score += std::max(0, 80 - std::abs(chars - typedLen) * 20);
    if (chars == typedLen) score += 5000;
    if (chars >= 2) score += 8;
    return score;
}

static std::string userInitialForCode(const std::string &code) {
    if (code.find('\'') != std::string::npos) return "";
    return pinyinInitialCodeCompat(code);
}

static std::string zeroInitialAlias(const std::string &code) {
    if (code == "i") return "yi";
    if (code == "ia") return "ya";
    if (code == "ian") return "yan";
    if (code == "iang") return "yang";
    if (code == "iao") return "yao";
    if (code == "ie") return "ye";
    if (code == "in") return "yin";
    if (code == "ing") return "ying";
    if (code == "iong") return "yong";
    if (code == "iu" || code == "iou") return "you";
    if (code == "u") return "wu";
    if (code == "ua") return "wa";
    if (code == "uai") return "wai";
    if (code == "uan") return "wan";
    if (code == "uang") return "wang";
    if (code == "uei" || code == "ui") return "wei";
    if (code == "uen" || code == "un") return "wen";
    if (code == "uo") return "wo";
    if (code == "ve" || code == "ue") return "yue";
    return "";
}

static std::string leadingZeroInitialAliasCode(const std::string &code) {
    int maxLen = std::min<int>(5, code.length());
    for (int len = maxLen; len >= 1; len--) {
        std::string alias = zeroInitialAlias(code.substr(0, len));
        if (!alias.empty()) return alias + code.substr(len);
    }
    return "";
}

static std::string pinyinSpellingAliasCode(const std::string &code) {
    if (code.empty()) return "";
    std::string out = code;
    bool changed = false;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] == 'v' && i > 0 && strchr("jqxy", out[i - 1])) {
            out[i] = 'u';
            changed = true;
        }
    }
    for (size_t i = 0; i + 2 < out.size(); i++) {
        if ((out[i] == 'l' || out[i] == 'n') && out[i + 1] == 'u' && out[i + 2] == 'e') {
            out[i + 1] = 'v';
            changed = true;
        }
    }
    return changed ? out : "";
}

static void addUniqueString(std::vector<std::string> &items, const std::string &value) {
    if (value.empty()) return;
    for (auto &item : items) if (item == value) return;
    items.push_back(value);
}

static std::vector<std::string> alternateInputCodes(const std::string &code) {
    std::vector<std::string> out;
    std::string leading = leadingZeroInitialAliasCode(code);
    addUniqueString(out, leading);
    std::string spelling = pinyinSpellingAliasCode(code);
    addUniqueString(out, spelling);
    if (!spelling.empty()) addUniqueString(out, leadingZeroInitialAliasCode(spelling));
    return out;
}

static std::vector<std::string> alternateLearningCodes(const std::string &code) {
    std::vector<std::string> out;
    std::string compact = ime::PinyinEngine::removeSplit(ime::PinyinEngine::normalize(code));
    if (!compact.empty() && compact != code) out.push_back(compact);
    for (auto &alias : alternateInputCodes(compact)) {
        if (alias != code && alias != compact) addUniqueString(out, alias);
    }
    return out;
}

static bool userCodeMatchesPrefix(const std::string &entryCode, const char *typed, int typedLen,
                                  std::string *matchedCode = nullptr) {
    if ((int)entryCode.length() >= typedLen && strncmp(entryCode.c_str(), typed, typedLen) == 0) {
        if (matchedCode) *matchedCode = entryCode;
        return true;
    }
    if (entryCode.find('\'') == std::string::npos) return false;
    std::string compact = ime::PinyinEngine::removeSplit(entryCode);
    if ((int)compact.length() >= typedLen && strncmp(compact.c_str(), typed, typedLen) == 0) {
        if (matchedCode) *matchedCode = compact;
        return true;
    }
    return false;
}

struct ImePerfTrace {
    const std::string &code;
    const std::vector<std::string> &candidates;
    int64_t startUs = IME_PERF_NOW();
    int64_t setupUs = 0;
    int64_t userUs = 0;
    int64_t singleUs = 0;
    int64_t segUs = 0;
    int64_t userPhraseUs = 0;
    int64_t phraseUs = 0;
    int64_t phraseSortUs = 0;
    int64_t userInitialUs = 0;
    int64_t initialUs = 0;
    int64_t shorthandUs = 0;
    int64_t partialUs = 0;
    bool hasVowel = false;
    bool incomplete = false;
    bool fixedPaging = false;
    size_t limit = 0;
    const char *exitName = "end";

    ImePerfTrace(const std::string &c, const std::vector<std::string> &cand)
        : code(c), candidates(cand) {}

    ~ImePerfTrace() {
        int64_t totalUs = IME_PERF_NOW() - startUs;
        if (totalUs < IME_PERF_SLOW_US &&
            setupUs < IME_PERF_SLOW_US &&
            userUs < IME_PERF_SLOW_US &&
            singleUs < IME_PERF_SLOW_US &&
            segUs < IME_PERF_SLOW_US &&
            phraseUs < IME_PERF_SLOW_US &&
            initialUs < IME_PERF_SLOW_US &&
            shorthandUs < IME_PERF_SLOW_US &&
            partialUs < IME_PERF_SLOW_US)
            return;
        ESP_LOGW(IME_TAG,
                 "perf lookup code='%s' exit=%s total=%lldus cand=%u limit=%u hv=%d inc=%d fixed=%d "
                 "setup=%lld user=%lld single=%lld seg=%lld userPhrase=%lld phrase=%lld sort=%lld "
                 "userInit=%lld init=%lld shorthand=%lld partial=%lld",
                 code.c_str(), exitName, (long long)totalUs, (unsigned)candidates.size(),
                 (unsigned)limit, hasVowel ? 1 : 0, incomplete ? 1 : 0, fixedPaging ? 1 : 0,
                 (long long)setupUs, (long long)userUs, (long long)singleUs,
                 (long long)segUs, (long long)userPhraseUs, (long long)phraseUs,
                 (long long)phraseSortUs, (long long)userInitialUs,
                 (long long)initialUs, (long long)shorthandUs, (long long)partialUs);
    }
};

static void appendUtf8(uint32_t cp, std::string &out) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static std::string lastUtf8Char(const std::string &text) {
    if (text.empty()) return "";
    size_t pos = text.size() - 1;
    while (pos > 0 && (((unsigned char)text[pos] & 0xC0) == 0x80)) pos--;
    return text.substr(pos);
}

static size_t utf8CharEnd(const std::string &text, size_t pos) {
    if (pos >= text.size()) return text.size();
    size_t next = pos + 1;
    while (next < text.size() && (((unsigned char)text[next] & 0xC0) == 0x80)) next++;
    return next;
}

static std::string utf8CharAt(const std::string &text, size_t pos) {
    if (pos >= text.size()) return "";
    size_t end = utf8CharEnd(text, pos);
    return text.substr(pos, end - pos);
}

static uint32_t utf8CodepointAt(const std::string &text, size_t pos) {
    if (pos >= text.size()) return 0;
    const unsigned char *s = (const unsigned char *)text.data();
    unsigned char c = s[pos];
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && pos + 1 < text.size())
        return ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[pos + 1] & 0x3F);
    if ((c & 0xF0) == 0xE0 && pos + 2 < text.size())
        return ((uint32_t)(c & 0x0F) << 12) |
               ((uint32_t)(s[pos + 1] & 0x3F) << 6) |
               (uint32_t)(s[pos + 2] & 0x3F);
    return 0;
}

static bool isCjkChar(const std::string &text) {
    uint32_t cp = utf8CodepointAt(text, 0);
    return (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

static bool validPredictEntry(const std::string &key, const std::string &word) {
    if (!isCjkChar(key) || !isCjkChar(utf8CharAt(word, 0))) return false;
    int chars = 0;
    for (size_t pos = 0; pos < word.size() && chars <= 4; ) {
        std::string ch = utf8CharAt(word, pos);
        if (!isCjkChar(ch)) return false;
        pos = utf8CharEnd(word, pos);
        chars++;
    }
    return chars >= 1 && chars <= 4;
}

// 词组是否含繁体字形(trad_table.h 位图, U+346E-U+9FD3)。
static bool wordHasTrad(const std::string &w) {
    const char *p = w.c_str();
    while (*p) {
        unsigned char c = (unsigned char)*p;
        uint32_t cp;
        if (c < 0x80) { cp = c; p += 1; }
        else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F); p += 2; }
        else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F); p += 3; }
        else if ((c & 0xF8) == 0xF0) { cp = ((c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) | (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F); p += 4; }
        else { p += 1; continue; }
        if (cp >= kTradLo && cp <= kTradHi) {
            uint32_t off = cp - kTradLo;
            if ((kTradBitmap[off / 8] >> (off % 8)) & 1) return true;
        }
    }
    return false;
}

// 词组在当前显示模式下是否显示: 繁体模式隐藏含简体字形的词组(wf!=0);
// 简体模式隐藏含繁体字形的词组; 单字(≤3字节)不受影响。
static bool wordVisible(bool trad, const std::string &w, uint8_t wf) {
    if (trad) return wf == 0;
    if (w.size() <= 3) return true;
    return !wordHasTrad(w);
}

// Embedded dictionary symbols (from CMakeLists EMBED_FILES "ime/ime_table_pinyin.bin")
extern const uint8_t ime_table_pinyin_bin_start[] asm("_binary_ime_table_pinyin_bin_start");
extern const uint8_t ime_table_pinyin_bin_end[]   asm("_binary_ime_table_pinyin_bin_end");

#if PJOURNAL_IME_ENABLE_LIANGFEN
// Embedded liangfen dictionary
extern const uint8_t liangfen_bin_start[] asm("_binary_liangfen_bin_start");
extern const uint8_t liangfen_bin_end[]   asm("_binary_liangfen_bin_end");
#endif

// Embedded English word list
extern const uint8_t english_words_txt_start[] asm("_binary_english_words_txt_start");
extern const uint8_t english_words_txt_end[]   asm("_binary_english_words_txt_end");

static inline std::string str_trim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool IME::parseHeader(const uint8_t *hdrIndex, size_t total) {
    if (!_dict.parse(hdrIndex, total)) {
        ESP_LOGE(IME_TAG, "bad dictionary magic");
        return false;
    }
    _scheme = (Scheme)_dict.scheme();
    _codeLen = _dict.codeLen();
    _recordSize = _dict.recordSize();
    switch (_scheme) {
    case PINYIN:    _maxCode = 63; break;
    case SHUANGPIN: _maxCode = 2; break;
    case WUBI:
    default:        _maxCode = 4; break;
    }
    _count = _dict.singleCount();
    _recordBase = HEADER_SIZE + (size_t)INDEX_ENTRIES * 4;
    return true;
}

bool IME::begin() {
    if (_loaded) return true;
    _blob = ime_table_pinyin_bin_start;
    _blobSize = (size_t)(ime_table_pinyin_bin_end - ime_table_pinyin_bin_start);
    if (_blobSize < HEADER_SIZE || !parseHeader(_blob, _blobSize)) {
        _blob = nullptr;
        return false;
    }
    _loaded = true;
    static const char *NAMES[] = {"Wubi", "Pinyin", "Shuangpin"};
    ESP_LOGI(IME_TAG, "ready: %s, %u records, codeLen %d",
             NAMES[_scheme <= SHUANGPIN ? _scheme : 0], (unsigned)_count, _codeLen);
    return true;
}

bool IME::loadUserDictFile(const char *path, std::vector<UserEntry> &entries,
                           bool &dirty, size_t maxEntries) {
    entries.clear();
    FILE *f = fopen(path, "r");
    if (!f) { dirty = false; return false; }

    // Read the whole file
    std::string allData;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) allData.append(buf, n);
    fclose(f);
    if (allData.empty()) { dirty = false; return true; }

    // Parse lines (same format as before: "code word count" per line)
    const size_t MAX_READ = 64 * 1024;
    bool hadDuplicates = false;
    size_t pos = 0;
    size_t bytesRead = 0;
    while (pos < allData.length() && bytesRead < MAX_READ) {
        size_t nl = allData.find('\n', pos);
        std::string line;
        if (nl == std::string::npos) {
            line = allData.substr(pos);
            pos = allData.length();
        } else {
            line = allData.substr(pos, nl - pos);
            pos = nl + 1;
        }
        bytesRead += line.length() + 1;
        line = str_trim(line);
        if (line.length() < 3) continue;
        auto sp1 = line.find(' ');
        if (sp1 == std::string::npos || sp1 < 1) continue;
        std::string code = line.substr(0, sp1);
        auto sp2 = line.find(' ', sp1 + 1);
        std::string word;
        int count = 1;
        bool trad = false;
        if (sp2 != std::string::npos) {
            word = line.substr(sp1 + 1, sp2 - sp1 - 1);
            std::string tail = line.substr(sp2 + 1);
            auto sp3 = tail.find(' ');
            // 纯数字解析:std::stoi 遇到非数字会抛异常,而本工程 C++ 异常关闭,
            // 畸形/被手工编辑的词库文件会让 stoi 直接 abort 重启。非法计数回退为 1。
            count = 1;
            std::string cntStr = (sp3 == std::string::npos) ? tail : tail.substr(0, sp3);
            bool validCount = !cntStr.empty();
            for (char cc : cntStr)
                if (cc < '0' || cc > '9') { validCount = false; break; }
            if (validCount) {
                count = 0;
                for (char cc : cntStr) count = count * 10 + (cc - '0');
            }
            if (count < 1) count = 1;
            if (sp3 != std::string::npos) {
                std::string fl = tail.substr(sp3 + 1);
                if (fl == "1" || fl == "t" || fl == "T") trad = true;
            }
        } else {
            word = line.substr(sp1 + 1);
        }
        if (code.length() >= 1 && word.length() >= 2) {
            bool isDup = false;
            for (auto &existing : entries) {
                if (existing.code == code && existing.word == word && existing.trad == trad) {
                    isDup = true;
                    hadDuplicates = true;
                    if (existing.count < count) existing.count = count;
                    break;
                }
            }
            if (!isDup) {
                if (entries.size() >= maxEntries) {
                    compactUserEntries(entries, maxEntries);
                    if (entries.size() >= maxEntries) entries.pop_back();
                    hadDuplicates = true;
                }
                entries.push_back({code, word, count, trad, userInitialForCode(code)});
            }
        }
    }
    if (compactUserEntries(entries, maxEntries)) hadDuplicates = true;
    if (hadDuplicates) {
        // 只在真正合并了计数时才需要保存
        dirty = true;
        saveUserDictFile(path, entries, dirty);
    } else {
        dirty = false;  // 无重复，无需保存
    }
    if (entries.size() > 0)
        ESP_LOGI(IME_TAG, "loaded %zu user words from %s", entries.size(), path);
    return true;
}

void IME::loadUserDict() {
    loadUserDictFile(USERDICT_FIXED_PATH, _fixedUserWords, _fixedUserDirty, USERDICT_FIXED_LIMIT);
    loadUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty, USERDICT_DYNAMIC_LIMIT);
    loadUserDictFile(USERPREDICT_PATH, _userPredictWords, _userPredictDirty, USERPREDICT_LIMIT);
    for (auto it = _userPredictWords.begin(); it != _userPredictWords.end(); ) {
        if (!validPredictEntry(it->code, it->word)) {
            it = _userPredictWords.erase(it);
            _userPredictDirty = true;
        } else {
            ++it;
        }
    }
    if (_userPredictDirty) saveUserDictFile(USERPREDICT_PATH, _userPredictWords, _userPredictDirty);
    _userDictLoaded = true;
}

void IME::ensureUserDictLoaded() {
    if (!_userDictLoaded) loadUserDict();
}

bool IME::compactUserEntries(std::vector<UserEntry> &entries, size_t limit) {
    bool changed = false;
    std::stable_sort(entries.begin(), entries.end(),
        [](const UserEntry &a, const UserEntry &b) {
            return a.count > b.count;
        });
    if (entries.size() > limit) {
        entries.resize(limit);
        changed = true;
    }
    return changed;
}

void IME::saveUserDictFile(const char *path, std::vector<UserEntry> &entries, bool &dirty) {
    if (!dirty) return;
    compactUserEntries(entries, entries.size());
    mkdir("/sdcard/settings", 0777);
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(IME_TAG, "failed to open userdict file %s", path);
        return;
    }

    for (auto &p : entries) {
        std::string line = p.code + " " + p.word + " " + std::to_string(p.count)
                         + (p.trad ? " 1" : "") + "\n";
        fwrite(line.data(), 1, line.size(), f);
    }

    if (fclose(f) != 0)
        ESP_LOGE(IME_TAG, "failed to flush userdict file");
    dirty = false;
}

void IME::addUserWord(const std::string &code, const std::string &word) {
    ensureUserDictLoaded();
    if (word.length() < 3 || code.length() == 0) return;
    for (auto &p : _fixedUserWords)
        if (p.code == code && p.word == word && p.trad == _trad) return;
    for (auto &p : _dynamicUserWords)
        if (p.code == code && p.word == word && p.trad == _trad) return;
    if (_dynamicUserWords.size() >= USERDICT_DYNAMIC_LIMIT) {
        compactUserEntries(_dynamicUserWords, USERDICT_DYNAMIC_LIMIT);
        if (_dynamicUserWords.size() >= USERDICT_DYNAMIC_LIMIT) _dynamicUserWords.pop_back();
    }
    _dynamicUserWords.push_back({code, word, 0, _trad, userInitialForCode(code)});
    _dynamicUserDirty = true;
    saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
}

void IME::removeUserWord(const std::string &code, const std::string &word) {
    ensureUserDictLoaded();
    for (auto it = _dynamicUserWords.begin(); it != _dynamicUserWords.end(); ++it) {
        if (it->code == code && it->word == word && it->trad == _trad) {
            _dynamicUserWords.erase(it);
            _dynamicUserDirty = true;
            saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
            return;
        }
    }
}

void IME::clearUserDict() {
    ensureUserDictLoaded();
    if (_dynamicUserWords.empty()) return;
    _dynamicUserWords.clear();
    _dynamicUserDirty = true;
    saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
}

void IME::pruneUserDict(int minCount) {
    ensureUserDictLoaded();
    auto it = _dynamicUserWords.begin();
    while (it != _dynamicUserWords.end()) {
        if (it->count < minCount) {
            it = _dynamicUserWords.erase(it);
            _dynamicUserDirty = true;
        } else ++it;
    }
    if (_dynamicUserDirty) saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
}

const std::vector<IME::UserEntryView> IME::userDictEntries(UserDictKind kind) const {
    const std::vector<UserEntry> &src =
        (kind == FIXED_DICT) ? _fixedUserWords :
        (kind == PREDICT_DICT) ? _userPredictWords : _dynamicUserWords;
    std::vector<UserEntryView> out;
    out.reserve(src.size());
    for (auto &p : src) out.push_back({p.code, p.word, p.count, p.trad});
    return out;
}

bool IME::addUserDictEntry(UserDictKind kind, const std::string &code, const std::string &word) {
    ensureUserDictLoaded();
    if (word.length() < 3 || code.length() == 0) return false;
    if (kind == PREDICT_DICT && !validPredictEntry(code, word)) return false;
    std::vector<UserEntry> &entries =
        (kind == FIXED_DICT) ? _fixedUserWords :
        (kind == PREDICT_DICT) ? _userPredictWords : _dynamicUserWords;
    bool &dirty =
        (kind == FIXED_DICT) ? _fixedUserDirty :
        (kind == PREDICT_DICT) ? _userPredictDirty : _dynamicUserDirty;
    const char *path =
        (kind == FIXED_DICT) ? USERDICT_FIXED_PATH :
        (kind == PREDICT_DICT) ? USERPREDICT_PATH : USERDICT_DYNAMIC_PATH;
    size_t limit =
        (kind == FIXED_DICT) ? USERDICT_FIXED_LIMIT :
        (kind == PREDICT_DICT) ? USERPREDICT_LIMIT : USERDICT_DYNAMIC_LIMIT;
    for (auto &p : entries) {
        if (p.code == code && p.word == word && p.trad == _trad) {
            p.count++;
            dirty = true;
            saveUserDictFile(path, entries, dirty);
            return true;
        }
    }
    if (entries.size() >= limit) {
        if (kind == FIXED_DICT) return false;
        compactUserEntries(entries, limit);
        if (entries.size() >= limit) entries.pop_back();
    }
    entries.push_back({code, word, 1, _trad, userInitialForCode(code)});
    dirty = true;
    saveUserDictFile(path, entries, dirty);
    return true;
}

void IME::removeUserDictEntries(UserDictKind kind, const std::vector<int> &indices) {
    ensureUserDictLoaded();
    std::vector<UserEntry> &entries =
        (kind == FIXED_DICT) ? _fixedUserWords :
        (kind == PREDICT_DICT) ? _userPredictWords : _dynamicUserWords;
    bool &dirty =
        (kind == FIXED_DICT) ? _fixedUserDirty :
        (kind == PREDICT_DICT) ? _userPredictDirty : _dynamicUserDirty;
    const char *path =
        (kind == FIXED_DICT) ? USERDICT_FIXED_PATH :
        (kind == PREDICT_DICT) ? USERPREDICT_PATH : USERDICT_DYNAMIC_PATH;
    for (int n = (int)entries.size() - 1; n >= 0; n--) {
        if (std::find(indices.begin(), indices.end(), n) != indices.end()) {
            entries.erase(entries.begin() + n);
            dirty = true;
        }
    }
    if (dirty) saveUserDictFile(path, entries, dirty);
}

void IME::clearUserDict(UserDictKind kind) {
    ensureUserDictLoaded();
    std::vector<UserEntry> &entries =
        (kind == FIXED_DICT) ? _fixedUserWords :
        (kind == PREDICT_DICT) ? _userPredictWords : _dynamicUserWords;
    bool &dirty =
        (kind == FIXED_DICT) ? _fixedUserDirty :
        (kind == PREDICT_DICT) ? _userPredictDirty : _dynamicUserDirty;
    const char *path =
        (kind == FIXED_DICT) ? USERDICT_FIXED_PATH :
        (kind == PREDICT_DICT) ? USERPREDICT_PATH : USERDICT_DYNAMIC_PATH;
    if (entries.empty()) return;
    entries.clear();
    dirty = true;
    saveUserDictFile(path, entries, dirty);
}

size_t IME::userDictSize(UserDictKind kind) const {
    return (kind == FIXED_DICT) ? _fixedUserWords.size() :
           (kind == PREDICT_DICT) ? _userPredictWords.size() : _dynamicUserWords.size();
}

void IME::loadEnglishDict() {
    if (_englishDictLoaded) return;
    _englishDictLoaded = true;
    _englishWords.clear();

    auto addWord = [&](const std::string &raw) {
        std::string w = str_trim(raw);
        if (w.empty() || _englishWords.size() >= ENGLISHDICT_LIMIT) return;
        bool ok = true;
        for (char c : w) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  c == '\'' || c == '-' || c == '_')) {
                ok = false;
                break;
            }
        }
        if (ok) {
            for (char &c : w) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            _englishWords.push_back(w);
        }
    };

    FILE *f = fopen(ENGLISHDICT_PATH, "r");
    if (f) {
        char buf[128];
        while (fgets(buf, sizeof(buf), f) && _englishWords.size() < ENGLISHDICT_LIMIT) {
            addWord(buf);
        }
        fclose(f);
    }

    const char *embeddedStart = (const char *)english_words_txt_start;
    const char *embeddedEnd = (const char *)english_words_txt_end;
    if (_englishWords.empty() && embeddedEnd > embeddedStart) {
        const char *p = embeddedStart;
        const char *end = embeddedEnd;
        std::string line;
        while (p < end && _englishWords.size() < ENGLISHDICT_LIMIT) {
            char c = *p++;
            if (c == '\n' || c == '\r') {
                addWord(line);
                line.clear();
            } else {
                line += c;
            }
        }
        addWord(line);
    }

    if (_englishWords.empty()) {
        for (auto w : BUILTIN_ENGLISH_WORDS) _englishWords.push_back(w);
    }
    std::sort(_englishWords.begin(), _englishWords.end());
    _englishWords.erase(std::unique(_englishWords.begin(), _englishWords.end()), _englishWords.end());
}

static std::string chineseDigits(uint64_t n, bool financial) {
    static const char *LOW[] = {"零","一","二","三","四","五","六","七","八","九"};
    static const char *FIN[] = {"零","壹","贰","叁","肆","伍","陆","柒","捌","玖"};
    static const char *UNIT_LOW[] = {"","十","百","千"};
    static const char *UNIT_FIN[] = {"","拾","佰","仟"};
    static const char *GROUP[] = {"","万","亿","兆"};
    const char **D = financial ? FIN : LOW;
    const char **U = financial ? UNIT_FIN : UNIT_LOW;
    if (n == 0) return D[0];

    auto groupText = [&](int g) {
        std::string out;
        bool zeroPending = false;
        for (int pos = 3; pos >= 0; pos--) {
            int base = 1;
            for (int i = 0; i < pos; i++) base *= 10;
            int digit = (g / base) % 10;
            if (digit == 0) {
                if (!out.empty()) zeroPending = true;
                continue;
            }
            if (zeroPending) {
                out += D[0];
                zeroPending = false;
            }
            if (!(pos == 1 && digit == 1 && out.empty() && !financial)) out += D[digit];
            out += U[pos];
        }
        return out;
    };

    std::vector<int> groups;
    while (n > 0 && groups.size() < 4) {
        groups.push_back((int)(n % 10000));
        n /= 10000;
    }
    std::string out;
    bool zeroBetween = false;
    for (int i = (int)groups.size() - 1; i >= 0; i--) {
        if (groups[i] == 0) {
            if (!out.empty()) zeroBetween = true;
            continue;
        }
        if (zeroBetween || (!out.empty() && groups[i] < 1000)) {
            out += D[0];
            zeroBetween = false;
        }
        out += groupText(groups[i]);
        out += GROUP[i];
    }
    return out;
}

static std::string romanNumber(int n) {
    struct R { int v; const char *s; };
    static const R MAP[] = {{90,"XC"},{50,"L"},{40,"XL"},{10,"X"},{9,"IX"},{5,"V"},{4,"IV"},{1,"I"}};
    std::string out;
    for (auto &r : MAP) {
        while (n >= r.v) { out += r.s; n -= r.v; }
    }
    return out;
}

static bool parseDateParts(const std::string &s, int &y, int &m, int &d) {
    std::vector<int> nums;
    std::string cur;
    for (char c : s) {
        if (c >= '0' && c <= '9') cur += c;
        else if (c == '.' || c == '-' || c == '/') {
            if (cur.empty()) return false;
            nums.push_back(atoi(cur.c_str()));
            cur.clear();
        } else return false;
    }
    if (!cur.empty()) nums.push_back(atoi(cur.c_str()));
    if (nums.size() == 3) {
        y = nums[0]; m = nums[1]; d = nums[2];
    } else if (nums.size() == 1 && s.length() == 8) {
        y = atoi(s.substr(0, 4).c_str());
        m = atoi(s.substr(4, 2).c_str());
        d = atoi(s.substr(6, 2).c_str());
    } else {
        return false;
    }
    return y >= 1 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

static std::string chineseYear(int y) {
    static const char *D[] = {"零","一","二","三","四","五","六","七","八","九"};
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d", y);
    std::string out;
    // 只遍历数字字符:按数组长度遍历会把 '\0' 和未初始化字节当下标,越界读指针导致崩溃
    for (char *p = buf; *p; p++) out += D[*p - '0'];
    return out;
}

static std::string chineseDayMonth(int n) {
    static const char *D[] = {"零","一","二","三","四","五","六","七","八","九"};
    if (n <= 10) return n == 10 ? "十" : D[n];
    if (n < 20) return std::string("十") + D[n % 10];
    if (n % 10 == 0) return std::string(D[n / 10]) + "十";
    return std::string(D[n / 10]) + "十" + D[n % 10];
}

static std::string capFirst(const std::string &w) {
    if (w.empty() || w[0] < 'a' || w[0] > 'z') return w;
    std::string r = w;
    r[0] = (char)(r[0] - 'a' + 'A');
    return r;
}

// v/t / v/d / v/w 的候选:当前时刻按 纯数字/数字加中文/纯中文(星期为 英文/中文)生成
static std::vector<std::string> vTimeDateWeek(const std::string &body) {
    std::vector<std::string> out;
    time_t now;
    time(&now);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[48];
    if (body == "/t") {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        out.push_back(buf);
        snprintf(buf, sizeof(buf), "%02d时%02d分%02d秒", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        out.push_back(buf);
        snprintf(buf, sizeof(buf), "%s时%s分%s秒", chineseDayMonth(tmv.tm_hour).c_str(),
                 chineseDayMonth(tmv.tm_min).c_str(), chineseDayMonth(tmv.tm_sec).c_str());
        out.push_back(buf);
    } else if (body == "/d") {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        out.push_back(buf);
        snprintf(buf, sizeof(buf), "%d年%d月%d日", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        out.push_back(buf);
        snprintf(buf, sizeof(buf), "%s年%s月%s日", chineseYear(tmv.tm_year + 1900).c_str(),
                 chineseDayMonth(tmv.tm_mon + 1).c_str(), chineseDayMonth(tmv.tm_mday).c_str());
        out.push_back(buf);
    } else if (body == "/w") {
        static const char *EN[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
        static const char *CN[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
        out.push_back(EN[tmv.tm_wday]);
        out.push_back(CN[tmv.tm_wday]);
    }
    return out;
}

#if PJOURNAL_IME_ENABLE_LIANGFEN
void IME::loadLfDict() {
    if (_lfBlob) return;
    _lfBlob = liangfen_bin_start;
    size_t total = (size_t)(liangfen_bin_end - liangfen_bin_start);
    if (total < 1354 + 16) { _lfBlob = nullptr; return; }

    // Parse index: 677 × uint16 LE
    _lfIndex.resize(INDEX_ENTRIES);
    for (int k = 0; k < INDEX_ENTRIES; k++) {
        _lfIndex[k] = (uint16_t)_lfBlob[k * 2] | ((uint16_t)_lfBlob[k * 2 + 1] << 8);
    }

    _lfRecordBase = INDEX_ENTRIES * 2;  // 1354
    _lfCount = (uint32_t)((total - _lfRecordBase) / 16);
    ESP_LOGI(IME_TAG, "Liangfen dict loaded: %u records", (unsigned)_lfCount);
}

void IME::searchLfWindow(const char *code, int len, uint32_t &lo, uint32_t &hi) {
    lo = 0; hi = _lfCount;
    if (_lfIndex.empty() || len < 1) return;
    int c0 = code[0] - 'a'; if (c0 < 0 || c0 >= 26) return;
    if (len == 1) { lo = _lfIndex[c0*26]; hi = _lfIndex[(c0+1)*26]; return; }
    int c1 = code[1] - 'a'; if (c1 < 0 || c1 >= 26) return;
    int k = c0 * 26 + c1;
    lo = _lfIndex[k]; hi = _lfIndex[k + 1];
}

bool IME::readLfCode(uint16_t i, char out[13]) {
    if (!_lfBlob || i >= _lfCount) return false;
    const uint8_t *rec = _lfBlob + _lfRecordBase + i * 16;
    int n = 0;
    for (; n < 12 && rec[n]; n++) out[n] = (char)rec[n];
    out[n] = '\0';
    return true;
}

bool IME::readLfHanzi(uint16_t i, char out[4]) {
    if (!_lfBlob || i >= _lfCount) return false;
    const uint8_t *rec = _lfBlob + _lfRecordBase + i * 16 + 12;
    out[0] = (char)rec[0]; out[1] = (char)rec[1]; out[2] = (char)rec[2]; out[3] = '\0';
    return true;
}
#endif

void IME::bumpFrequency(const std::string &code, const std::string &word) {
    ensureUserDictLoaded();
    for (auto &p : _fixedUserWords) {
        if (p.code == code && p.word == word && p.trad == _trad) return;
    }
    for (auto it = _dynamicUserWords.begin(); it != _dynamicUserWords.end(); ++it) {
        if (it->code == code && it->word == word && it->trad == _trad) {
            it->count++;
            _dynamicUserDirty = true;
            saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
            return;
        }
    }
    if (word.length() >= 3 && code.length() >= 1) {
        if (_dynamicUserWords.size() >= USERDICT_DYNAMIC_LIMIT) {
            compactUserEntries(_dynamicUserWords, USERDICT_DYNAMIC_LIMIT);
            if (_dynamicUserWords.size() >= USERDICT_DYNAMIC_LIMIT) _dynamicUserWords.pop_back();
        }
        _dynamicUserWords.push_back({code, word, 1, _trad, userInitialForCode(code)});
        _dynamicUserDirty = true;
        saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
    }
}

void IME::bumpPredictFrequency(const std::string &key, const std::string &word, bool saveNow) {
    ensureUserDictLoaded();
    if (key.empty() || word.empty()) return;
    if (!validPredictEntry(key, word)) return;
    for (auto &p : _userPredictWords) {
        if (p.code == key && p.word == word && p.trad == _trad) {
            p.count++;
            _userPredictDirty = true;
            if (saveNow) saveUserDictFile(USERPREDICT_PATH, _userPredictWords, _userPredictDirty);
            return;
        }
    }
    if (_userPredictWords.size() >= USERPREDICT_LIMIT) {
        compactUserEntries(_userPredictWords, USERPREDICT_LIMIT);
        if (_userPredictWords.size() >= USERPREDICT_LIMIT) _userPredictWords.pop_back();
    }
    _userPredictWords.push_back({key, word, 1, _trad, ""});
    _userPredictDirty = true;
    if (saveNow) saveUserDictFile(USERPREDICT_PATH, _userPredictWords, _userPredictDirty);
}

void IME::learnPredictPairs(const std::string &text) {
    if (text.size() < 6) return;
    ensureUserDictLoaded();
    std::vector<std::string> chars;
    int learned = 0;
    auto learnSegment = [&]() {
        for (size_t i = 0; i + 1 < chars.size() && learned < 18; i++) {
            std::string tail;
            for (size_t j = i + 1; j < chars.size() && j <= i + 3 && learned < 18; j++) {
                tail += chars[j];
                bumpPredictFrequency(chars[i], tail, false);
                learned++;
            }
        }
        chars.clear();
    };
    size_t pos = 0;
    while (pos < text.size() && learned < 18) {
        std::string ch = utf8CharAt(text, pos);
        pos = utf8CharEnd(text, pos);
        if (!isCjkChar(ch)) {
            if (chars.size() > 1) learnSegment();
            else chars.clear();
            continue;
        }
        chars.push_back(ch);
        if (chars.size() >= 12) learnSegment();
    }
    if (chars.size() > 1 && learned < 18) learnSegment();
    if (_userPredictDirty) saveUserDictFile(USERPREDICT_PATH, _userPredictWords, _userPredictDirty);
}

void IME::rememberCommittedText(const std::string &text) {
    if (text.empty()) return;
    std::string first = utf8CharAt(text, 0);
    if (!isCjkChar(first)) {
        _lastCommitChar.clear();
        return;
    }
    if (_lastCommitChar.size() >= 3 && first.size() >= 3) {
        bumpPredictFrequency(_lastCommitChar, first, false);
        std::string second = utf8CharAt(text, utf8CharEnd(text, 0));
        if (isCjkChar(second)) bumpPredictFrequency(_lastCommitChar, first + second, false);
    }
    learnPredictPairs(text);
    if (_userPredictDirty) saveUserDictFile(USERPREDICT_PATH, _userPredictWords, _userPredictDirty);
    std::string last = lastUtf8Char(text);
    if (isCjkChar(last)) _lastCommitChar = last;
    else _lastCommitChar.clear();
}

bool IME::readCode(uint32_t i, char out[MAX_CODE_LEN + 1]) {
    return _dict.readSingleCode(i, out);
}

bool IME::readHanzi(uint32_t i, char out[HANZI_SIZE + 1]) {
    return _dict.readSingleText(i, out);
}

uint8_t IME::readRecordFlag(uint32_t i) {
    return _dict.readSingleFlag(i);
}

bool IME::hasCandidate(const std::string &text) const {
    for (auto &e : _all) {
        if (e == text) return true;
    }
    return false;
}

bool IME::appendCandidate(const std::string &text, int candLen) {
    if (_all.size() >= _candidateLimit || hasCandidate(text)) return false;
    _all.push_back(text);
    _candLen.push_back(candLen);
    return true;
}

#if PJOURNAL_IME_ENABLE_LIANGFEN
uint8_t IME::readLfFlag(uint16_t i) {
    const uint8_t *rec = _lfBlob + _lfRecordBase + (size_t)i * 16;
    return rec[15];
}
#endif

void IME::setActive(bool on) {
    _active = on;
    if (on) ensureUserDictLoaded();
    reset();
}

void IME::reset() {
    _code.clear();
    _displayCodeDirty = true;
    _all.clear();
    _candLen.clear();
    if (_all.capacity() > MAX_CANDIDATES * 2) _all.shrink_to_fit();
    else if (_all.capacity() < MAX_CANDIDATES / 3) _all.reserve(MAX_CANDIDATES / 3);
    _page.clear();
    if (_page.capacity() > _pageSize * 2) _page.shrink_to_fit();
    else if (_page.capacity() < _pageSize) _page.reserve(_pageSize);
    _pageStart = 0;
    _curPage = 0;
    _prefix.clear();
    _remainder.clear();
    _lfMode = false;
    _deleteMode = false;
    _vMode = false;
    _vSel = 0;
    _fixedCandidatePaging = false;
    _candidateLimit = MAX_CANDIDATES;
    _englishCompose = false;
}

int IME::pinyinPrefixLen(const std::string &code) {
    int i = 0;
    while (i < (int)code.length() && code[i] >= 'a' && code[i] <= 'z') i++;
    return i;
}

void IME::searchWindow(const char *code, int len, uint32_t &lo, uint32_t &hi) {
    _dict.singleWindow(code, len, lo, hi);
}

void IME::lookup() {
    ImePerfTrace perf(_code, _all);
    _all.clear();
    _candLen.clear();
    _pageStart = 0;
    _curPage = 0;
    _maxMatchLen = 0;
    if (_prefix.length() == 0) _codeOrig = _code;
    static bool dictLoaded = false;
    if (!dictLoaded) {
        dictLoaded = true;
        ensureUserDictLoaded();
    }

    if (!_loaded || (_code.length() == 0 && !_deleteMode)) {
        perf.exitName = "empty";
        buildPage();
        return;
    }

    // 单引号编码分词: 显式按音节分段匹配(词组/补充表/用户词典)
    if (_code.find('\'') != std::string::npos) {
        int64_t t = IME_PERF_NOW();
        lookupSegmented();
        perf.segUs += IME_PERF_NOW() - t;
        perf.exitName = "segmented";
        buildPage();
        return;
    }

    int64_t setupStartUs = IME_PERF_NOW();
    const char *q = _code.c_str();
    int qlen = (int)_code.length();

    int pinyinLen = pinyinPrefixLen(_code);
    std::string pinyinCode = _code.substr(0, pinyinLen);

    // First char uppercase: treat as literal, use partial match for remainder
    if (pinyinLen == 0 && _code.length() > 0) {
        _all.push_back(_code.substr(0, 1));
        _candLen.push_back(0);
        _partialStart = 0;
        _remainder = _code.substr(1);
        perf.setupUs += IME_PERF_NOW() - setupStartUs;
        perf.exitName = "literal";
        buildPage();
        return;
    }

    q = pinyinCode.c_str();
    qlen = pinyinLen;
    std::vector<std::string> aliasCodes = alternateInputCodes(pinyinCode);
    bool hasVowel = false;
    for (int i = 0; i < qlen; i++) {
        if (strchr("aeiouv", q[i])) { hasVowel = true; break; }
    }
    ime::PinyinSplit primarySplit = ime::PinyinEngine::primarySplit(pinyinCode, true);
    bool incompletePinyinInput = false;
    if (!primarySplit.tokens.empty()) {
        incompletePinyinInput = primarySplit.tokens.back().partial;
    }
    _fixedCandidatePaging = (!hasVowel && qlen <= 2) || incompletePinyinInput;
    if (incompletePinyinInput) {
        _candidateLimit = IME_PARTIAL_PINYIN_CANDIDATE_LIMIT;
    } else if (_fixedCandidatePaging) {
        _candidateLimit = IME_SHORT_CONSONANT_CANDIDATE_LIMIT;
    } else {
        _candidateLimit = IME_FAST_CANDIDATE_LIMIT;
    }
    perf.setupUs += IME_PERF_NOW() - setupStartUs;
    perf.hasVowel = hasVowel;
    perf.incomplete = incompletePinyinInput;
    perf.fixedPaging = _fixedCandidatePaging;
    perf.limit = _candidateLimit;

#if PJOURNAL_IME_ENABLE_LIANGFEN
    if (_lfMode && _lfBlob) {
        uint32_t llo, lhi;
        searchLfWindow(q, qlen, llo, lhi);
        while (llo < lhi) {
            uint32_t mid = llo + (lhi - llo) / 2;
            char code[13]; if (!readLfCode(mid, code)) break;
            if (strncmp(code, q, qlen) < 0) llo = mid + 1;
            else lhi = mid;
        }
        for (uint32_t i = llo; i < _lfCount && _all.size() < IME_FAST_CANDIDATE_LIMIT; i++) {
            char code[13]; if (!readLfCode(i, code)) break;
            if (strncmp(code, q, qlen) != 0) break;
            uint8_t f = readLfFlag(i);
            if (f & (_trad ? 0x01 : 0x02)) continue;
            char hz[4]; if (!readLfHanzi(i, hz)) break;
            appendCandidate(std::string(hz), 0);
        }
        buildPage();
        return;
    }
#endif

    if (_deleteMode) {
        int64_t t = IME_PERF_NOW();
        std::vector< std::pair<int, std::string> > userMatches;
        auto codeMatchesDelete = [&](const std::string &entryCode) -> bool {
            if (qlen == 0 || userCodeMatchesPrefix(entryCode, q, qlen)) return true;
            for (auto &aliasCode : aliasCodes) {
                if (userCodeMatchesPrefix(entryCode, aliasCode.c_str(), (int)aliasCode.length())) return true;
            }
            return false;
        };
        for (auto &p : _dynamicUserWords) {
            if (p.trad != _trad) continue;
            if (codeMatchesDelete(p.code)) {
                bool found = false;
                for (auto &m : userMatches) {
                    if (m.second == p.word) {
                        found = true;
                        if (m.first < p.count) m.first = p.count;
                        break;
                    }
                }
                if (!found) userMatches.push_back({p.count, p.word});
            }
        }
        std::sort(userMatches.begin(), userMatches.end(),
            [](const std::pair<int,std::string> &a, const std::pair<int,std::string> &b) {
                return a.first > b.first;
        });
        for (auto &m : userMatches) {
            appendCandidate(m.second, 0);
            if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
        }
        perf.userUs += IME_PERF_NOW() - t;
        perf.exitName = "delete";
        buildPage();
        return;
    }

    // Phase 1: user dict single chars — highest priority (phrases emitted in Phase 3)
    std::vector< std::pair<int, std::string> > userWordFreq;
    std::vector< std::pair<int, std::string> > userPrefixWordFreq;
    std::vector< std::pair<int, std::string> > userExactWordFreq;
    {
        int64_t t = IME_PERF_NOW();
        std::vector< std::pair<int, std::string> > userSingleFreq;
        auto scanUserWords = [&](const std::vector<UserEntry> &entries) {
        for (auto &p : entries) {
            if (p.trad != _trad) continue;
            std::string matchedCode;
            bool matched = userCodeMatchesPrefix(p.code, q, qlen, &matchedCode);
            for (auto &aliasCode : aliasCodes) {
                if (matched) break;
                matched = userCodeMatchesPrefix(p.code, aliasCode.c_str(),
                                                (int)aliasCode.length(), &matchedCode);
            }
            if (!matched) continue;
            int score = userCandidateScore(matchedCode, p.count, qlen);
            bool exactCodeMatch = pinyinCodeEqualsAny(matchedCode, pinyinCode, aliasCodes);
            if (p.word.length() <= 3) {
                // single char
                bool found = false;
                for (auto &uf : userSingleFreq) {
                    if (uf.second == p.word) {
                        found = true;
                        if (uf.first < score) uf.first = score;
                        break;
                    }
                }
                if (!found) userSingleFreq.push_back({score, p.word});
            } else {
                // phrase
                auto &target = exactCodeMatch ? userWordFreq : userPrefixWordFreq;
                bool found = false;
                for (auto &uf : target) {
                    if (uf.second == p.word) {
                        found = true;
                        if (uf.first < score) uf.first = score;
                        break;
                    }
                }
                if (!found) target.push_back({score, p.word});
                if (exactCodeMatch) {
                    bool exactFound = false;
                    for (auto &uf : userExactWordFreq) {
                        if (uf.second == p.word) {
                            exactFound = true;
                            if (uf.first < score) uf.first = score;
                            break;
                        }
                    }
                    if (!exactFound) userExactWordFreq.push_back({score, p.word});
                }
            }
        }
        };
        scanUserWords(_fixedUserWords);
        scanUserWords(_dynamicUserWords);
        std::sort(userSingleFreq.begin(), userSingleFreq.end(),
            [](const std::pair<int,std::string> &a, const std::pair<int,std::string> &b) {
                return a.first > b.first;
        });
        for (auto &f : userSingleFreq) {
            appendCandidate(f.second, 0);
            if (_all.size() >= _candidateLimit) break;
        }
        perf.userUs += IME_PERF_NOW() - t;
        if (_all.size() >= _candidateLimit) { perf.exitName = "user-single-limit"; buildPage(); return; }
    }

    // Frequently selected full-code phrases should not be buried behind many single chars.
    if (hasVowel && !incompletePinyinInput && !userExactWordFreq.empty()) {
        int64_t t = IME_PERF_NOW();
        std::sort(userExactWordFreq.begin(), userExactWordFreq.end(),
            [](const std::pair<int,std::string> &a, const std::pair<int,std::string> &b) {
                return a.first > b.first;
        });
        int promoted = 0;
        for (auto &f : userExactWordFreq) {
            if (appendCandidate(f.second, qlen) && ++promoted >= 4) break;
            if (_all.size() >= _candidateLimit) break;
        }
        perf.userPhraseUs += IME_PERF_NOW() - t;
        if (_all.size() >= _candidateLimit) { perf.exitName = "user-exact-phrase-limit"; buildPage(); return; }
    }

    // Phase 2: single char prefix match (dictionary)
    {
    int64_t t = IME_PERF_NOW();
    if (hasVowel) {
        uint32_t lo, hi;
        searchWindow(q, qlen, lo, hi);
        uint32_t scanEnd = hi;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            char code[7];
            if (!readCode(mid, code)) break;
            if (strncmp(code, q, qlen) < 0) lo = mid + 1;
            else hi = mid;
        }
        int singleScanned = 0;
        for (uint32_t i = lo; i < scanEnd && _all.size() < _candidateLimit &&
                              singleScanned++ < IME_MAX_SINGLE_RECORD_SCAN; i++) {
            char code[7];
            if (!readCode(i, code)) break;
            if (strncmp(code, q, qlen) != 0) break;
            uint8_t f = readRecordFlag(i);
            if (f & (_trad ? 0x01 : 0x02)) continue;
            char hz[4];
            if (!readHanzi(i, hz)) break;
            int codeLen = (int)strlen(code);
            if (appendCandidate(std::string(hz), codeLen)) {
                if ((int)strlen(code) > _maxMatchLen) _maxMatchLen = (int)strlen(code);
            }
        }
    }
    if (!hasVowel && _all.size() < _candidateLimit) {
        std::string fallback = ime::PinyinEngine::singleKeyFallbackSyllable(pinyinCode);
        if (!fallback.empty()) appendSingleCharCandidates(fallback, qlen);
    }
    if (hasVowel && _all.size() < _candidateLimit) {
        std::string alias = zeroInitialAlias(pinyinCode);
        if (!alias.empty()) appendSingleCharCandidates(alias, qlen);
        for (auto &aliasCode : aliasCodes) {
            if (_all.size() >= _candidateLimit) break;
            appendSingleCharCandidates(aliasCode, qlen);
        }
    }
    perf.singleUs += IME_PERF_NOW() - t;
    }
    if (_all.size() >= _candidateLimit) { perf.exitName = "single-limit"; buildPage(); return; }

    // 补充词典表分段匹配: xian/xi'an/xi'a -> 西安, anguang -> 暗光。
    // 拼音段解析由独立引擎负责, 这里仍只关心候选生成和去重。
    if (!incompletePinyinInput && hasVowel && qlen >= IME_SEG_TABLE_MIN_LEN && _all.size() < IME_FAST_CANDIDATE_LIMIT) {
        int64_t t = IME_PERF_NOW();
        std::vector<ime::PinyinSplit> splits = ime::PinyinEngine::splitVariants(pinyinCode, true, 6);
        std::vector<ime::PinyinSplit> aliasSplits;
        for (auto &aliasCode : aliasCodes) {
            std::vector<ime::PinyinSplit> moreSplits = ime::PinyinEngine::splitVariants(aliasCode, true, 4);
            aliasSplits.insert(aliasSplits.end(), moreSplits.begin(), moreSplits.end());
        }
        for (int i = 0; i < SEG_TABLE_COUNT && _all.size() < IME_FAST_CANDIDATE_LIMIT; i++) {
            std::vector<std::string> entrySyl = splitSyllableText(SEG_TABLE[i].syllables);
            bool matched = false;
            auto matchSplits = [&](const std::vector<ime::PinyinSplit> &variants) -> bool {
            for (auto &split : variants) {
                std::string entryCode;
                for (auto &s : entrySyl) entryCode += s;
                std::string typedCode = pinyinJoinedCode(split.tokens);
                if ((int)typedCode.length() > (int)entryCode.length()) continue;
                if (strncmp(entryCode.c_str(), typedCode.c_str(), typedCode.length()) != 0) continue;
                if (pinyinSegmentsMatch(split.tokens, entrySyl)) return true;
            }
            return false;
            };
            matched = matchSplits(splits);
            if (!matched && !aliasSplits.empty())
                matched = matchSplits(aliasSplits);
            if (!matched) continue;
            std::string w = SEG_TABLE[i].word;
            if (appendCandidate(w, qlen)) {
                if (qlen > _maxMatchLen) _maxMatchLen = qlen;
            }
        }
        perf.segUs += IME_PERF_NOW() - t;
    }
    if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) { perf.exitName = "seg-limit"; buildPage(); return; }

    // Phase 3: user dict phrases — after dictionary single chars, before dictionary phrases
    {
        int64_t t = IME_PERF_NOW();
        std::sort(userWordFreq.begin(), userWordFreq.end(),
            [](const std::pair<int,std::string> &a, const std::pair<int,std::string> &b) {
                return a.first > b.first;
        });
        for (auto &f : userWordFreq) {
            appendCandidate(f.second, 0);
            if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
        }
        perf.userPhraseUs += IME_PERF_NOW() - t;
        if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) { perf.exitName = "user-phrase-limit"; buildPage(); return; }
    }
    size_t p4Start = _all.size();  // 词典词组排序起点(不含用户词组)

    // Phase 4: phrase prefix match (word dictionary)
    if (!incompletePinyinInput && hasVowel && qlen >= IME_PHRASE_PREFIX_MIN_LEN && _dict.hasWords()) {
        int64_t t = IME_PERF_NOW();
        const uint8_t *wordData = _dict.wordData();
        auto scanPhrasePrefix = [&](const char *scanCode, int scanLen, bool aliasScan) {
            if (!scanCode || scanLen < IME_PHRASE_PREFIX_MIN_LEN) return;
            size_t wlo = 0, whi = _dict.wordDataSize();
            _dict.wordWindow(scanCode, scanLen, wlo, whi);
            size_t wpos = wlo;
            int safety = 0;
            while (wpos < whi && _all.size() < IME_FAST_CANDIDATE_LIMIT && safety++ < IME_MAX_PHRASE_GROUP_SCAN) {
                uint8_t cl = wordData[wpos];
                if (cl == 0 || wpos + 1 + cl > whi) break;
                const char *wc = (const char *)wordData + wpos + 1;
                size_t next = wpos + 1 + cl;
                if (next >= whi) break;
                uint8_t n = wordData[next++];
                int matchLen = std::min((int)cl, scanLen);
                bool groupMatch = (strncmp(wc, scanCode, matchLen) == 0);
                for (uint8_t j = 0; j < n && next < whi; j++) {
                    uint8_t wl = wordData[next++];
                    if (wl == 0 || next + wl + 1 > whi) {
                        next = whi;
                        break;
                    }
                    uint8_t wf = wordData[next + wl];
                    if (groupMatch && !(cl < scanLen && wl <= 3)) {
                        std::string w((const char *)wordData + next, wl);
                        int consumedLen = aliasScan ? qlen : (int)cl;
                        if (wordVisible(_trad, w, wf) && appendCandidate(w, consumedLen)) {
                            if (cl > _maxMatchLen) _maxMatchLen = cl;
                        }
                    }
                    if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) {
                        next += wl + 1;
                        break;
                    }
                    next += wl + 1;
                }
                wpos = next;
            }
        };
        scanPhrasePrefix(q, qlen, false);
        for (auto &aliasCode : aliasCodes) {
            if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
            scanPhrasePrefix(aliasCode.c_str(), (int)aliasCode.length(), true);
        }
        perf.phraseUs += IME_PERF_NOW() - t;
    }
    // Sort Phase 4 entries by exactness, consumed length, and syllable-count closeness.
    {
        int64_t t = IME_PERF_NOW();
        size_t p4End = _all.size();
        size_t p4Count = p4End - p4Start;
        if (p4Count > 1) {
            std::vector<int> order(p4Count);
            for (size_t i = 0; i < order.size(); i++) order[i] = (int)(p4Start + i);
            std::stable_sort(order.begin(), order.end(),
                [this, qlen, &primarySplit](int a, int b) {
                    int syllables = (int)primarySplit.tokens.size();
                    return phraseCandidateScore(_all[a], _candLen[a], qlen, syllables) >
                           phraseCandidateScore(_all[b], _candLen[b], qlen, syllables);
                });
            std::vector<std::string> sortedAll(_all.begin(), _all.begin() + p4Start);
            std::vector<int> sortedLen(_candLen.begin(), _candLen.begin() + p4Start);
            sortedAll.reserve(_all.size());
            sortedLen.reserve(_candLen.size());
            for (int i : order) {
                sortedAll.push_back(std::move(_all[i]));
                sortedLen.push_back(_candLen[i]);
            }
            _all.swap(sortedAll);
            _candLen.swap(sortedLen);
        }
        perf.phraseSortUs += IME_PERF_NOW() - t;
    }
    if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) { perf.exitName = "phrase-limit"; buildPage(); return; }

    // Phase 4b: longer user phrases that only prefix-match the current input.
    // Exact user phrases stay early; prefix-only phrases must not bury dictionary exact matches.
    if (!userPrefixWordFreq.empty()) {
        int64_t t = IME_PERF_NOW();
        std::sort(userPrefixWordFreq.begin(), userPrefixWordFreq.end(),
            [](const std::pair<int,std::string> &a, const std::pair<int,std::string> &b) {
                return a.first > b.first;
        });
        for (auto &f : userPrefixWordFreq) {
            appendCandidate(f.second, 0);
            if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
        }
        perf.userPhraseUs += IME_PERF_NOW() - t;
        if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) { perf.exitName = "user-prefix-phrase-limit"; buildPage(); return; }
    }

    // Phase 5: user dict initial match
    if (!hasVowel && qlen >= IME_USER_INITIAL_MIN_LEN &&
        (_fixedUserWords.size() > 0 || _dynamicUserWords.size() > 0)) {
        int64_t t = IME_PERF_NOW();
        std::vector< std::pair<int, std::string> > userInitFreq;
        auto scanInitialWords = [&](const std::vector<UserEntry> &entries) {
        for (auto &p : entries) {
            if (p.trad != _trad) continue;
            if (p.code.find('\'') != std::string::npos) continue;  // 撇号码只在分词路径匹配
            const std::string &init = p.initial;
            if ((int)init.length() >= qlen && strncmp(init.c_str(), q, qlen) == 0) {
                int score = p.count * 8 + (((int)init.length() == qlen) ? 100000 : std::max(0, 64 - ((int)init.length() - qlen)));
                bool found = false;
                for (auto &uf : userInitFreq) {
                    if (uf.second == p.word) {
                        found = true;
                        if (uf.first < score) uf.first = score;
                        break;
                    }
                }
                if (!found) userInitFreq.push_back({score, p.word});
            }
        }
        };
        scanInitialWords(_fixedUserWords);
        scanInitialWords(_dynamicUserWords);
        std::sort(userInitFreq.begin(), userInitFreq.end(),
            [](const std::pair<int,std::string> &a, const std::pair<int,std::string> &b) {
                return a.first > b.first;
        });
        for (auto &f : userInitFreq) {
            appendCandidate(f.second, 0);
            if (_all.size() >= _candidateLimit) break;
        }
        perf.userInitialUs += IME_PERF_NOW() - t;
        if (_all.size() >= _candidateLimit) { perf.exitName = "user-initial-limit"; buildPage(); return; }
    }

    // Phase 6: initial match (no vowel, consonant-only)
    if (!hasVowel && qlen >= IME_DICT_INITIAL_MIN_LEN && _dict.hasWords()) {
        int64_t t = IME_PERF_NOW();
        struct ScoredPhrase {
            int score;
            int candLen;
            std::string word;
        };
        std::vector<ScoredPhrase> initialCandidates;
        initialCandidates.reserve(std::min<int>(IME_MAX_INITIAL_COLLECT, (int)_candidateLimit * 6));
        auto addInitialCandidate = [&](const std::string &word, int candLen, int score) {
            for (auto &item : initialCandidates) {
                if (item.word == word) {
                    if (item.score < score) {
                        item.score = score;
                        item.candLen = candLen;
                    }
                    return;
                }
            }
            if ((int)initialCandidates.size() < IME_MAX_INITIAL_COLLECT)
                initialCandidates.push_back({score, candLen, word});
        };
        int scanBudget = IME_MAX_INITIAL_GROUP_SCAN;
        if (qlen <= 2) scanBudget = IME_MAX_SHORT_INITIAL_GROUP_SCAN;
        else if (qlen == 3) scanBudget = IME_MAX_MEDIUM_INITIAL_GROUP_SCAN;
        else scanBudget = IME_MAX_LONG_INITIAL_GROUP_SCAN;
        const uint8_t *wordData = _dict.wordData();
        size_t slo = 0, shi = _dict.wordDataSize();
        _dict.wordWindow(q, 1, slo, shi);
        size_t spos = slo;
        int safety = 0;
        while (spos < shi && safety++ < scanBudget &&
               (int)initialCandidates.size() < IME_MAX_INITIAL_COLLECT) {
            uint8_t cl = wordData[spos];
            if (cl == 0 || spos + 1 + cl > shi) break;
            const char *wc = (const char *)wordData + spos + 1;
            size_t next = spos + 1 + cl;
            if (next >= shi) break;
            uint8_t n = wordData[next++];
            bool initMatch = pinyinInitialStartsWithCompat(wc, cl, q, qlen);
            std::string groupInit;
            if (initMatch) groupInit = pinyinInitialCodeCompat(std::string(wc, cl));
            for (uint8_t j = 0; j < n && next < shi; j++) {
                uint8_t wl = wordData[next++];
                if (wl == 0 || next + wl + 1 > shi) {
                    next = shi;
                    break;
                }
                uint8_t wf = wordData[next + wl];
                if (initMatch) {
                    std::string w((const char *)wordData + next, wl);
                    if (wordVisible(_trad, w, wf)) {
                        int score = initialPhraseCandidateScoreFromLength((int)groupInit.length(), qlen, w);
                        if (score >= 0) addInitialCandidate(w, cl, score);
                    }
                }
                next += wl + 1;
            }
            spos = next;
        }
        std::stable_sort(initialCandidates.begin(), initialCandidates.end(),
            [](const ScoredPhrase &a, const ScoredPhrase &b) {
                if (a.score != b.score) return a.score > b.score;
                return a.word.length() < b.word.length();
        });
        for (auto &item : initialCandidates) {
            if (_all.size() >= _candidateLimit) break;
            if (appendCandidate(item.word, item.candLen) && item.candLen > _maxMatchLen)
                _maxMatchLen = item.candLen;
        }
        perf.initialUs += IME_PERF_NOW() - t;
    }

    // Phase 7: shorthand + tail match
    {
        int64_t t = IME_PERF_NOW();
        bool shorthandTail = false;
        std::string typedInit;
        std::string typedTail;
        if (!incompletePinyinInput && primarySplit.tokens.size() > 1 && qlen >= 3 && hasVowel) {
            int lastSylStart = qlen;
            for (int i = qlen - 1; i >= 1; i--) {
                if (strchr("aeiouv", q[i])) {
                    int j = i;
                    while (j > 0 && strchr("aeiouv", q[j-1])) j--;
                    if (j > 0 && strchr("bcdfghjklmnpqrstwxyz", q[j-1])) {
                        lastSylStart = j;
                        break;
                    }
                }
            }
            if (lastSylStart >= 2 && lastSylStart < qlen) {
                bool isPureConsonant = true;
                for (int i = 0; i < lastSylStart; i++) {
                    if (strchr("aeiouv", q[i])) { isPureConsonant = false; break; }
                }
                if (isPureConsonant) {
                    shorthandTail = true;
                    typedInit = std::string(q, lastSylStart);
                    typedTail = std::string(q + lastSylStart, qlen - lastSylStart);
                }
            }
        }
        if (shorthandTail && _dict.hasWords() && _all.size() < IME_FAST_CANDIDATE_LIMIT) {
            size_t slo = 0, shi = _dict.wordDataSize();
            _dict.wordWindow(typedInit.c_str(), 1, slo, shi);
            size_t spos = slo;
            int safety = 0;
            const uint8_t *wordData = _dict.wordData();
            const char *typedInitChars = typedInit.c_str();
            const char *typedTailChars = typedTail.c_str();
            int typedInitLen = (int)typedInit.length();
            int typedTailLen = (int)typedTail.length();
            while (spos < shi && _all.size() < IME_FAST_CANDIDATE_LIMIT && safety++ < IME_MAX_SHORTHAND_GROUP_SCAN) {
                uint8_t cl = wordData[spos];
                if (cl == 0 || spos + 1 + cl > shi) break;
                const char *wc = (const char *)wordData + spos + 1;
                size_t next = spos + 1 + cl;
                if (next >= shi) break;
                uint8_t n = wordData[next++];
                bool initMatch = pinyinInitialStartsWithCompat(wc, cl, typedInitChars, typedInitLen);
                int lastSylStart = cl;
                for (int i = cl - 1; i >= 0; i--) {
                    if (strchr("aeiouv", wc[i])) {
                        int j = i;
                        while (j > 0 && strchr("aeiouv", wc[j-1])) j--;
                        if (j > 0) { lastSylStart = j; break; }
                    }
                }
                const char *candTailStart = wc + lastSylStart;
                int candTailLen = cl - lastSylStart;
                bool tailMatch = false;
                if (candTailLen >= typedTailLen)
                    tailMatch = (strncmp(candTailStart, typedTailChars, typedTailLen) == 0);
                else
                    tailMatch = (strncmp(typedTailChars, candTailStart, candTailLen) == 0);
                bool groupMatch = initMatch && tailMatch;
                for (uint8_t j = 0; j < n && next < shi; j++) {
                    uint8_t wl = wordData[next++];
                    if (wl == 0 || next + wl + 1 > shi) {
                        next = shi;
                        break;
                    }
                    uint8_t wf = wordData[next + wl];
                    if (groupMatch) {
                        std::string w((const char *)wordData + next, wl);
                        if (wordVisible(_trad, w, wf) && appendCandidate(w, cl)) {
                            if (cl > _maxMatchLen) _maxMatchLen = cl;
                        }
                    }
                    if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) {
                        next += wl + 1;
                        break;
                    }
                    next += wl + 1;
                }
                spos = next;
            }
        }
        perf.shorthandUs += IME_PERF_NOW() - t;
    }

    // Phase 8: partial (逐字) match
    _partialStart = (int)_all.size();
    _remainder.clear();
    if (qlen > 1 && _all.size() < _candidateLimit && (!incompletePinyinInput || _all.empty())) {
        int64_t t = IME_PERF_NOW();
        uint32_t zlo, zhi;
        std::vector<int> tryLens = ime::PinyinEngine::prefixMatchLengths(std::string(q, qlen));
        int maxTry = qlen - 1;
        if (_maxMatchLen > 0 && _maxMatchLen < maxTry) maxTry = _maxMatchLen - 1;
        for (int len = maxTry; len >= 1; len--) {
            if (std::find(tryLens.begin(), tryLens.end(), len) == tryLens.end())
                tryLens.push_back(len);
        }
        for (int tryLen : tryLens) {
            if (tryLen >= qlen || tryLen < 1 || _all.size() >= _candidateLimit) continue;
            searchWindow(q, tryLen, zlo, zhi);
            uint32_t sEnd = zhi;
            int bcount = 0;
            while (zlo < zhi && bcount++ < 200) {
                uint32_t mid = zlo + (zhi - zlo) / 2;
                char code[7]; if (!readCode(mid, code)) break;
                if (strncmp(code, q, tryLen) < 0) zlo = mid + 1;
                else zhi = mid;
            }
            int partialScanned = 0;
            for (uint32_t i = zlo; i < sEnd && _all.size() < _candidateLimit &&
                                  partialScanned++ < IME_MAX_PARTIAL_RECORD_SCAN; i++) {
                char code[7]; if (!readCode(i, code)) break;
                if (strncmp(code, q, tryLen) != 0) break;
                uint8_t f = readRecordFlag(i);
                if (f & (_trad ? 0x01 : 0x02)) continue;
                char hz[4]; if (!readHanzi(i, hz)) break;
                appendCandidate(std::string(hz), 0);
            }
            if (_all.size() > (size_t)_partialStart) {
                _remainder = _code.substr(tryLen);
                break;
            }
        }
        perf.partialUs += IME_PERF_NOW() - t;
    }
    perf.exitName = "end";
    buildPage();
}

void IME::lookupEnglishMode() {
    _all.clear();
    _candLen.clear();
    _pageStart = 0;
    _curPage = 0;
    loadEnglishDict();
    std::string q = _code;
    std::string lower = q;
    for (char &c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    // 输入首字母大写时,命中的英文词条首字母跟随大写
    bool upper = !q.empty() && q[0] >= 'A' && q[0] <= 'Z';
    auto it = std::lower_bound(_englishWords.begin(), _englishWords.end(), lower);
    for (; it != _englishWords.end(); ++it) {
        if (_all.size() >= MAX_CANDIDATES) break;
        if (it->find(lower) != 0) break;
        _all.push_back(upper ? capFirst(*it) : *it);
        _candLen.push_back((int)q.length());
    }
    bool exact = false;
    for (auto &w : _all) if (w == q) { exact = true; break; }
    if (!exact && !_code.empty()) {
        _all.insert(_all.begin(), q);
        _candLen.insert(_candLen.begin(), (int)q.length());
    }
    buildPage();
}

// v模式颜文字搜索匹配: 编码按音节表贪心切分, 查询串逐音节消费 1..音节长 个
// 字符(全拼前缀或声母缩写均可, 如 k/ka/kai/kx 都命中 kaixin)。音节表按长度
// 降序生成, 首个 strncmp 命中即最长音节。
static bool vKaomojiMatch(const char *q, size_t qlen, const char *code) {
    size_t qi = 0, ci = 0;
    while (code[ci]) {
        if (qi >= qlen) return true;
        size_t slen = 0;
        for (unsigned s = 0; s < K_KAOMOJI_SYLL_COUNT; s++) {
            size_t l = strlen(K_KAOMOJI_SYLLS[s]);
            if (strncmp(code + ci, K_KAOMOJI_SYLLS[s], l) == 0) { slen = l; break; }
        }
        if (slen == 0) slen = 1;
        size_t k = 0;
        while (k < slen && qi + k < qlen && code[ci + k] == q[qi + k]) k++;
        if (k == 0) return false;
        qi += k;
        ci += slen;
    }
    return qi >= qlen;
}

void IME::lookupKaomoji(const std::string &query) {
    if (query.empty()) return;
    for (unsigned i = 0; i < K_KAOMOJI_COUNT && _all.size() < MAX_CANDIDATES; i++) {
        const char *code = K_KAOMOJI_TABLE[i].code;
        if (code[0] == '\0' || code[0] != query[0]) continue;  // 首字符过滤+跳过常用块
        if (!vKaomojiMatch(query.data(), query.size(), code)) continue;
        const char *face = K_KAOMOJI_TABLE[i].face;
        bool dup = false;
        for (auto &e : _all) if (e == face) { dup = true; break; }
        if (!dup) {
            _all.push_back(face);
            _candLen.push_back((int)_code.length());
        }
    }
}

void IME::lookupVMode() {
    _all.clear();
    _candLen.clear();
    _pageStart = 0;
    _curPage = 0;
    _vSel = 0;
    std::string body = _code.length() > 1 ? _code.substr(1) : "";
    if (body.empty()) {
        // 裸 v: 常用文字表情(原中文标点候选改由 v/bd/ 搜索)
        for (unsigned i = 0; i < K_KAOMOJI_HOT && i < K_KAOMOJI_COUNT; i++) {
            _all.push_back(K_KAOMOJI_TABLE[i].face);
            _candLen.push_back(1);
        }
        buildPage();
        return;
    }

    // 闭合命令(v/t/ v/d/ v/w/,以 / 结尾):出候选,数字键/方向键选择。
    // 未闭合(v/t)不出候选,避免选词歧义。
    if (body == "/t/" || body == "/d/" || body == "/w/") {
        for (auto &s : vTimeDateWeek(body.substr(0, body.size() - 1))) {
            _all.push_back(s);
            _candLen.push_back((int)_code.length());
        }
        buildPage();
        return;
    }

    if (body[0] == '/') {
        std::string num = body.substr(1);
        bool allDigits = !num.empty();
        for (char c : num) if (c < '0' || c > '9') { allDigits = false; break; }
        if (allDigits) {
            uint64_t n = 0;
            for (char c : num) n = n * 10 + (uint64_t)(c - '0');
            _all.push_back(chineseDigits(n, false));
            _candLen.push_back((int)_code.length());
            _all.push_back(chineseDigits(n, true));
            _candLen.push_back((int)_code.length());
            if (n >= 1 && n <= 99) {
                _all.push_back(romanNumber((int)n));
                _candLen.push_back((int)_code.length());
            }
        } else {
            // v/编码(闭合 v/编码/ 可数字键直选): 按拼音/声母前缀搜文字表情与标点。
            // 纯字母才进搜索; 数字/混合编码无候选。
            std::string q = num;
            if (!q.empty() && q.back() == '/') q.pop_back();
            bool allAlpha = !q.empty();
            for (char c : q) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { allAlpha = false; break; }
            }
            if (allAlpha) {
                std::string lq = q;
                for (char &c : lq) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                lookupKaomoji(lq);
            }
        }
        buildPage();
        return;
    }

    int y = 0, m = 0, d = 0;
    bool isDate = parseDateParts(body, y, m, d);
    if (isDate) {
        char arabic[32];
        snprintf(arabic, sizeof(arabic), "%d年%d月%d日", y, m, d);
        bool dup = false;
        for (auto &e : _all) if (e == arabic) { dup = true; break; }
        if (!dup) {
            _all.push_back(arabic);
            _candLen.push_back((int)_code.length());
        }
        std::string cn = chineseYear(y) + "年" + chineseDayMonth(m) + "月" + chineseDayMonth(d) + "日";
        dup = false;
        for (auto &e : _all) if (e == cn) { dup = true; break; }
        if (!dup) {
            _all.push_back(cn);
            _candLen.push_back((int)_code.length());
        }
    }

    bool allAlpha = true;
    for (char c : body) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { allAlpha = false; break; }
    }
    if (allAlpha) {
        loadEnglishDict();
        std::string lower = body;
        for (char &c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        // 输入首字母大写时,命中的英文词条首字母跟随大写
        bool upper = body[0] >= 'A' && body[0] <= 'Z';
        auto it = std::lower_bound(_englishWords.begin(), _englishWords.end(), lower);
        for (; it != _englishWords.end(); ++it) {
            if (_all.size() >= MAX_CANDIDATES) break;
            if (it->find(lower) != 0) break;
            std::string w = upper ? capFirst(*it) : *it;
            bool dup = false;
            for (auto &e : _all) if (e == w) { dup = true; break; }
            if (!dup) {
                _all.push_back(w);
                _candLen.push_back((int)_code.length());
            }
        }
        bool dup = false;
        for (auto &e : _all) if (e == body) { dup = true; break; }
        if (!dup) {
            _all.insert(_all.begin(), body);
            _candLen.insert(_candLen.begin(), (int)_code.length());
        }
    }

    buildPage();
}

// 单引号分词查词: 编码形如 "xi'an" / "an'guang", 按 ' 切成音节段。
// 1) 用户词典整码/去分隔整码匹配; 2) 补充词典表(seg_table.h) 分段前缀匹配;
// 3) 主词典词组: 拼接码精确匹配且字数=段数;
// 4) 逐字匹配: 首段单字候选, 选中后按 seg0Next 消费跳下一段续拼(见 commit 的 candContinue)。
// 全部视为整码消费。
void IME::lookupSegmented() {
    std::vector<std::string> segs;
    ime::PinyinSplit split = ime::PinyinEngine::primarySplit(_code, true);
    for (auto &token : split.tokens) segs.push_back(token.text);
    if (segs.empty()) segs = splitSyllableText(_code.c_str());
    if (segs.empty()) return;
    std::string q;
    for (auto &s : segs) q += s;
    std::vector<std::string> aliasCodes = alternateInputCodes(q);
    int fullLen = (int)_code.length();

    // 1) 用户词典: 显式分词和去分隔编码共享学习结果
    std::vector< std::pair<int, std::string> > userMatches;
    auto scanSegmentedUserWords = [&](const std::vector<UserEntry> &entries) {
    for (auto &p : entries) {
        if (p.trad != _trad) continue;
        if (p.word.length() <= 3) continue;
        std::string matchedCode;
        bool matched = userCodeMatchesPrefix(p.code, q.c_str(), (int)q.length(), &matchedCode);
        for (auto &aliasCode : aliasCodes) {
            if (matched) break;
            matched = userCodeMatchesPrefix(p.code, aliasCode.c_str(), (int)aliasCode.length(), &matchedCode);
        }
        if (!matched) continue;
        int score = userCandidateScore(matchedCode, p.count, (int)q.length());
        bool found = false;
        for (auto &m : userMatches) {
            if (m.second == p.word) {
                found = true;
                if (m.first < score) m.first = score;
                break;
            }
        }
        if (!found) userMatches.push_back({score, p.word});
    }
    };
    scanSegmentedUserWords(_fixedUserWords);
    scanSegmentedUserWords(_dynamicUserWords);
    std::sort(userMatches.begin(), userMatches.end(),
        [](const std::pair<int,std::string> &a, const std::pair<int,std::string> &b) {
            return a.first > b.first;
    });
    for (auto &m : userMatches) {
        appendCandidate(m.second, fullLen);
        if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
    }

    // 2) 补充词典表
    std::vector<ime::PinyinSplit> aliasSplits;
    for (auto &aliasCode : aliasCodes) {
        std::vector<ime::PinyinSplit> moreSplits = ime::PinyinEngine::splitVariants(aliasCode, true, 4);
        aliasSplits.insert(aliasSplits.end(), moreSplits.begin(), moreSplits.end());
    }
    for (int i = 0; i < SEG_TABLE_COUNT && _all.size() < IME_FAST_CANDIDATE_LIMIT; i++) {
        const char *enc = SEG_TABLE[i].syllables;
        std::vector<std::string> entrySyl;
        {
            std::string s;
            for (const char *p = enc; *p; p++) {
                if (*p == ' ') { if (!s.empty()) entrySyl.push_back(s); s.clear(); }
                else s += *p;
            }
            if (!s.empty()) entrySyl.push_back(s);
        }
        if (segs.size() > entrySyl.size()) continue;
        bool ok = true;
        for (size_t k = 0; k < segs.size() && ok; k++) {
            if (strncmp(segs[k].c_str(), entrySyl[k].c_str(), segs[k].length()) != 0)
                ok = false;
        }
        std::string entryCode;
        for (auto &s : entrySyl) entryCode += s;
        if (ok) {
            ok = strncmp(q.c_str(), entryCode.c_str(), q.length()) == 0;
        }
        if (!ok && !aliasSplits.empty()) {
            for (auto &split : aliasSplits) {
                std::string typedCode = pinyinJoinedCode(split.tokens);
                if ((int)typedCode.length() > (int)entryCode.length()) continue;
                if (strncmp(entryCode.c_str(), typedCode.c_str(), typedCode.length()) != 0) continue;
                if (pinyinSegmentsMatch(split.tokens, entrySyl)) { ok = true; break; }
            }
        }
        if (!ok) continue;
        appendCandidate(SEG_TABLE[i].word, fullLen);
    }

    // 3) 主词典词组: 拼接码精确匹配 + 字数/3 == 段数
    if (_dict.hasWords() && q.length() >= 2 && _all.size() < IME_FAST_CANDIDATE_LIMIT) {
        const uint8_t *wordData = _dict.wordData();
        int wordTextLen = (int)segs.size() * 3;
        auto scanExactPhrase = [&](const std::string &scanCode) {
            if (scanCode.length() < 2) return;
            size_t wlo = 0, whi = _dict.wordDataSize();
            _dict.wordWindow(scanCode.c_str(), (int)scanCode.length(), wlo, whi);
            size_t wpos = wlo;
            int safety = 0;
            int scanLen = (int)scanCode.length();
            while (wpos < whi && _all.size() < IME_FAST_CANDIDATE_LIMIT &&
                   safety++ < IME_MAX_PHRASE_GROUP_SCAN) {
                uint8_t cl = wordData[wpos];
                if (cl == 0 || wpos + 1 + cl > whi) break;
                const char *wc = (const char *)wordData + wpos + 1;
                size_t next = wpos + 1 + cl;
                if (next >= whi) break;
                uint8_t n = wordData[next++];
                bool groupMatch = ((int)cl == scanLen && strncmp(wc, scanCode.c_str(), scanCode.length()) == 0);
                for (uint8_t j = 0; j < n && next < whi; j++) {
                    uint8_t wl = wordData[next++];
                    if (wl == 0 || next + wl + 1 > whi) {
                        next = whi;
                        break;
                    }
                    uint8_t wf = wordData[next + wl];
                    if (groupMatch && (int)wl == wordTextLen) {
                        std::string w((const char *)wordData + next, wl);
                        if (wordVisible(_trad, w, wf)) appendCandidate(w, fullLen);
                        if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
                    }
                    next += wl + 1;
                }
                wpos = next;
            }
        };
        scanExactPhrase(q);
        for (auto &aliasCode : aliasCodes) {
            if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
            scanExactPhrase(aliasCode);
        }
    }

    // 4) 逐字匹配: 首段单字前缀候选(词组之后)。选中后消费 seg0Next 字节跳到
    //    下一段续拼(如 xi'an 选"西"后余下 "an" 查字), 由 commit 的 candContinue 推进。
    if (_all.size() < IME_FAST_CANDIDATE_LIMIT) {
        int seg0Next = (segs.size() > 1) ? ((int)segs[0].length() + 1) : fullLen;
        appendSingleCharCandidates(segs[0], seg0Next);
    }

    _partialStart = (int)_all.size();
}

// 主词典单字前缀匹配: 与 lookup() Phase 2 相同扫描, 但消费长度由调用方指定
// (分词逐字续拼时是跳到下一段的字节偏移, 而非词典码长)。
void IME::appendSingleCharCandidates(const std::string &prefix, int candLen) {
    int qlen = (int)prefix.length();
    if (qlen < 1 || _all.size() >= _candidateLimit) return;
    uint32_t lo, hi;
    searchWindow(prefix.c_str(), qlen, lo, hi);
    uint32_t scanEnd = hi;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        char code[7];
        if (!readCode(mid, code)) break;
        if (strncmp(code, prefix.c_str(), qlen) < 0) lo = mid + 1;
        else hi = mid;
    }
    int scanBudget = (candLen > 0 && candLen <= 2) ?
        IME_MAX_SHORT_CONSONANT_SINGLE_SCAN : IME_MAX_SINGLE_RECORD_SCAN;
    int scanned = 0;
    for (uint32_t i = lo; i < scanEnd && _all.size() < _candidateLimit &&
                          scanned++ < scanBudget; i++) {
        char code[7];
        if (!readCode(i, code)) break;
        if (strncmp(code, prefix.c_str(), qlen) != 0) break;
        uint8_t f = readRecordFlag(i);
        if (f & (_trad ? 0x01 : 0x02)) continue;
        char hz[4];
        if (!readHanzi(i, hz)) break;
        appendCandidate(std::string(hz), candLen);
    }
}

void IME::beginPredict(const std::string &text) {
    reset();
    if (text.empty()) return;
    ensureUserDictLoaded();
    _predChar = text;
    std::vector< std::pair<int, std::string> > userPredict;
    for (auto &p : _userPredictWords) {
        if (p.trad != _trad) continue;
        if (p.code == text) userPredict.push_back({p.count, p.word});
    }
    std::stable_sort(userPredict.begin(), userPredict.end(),
        [](const std::pair<int, std::string> &a, const std::pair<int, std::string> &b) {
            return a.first > b.first;
        });
    for (auto &p : userPredict) appendCandidate(p.second, 0);
    if (_dict.hasPredictions()) {
        size_t pos = 0;
        ime::Im3Dictionary::PredictGroup group;
        while (_dict.nextPredictGroup(pos, group)) {
            if (group.key == text) {
                for (auto &word : group.candidates) {
                    appendCandidate(word, 0);
                }
                break;
            }
        }
    }
    for (auto &entry : BUILTIN_PREDICT) {
        if (text == entry.key) {
            for (int i = 0; entry.candidates[i]; i++)
                appendCandidate(entry.candidates[i], 0);
            break;
        }
    }
    if (_all.empty()) {
        _predChar.clear();
        reset();
        return;
    }
    _predicting = true;
    buildPage();
}

void IME::buildPage() {
    int64_t pageStartUs = IME_PERF_NOW();
    _page.clear();
    _vSel = 0;  // 换页/重新查词后高亮回到首个候选
    if (_all.empty()) {
        _pageStart = 0;
        _curPage = 0;
        _pageStarts.clear();
        return;
    }
    if (!_fixedCandidatePaging && _widthFn && _displayWidth > 0) {
        // 按显示宽度分页: 与各界面候选行渲染一致, " 编号." 前缀 + 候选文本,
        // 一行放不下则把该候选归入下一页, 保证候选不被隐藏。
        _pageStarts.clear();
        _pageStarts.push_back(0);
        int lineW = 0;
        for (int i = 0; i < (int)_all.size(); i++) {
            char num[16];
            snprintf(num, sizeof(num), " %d.", i - (int)_pageStarts.back() + 1);
            std::string part = std::string(num) + _all[i];
            int partW = _widthFn(part.c_str());
            if (lineW > 0 && lineW + partW > _displayWidth) {
                _pageStarts.push_back(i);
                lineW = 0;
                snprintf(num, sizeof(num), " 1.");
                partW = _widthFn((std::string(num) + _all[i]).c_str());
            }
            lineW += partW;
        }
        if (_curPage < 0) _curPage = 0;
        if (_curPage >= (int)_pageStarts.size()) _curPage = (int)_pageStarts.size() - 1;
        _pageStart = _pageStarts[_curPage];
        int end = (_curPage + 1 < (int)_pageStarts.size()) ? _pageStarts[_curPage + 1] : (int)_all.size();
        for (int i = _pageStart; i < end; i++) _page.push_back(_all[i]);
    } else {
        // 退化: 未注册宽度回调时按固定每页数量分页
        _pageStarts.clear();
        for (int i = 0; i < (int)_all.size(); i += _pageSize) _pageStarts.push_back(i);
        if (_curPage < 0) _curPage = 0;
        if (_curPage >= (int)_pageStarts.size()) _curPage = (int)_pageStarts.size() - 1;
        _pageStart = _pageStarts[_curPage];
        for (int i = _pageStart; i < (int)_all.size() && (int)_page.size() < _pageSize; i++)
            _page.push_back(_all[i]);
    }
    int64_t pageUs = IME_PERF_NOW() - pageStartUs;
    if (pageUs >= IME_PERF_SLOW_US) {
        ESP_LOGW(IME_TAG, "perf page code='%s' total=%lldus all=%u page=%u fixed=%d width=%d",
                 _code.c_str(), (long long)pageUs, (unsigned)_all.size(),
                 (unsigned)_page.size(), _fixedCandidatePaging ? 1 : 0, _displayWidth);
    }
}

bool IME::pagePrev() {
    if (_curPage <= 0) return false;
    _curPage--;
    buildPage();
    return true;
}

bool IME::pageNext() {
    if (_curPage + 1 >= (int)_pageStarts.size()) return false;
    _curPage++;
    buildPage();
    return true;
}

bool IME::commit(int idx, std::string &out) {
    if (idx < 0 || idx >= (int)_page.size()) return false;
    out = _page[idx];
    if (_vMode || _englishCompose) {
        _lastCommitChar.clear();
        reset();
        return true;
    }
    if (_deleteMode) {
        auto eraseMatch = [&](bool requireCode) -> bool {
            for (auto it = _dynamicUserWords.begin(); it != _dynamicUserWords.end(); ++it) {
                if (it->word == out && it->trad == _trad &&
                    (!requireCode || it->code == _code)) {
                    _dynamicUserWords.erase(it);
                    _dynamicUserDirty = true;
                    saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
                    return true;
                }
            }
            return false;
        };
        if (!eraseMatch(true)) {
            eraseMatch(false);
        }
        out.clear();
        reset();
        return true;
    }
    int partialRel = _partialStart - _pageStart;
    bool partial = (_remainder.length() > 0 && idx >= partialRel);
    int pLen = pinyinPrefixLen(_code);
    // 大写后缀才拼进输出; 撇号码("xi'an")剩余部分是纯小写+分隔符, 不能当后缀
    bool hasUpperSuffix = false;
    for (int i = pLen; i < (int)_code.length(); i++) {
        if (_code[i] >= 'A' && _code[i] <= 'Z') { hasUpperSuffix = true; break; }
    }
    // Use per-candidate code length from _candLen for continuation
    int candIdx = idx + _pageStart;
    int consumedLen = (candIdx < (int)_candLen.size()) ? _candLen[candIdx] : 0;
    bool candContinue = (!partial && consumedLen > 0
                         && consumedLen < (int)_code.length()
                         && consumedLen <= 17);
    if (partial || candContinue) {
        if (!partial)
            _remainder = _code.substr(consumedLen);
        if (_remainder.length() == 0 || _remainder.length() >= _code.length()) {
            _prefix.clear();
            _displayCodeDirty = true;
            _remainder.clear();
            reset();
            return true;
        }
        _prefix += out;
        _displayCodeDirty = true;
        _code = _remainder;
        _remainder.clear();
        _partialStart = 0;
        _maxMatchLen = 0;
        out.clear();
        lookup();
        return false;
    }
    if (_prefix.length() > 0) {
        _prefix += out;
        if (hasUpperSuffix) _prefix += _code.substr(pLen);
        _displayCodeDirty = true;
        if (!hasUpperSuffix) {
            addUserWord(_codeOrig, _prefix);
            bumpFrequency(_codeOrig, _prefix);
            for (auto &code : alternateLearningCodes(_codeOrig)) bumpFrequency(code, _prefix);
        }
        out = _prefix;
    } else {
        if (hasUpperSuffix) out += _code.substr(pLen);
        if (!hasUpperSuffix) {
            bumpFrequency(_code, out);
            for (auto &code : alternateLearningCodes(_code)) bumpFrequency(code, out);
        }
    }
    _prefix.clear();
    _displayCodeDirty = true;
    _codeOrig.clear();
    std::string predictKey = lastUtf8Char(out);
    rememberCommittedText(out);
    reset();
    beginPredict(predictKey);
    return true;
}

bool IME::handleFullwidthPunct(int key, std::string &out) {
    // Map ASCII punctuation to fullwidth equivalents when IME is active
    // Only convert specific punctuation, others remain half-width
    switch (key) {
    case ',':  out = "，"; return true; // ，
    case '.':  out = "。"; return true; // 。
    case '?':  out = "？"; return true; // ？
    case ';':  out = "；"; return true; // ；
    case ':':  out = "："; return true; // ：
    case '!':  out = "！"; return true; // ！
    case '(':  out = "（"; return true; // （
    case ')':  out = "）"; return true; // ）
    case '[':  out = "【"; return true; // 【
    case ']':  out = "】"; return true; // 】
    case '{':  out = "「"; return true; // 「
    case '}':  out = "」"; return true; // 」
    case '\\': out = "、"; return true; // 、
    case '^':  out = "……"; return true; // ……
    case '<':  out = "《"; return true; // 《
    case '>':  out = "》"; return true; // 》
    case '`':  out = "·"; return true; // ·
    case '_':  out = "——"; return true; // ——
    case '$':  out = "¥"; return true; // ¥
    case '\'':
        // Single quote pairing: first press = ‘, second press = ’
        if (_singleQuoteOpen) {
            out = "’";
            _singleQuoteOpen = false;
        } else {
            out = "‘";
            _singleQuoteOpen = true;
        }
        return true;
    case '"':
        // Double quote pairing: first press = “, second press = ”
        if (_doubleQuoteOpen) {
            out = "”";
            _doubleQuoteOpen = false;
        } else {
            out = "“";
            _doubleQuoteOpen = true;
        }
        return true;
    default:   return false; // Other characters remain half-width
    }
}

bool IME::handleFullwidthChar(int key, std::string &out) {
    // Fullwidth mode: map ASCII letters, digits, space, and remaining symbols to fullwidth
    if (key >= 'A' && key <= 'Z') {
        // U+FF21 = fullwidth A
        uint32_t cp = 0xFF21 + (key - 'A');
        appendUtf8(cp, out);
        return true;
    }
    if (key >= 'a' && key <= 'z') {
        // U+FF41 = fullwidth a
        uint32_t cp = 0xFF41 + (key - 'a');
        appendUtf8(cp, out);
        return true;
    }
    if (key >= '0' && key <= '9') {
        // U+FF10 = fullwidth 0
        uint32_t cp = 0xFF10 + (key - '0');
        appendUtf8(cp, out);
        return true;
    }
    if (key == ' ') {
        // U+3000 = ideographic space (fullwidth space)
        appendUtf8(0x3000, out);
        return true;
    }
    // Remaining printable ASCII not already handled by handleFullwidthPunct
    if (key >= 0x21 && key <= 0x7E) {
        // U+FF01 = fullwidth !, offset from '!' is key - 0x21
        uint32_t cp = 0xFF01 + (key - 0x21);
        appendUtf8(cp, out);
        return true;
    }
    return false;
}

bool IME::handleKey(int key, std::string &out) {
    if (!_active) return false;
    if (_english && !_englishCompose) {
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
            _englishCompose = true;
            _code = (char)key;
            _displayCodeDirty = true;
            lookupEnglishMode();
            return true;
        }
        return false;
    }
    if (_englishCompose) {
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
            key == '\'' || key == '-' || key == '_') {
            if ((int)_code.length() < 32) {
                _code += (char)key;
                _displayCodeDirty = true;
                lookupEnglishMode();
            }
            return true;
        }
        if (key >= '1' && key <= '9') { commit(key - '1', out); return true; }
        if (key == ' ') {
            if (_page.size() > 0) commit(0, out);
            else { out = _code; _lastCommitChar.clear(); reset(); }
            return true;
        }
        if (key == '\n') { out = _code; _lastCommitChar.clear(); reset(); return true; }
        if (key == '\b') {
            if (_code.length() > 0) _code.erase(_code.length() - 1);
            _displayCodeDirty = true;
            if (_code.empty()) reset();
            else lookupEnglishMode();
            return true;
        }
        if (key == 27) { reset(); return true; }
        if (key == IME_KEY_UP || key == '-' || key == ';' || key == ',') { pagePrev(); return true; }
        if (key == IME_KEY_DOWN || key == '=' || key == '.') { pageNext(); return true; }
        if (_page.size() > 0) commit(0, out);
        else { out = _code; _lastCommitChar.clear(); }
        reset();
        return true;
    }
    if (_vMode) {
        if (_code == "v" && key >= 0x21 && key <= 0x7E &&
            !((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
              (key >= '0' && key <= '9') || key == '/')) {
            out.assign(1, (char)key);
            _lastCommitChar.clear();
            reset();
            return true;
        }
        // 数字键直选候选: v/字母编码(含闭合 v/编码/ 与命令 v/t/ v/d/ v/w/)。
        // 裸v与数字/日期编码不拦截, 数字键继续进编码(如 v/5、v2026-9-7)。
        bool vDigitSel = false;
        if (!_page.empty() && _code.length() > 2 && _code[1] == '/') {
            std::string q = _code.substr(2);
            if (!q.empty() && q.back() == '/') q.pop_back();
            vDigitSel = !q.empty();
            for (char c : q) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { vDigitSel = false; break; }
            }
        }
        if (vDigitSel && key >= '1' && key <= '9') { commit(key - '1', out); return true; }
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
            (key >= '0' && key <= '9') || key == '.' || key == '-' ||
            key == '/' || key == '!') {
            if ((int)_code.length() < 32) {
                _code += (char)key;
                _displayCodeDirty = true;
                lookupVMode();
            }
            return true;
        }
        if (key == IME_KEY_LEFT) {
            if (!_page.empty()) _vSel = (_vSel + (int)_page.size() - 1) % (int)_page.size();
            return true;
        }
        if (key == IME_KEY_RIGHT) {
            if (!_page.empty()) _vSel = (_vSel + 1) % (int)_page.size();
            return true;
        }
        if (key == ' ') {
            if (_page.size() > 0) commit(_vSel, out);
            else { out = _code.length() > 1 ? _code.substr(1) : ""; _lastCommitChar.clear(); reset(); }
            return true;
        }
        if (key == '\n') {
            out = _page.size() > 0 ? _page[_vSel] : (_code.length() > 1 ? _code.substr(1) : "");
            _lastCommitChar.clear();
            reset();
            return true;
        }
        if (key == '\b') {
            if (_code.length() > 1) {
                _code.erase(_code.length() - 1);
                _displayCodeDirty = true;
                lookupVMode();
            } else reset();
            return true;
        }
        if (key == 27) { reset(); return true; }
        if (key == IME_KEY_UP || key == '-' || key == ';' || key == ',') { pagePrev(); return true; }
        if (key == IME_KEY_DOWN || key == '=' || key == '\'') { pageNext(); return true; }
        return true;
    }
    if (_predicting) {
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
            _predicting = false;
            _code = (char)key;
            _displayCodeDirty = true;
            lookup();
            return true;
        }
        if (key >= '1' && key <= '9') {
            int idx = key - '1';
            if (idx < (int)_page.size()) {
                std::string predKey = _predChar;
                out = _page[idx];
                _predicting = false;
                bumpPredictFrequency(predKey, out);
                rememberCommittedText(out);
                beginPredict(lastUtf8Char(out));
            }
            return true;
        }
        if (key == ' ') {
            if (_page.size() > 0) {
                std::string predKey = _predChar;
                out = _page[0];
                _predicting = false;
                bumpPredictFrequency(predKey, out);
                rememberCommittedText(out);
                beginPredict(lastUtf8Char(out));
            }
            return true;
        }
        if (key == IME_KEY_UP || key == '-' || key == ';' || key == ',') { pagePrev(); return true; }
        if (key == IME_KEY_DOWN || key == '=' || key == '\'' || key == '.') { pageNext(); return true; }
        if (key == '\b' || key == 27 || key == '\n') {
            _predicting = false;
            return true;
        }
        _predicting = false;
        return false;
    }
    // In fullwidth mode with no composition, output letters/digits/space as fullwidth
    if (_fullwidth && _code.length() == 0 && !_deleteMode && !_lfMode && !_vMode) {
        if (((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) ||
            (key >= '0' && key <= '9') || key == ' ') {
            return handleFullwidthChar(key, out);
        }
    }
    if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
        char cl = (char)tolower(key);
        char c = (char)key;
        if (_code.length() == 0 && !_deleteMode && !_lfMode && key >= 'A' && key <= 'Z') {
            _englishCompose = true;
            _code = (char)key;
            _displayCodeDirty = true;
            lookupEnglishMode();
            return true;
        }
        if (_code.length() == 0 && !_deleteMode && !_lfMode && cl == 'v') {
            _vMode = true;
            _code = "v";
            _displayCodeDirty = true;
            lookupVMode();
            return true;
        }
#if PJOURNAL_IME_ENABLE_LIANGFEN
        if (_code.length() == 0 && !_deleteMode && !_lfMode && cl == 'u') {
            loadLfDict();
            if (_lfBlob) { _lfMode = true; _maxCode = 12; return true; }
        }
#endif
        if ((int)_code.length() < _maxCode) {
            _code += c;
            _displayCodeDirty = true;
            lookup();
        }
        return true;
    }
    if (_code.length() == 0) {
        if (handleFullwidthPunct(key, out)) { _lastCommitChar.clear(); return true; }
        if (_fullwidth && handleFullwidthChar(key, out)) { _lastCommitChar.clear(); return true; }
        return false;
    }
    // 单引号编码分词: 拼音模式下 ' 显式分隔音节(如 xi'an); 两分模式保留翻页
    if (key == '\'' && !_lfMode) {
        if (_code.back() == '\'') return true;
        if ((int)_code.length() < _maxCode) {
            _code += '\'';
            _displayCodeDirty = true;
            lookup();
        }
        return true;
    }
    if (key >= '1' && key <= '9') {
        commit(key - '1', out);
        return true;
    }
    if (key == ' ') {
        if (_page.size() > 0) commit(0, out);
        else reset();
        return true;
    }
    if (key == '\n') {
        out = _code;
        _lastCommitChar.clear();
        reset();
        return true;
    }
    if (key == '\b') {
        if (_prefix.length() > 0) {
            _code = _codeOrig;
            if (_code.length() > 0) {
                _code.erase(_code.length() - 1);
            }
            _prefix.clear();
            _remainder.clear();
            _partialStart = 0;
            _maxMatchLen = 0;
            _displayCodeDirty = true;
            if (_code.length() == 0) reset();
            else lookup();
        } else if (_code.length() > 0) {
            _code.erase(_code.length() - 1);
            _displayCodeDirty = true;
            if (_code.length() == 0) reset();
            else lookup();
        }
        return true;
    }
    if (key == 27) {
        reset();
        return true;
    }
    if (key == IME_KEY_UP || key == '-' || key == ';' || key == ',') { pagePrev(); return true; }
    if (key == IME_KEY_DOWN || key == '=' || key == '.') { pageNext(); return true; }
    if (_page.size() > 0) {
        commit(0, out);
        return true;
    }
    reset();
    return true;
}
