#include "yong_dict.h"
#include "yong_pinyin.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ime {

static const uint8_t kIm3Magic[4] = {'I', 'M', 'E', '3'};

uint32_t Im3Dictionary::readU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t Im3Dictionary::packInitialPrefix(const char *s, int len) {
    if (!s || len < 1) return 0;
    uint32_t key = 0;
    int n = std::min(len, 6);
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c < 'a' || c > 'z') return 0;
        key |= (uint32_t)(c - 'a' + 1) << ((5 - i) * 5);
    }
    return key;
}

uint32_t Im3Dictionary::initialPrefixMask(int len) {
    if (len <= 0) return 0;
    if (len >= 6) return 0x3fffffff;
    return 0x3fffffffu & ~((1u << ((6 - len) * 5)) - 1u);
}

int Im3Dictionary::utf8CharLen(uint8_t c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

bool Im3Dictionary::isPadding(const uint8_t *p, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (p[i] != 0x00 && p[i] != 0xff) return false;
    }
    return true;
}

bool Im3Dictionary::parse(const uint8_t *blob, size_t size) {
    _valid = false;
    _blob = nullptr;
    _blobSize = 0;
    _scheme = WUBI;
    _codeLen = 0;
    _recordSize = 0;
    _singleCount = 0;
    _singleBase = 0;
    _singleIndex.clear();
    _wordCount = 0;
    _wordIndex.clear();
    _wordData = nullptr;
    _wordDataSize = 0;
    _initialIndex.clear();
    _predictCount = 0;
    _predictData = nullptr;
    _predictDataSize = 0;

    if (!blob || size < kHeaderSize || std::memcmp(blob, kIm3Magic, sizeof(kIm3Magic)) != 0)
        return false;

    int codeLen = blob[5];
    if (codeLen < 1 || codeLen > kMaxCodeLen) return false;

    uint32_t singleCount = readU32(blob + 8);
    size_t singleBase = kHeaderSize + (size_t)kIndexEntries * 4;
    size_t recordSize = (size_t)codeLen + kHanziSize + kFlagSize;
    size_t singleEnd = singleBase + (size_t)singleCount * recordSize;
    if (singleCount == 0 || singleEnd > size) return false;

    _blob = blob;
    _blobSize = size;
    _scheme = (Scheme)blob[4];
    _codeLen = codeLen;
    _recordSize = (int)recordSize;
    _singleCount = singleCount;
    _singleBase = singleBase;
    _singleIndex.resize(kIndexEntries);
    for (int i = 0; i < kIndexEntries; i++)
        _singleIndex[i] = readU32(blob + kHeaderSize + (size_t)i * 4);

    size_t wordBase = singleEnd;
    if (wordBase + 4 <= size) {
        const uint8_t *wp = blob + wordBase;
        _wordCount = readU32(wp);
        if (_wordCount > 0 && wordBase + 4 + (size_t)kIndexEntries * 4 <= size) {
            wp += 4;
            _wordIndex.resize(kIndexEntries);
            for (int i = 0; i < kIndexEntries; i++)
                _wordIndex[i] = readU32(wp + (size_t)i * 4);
            wp += (size_t)kIndexEntries * 4;
            size_t maxWordDataSize = size - singleEnd - 4 - (size_t)kIndexEntries * 4;
            size_t wordDataSize = _wordIndex[kIndexEntries - 1];
            bool indexOk = wordDataSize <= maxWordDataSize;
            for (int i = 1; indexOk && i < kIndexEntries; i++) {
                if (_wordIndex[i] < _wordIndex[i - 1] || _wordIndex[i] > wordDataSize)
                    indexOk = false;
            }
            if (indexOk) {
                _wordData = wp;
                _wordDataSize = wordDataSize;
                _initialIndex.reserve(_wordCount);
                size_t pos = 0;
                while (pos < _wordDataSize) {
                    size_t groupPos = pos;
                    uint8_t cl = _wordData[pos];
                    if (cl == 0 || pos + 1 + cl > _wordDataSize) break;
                    std::string code((const char *)_wordData + pos + 1, cl);
                    pos += 1 + cl;
                    if (pos >= _wordDataSize) break;
                    uint8_t n = _wordData[pos++];
                    std::string init = PinyinEngine::initialCode(code);
                    uint32_t key = packInitialPrefix(init.c_str(), (int)init.size());
                    if (key != 0) {
                        InitialEntry entry;
                        entry.key = key;
                        entry.posLen = ((uint32_t)std::min<size_t>(init.size(), 6) << 24) |
                                       ((uint32_t)groupPos & 0x00ffffffu);
                        _initialIndex.push_back(entry);
                    }
                    for (uint8_t j = 0; j < n && pos < _wordDataSize; j++) {
                        uint8_t wl = _wordData[pos++];
                        if (wl == 0 || pos + wl + 1 > _wordDataSize) {
                            pos = _wordDataSize;
                            break;
                        }
                        pos += wl + 1;
                    }
                }
                std::stable_sort(_initialIndex.begin(), _initialIndex.end(),
                    [](const InitialEntry &a, const InitialEntry &b) {
                        if (a.key != b.key) return a.key < b.key;
                        return a.pos() < b.pos();
                });
            } else {
                _wordCount = 0;
                _wordIndex.clear();
                _initialIndex.clear();
            }

            size_t predBase = wordBase + 4 + (size_t)kIndexEntries * 4 + _wordDataSize;
            if (_wordData && predBase + 4 <= size && !isPadding(blob + predBase, size - predBase)) {
                const uint8_t *pp = blob + predBase;
                uint32_t predictCount = readU32(pp);
                if (predictCount > 0 && predictCount < 100000) {
                    _predictCount = predictCount;
                    _predictData = pp + 4;
                    _predictDataSize = size - predBase - 4;
                }
            }
        }
    }

    _valid = true;
    return true;
}

