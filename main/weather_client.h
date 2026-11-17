#pragma once

#include <string>

// 当前天气（只要天气现象 + 气温，用于模板日记 {{weather}} 数据段）。
struct WeatherResult {
    bool success = false;
    std::string summary;   // 如 "多云 6℃"
    std::string error;     // 失败原因（单行，可直接显示在状态栏）
};

// 按城市名查询当前天气。数据源 uapis.cn，无需 Key。
WeatherResult weatherFetchCurrent(const std::string &city);
