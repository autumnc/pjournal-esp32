#include "click_samples.h"

#include <cstring>

// 生成的采样数据:每个音色的若干 take。改音色只改生成脚本,数据本身不要手改。
#include "click_samples_data.inc"

// 键盘轴体 18 种,来自 Mechvibes(https://github.com/hainguyents13/mechvibes, MIT)的
// 内置音色包:按它 config.json 里标好的每键边界切,没有做起音检测。
// 顺序即设置界面的循环顺序(与 screen_settings.cpp TIMBRE_OPTS 同步)。
static const ClickSet *const k_sets[] = {
    &k_set_mx_black_abs, &k_set_mx_black_pbt, &k_set_mx_blue_abs,  &k_set_mx_blue_pbt,
    &k_set_mx_brown_abs, &k_set_mx_brown_pbt, &k_set_mx_red_abs,   &k_set_mx_red_pbt,
    &k_set_cream_travel, &k_set_nk_cream,     &k_set_holy_pandas,  &k_set_eg_crystal,
    &k_set_eg_oreo,      &k_set_turquoise,    &k_set_mx_black_tr,  &k_set_mx_blue_tr,
    &k_set_mx_brown_tr,  &k_set_topre_purple,
};

// 改用采样前的旧音色名(老配置里的 click_timbre 还存着这些),各指到听感最接近的包上。
static const struct { const char *old_name, *now_name; } k_renamed[] = {
    {"mechanical", "mx_brown_pbt"},   {"topre", "topre_purple"},
    {"kailh",      "mx_blue_pbt"},    {"clack", "mx_blue_pbt"},
    {"soft",       "nk_cream"},       {"electronic", "mx_red_pbt"},
    {"wooden",     "topre_purple"},   {"crisp", "eg_crystal"},
    {"chime",      "turquoise"},
};

const char *click_timbre_current_name(const char *name) {
    for (const auto &r : k_renamed)
        if (strcmp(r.old_name, name) == 0) return r.now_name;
    return name;
}

const ClickSet *click_sample_set(const char *name) {
    name = click_timbre_current_name(name);
    for (const ClickSet *s : k_sets)
        if (strcmp(s->name, name) == 0) return s;
    return nullptr;
}
