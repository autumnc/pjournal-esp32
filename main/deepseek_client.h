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
};

extern DeepseekClient g_deepseek;
