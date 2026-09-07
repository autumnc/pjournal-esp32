#include "IME.h"
#include "seg_table.h"
#include "trad_table.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <esp_log.h>
#include <sys/stat.h>

static const char *IME_TAG = "IME";
static const uint8_t IME_MAGIC[4] = {'I', 'M', 'E', '3'};

// 用户词典持久化到 SD 卡(与设置同目录)。NVS 分区仅 24KB 且写入失败会静默
// 丢失/启动时整区擦除,改用 SD 文件后容量无上限,重启与重刷固件均保留。
static const char *USERDICT_DYNAMIC_PATH = "/sdcard/settings/userdict.txt";
static const char *USERDICT_FIXED_PATH = "/sdcard/settings/userdict_fixed.txt";
static const char *ENGLISHDICT_PATH = "/sdcard/settings/englishdict.txt";
static const size_t USERDICT_FIXED_LIMIT = 500;
static const size_t USERDICT_DYNAMIC_LIMIT = 1000;
static const size_t ENGLISHDICT_LIMIT = 10000;
static const int IME_KEY_UP = 0x80;
static const int IME_KEY_DOWN = 0x81;

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

static const char *V_PUNCT_CANDIDATES[] = {
    "，", "。", "、", "？", "！", "：", "；", "…", "……", "—", "——", "·",
    "“", "”", "“”", "‘", "’", "‘’", "「", "」", "「」", "『", "』", "『』",
    "（", "）", "（）", "【", "】", "【】", "［", "］", "［］", "〔", "〕", "〔〕",
    "｛", "｝", "｛｝", "《", "》", "《》", "〈", "〉", "〈〉", "〖", "〗", "〖〗",
    "〝", "〞", "〝〞",
    "．", "／", "＼", "～", "｜", "＃", "＠", "＆", "％", "＊", "＋", "－", "＝",
    "＿", "＾", "＄", "＜", "＞",
    ",", ".", "?", "!", ":", ";", "'", "\"", "(", ")", "[", "]", "{", "}",
    "<", ">", "/", "\\", "-", "_", "+", "=", "*", "#", "@", "&", "%", "$",
    "^", "~", "`", "|"
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

// Embedded liangfen dictionary
extern const uint8_t liangfen_bin_start[] asm("_binary_liangfen_bin_start");
extern const uint8_t liangfen_bin_end[]   asm("_binary_liangfen_bin_end");

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
    if (memcmp(hdrIndex, IME_MAGIC, 4) != 0) {
        ESP_LOGE(IME_TAG, "bad dictionary magic");
        return false;
    }
    _scheme = (Scheme)hdrIndex[4];
    _codeLen = hdrIndex[5];
    if (_codeLen < 1 || _codeLen > MAX_CODE_LEN) {
        ESP_LOGE(IME_TAG, "bad codeLen %d", _codeLen);
        return false;
    }
    _recordSize = _codeLen + HANZI_SIZE + FLAG_SIZE;
    switch (_scheme) {
    case PINYIN:    _maxCode = 20; break;
    case SHUANGPIN: _maxCode = 2; break;
    case WUBI:
    default:        _maxCode = 4; break;
    }
    _count = (uint32_t)hdrIndex[8] | ((uint32_t)hdrIndex[9] << 8) |
             ((uint32_t)hdrIndex[10] << 16) | ((uint32_t)hdrIndex[11] << 24);
    _recordBase = HEADER_SIZE + (size_t)INDEX_ENTRIES * 4;
    size_t need = _recordBase + (size_t)_count * _recordSize;
    if (_count == 0 || need > total) {
        ESP_LOGE(IME_TAG, "dictionary size mismatch: need %u, have %u",
                 (unsigned)need, (unsigned)total);
        return false;
    }
    _index.resize(INDEX_ENTRIES);
    for (int k = 0; k < INDEX_ENTRIES; k++) {
        const uint8_t *p = hdrIndex + HEADER_SIZE + (size_t)k * 4;
        _index[k] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    size_t wordBase = _recordBase + (size_t)_count * _recordSize;
    if (wordBase + 4 <= total) {
        const uint8_t *wp = hdrIndex + wordBase;
        _wordCount = (uint32_t)wp[0] | ((uint32_t)wp[1] << 8) |
                     ((uint32_t)wp[2] << 16) | ((uint32_t)wp[3] << 24);
        if (_wordCount > 0 && wordBase + 4 + INDEX_ENTRIES * 4 <= total) {
            wp += 4;
            _wordIndex.resize(INDEX_ENTRIES);
            for (int k = 0; k < INDEX_ENTRIES; k++) {
                const uint8_t *p = wp + (size_t)k * 4;
                _wordIndex[k] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            }
            wp += INDEX_ENTRIES * 4;
            _wordData = wp;
            _wordDataSize = total - (_recordBase + _count * _recordSize)
                            - 4 - INDEX_ENTRIES * 4;
            size_t predBase = wordBase + 4 + INDEX_ENTRIES * 4 + _wordDataSize;
            if (predBase + 4 <= total) {
                const uint8_t *pp = hdrIndex + predBase;
                _predCount = (uint32_t)pp[0] | ((uint32_t)pp[1] << 8) |
                             ((uint32_t)pp[2] << 16) | ((uint32_t)pp[3] << 24);
                if (_predCount > 0) {
                    _predData = pp + 4;
                    _predDataSize = total - predBase - 4;
                }
            }
        }
    }
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
                    entries.erase(entries.begin());
                    hadDuplicates = true;
                }
                entries.push_back({code, word, count, trad});
            }
        }
    }
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
    _userDictLoaded = true;
}

