#pragma once

#include <string>

struct DeepseekResult {
    bool success;
    std::string content;
};

class DeepseekClient {
public:
    // Generate a journal writing prompt via Deepseek chat API
    DeepseekResult generatePrompt(const std::string &userContext);
    // Light polish of editor text (fallback when Xiaozhi fails).
    // customInstr may be empty (use the default polish rules only).
    DeepseekResult polishText(const std::string &text, const std::string &customInstr);
};

extern DeepseekClient g_deepseek;
