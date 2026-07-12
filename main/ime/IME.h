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
    bool composing() const { return _code.length() > 0 || _predicting || _lfMode || _deleteMode; }

    bool isLfMode() const { return _lfMode; }
    bool isDeleteMode() const { return _deleteMode; }

    void beginPredict(const std::string &text);
    void endPredict() { _predicting = false; _predChar = ""; }
    bool predicting() const { return _predicting; }

    void removeUserWord(const std::string &code, const std::string &word);
    void clearUserDict();
    void pruneUserDict(int minCount = 0);
    size_t userDictSize() const { return _userWords.size(); }

    static IME &getInstance() {
        static IME instance;
        return instance;
    }
    IME(const IME &) = delete;
    IME &operator=(const IME &) = delete;

    void setPageSize(int n) { _pageSize = n; }
    int pageSize() const { return _pageSize; }
    int totalCandidates() const { return (int)_all.size(); }
    int currentPage() const { return _pageStart / _pageSize + 1; }

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

    struct UserEntry { std::string code; std::string word; int count; };
    std::vector<UserEntry> _userWords;
    bool _userDirty = false;
    void loadUserDict();
    void saveUserDict();
    void addUserWord(const std::string &code, const std::string &word);
    void bumpFrequency(const std::string &code, const std::string &word);

    bool _deleteMode = false;
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
    bool parseHeader(const uint8_t *hdrIndex, size_t total);
    bool readCode(uint32_t i, char out[MAX_CODE_LEN + 1]);
    bool readHanzi(uint32_t i, char out[HANZI_SIZE + 1]);

    std::string _code;
    std::vector<std::string> _all;
    std::vector<std::string> _page;
    int _pageStart = 0;
    int _pageSize = 9;

    mutable std::string _displayCodeCache;
    mutable bool _displayCodeDirty = true;

    void reset();
    void lookup();
    void buildPage();
    bool commit(int idx, std::string &out);
    bool handleFullwidthPunct(int key, std::string &out);
};