void IME::ensureUserDictLoaded() {
    if (!_userDictLoaded) loadUserDict();
}

void IME::saveUserDictFile(const char *path, std::vector<UserEntry> &entries, bool &dirty) {
    if (!dirty) return;
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
    if (_dynamicUserWords.size() >= USERDICT_DYNAMIC_LIMIT)
        _dynamicUserWords.erase(_dynamicUserWords.begin());
    _dynamicUserWords.push_back({code, word, 0, _trad});
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
    const std::vector<UserEntry> &src = (kind == FIXED_DICT) ? _fixedUserWords : _dynamicUserWords;
    std::vector<UserEntryView> out;
    out.reserve(src.size());
    for (auto &p : src) out.push_back({p.code, p.word, p.count, p.trad});
    return out;
}

bool IME::addUserDictEntry(UserDictKind kind, const std::string &code, const std::string &word) {
    ensureUserDictLoaded();
    if (word.length() < 3 || code.length() == 0) return false;
    std::vector<UserEntry> &entries = (kind == FIXED_DICT) ? _fixedUserWords : _dynamicUserWords;
    bool &dirty = (kind == FIXED_DICT) ? _fixedUserDirty : _dynamicUserDirty;
    const char *path = (kind == FIXED_DICT) ? USERDICT_FIXED_PATH : USERDICT_DYNAMIC_PATH;
    size_t limit = (kind == FIXED_DICT) ? USERDICT_FIXED_LIMIT : USERDICT_DYNAMIC_LIMIT;
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
        entries.erase(entries.begin());
    }
    entries.push_back({code, word, 1, _trad});
    dirty = true;
    saveUserDictFile(path, entries, dirty);
    return true;
}

void IME::removeUserDictEntries(UserDictKind kind, const std::vector<int> &indices) {
    ensureUserDictLoaded();
    std::vector<UserEntry> &entries = (kind == FIXED_DICT) ? _fixedUserWords : _dynamicUserWords;
    bool &dirty = (kind == FIXED_DICT) ? _fixedUserDirty : _dynamicUserDirty;
    const char *path = (kind == FIXED_DICT) ? USERDICT_FIXED_PATH : USERDICT_DYNAMIC_PATH;
    for (int n = (int)entries.size() - 1; n >= 0; n--) {
        if (std::find(indices.begin(), indices.end(), n) != indices.end()) {
            entries.erase(entries.begin() + n);
            dirty = true;
        }
    }
    if (dirty) saveUserDictFile(path, entries, dirty);
}

