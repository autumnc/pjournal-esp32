#pragma once

#include <cstdint>

// 打字音的采样:每个音色存若干个「一次敲击」的 take(48k 单声道 s16)。
// 播放时随机挑一个 take,再做变调/变音量/左右微差,所以每次上屏的波形都不一样
// ——「每次都是同一个波」正是合成音听起来假的主要原因。
//
// 数据由 click_samples_data.inc 提供(生成脚本烘出来,不要手改)。
struct ClickTake {
    const int16_t *pcm;
    int frames;
};

struct ClickSet {
    const char *name;
    const ClickTake *takes;
    int n;
    int gain_pct;    // 该音色的固定音量微调(%)
    int detune_pct;  // 每次播放的最大随机变调(±%)
};

// 按名字取音色,取不到返回 nullptr。
const ClickSet *click_sample_set(const char *name);

// 改名前的旧音色名换成现在用的(老配置里可能还存着旧名)。
const char *click_timbre_current_name(const char *name);
