#pragma once

#include <string>
#include <vector>
#include <cstdint>

class IME {
public:
    enum Scheme { WUBI = 0, PINYIN = 1, SHUANGPIN = 2 };

    bool begin();
    bool loaded() const { return _loaded; }
    Scheme scheme() const { return _scheme; }

    bool active() const { return _active; }
    void setActive(bool on);
    void toggle() { setActive(!_active); }

    bool fullwidth() const { return _fullwidth; }
    void toggleFullwidth() { _fullwidth = !_fullwidth; }
    void setFullwidth(bool on) { _fullwidth = on; }

    bool trad() const { return _trad; }
    void toggleTrad() { _trad = !_trad; buildPage(); }
    void setTrad(bool on) { _trad = on; buildPage(); }

    bool english() const { return _english; }
    void toggleEnglish() { setEnglish(!_english); }
    void setEnglish(bool on) { _english = on; if (on) reset(); }

    bool handleKey(int key, std::string &out);

    std::string displayCode() const {
        if (_displayCodeDirty) {
            _displayCodeCache = _prefix + _code;
            _displayCodeDirty = false;
        }
        return _displayCodeCache;
    }
    const std::string &composition() const { return _code; }
    const std::vector<std::string> &candidates() const { return _page; }
    // 页内高亮候选下标(v 模式左右方向键选择,渲染反白用)
    int highlightIdx() const { return _vSel; }
    bool composing() const { return _code.length() > 0 || _predicting || _lfMode || _deleteMode || _vMode || _englishCompose; }

    bool isLfMode() const { return _lfMode; }
    bool isDeleteMode() const { return _deleteMode; }

    void beginPredict(const std::string &text);
    void endPredict() { _predicting = false; _predChar = ""; }
    bool predicting() const { return _predicting; }
    void cancelComposition() { reset(); }

    enum UserDictKind { FIXED_DICT = 0, DYNAMIC_DICT = 1 };
    struct UserEntryView { std::string code; std::string word; int count; bool trad = false; };
    const std::vector<UserEntryView> userDictEntries(UserDictKind kind) const;
    bool addUserDictEntry(UserDictKind kind, const std::string &code, const std::string &word);
    void removeUserDictEntries(UserDictKind kind, const std::vector<int> &indices);
    size_t userDictSize(UserDictKind kind) const;
    void ensureUserDictLoaded();

    void removeUserWord(const std::string &code, const std::string &word);
    void clearUserDict();
    void pruneUserDict(int minCount = 0);
    size_t userDictSize() const { return _dynamicUserWords.size(); }

    static IME &getInstance() {
        static IME instance;
        return instance;
    }
    IME(const IME &) = delete;
    IME &operator=(const IME &) = delete;

    void setPageSize(int n) { _pageSize = n; }
    // 返回当前页实际候选数量(界面用 (i % pageSize)+1 编号, 页内从 1 起)
    int pageSize() const { int n = (int)_page.size(); return n >= 1 ? n : 1; }
    int totalCandidates() const { return (int)_all.size(); }
    int totalPages() const { return _pageStarts.empty() ? 1 : (int)_pageStarts.size(); }
    int currentPage() const { return _curPage + 1; }

    using WidthFn = int (*)(const char *text);
    void setWidthFn(WidthFn fn) { _widthFn = fn; }
    // 候选行可用像素宽度(与各界面渲染 curW+partW+8>SCREEN_W 的 8px 余量一致)
    void setDisplayWidth(int w) { _displayWidth = w; }

private:
    IME() {}

    static const int HEADER_SIZE = 12;
    static const int HANZI_SIZE = 3;
    static const int FLAG_SIZE = 1;
    int _codeLen = 6;
    int _recordSize = 6 + HANZI_SIZE + FLAG_SIZE;
    int _maxCode = 4;
    Scheme _scheme = WUBI;

    static const int INDEX_ENTRIES = 26 * 26 + 1; // 677
    static const int MAX_CODE_LEN = 6;
    static const int MAX_CANDIDATES = 300;

    bool _loaded = false;
    bool _active = false;