size_t IME::userDictSize(UserDictKind kind) const {
    return (kind == FIXED_DICT) ? _fixedUserWords.size() : _dynamicUserWords.size();
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
    for (char c : buf) out += D[c - '0'];
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

// v/time / v/date / v/week 的候选:当前时刻按 纯数字/数字加中文/纯中文(星期为 英文/中文)生成
static std::vector<std::string> vTimeDateWeek(const std::string &body) {
    std::vector<std::string> out;
    time_t now;
    time(&now);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[48];
    if (body == "/time") {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        out.push_back(buf);
        snprintf(buf, sizeof(buf), "%02d时%02d分%02d秒", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        out.push_back(buf);
        snprintf(buf, sizeof(buf), "%s时%s分%s秒", chineseDayMonth(tmv.tm_hour).c_str(),
                 chineseDayMonth(tmv.tm_min).c_str(), chineseDayMonth(tmv.tm_sec).c_str());
        out.push_back(buf);
    } else if (body == "/date") {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        out.push_back(buf);
        snprintf(buf, sizeof(buf), "%d年%d月%d日", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        out.push_back(buf);
        snprintf(buf, sizeof(buf), "%s年%s月%s日", chineseYear(tmv.tm_year + 1900).c_str(),
                 chineseDayMonth(tmv.tm_mon + 1).c_str(), chineseDayMonth(tmv.tm_mday).c_str());
        out.push_back(buf);
    } else if (body == "/week") {
        static const char *EN[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
        static const char *CN[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
        out.push_back(EN[tmv.tm_wday]);
        out.push_back(CN[tmv.tm_wday]);
    }
    return out;
}

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

void IME::bumpFrequency(const std::string &code, const std::string &word) {
    ensureUserDictLoaded();
    for (auto &p : _fixedUserWords) {
        if (p.code == code && p.word == word && p.trad == _trad) return;
    }
    for (auto it = _dynamicUserWords.begin(); it != _dynamicUserWords.end(); ++it) {
        if (it->code == code && it->word == word) {
            it->count++;
            _dynamicUserDirty = true;
            saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
            return;
        }
    }
    if (word.length() >= 3 && code.length() >= 1) {
        addUserWord(code, word);
        for (auto &p : _dynamicUserWords)
            if (p.code == code && p.word == word) { p.count++; break; }
    }
}

bool IME::readCode(uint32_t i, char out[MAX_CODE_LEN + 1]) {
    const uint8_t *rec = _blob + _recordBase + (size_t)i * _recordSize;
    int n = 0;
    for (; n < _codeLen && rec[n]; n++) out[n] = (char)rec[n];
    out[n] = '\0';
    return true;
}

bool IME::readHanzi(uint32_t i, char out[HANZI_SIZE + 1]) {
    const uint8_t *rec = _blob + _recordBase + (size_t)i * _recordSize + _codeLen;
    out[0] = (char)rec[0]; out[1] = (char)rec[1];
    out[2] = (char)rec[2]; out[3] = '\0';
    return true;
}

uint8_t IME::readRecordFlag(uint32_t i) {
    const uint8_t *rec = _blob + _recordBase + (size_t)i * _recordSize;
    return rec[_codeLen + HANZI_SIZE];
}

uint8_t IME::readLfFlag(uint16_t i) {
    const uint8_t *rec = _lfBlob + _lfRecordBase + (size_t)i * 16;
    return rec[15];
}

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
    _englishCompose = false;
}

int IME::pinyinPrefixLen(const std::string &code) {
    int i = 0;
    while (i < (int)code.length() && code[i] >= 'a' && code[i] <= 'z') i++;
    return i;
}

void IME::searchWindow(const char *code, int len, uint32_t &lo, uint32_t &hi) {
    lo = 0; hi = _count;
    if (_index.empty() || len < 1) return;
    int c0 = code[0] - 'a';
    if (c0 < 0 || c0 >= 26) return;
    if (len == 1) {
        lo = _index[c0 * 26];
        hi = _index[(c0 + 1) * 26];
        return;
    }
    int c1 = code[1] - 'a';
    if (c1 < 0 || c1 >= 26) return;
    int k = c0 * 26 + c1;
    lo = _index[k];
    hi = _index[k + 1];
}

void IME::lookup() {
    _all.clear();
    _candLen.clear();
    _pageStart = 0;
    _curPage = 0;
    _maxMatchLen = 0;
    if (_prefix.length() == 0) _codeOrig = _code;
    static bool dictLoaded = false;
    if (!dictLoaded) { dictLoaded = true; ensureUserDictLoaded(); loadLfDict(); }

    if (!_loaded || (_code.length() == 0 && !_deleteMode)) {
        buildPage();
        return;
    }

    // 单引号编码分词: 显式按音节分段匹配(词组/补充表/用户词典)
    if (_code.find('\'') != std::string::npos) {
        lookupSegmented();
        buildPage();
        return;
    }

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
        buildPage();
        return;
    }

    q = pinyinCode.c_str();
    qlen = pinyinLen;

    if (_lfMode && _lfBlob) {
        uint32_t llo, lhi;
        searchLfWindow(q, qlen, llo, lhi);
        while (llo < lhi) {
            uint32_t mid = llo + (lhi - llo) / 2;
            char code[13]; if (!readLfCode(mid, code)) break;
            if (strncmp(code, q, qlen) < 0) llo = mid + 1;
            else lhi = mid;
        }
        for (uint32_t i = llo; i < _lfCount && _all.size() < MAX_CANDIDATES; i++) {
            char code[13]; if (!readLfCode(i, code)) break;
            if (strncmp(code, q, qlen) != 0) break;
            uint8_t f = readLfFlag(i);
            if (f & (_trad ? 0x01 : 0x02)) continue;
            char hz[4]; if (!readLfHanzi(i, hz)) break;
            std::string h(hz);
            bool dup = false;
            for (auto &e : _all) if (e == h) { dup = true; break; }
            if (!dup) {
                _all.push_back(h);
                _candLen.push_back(0);
            }
        }
        buildPage();
        return;
    }

    if (_deleteMode) {
        std::vector< std::pair<int, std::string> > userMatches;
        for (auto &p : _dynamicUserWords) {
            if (p.trad != _trad) continue;
            if (qlen == 0 ||
                ((int)p.code.length() >= qlen && strncmp(p.code.c_str(), q, qlen) == 0)) {
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
            _all.push_back(m.second);
            _candLen.push_back(0);
            if (_all.size() >= MAX_CANDIDATES) break;
        }
        buildPage();
        return;
    }

    // Phase 1: user dict single chars — highest priority (phrases emitted in Phase 3)
    std::vector< std::pair<int, std::string> > userWordFreq;
    {
        std::vector< std::pair<int, std::string> > userSingleFreq;
        auto scanUserWords = [&](const std::vector<UserEntry> &entries) {
        for (auto &p : entries) {
            if (p.trad != _trad) continue;
            if ((int)p.code.length() < qlen) continue;
            if (strncmp(p.code.c_str(), q, qlen) != 0) continue;
            if (p.word.length() <= 3) {
                // single char
                bool found = false;
                for (auto &uf : userSingleFreq) {
                    if (uf.second == p.word) {
                        found = true;
                        if (uf.first < p.count) uf.first = p.count;
                        break;
                    }
                }
                if (!found) userSingleFreq.push_back({p.count, p.word});
            } else {
                // phrase
                bool found = false;
                for (auto &uf : userWordFreq) {
                    if (uf.second == p.word) {
                        found = true;
                        if (uf.first < p.count) uf.first = p.count;
                        break;
                    }
                }
                if (!found) userWordFreq.push_back({p.count, p.word});
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
            _all.push_back(f.second);
            _candLen.push_back(0);
            if (_all.size() >= MAX_CANDIDATES) break;
        }
        if (_all.size() >= MAX_CANDIDATES) { buildPage(); return; }
    }

    // Phase 2: single char prefix match (dictionary)
    bool hasVowel = false;
    for (int i = 0; i < qlen; i++) {
        if (strchr("aeiouv", q[i])) { hasVowel = true; break; }
    }
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
        for (uint32_t i = lo; i < scanEnd && _all.size() < MAX_CANDIDATES; i++) {
            char code[7];
            if (!readCode(i, code)) break;
            if (strncmp(code, q, qlen) != 0) break;
            uint8_t f = readRecordFlag(i);
            if (f & (_trad ? 0x01 : 0x02)) continue;
            char hz[4];
            if (!readHanzi(i, hz)) break;
            std::string h(hz);
            bool dup = false;
            for (auto &e : _all) if (e == h) { dup = true; break; }
            if (!dup) {
                _all.push_back(h);
                _candLen.push_back((int)strlen(code));
                if ((int)strlen(code) > _maxMatchLen) _maxMatchLen = (int)strlen(code);
            }
        }
    }
    if (_all.size() >= MAX_CANDIDATES) { buildPage(); return; }

    // 补充词典表全码精确匹配: xian→西安, anguang→暗光 (优先级: 单字之后, 词组之前)
    if (_all.size() < MAX_CANDIDATES) {
        for (int i = 0; i < SEG_TABLE_COUNT && _all.size() < MAX_CANDIDATES; i++) {
            std::string entryCode;
            for (const char *p = SEG_TABLE[i].syllables; *p; p++)
                if (*p != ' ') entryCode += *p;
            if ((int)entryCode.length() != qlen ||
                strncmp(entryCode.c_str(), q, qlen) != 0) continue;
            std::string w = SEG_TABLE[i].word;
            bool dup = false;
            for (auto &e : _all) if (e == w) { dup = true; break; }
            if (!dup) {
                _all.push_back(w);
                _candLen.push_back(qlen);
                if (qlen > _maxMatchLen) _maxMatchLen = qlen;
            }
        }
    }
    if (_all.size() >= MAX_CANDIDATES) { buildPage(); return; }

    // Phase 3: user dict phrases — after dictionary single chars, before dictionary phrases
    {
        std::sort(userWordFreq.begin(), userWordFreq.end(),
            [](const std::pair<int,std::string> &a, const std::pair<int,std::string> &b) {
                return a.first > b.first;
            });
        for (auto &f : userWordFreq) {
            _all.push_back(f.second);
            _candLen.push_back(0);
            if (_all.size() >= MAX_CANDIDATES) break;
        }
        if (_all.size() >= MAX_CANDIDATES) { buildPage(); return; }
    }
    size_t p4Start = _all.size();  // 词典词组排序起点(不含用户词组)

    // Phase 4: phrase prefix match (word dictionary)
    if (hasVowel && _wordCount > 0 && _wordData) {
        size_t wlo = 0, whi = _wordDataSize;
        if (qlen >= 2) {
            int k = (q[0] - 'a') * 26 + (q[1] - 'a');
            if (k >= 0 && k < INDEX_ENTRIES) {
                wlo = _wordIndex[k];
                whi = (k + 1 < INDEX_ENTRIES) ? _wordIndex[k + 1] : _wordDataSize;
            }
        }
        size_t wpos = wlo;
        int safety = 0;
        while (wpos < whi && _all.size() < MAX_CANDIDATES && safety++ < 5000) {
            uint8_t cl = _wordData[wpos];
            if (cl == 0 || wpos + 1 + cl > whi) break;
            const char *wc = (const char *)_wordData + wpos + 1;
            wpos += 1 + cl;
            if (wpos >= whi) break;
            uint8_t n = _wordData[wpos++];
            int matchLen = std::min((int)cl, qlen);
            if (strncmp(wc, q, matchLen) == 0) {
                for (uint8_t j = 0; j < n && wpos < whi; j++) {
                    uint8_t wl = _wordData[wpos++];
                    if (wl == 0 || wpos + wl + 1 > whi) break;
                    uint8_t wf = _wordData[wpos + wl];
                    if ((int)cl < qlen && wl <= 3) {
                        wpos += wl + 1;
                        continue;
                    }
                    std::string w;
                    w.append((const char *)_wordData + wpos, wl);
                    if (wordVisible(_trad, w, wf)) {
                        bool dup = false;
                        for (auto &e : _all) if (e == w) { dup = true; break; }
                        if (!dup) {
                            _all.push_back(w);
                            _candLen.push_back(std::min((int)cl, qlen));
                            if ((int)cl > _maxMatchLen) _maxMatchLen = (int)cl;
                        }
                    }
                    wpos += wl + 1;
                }
            } else {
                for (uint8_t j = 0; j < n && wpos < whi; j++) {
                    uint8_t wl = _wordData[wpos++];
                    if (wpos + wl + 1 > whi) break;
                    wpos += wl + 1;
                }
            }
        }
    }
    // Sort Phase 4 entries by consumed length descending
    {
        size_t p4End = _all.size();
        size_t p4Count = p4End - p4Start;
        if (p4Count > 1) {
            std::vector<int> order(p4Count);
            for (size_t i = 0; i < order.size(); i++) order[i] = (int)(p4Start + i);
            std::stable_sort(order.begin(), order.end(),
                [this](int a, int b) { return _candLen[a] > _candLen[b]; });
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
    }
    if (_all.size() >= MAX_CANDIDATES) { buildPage(); return; }

    // Phase 5: user dict initial match
    if (!hasVowel && (_fixedUserWords.size() > 0 || _dynamicUserWords.size() > 0)) {
        std::vector< std::pair<int, std::string> > userInitFreq;
        auto scanInitialWords = [&](const std::vector<UserEntry> &entries) {
        for (auto &p : entries) {
            if (p.trad != _trad) continue;
            if (p.code.find('\'') != std::string::npos) continue;  // 撇号码只在分词路径匹配
            int cl = (int)p.code.length();
            if (cl < 2) continue;
            const char *wc = p.code.c_str();
            char init[13]; int o = 0;
            for (int i = 0; i < cl && o < 12; ) {
                if (strchr("aeiouv", wc[i])) {
                    while (i < cl && strchr("aeiouvngr", wc[i]) && o < 12)
                        init[o++] = wc[i++];
                    continue;
                }
                if (i+1 < cl && (wc[i]=='z'||wc[i]=='c'||wc[i]=='s') && wc[i+1]=='h') {
                    init[o++] = wc[i];
                    init[o++] = wc[i+1];
                    i += 2;
                } else {
                    init[o++] = wc[i];
                    i++;
                }
                while (i < cl && strchr("aeiouv", wc[i])) i++;
                if (i < cl && strchr("ngr", wc[i])) {
                    int j = i;
                    while (j < cl && strchr("ngr", wc[j])) j++;
                    if (j >= cl || !strchr("aeiouv", wc[j])) i = j;
                }
            }
            init[o] = 0;
            if (o >= qlen && strncmp(init, q, qlen) == 0) {
                bool found = false;
                for (auto &uf : userInitFreq) {
                    if (uf.second == p.word) {
                        found = true;
                        if (uf.first < p.count) uf.first = p.count;
                        break;
                    }
                }
                if (!found) userInitFreq.push_back({p.count, p.word});
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
            _all.push_back(f.second);
            _candLen.push_back(0);
            if (_all.size() >= MAX_CANDIDATES) break;
        }
        if (_all.size() >= MAX_CANDIDATES) { buildPage(); return; }
    }

    // Phase 6: initial match (no vowel, consonant-only)
    if (!hasVowel && _wordCount > 0 && _wordData) {
        size_t slo = 0, shi = _wordDataSize;
        if (qlen >= 1 && q[0] >= 'a' && q[0] <= 'z') {
            int k = (q[0] - 'a') * 26;
            slo = _wordIndex[k];
            shi = (k + 26 < INDEX_ENTRIES) ? _wordIndex[k + 26] : _wordDataSize;
        }
        size_t spos = slo;
        int safety = 0;
        while (spos < shi && _all.size() < MAX_CANDIDATES && safety++ < 60000) {
            uint8_t cl = _wordData[spos];
            if (cl == 0 || spos + 1 + cl > shi) break;
            const char *wc = (const char *)_wordData + spos + 1;
            spos += 1 + cl;
            if (spos >= shi) break;
            uint8_t n = _wordData[spos++];
            char init[13]; int o = 0;
            for (int i = 0; i < cl && o < 12; ) {
                if (strchr("aeiouv", wc[i])) {
                    while (i < cl && strchr("aeiouvngr", wc[i]) && o < 12)
                        init[o++] = wc[i++];
                    continue;
                }
                if (i+1 < cl && (wc[i]=='z'||wc[i]=='c'||wc[i]=='s') && wc[i+1]=='h') {
                    init[o++] = wc[i];
                    init[o++] = wc[i+1];
                    i += 2;
                } else {
                    init[o++] = wc[i];
                    i++;
                }
                while (i < cl && strchr("aeiouv", wc[i])) i++;
                if (i < cl && strchr("ngr", wc[i])) {
                    int j = i;
                    while (j < cl && strchr("ngr", wc[j])) j++;
                    if (j >= cl || !strchr("aeiouv", wc[j])) i = j;
                }
            }
            init[o] = 0;
            if (o >= qlen && strncmp(init, q, qlen) == 0) {
                for (uint8_t j = 0; j < n && spos < shi; j++) {
                    uint8_t wl = _wordData[spos++];
                    if (wl == 0 || spos + wl + 1 > shi) break;
                    uint8_t wf = _wordData[spos + wl];
                    std::string w;
                    w.append((const char *)_wordData + spos, wl);
                    if (_all.size() < MAX_CANDIDATES && wordVisible(_trad, w, wf)) {
                        bool dup = false;
                        for (auto &e : _all) if (e == w) { dup = true; break; }
                        if (!dup) {
                            _all.push_back(w);
                            _candLen.push_back((int)cl);
                            if ((int)cl > _maxMatchLen) _maxMatchLen = (int)cl;
                        }
                    }
                    spos += wl + 1;
                }
            } else {
                for (uint8_t j = 0; j < n && spos < shi; j++) {
                    uint8_t wl = _wordData[spos++];
                    if (spos + wl + 1 > shi) break;
                    spos += wl + 1;
                }
            }
        }
    }

    // Phase 7: shorthand + tail match
    {
        bool shorthandTail = false;
        std::string typedInit;
        std::string typedTail;
        if (qlen >= 3 && hasVowel) {
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
        if (shorthandTail && _wordCount > 0 && _wordData && _all.size() < MAX_CANDIDATES) {
            size_t slo = 0, shi = _wordDataSize;
            if (typedInit.length() >= 1 && typedInit[0] >= 'a' && typedInit[0] <= 'z') {
                int k = (typedInit[0] - 'a') * 26;
                slo = _wordIndex[k];
                shi = (k + 26 < INDEX_ENTRIES) ? _wordIndex[k + 26] : _wordDataSize;
            }
            size_t spos = slo;
            int safety = 0;
            while (spos < shi && _all.size() < MAX_CANDIDATES && safety++ < 60000) {
                uint8_t cl = _wordData[spos];
                if (cl == 0 || spos + 1 + cl > shi) break;
                const char *wc = (const char *)_wordData + spos + 1;
                spos += 1 + cl;
                if (spos >= shi) break;
                uint8_t n = _wordData[spos++];
                char init[13]; int o = 0;
                for (int i = 0; i < cl && o < 12; ) {
                    if (strchr("aeiouv", wc[i])) {
                        while (i < cl && strchr("aeiouvngr", wc[i]) && o < 12)
                            init[o++] = wc[i++];
                        continue;
                    }
                    if (i+1 < cl && (wc[i]=='z'||wc[i]=='c'||wc[i]=='s') && wc[i+1]=='h') {
                        init[o++] = wc[i];
                        init[o++] = wc[i+1];
                        i += 2;
                    } else {
                        init[o++] = wc[i];
                        i++;
                    }
                    while (i < cl && strchr("aeiouv", wc[i])) i++;
                    if (i < cl && strchr("ngr", wc[i])) {
                        int j = i;
                        while (j < cl && strchr("ngr", wc[j])) j++;
                        if (j >= cl || !strchr("aeiouv", wc[j])) i = j;
                    }
                }
                init[o] = 0;
                if (o < (int)typedInit.length() || strncmp(init, typedInit.c_str(), typedInit.length()) != 0) {
                    for (uint8_t j = 0; j < n && spos < shi; j++) {
                        uint8_t wl = _wordData[spos++];
                        if (spos + wl + 1 > shi) break;
                        spos += wl + 1;
                    }
                    continue;
                }
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
                if (candTailLen >= (int)typedTail.length())
                    tailMatch = (strncmp(candTailStart, typedTail.c_str(), typedTail.length()) == 0);
                else
                    tailMatch = (strncmp(typedTail.c_str(), candTailStart, candTailLen) == 0);
                if (!tailMatch) {
                    for (uint8_t j = 0; j < n && spos < shi; j++) {
                        uint8_t wl = _wordData[spos++];
                        if (spos + wl + 1 > shi) break;
                        spos += wl + 1;
                    }
                    continue;
                }
                for (uint8_t j = 0; j < n && spos < shi; j++) {
                    uint8_t wl = _wordData[spos++];
                    if (wl == 0 || spos + wl + 1 > shi) break;
                    uint8_t wf = _wordData[spos + wl];
                    std::string w;
                    w.append((const char *)_wordData + spos, wl);
                    if (_all.size() < MAX_CANDIDATES && wordVisible(_trad, w, wf)) {
                        bool dup = false;
                        for (auto &e : _all) if (e == w) { dup = true; break; }
                        if (!dup) {
                            _all.push_back(w);
                            _candLen.push_back((int)cl);
                            if ((int)cl > _maxMatchLen) _maxMatchLen = (int)cl;
                        }
                    }
                    spos += wl + 1;
                }
            }
        }
    }

    // Phase 8: partial (逐字) match
    _partialStart = (int)_all.size();
    _remainder.clear();
    if (qlen > 1 && _all.size() < MAX_CANDIDATES) {
        uint32_t zlo, zhi;
        int maxTry = qlen - 1;
        if (_maxMatchLen > 0 && _maxMatchLen < maxTry)
            maxTry = _maxMatchLen - 1;
        for (int tryLen = maxTry; tryLen >= 1 && _all.size() < MAX_CANDIDATES; tryLen--) {
            searchWindow(q, tryLen, zlo, zhi);
            uint32_t sEnd = zhi;
            int bcount = 0;
            while (zlo < zhi && bcount++ < 200) {
                uint32_t mid = zlo + (zhi - zlo) / 2;
                char code[7]; if (!readCode(mid, code)) break;
                if (strncmp(code, q, tryLen) < 0) zlo = mid + 1;
                else zhi = mid;
            }
            for (uint32_t i = zlo; i < sEnd && _all.size() < MAX_CANDIDATES; i++) {
                char code[7]; if (!readCode(i, code)) break;
                if (strncmp(code, q, tryLen) != 0) break;
                uint8_t f = readRecordFlag(i);
                if (f & (_trad ? 0x01 : 0x02)) continue;
                char hz[4]; if (!readHanzi(i, hz)) break;
                std::string h(hz);
                bool dup = false;
                for (auto &e : _all) if (e == h) { dup = true; break; }
                if (!dup) {
                    _all.push_back(h);
                    _candLen.push_back(0);
                }
            }
            if (_all.size() > (size_t)_partialStart) {
                _remainder = _code.substr(tryLen);
                break;
            }
        }
    }
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

void IME::lookupVMode() {
    _all.clear();
    _candLen.clear();
    _pageStart = 0;
    _curPage = 0;
    std::string body = _code.length() > 1 ? _code.substr(1) : "";
    if (body.empty()) {
        for (auto p : V_PUNCT_CANDIDATES) {
            _all.push_back(p);
            _candLen.push_back(1);
        }
        buildPage();
        return;
    }

    if (body == "/time" || body == "/date" || body == "/week") {
        for (auto &s : vTimeDateWeek(body)) {
            _all.push_back(s);
            _candLen.push_back((int)_code.length());
        }
        buildPage();
        return;
    }

    if (body.length() > 1 && body[0] == '/') {
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
// 1) 补充词典表(seg_table.h) 分段前缀匹配; 2) 用户词典带撇号码精确匹配;
// 3) 主词典词组: 拼接码精确匹配且字数=段数;
// 4) 逐字匹配: 首段单字候选, 选中后按 seg0Next 消费跳下一段续拼(见 commit 的 candContinue)。
// 全部视为整码消费。
void IME::lookupSegmented() {
    std::vector<std::string> segs;
    std::string cur;
    for (char c : _code) {
        if (c == '\'') {
            if (!cur.empty()) segs.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) segs.push_back(cur);
    if (segs.empty()) return;
    std::string q;
    for (auto &s : segs) q += s;
    int fullLen = (int)_code.length();

    // 1) 补充词典表
    for (int i = 0; i < SEG_TABLE_COUNT && _all.size() < MAX_CANDIDATES; i++) {
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
        if (!ok) continue;
        std::string entryCode;
        for (auto &s : entrySyl) entryCode += s;
        if (strncmp(q.c_str(), entryCode.c_str(), q.length()) != 0) continue;
        std::string w = SEG_TABLE[i].word;
        bool dup = false;
        for (auto &e : _all) if (e == w) { dup = true; break; }
        if (!dup) {
            _all.push_back(w);
            _candLen.push_back(fullLen);
        }
    }

    // 2) 用户词典: 带撇号码的整码精确匹配(用户曾用该分段码提交过的词)
    auto scanSegmentedUserWords = [&](const std::vector<UserEntry> &entries) {
    for (auto &p : entries) {
        if (_all.size() >= MAX_CANDIDATES) break;
        if (p.trad != _trad) continue;
        if (p.code.find('\'') == std::string::npos) continue;
        if (p.code != _code) continue;
        bool dup = false;
        for (auto &e : _all) if (e == p.word) { dup = true; break; }
        if (!dup) {
            _all.push_back(p.word);
            _candLen.push_back(fullLen);
        }
    }
    };
    scanSegmentedUserWords(_fixedUserWords);
    scanSegmentedUserWords(_dynamicUserWords);

    // 3) 主词典词组: 拼接码精确匹配 + 字数/3 == 段数
    if (_wordCount > 0 && _wordData && q.length() >= 2 && _all.size() < MAX_CANDIDATES) {
        int k = (q[0] - 'a') * 26 + (q[1] - 'a');
        size_t wlo = (k >= 0 && k < INDEX_ENTRIES) ? _wordIndex[k] : 0;
        size_t whi = (k + 1 < INDEX_ENTRIES) ? _wordIndex[k + 1] : _wordDataSize;
        size_t wpos = wlo;
        int safety = 0;
        while (wpos < whi && _all.size() < MAX_CANDIDATES && safety++ < 5000) {
            uint8_t cl = _wordData[wpos];
            if (cl == 0 || wpos + 1 + cl > whi) break;
            const char *wc = (const char *)_wordData + wpos + 1;
            wpos += 1 + cl;
            if (wpos >= whi) break;
            uint8_t n = _wordData[wpos++];
            if ((int)cl == (int)q.length() && strncmp(wc, q.c_str(), cl) == 0) {
                for (uint8_t j = 0; j < n && wpos < whi; j++) {
                    uint8_t wl = _wordData[wpos++];
                    if (wl == 0 || wpos + wl + 1 > whi) break;
                    uint8_t wf = _wordData[wpos + wl];
                    std::string w((const char *)_wordData + wpos, wl);
                    if ((int)wl == (int)segs.size() * 3 && wordVisible(_trad, w, wf)) {
                        bool dup = false;
                        for (auto &e : _all) if (e == w) { dup = true; break; }
                        if (!dup) {
                            _all.push_back(w);
                            _candLen.push_back(fullLen);
                        }
                    }
                    wpos += wl + 1;
                }
            } else {
                for (uint8_t j = 0; j < n && wpos < whi; j++) {
                    uint8_t wl = _wordData[wpos++];
                    if (wpos + wl + 1 > whi) break;
                    wpos += wl + 1;
                }
            }
        }
    }

    // 4) 逐字匹配: 首段单字前缀候选(词组之后)。选中后消费 seg0Next 字节跳到
    //    下一段续拼(如 xi'an 选"西"后余下 "an" 查字), 由 commit 的 candContinue 推进。
    if (_all.size() < MAX_CANDIDATES) {
        int seg0Next = (segs.size() > 1) ? ((int)segs[0].length() + 1) : fullLen;
        appendSingleCharCandidates(segs[0], seg0Next);
    }

    _partialStart = (int)_all.size();
}

// 主词典单字前缀匹配: 与 lookup() Phase 2 相同扫描, 但消费长度由调用方指定
// (分词逐字续拼时是跳到下一段的字节偏移, 而非词典码长)。
void IME::appendSingleCharCandidates(const std::string &prefix, int candLen) {
    int qlen = (int)prefix.length();
    if (qlen < 1 || _all.size() >= MAX_CANDIDATES) return;
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
    for (uint32_t i = lo; i < scanEnd && _all.size() < MAX_CANDIDATES; i++) {
        char code[7];
        if (!readCode(i, code)) break;
        if (strncmp(code, prefix.c_str(), qlen) != 0) break;
        uint8_t f = readRecordFlag(i);
        if (f & (_trad ? 0x01 : 0x02)) continue;
        char hz[4];
        if (!readHanzi(i, hz)) break;
        std::string h(hz);
        bool dup = false;
        for (auto &e : _all) if (e == h) { dup = true; break; }
        if (!dup) {
            _all.push_back(h);
            _candLen.push_back(candLen);
        }
    }
}

void IME::beginPredict(const std::string &text) {
    if (!_predData || _predDataSize < 3) return;
    reset();
    _predChar = text;
    _predicting = true;
    const char *first = text.c_str();
    const uint8_t *p = _predData;
    while (p + 3 <= _predData + _predDataSize) {
        int charLen = 0;
        if ((*p & 0xE0) == 0xC0) charLen = 2;
        else if ((*p & 0xF0) == 0xE0) charLen = 3;
        else charLen = 1;
        if (charLen < 1 || p + charLen + 1 > _predData + _predDataSize) break;
        if (charLen == (int)text.length() && memcmp(p, first, charLen) == 0) {
            p += charLen;
            uint8_t n = *p++;
            for (uint8_t j = 0; j < n && p < _predData + _predDataSize; j++) {
                uint8_t wl = *p++;
                if (wl == 0 || p + wl > _predData + _predDataSize) break;
                std::string word;
                word.append((const char *)p, wl);
                p += wl;
                _all.push_back(word);
                _candLen.push_back(0);
            }
            buildPage();
            return;
        }
        uint8_t n = p[charLen];
        p += charLen + 1;
        for (size_t j = 0; j < n && p < _predData + _predDataSize; j++) {
            if (p + 1 > _predData + _predDataSize) break;
            uint8_t wl = *p;
            p += 1 + wl;
        }
    }
}

void IME::buildPage() {
    _page.clear();
    if (_all.empty()) {
        _pageStart = 0;
        _curPage = 0;
        _pageStarts.clear();
        return;
    }
    if (_widthFn && _displayWidth > 0) {
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
        reset();
        return true;
    }
    if (_deleteMode) {
        for (auto it = _dynamicUserWords.begin(); it != _dynamicUserWords.end(); ++it) {
            if (it->word == out && it->trad == _trad) {
                _dynamicUserWords.erase(it);
                _dynamicUserDirty = true;
                saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
                break;
            }
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
        }
        out = _prefix;
    } else {
        if (hasUpperSuffix) out += _code.substr(pLen);
        if (!hasUpperSuffix) bumpFrequency(_code, out);
    }
    _prefix.clear();
    _displayCodeDirty = true;
    _codeOrig.clear();
    reset();
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
            else { out = _code; reset(); }
            return true;
        }
        if (key == '\n') { out = _code; reset(); return true; }
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
        else out = _code;
        reset();
        return true;
    }
    if (_vMode) {
        if (_code == "v" && key >= 0x21 && key <= 0x7E &&
            !((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
              (key >= '0' && key <= '9') || key == '/')) {
            out.assign(1, (char)key);
            reset();
            return true;
        }
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
        if (key >= '1' && key <= '9') { commit(key - '1', out); return true; }
        if (key == ' ') {
            if (_page.size() > 0) commit(0, out);
            else { out = _code.length() > 1 ? _code.substr(1) : ""; reset(); }
            return true;
        }
        if (key == '\n') {
            out = _page.size() > 0 ? _page[0] : (_code.length() > 1 ? _code.substr(1) : "");
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
                out = _page[idx];
                _predicting = false;
            }
            return true;
        }
        if (key == ' ') {
            if (_page.size() > 0) {
                out = _page[0];
                _predicting = false;
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
        if (_code.length() == 0 && !_deleteMode && !_lfMode && cl == 'u') {
            loadLfDict();
            if (_lfBlob) { _lfMode = true; _maxCode = 12; return true; }
        }
        if ((int)_code.length() < _maxCode) {
            _code += c;
            _displayCodeDirty = true;
            lookup();
        }
        return true;
    }
    if (_code.length() == 0) {
        if (handleFullwidthPunct(key, out)) return true;
        if (_fullwidth && handleFullwidthChar(key, out)) return true;
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
