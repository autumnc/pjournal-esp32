#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "ime_config.h"

namespace ime {

class Im3Dictionary {
public:
    enum Scheme : uint8_t { WUBI = 0, PINYIN = 1, SHUANGPIN = 2 };

    static const int kHeaderSize = 12;
    static const int kIndexEntries = 26 * 26 + 1;
    static const int kMaxCodeLen = 6;
    static const int kHanziSize = 3;
    static const int kFlagSize = 1;

    struct WordCandidate {
        std::string text;
        uint8_t flag = 0;
    };

    struct WordGroup {
        std::string code;
        std::vector<WordCandidate> candidates;
    };

    struct PredictGroup {
        std::string key;
        std::vector<std::string> candidates;
    };

    bool parse(const uint8_t *blob, size_t size);
    bool valid() const { return _valid; }

    Scheme scheme() const { return _scheme; }
    int codeLen() const { return _codeLen; }
    int recordSize() const { return _recordSize; }
    uint32_t singleCount() const { return _singleCount; }
    uint32_t wordCount() const { return _wordCount; }
    bool hasWords() const { return _wordCount > 0 && _wordData && _wordDataSize > 0; }

    const uint8_t *wordData() const { return _wordData; }
    size_t wordDataSize() const { return _wordDataSize; }
    const uint8_t *predictData() const { return _predictData; }
    size_t predictDataSize() const { return _predictDataSize; }
    uint32_t predictCount() const { return _predictCount; }
    bool hasPredictions() const { return _predictCount > 0 && _predictData && _predictDataSize > 0; }

    void singleWindow(const char *code, int len, uint32_t &lo, uint32_t &hi) const;
    uint32_t lowerBoundSingle(const char *code, int len, uint32_t lo, uint32_t hi) const;
    bool readSingleCode(uint32_t i, char out[kMaxCodeLen + 1]) const;
    bool readSingleText(uint32_t i, char out[kHanziSize + 1]) const;
    uint8_t readSingleFlag(uint32_t i) const;

    void wordWindow(const char *code, int len, size_t &lo, size_t &hi) const;
    bool nextWordGroup(size_t &pos, size_t end, WordGroup &out) const;
    bool nextPredictGroup(size_t &pos, PredictGroup &out) const;
    bool findPredictGroup(const std::string &key, PredictGroup &out) const;

private:
    struct PredictIndexEntry {
        uint32_t hash = 0;
        uint32_t offset = 0;
    };

    static uint32_t readU32(const uint8_t *p);
    static uint32_t hashBytes(const char *data, size_t len);
    static int utf8CharLen(uint8_t c);
    static bool isPadding(const uint8_t *p, size_t size);
    bool buildPredictIndex() const;
    bool readPredictGroupAt(size_t pos, PredictGroup &out) const;

    bool _valid = false;
    const uint8_t *_blob = nullptr;
    size_t _blobSize = 0;

    Scheme _scheme = WUBI;
    int _codeLen = 0;
    int _recordSize = 0;
    uint32_t _singleCount = 0;
    size_t _singleBase = 0;
    std::vector<uint32_t> _singleIndex;

    uint32_t _wordCount = 0;
    std::vector<uint32_t> _wordIndex;
    const uint8_t *_wordData = nullptr;
    size_t _wordDataSize = 0;

    uint32_t _predictCount = 0;
    const uint8_t *_predictData = nullptr;
    size_t _predictDataSize = 0;
    mutable bool _predictIndexBuilt = false;
    mutable std::vector<PredictIndexEntry> _predictIndex;
};

} // namespace ime
