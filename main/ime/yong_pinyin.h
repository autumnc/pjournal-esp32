#pragma once

#include "ime_config.h"

#include <string>
#include <vector>

namespace ime {

struct PinyinToken {
    std::string text;
    bool partial = false;
};

struct PinyinSplit {
    std::vector<PinyinToken> tokens;
    int score = 0;
};

class PinyinEngine {
public:
    static bool enabled();
    static bool isCodeChar(char c);
    static std::string normalize(const std::string &code);
    static std::string removeSplit(const std::string &code);

    static bool isValidSyllable(const std::string &s);
    static bool isSyllablePrefix(const std::string &s);
    static bool isValidCode(const std::string &code);

    static PinyinSplit primarySplit(const std::string &code, bool allowPartial);
    static std::vector<PinyinSplit> splitVariants(const std::string &code,
                                                  bool allowPartial,
                                                  int maxVariants = 8);
    static std::vector<int> prefixMatchLengths(const std::string &code);
    static std::string singleKeyFallbackSyllable(const std::string &code);
    static std::string initialCode(const std::string &code);
};

} // namespace ime