void Im3Dictionary::singleWindow(const char *code, int len, uint32_t &lo, uint32_t &hi) const {
    lo = 0;
    hi = _singleCount;
    if (!_valid || _singleIndex.empty() || !code || len < 1) return;
    int c0 = code[0] - 'a';
    if (c0 < 0 || c0 >= 26) return;
    if (len == 1) {
        lo = _singleIndex[c0 * 26];
        hi = _singleIndex[(c0 + 1) * 26];
        return;
    }
    int c1 = code[1] - 'a';
    if (c1 < 0 || c1 >= 26) return;
    int k = c0 * 26 + c1;
    lo = _singleIndex[k];
    hi = _singleIndex[k + 1];
}

uint32_t Im3Dictionary::lowerBoundSingle(const char *code, int len, uint32_t lo, uint32_t hi) const {
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        char item[kMaxCodeLen + 1];
        if (!readSingleCode(mid, item)) break;
        if (std::strncmp(item, code, len) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

bool Im3Dictionary::readSingleCode(uint32_t i, char out[kMaxCodeLen + 1]) const {
    if (!_valid || !out || i >= _singleCount) return false;
    const uint8_t *rec = _blob + _singleBase + (size_t)i * _recordSize;
    int n = 0;
    for (; n < _codeLen && rec[n]; n++) out[n] = (char)rec[n];
    out[n] = '\0';
    return true;
}

bool Im3Dictionary::readSingleText(uint32_t i, char out[kHanziSize + 1]) const {
    if (!_valid || !out || i >= _singleCount) return false;
    const uint8_t *rec = _blob + _singleBase + (size_t)i * _recordSize + _codeLen;
    out[0] = (char)rec[0];
    out[1] = (char)rec[1];
    out[2] = (char)rec[2];
    out[3] = '\0';
    return true;
}

uint8_t Im3Dictionary::readSingleFlag(uint32_t i) const {
    if (!_valid || i >= _singleCount) return 0;
    const uint8_t *rec = _blob + _singleBase + (size_t)i * _recordSize;
    return rec[_codeLen + kHanziSize];
}

void Im3Dictionary::wordWindow(const char *code, int len, size_t &lo, size_t &hi) const {
    lo = 0;
    hi = _wordDataSize;
    if (!_valid || _wordIndex.empty() || !_wordData || !code || len < 1) return;
    int c0 = code[0] - 'a';
    if (c0 < 0 || c0 >= 26) return;
    if (len == 1) {
        lo = _wordIndex[c0 * 26];
        hi = (c0 + 1 < 26) ? _wordIndex[(c0 + 1) * 26] : _wordIndex[kIndexEntries - 1];
        return;
    }
    int c1 = code[1] - 'a';
    if (c1 < 0 || c1 >= 26) return;
    int k = c0 * 26 + c1;
    lo = _wordIndex[k];
    hi = (k + 1 < kIndexEntries) ? _wordIndex[k + 1] : _wordDataSize;
}

void Im3Dictionary::initialWindow(const char *initial, int len, size_t &lo, size_t &hi) const {
    lo = 0;
    hi = 0;
    if (!_valid || _initialIndex.empty() || !initial || len < 1) return;
    int prefixLen = std::min(len, 6);
    uint32_t prefix = packInitialPrefix(initial, prefixLen);
    if (prefix == 0) return;
    uint32_t mask = initialPrefixMask(prefixLen);
    auto first = std::lower_bound(_initialIndex.begin(), _initialIndex.end(), prefix,
        [](const InitialEntry &entry, uint32_t value) {
            return entry.key < value;
    });
    uint32_t upper = prefix | (~mask & 0x3fffffffu);
    auto last = std::upper_bound(_initialIndex.begin(), _initialIndex.end(), upper,
        [](uint32_t value, const InitialEntry &entry) {
            return value < entry.key;
    });
    lo = (size_t)(first - _initialIndex.begin());
    hi = (size_t)(last - _initialIndex.begin());
}

bool Im3Dictionary::readInitialEntry(size_t i, InitialEntry &out) const {
    if (!_valid || i >= _initialIndex.size()) return false;
    out = _initialIndex[i];
    return true;
}

bool Im3Dictionary::nextWordGroup(size_t &pos, size_t end, WordGroup &out) const {
    out.code.clear();
    out.candidates.clear();
    if (!_valid || !_wordData || pos >= end || end > _wordDataSize) return false;

    uint8_t cl = _wordData[pos];
    if (cl == 0 || pos + 1 + cl > end) {
        pos = end;
        return false;
    }
    out.code.assign((const char *)_wordData + pos + 1, cl);
    pos += 1 + cl;
    if (pos >= end) {
        pos = end;
        return false;
    }

    uint8_t n = _wordData[pos++];
    out.candidates.reserve(n);
    for (uint8_t i = 0; i < n && pos < end; i++) {
        uint8_t wl = _wordData[pos++];
        if (wl == 0 || pos + wl + 1 > end) {
            pos = end;
            return false;
        }
        WordCandidate cand;
        cand.text.assign((const char *)_wordData + pos, wl);
        cand.flag = _wordData[pos + wl];
        out.candidates.push_back(std::move(cand));
        pos += wl + 1;
    }
    return true;
}

bool Im3Dictionary::nextPredictGroup(size_t &pos, PredictGroup &out) const {
    out.key.clear();
    out.candidates.clear();
    if (!_valid || !_predictData || pos >= _predictDataSize) return false;

    const uint8_t *base = _predictData;
    size_t end = _predictDataSize;
    int keyLen = utf8CharLen(base[pos]);
    if (keyLen <= 0 || pos + (size_t)keyLen + 1 > end) {
        pos = end;
        return false;
    }

    out.key.assign((const char *)base + pos, keyLen);
    pos += keyLen;
    uint8_t n = base[pos++];
    out.candidates.reserve(n);
    for (uint8_t i = 0; i < n && pos < end; i++) {
        uint8_t wl = base[pos++];
        if (wl == 0 || pos + wl > end) {
            pos = end;
            return false;
        }
        out.candidates.emplace_back((const char *)base + pos, wl);
        pos += wl;
    }
    return true;
}

} // namespace ime