    const uint8_t *_blob = nullptr;
    size_t _blobSize = 0;
    uint32_t _count = 0;
    size_t _recordBase = HEADER_SIZE + INDEX_ENTRIES * 4;
    std::vector<uint32_t> _index;

    uint32_t _wordCount = 0;
    std::vector<uint32_t> _wordIndex;
    const uint8_t *_wordData = nullptr;
    size_t _wordDataSize = 0;

    uint32_t _predCount = 0;
    const uint8_t *_predData = nullptr;
    size_t _predDataSize = 0;
    bool _predicting = false;
    std::string _predChar;
    int _partialStart = 0;
    int _maxMatchLen = 0;
    std::string _prefix;
    std::string _remainder;
    std::string _codeOrig;

    struct UserEntry { std::string code; std::string word; int count; bool trad = false; };
    std::vector<UserEntry> _fixedUserWords;
    std::vector<UserEntry> _dynamicUserWords;
    bool _fixedUserDirty = false;
    bool _dynamicUserDirty = false;
    bool _userDictLoaded = false;
    void loadUserDict();
    bool loadUserDictFile(const char *path, std::vector<UserEntry> &entries, bool &dirty, size_t maxEntries);
    void saveUserDictFile(const char *path, std::vector<UserEntry> &entries, bool &dirty);
    void addUserWord(const std::string &code, const std::string &word);
    void bumpFrequency(const std::string &code, const std::string &word);

    bool _deleteMode = false;
    bool _vMode = false;
    int _vSel = 0;  // v 模式页内高亮候选(左右键移动)
    bool _englishCompose = false;
    bool _englishDictLoaded = false;
    std::vector<std::string> _englishWords;
    bool _lfMode = false;
    const uint8_t *_lfBlob = nullptr;
    uint32_t _lfCount = 0;
    size_t _lfRecordBase = 0;
    std::vector<uint16_t> _lfIndex;
    void loadLfDict();
    void searchLfWindow(const char *code, int len, uint32_t &lo, uint32_t &hi);
    bool readLfCode(uint16_t i, char out[13]);
    bool readLfHanzi(uint16_t i, char out[4]);

    void searchWindow(const char *code, int len, uint32_t &lo, uint32_t &hi);
    static int pinyinPrefixLen(const std::string &code);
    bool parseHeader(const uint8_t *hdrIndex, size_t total);
    bool readCode(uint32_t i, char out[MAX_CODE_LEN + 1]);
    bool readHanzi(uint32_t i, char out[HANZI_SIZE + 1]);
    uint8_t readRecordFlag(uint32_t i);
    uint8_t readLfFlag(uint16_t i);

    std::string _code;
    std::vector<std::string> _all;
    std::vector<int> _candLen;  // code length per candidate in _all
    std::vector<std::string> _page;
    int _pageStart = 0;
    int _pageSize = 9;
    int _curPage = 0;                    // 当前页索引(第 _curPage+1 页)
    std::vector<int> _pageStarts;        // 每页起始候选索引; 按实测宽度分页时由 buildPage 重建
    WidthFn _widthFn = nullptr;          // 候选文本宽度测量回调
    int _displayWidth = 0;               // 候选行可用像素宽度(0=退化为固定 _pageSize 分页)

    mutable std::string _displayCodeCache;
    mutable bool _displayCodeDirty = true;

    bool _singleQuoteOpen = false;  // Track single quote pairing state
    bool _doubleQuoteOpen = false;  // Track double quote pairing state
    bool _fullwidth = false;        // Fullwidth character mode
    bool _trad = false;             // Traditional mode: hide simplified-only, show trad counterparts
    bool _english = false;          // Temp English mode: pass keys through as ASCII

    void reset();
    void lookup();
    void lookupSegmented();  // 单引号分词编码的查词路径
    void lookupVMode();
    void lookupEnglishMode();
    void loadEnglishDict();
    void appendSingleCharCandidates(const std::string &prefix, int candLen);  // 主词典单字前缀候选
    void buildPage();
    bool pagePrev();
    bool pageNext();
    bool commit(int idx, std::string &out);
    bool handleFullwidthPunct(int key, std::string &out);
    bool handleFullwidthChar(int key, std::string &out);
};
