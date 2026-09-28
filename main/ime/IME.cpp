#include "IME.h"
#include "yong_pinyin.h"
#include "seg_table.h"
#include "trad_table.h"
#include "kaomoji_table.h"
#include "settings_manager.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <unordered_map>
#include <esp_log.h>
#include <esp_timer.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *IME_TAG = "IME";
static const int64_t IME_PERF_SLOW_US = 12000;

#if PJOURNAL_IME_PERF_LOG
#define IME_PERF_NOW() esp_timer_get_time()
#else
#define IME_PERF_NOW() 0
#endif

// 用户词典持久化到 SD 卡(与设置同目录)。NVS 分区仅 24KB 且写入失败会静默
// 丢失/启动时整区擦除,改用 SD 文件后容量无上限,重启与重刷固件均保留。
static const char *USERDICT_DYNAMIC_PATH = "/sdcard/settings/userdict.txt";
static const char *USERDICT_FIXED_PATH = "/sdcard/settings/userdict_fixed.txt";
static const char *USERPREDICT_PATH = "/sdcard/settings/userpredict.txt";
static const char *USERPREDICT_REJECT_PATH = "/sdcard/settings/userpredict_reject.txt";
static const char *ENGLISHDICT_PATH = "/sdcard/settings/englishdict.txt";
static const char *USERDICT_JOURNAL_SUFFIX = ".journal";
static const size_t USERDICT_FIXED_LIMIT = 1000;
static const size_t USERDICT_DYNAMIC_LIMIT = 5000;
static const size_t USERPREDICT_LIMIT = 2000;
static const size_t USERPREDICT_REJECT_LIMIT = 1000;
static const size_t ENGLISHDICT_LIMIT = 10000;
static const int64_t USERDICT_DEFER_SAVE_US = 2000000;
static const int64_t USERDICT_JOURNAL_DEFER_US = PJOURNAL_IME_USERDICT_JOURNAL_DEFER_US;
static const size_t USERDICT_JOURNAL_BATCH_LIMIT = PJOURNAL_IME_USERDICT_JOURNAL_BATCH_LIMIT;
static const int IME_SCORE_EXACT_CODE = 100000;
static const int IME_SCORE_USER_COUNT_CAP = 2000;
static const int IME_SCORE_USER_COUNT_WEIGHT = 8;
static const int IME_SCORE_PREFIX_BASE = 64;
static const int IME_SCORE_CODE_MATCH_UNIT = 1000;
static const int IME_SCORE_PHRASE_OVERMATCH_UNIT = 1200;
static const int IME_SCORE_PHRASE_OVERMATCH_CAP = 8000;
static const int IME_SCORE_SYLLABLE_BASE = 64;
static const int IME_SCORE_SYLLABLE_DISTANCE_UNIT = 16;
static const int IME_SCORE_MULTI_CHAR_BONUS = 8;
static const int IME_MATURE_LONG_PHRASE_MIN = 6;
static const int IME_MATURE_VERY_LONG_PHRASE_MIN = 8;
static const int IME_MATURE_LONG_PHRASE_NUM = 3;
static const int IME_MATURE_LONG_PHRASE_DEN = 5;
static const int IME_INITIAL_SCORE_BASE = 80;
static const int IME_INITIAL_SCORE_OVERMATCH_UNIT = 16;
static const int IME_INITIAL_SCORE_CHAR_DISTANCE_UNIT = 20;
static const int IME_INITIAL_SCORE_LENGTH_MATCH_BONUS = 5000;
static const int IME_INITIAL_SCORE_COMPACT_BONUS = 500;
static const int IME_INITIAL_SCORE_LONG_OVERMATCH_UNIT = 900;
static const int IME_INITIAL_SCORE_LONG_OVERMATCH_CAP = 6000;
static const int IME_INITIAL_MATURE_LONG_MIN = 4;
static const int IME_INITIAL_MATURE_VERY_LONG_MIN = 5;
static const int IME_INITIAL_MATURE_NUM = 2;
static const int IME_INITIAL_MATURE_DEN = 3;
static const int IME_KEY_UP = 0x80;
static const int IME_KEY_DOWN = 0x81;
static const int IME_KEY_LEFT = 0x82;
static const int IME_KEY_RIGHT = 0x83;
// 短码(2 字母纯辅音)预算只有 12, 单字展开一旦填满就没有槽位留给精选简码词组
// (Phase 4c)和词典简码(Phase 6)。给这两个阶段预留几个槽位, 保证简码可达。
static const size_t IME_SHORTCUT_RESERVE = 3;
// 单字候选的搭配加分。词典按词频存储, 所以用扫描位置当词频先验(每位置 1000 分),
// 让加分表现为"前移几个位次"而不是"直接翻盘"; 上限压到 3 个位次, 避免一次偶然
// 学习就把常用字挤出首屏。
static const int IME_SINGLE_PRIOR_STEP = 1000;
static const int IME_SINGLE_CTX_BOOST_CAP = 3000;
// 整句覆盖(词图 beam search)。长全拼串在词库里没有整串条目, 前面的相位只会给出首音节
// 的单字(Phase 8 逐字匹配一定能把首字母那几个码填满), 所以整句候选不能是"兜底追加",
// 必须排在所有相位之前才有机会出现在首屏。
static const int IME_SENTENCE_MIN_LEN = 6;
// 上限按"整句全拼"定, 典型的六到八字句子(shurufazhendehenhaoyong=23)要能覆盖到。
// 再长也只是让总扫描预算先耗完、后续位置收不到弧, 落回一个候选都不出, 不会出错。
static const int IME_SENTENCE_MAX_LEN = 24;
static const int IME_SENTENCE_BEAM = 8;
static const int IME_SENTENCE_RESULTS = 3;
// 每个起点的桶扫描上限。sh/zh 这类桶有 3000+ 组, 要走到 "jintian…"/"shuru…" 这种
// 靠后的前缀必须扫穿大半个桶, 截断太早会连一个词弧都收不到。
static const int IME_SENTENCE_BUCKET_SCAN = 3600;
// 整次 lookup 的组扫描总量上限, 兜住 16 个起点 × 大桶的最坏开销。
static const int IME_SENTENCE_TOTAL_SCAN = 8000;
static const int IME_SENTENCE_WORD_ARCS = 3;    // 每个词条取词频最高的几个词
static const int IME_SENTENCE_SINGLE_ARCS = 2;  // 每个音节取最高频的几个单字
// 至少要四个音节才拼句。三个音节的输入在词库里基本都有现成的词("xiexieni"→谢谢你,
// "renminbi"→人民币), 而拼句在三个音节上经常切错("nihaoma"→你号码、"wanshanghao"→
// 玩上好), 排在首位反而是负收益。四个音节以上词库就几乎不出候选了(实测
// tahenhaokan/wodejiaxiang/nizaiganshenme 都没有词条), 追加只有好处。
static const int IME_SENTENCE_MIN_TOKENS = 4;
// 整句相位单独的低阈值探针: 它可能只有几毫秒, 达不到 12ms 的总阈值, 但那正是
// 需要盯着看的数字。
static const int IME_SENTENCE_PERF_LOG_US = 3000;
// 弧打分。词弧按字数给正分, 单字给负分(等于每条弧的固定罚项), 这样 DP 才不会把
// "xian" 拆成 "xi"+"an" 去多赚一条弧的分——没有弧罚项时它一定会这么拆。
static const int IME_SENTENCE_WORD_UNIT = 4000;
static const int IME_SENTENCE_ARC_PENALTY = 500;
static const int IME_SENTENCE_SINGLE = -400;
static const int IME_SENTENCE_RANK_WORD = 300;
static const int IME_SENTENCE_RANK_SINGLE = 60;
// 位置长度偏置。词库没有词频, 同分路径只能靠"尽量少切、长弧优先"来定序: Σcl 恒等于
// 整串长度, 但按位置加权后 Σ cl*(len-pos) 在同样条数下偏爱靠前的长弧(等价于最大化
// 最长匹配), 于是 "womenxianzaiqu" 选 我们+现在+去 而不是 我们+先+在+去。
static const int IME_SENTENCE_LEN_BIAS = 1;
// 只给整句首词的弧加搭配分(上文最该预测的就是下一句的起头), 且压得比单字那条更小:
// 首词选错会带偏整句, 所以不让一次偶发学习压过词库本身的词频。
static const int IME_SENTENCE_CTX_CAP = 1500;
static inline std::string str_trim(const std::string &s);
#if PJOURNAL_IME_FAST_LOOKUP
static const int IME_SEG_TABLE_MIN_LEN = 2;
static const int IME_PHRASE_PREFIX_MIN_LEN = 2;
static const int IME_USER_INITIAL_MIN_LEN = 2;
static const int IME_DICT_INITIAL_MIN_LEN = 2;
static const int IME_MAX_PHRASE_GROUP_SCAN = 900;
static const int IME_MAX_INITIAL_GROUP_SCAN = 700;
static const int IME_MAX_SHORT_INITIAL_GROUP_SCAN = 160;
static const int IME_MAX_MEDIUM_INITIAL_GROUP_SCAN = 360;
static const int IME_MAX_LONG_INITIAL_GROUP_SCAN = 220;
static const int IME_MAX_SHORTHAND_GROUP_SCAN = 900;
static const int IME_MAX_SINGLE_RECORD_SCAN = 120;
static const int IME_MAX_SHORT_CONSONANT_SINGLE_SCAN = 18;
static const int IME_MAX_PARTIAL_RECORD_SCAN = 180;
static const int IME_MAX_INITIAL_COLLECT = 72;
static const int IME_MAX_USER_FIXED_SCAN = 24;
static const int IME_MAX_USER_PREFIX_SCAN = 160;
static const int IME_MAX_USER_PHRASE_KEEP = 32;
static const size_t IME_FAST_CANDIDATE_LIMIT = 80;
static const size_t IME_SHORT_CONSONANT_CANDIDATE_LIMIT = 12;
static const size_t IME_PARTIAL_PINYIN_CANDIDATE_LIMIT = 12;
#else
static const int IME_SEG_TABLE_MIN_LEN = 1;
static const int IME_PHRASE_PREFIX_MIN_LEN = 1;
static const int IME_USER_INITIAL_MIN_LEN = 1;
static const int IME_DICT_INITIAL_MIN_LEN = 1;
static const int IME_MAX_PHRASE_GROUP_SCAN = 5000;
static const int IME_MAX_INITIAL_GROUP_SCAN = 60000;
static const int IME_MAX_SHORT_INITIAL_GROUP_SCAN = 60000;
static const int IME_MAX_MEDIUM_INITIAL_GROUP_SCAN = 60000;
static const int IME_MAX_LONG_INITIAL_GROUP_SCAN = 60000;
static const int IME_MAX_SHORTHAND_GROUP_SCAN = 60000;
static const int IME_MAX_SINGLE_RECORD_SCAN = 1000000;
static const int IME_MAX_SHORT_CONSONANT_SINGLE_SCAN = 1000000;
static const int IME_MAX_PARTIAL_RECORD_SCAN = 1000000;
static const int IME_MAX_INITIAL_COLLECT = 300;
static const int IME_MAX_USER_FIXED_SCAN = 5000;
static const int IME_MAX_USER_PREFIX_SCAN = 5000;
static const int IME_MAX_USER_PHRASE_KEEP = 300;
static const size_t IME_FAST_CANDIDATE_LIMIT = 300;
static const size_t IME_SHORT_CONSONANT_CANDIDATE_LIMIT = 300;
static const size_t IME_PARTIAL_PINYIN_CANDIDATE_LIMIT = 300;
#endif

static const char *BUILTIN_ENGLISH_WORDS[] = {
    "about", "after", "again", "also", "android", "api", "app", "apple",
    "backup", "because", "before", "between", "build", "cache", "calendar",
    "change", "cloud", "code", "commit", "config", "content", "context",
    "data", "debug", "device", "document", "editor", "email", "error",
    "event", "export", "feature", "file", "filter", "firmware", "flash",
    "format", "function", "github", "hello", "history", "image", "import",
    "input", "issue", "journal", "keyboard", "local", "manager", "markdown",
    "memory", "message", "network", "note", "openai", "output", "password",
    "plugin", "project", "prompt", "python", "release", "request", "screen",
    "search", "setting", "storage", "sync", "system", "task", "today",
    "token", "update", "upload", "user", "version", "voice", "wifi", "word",
    "work", "write", "espidf", "esp32", "freertos", "lvgl", "lvgl9", "wifi6"
};

static const char *TECH_INLINE_WORDS[] = {
    "api", "app", "build", "cache", "commit", "config", "debug", "esp-idf",
    "esp32", "esp32s3", "espidf", "firmware", "flash", "freertos", "github",
    "github.com", "input", "lvgl", "lvgl9", "markdown", "openai", "python",
    "release", "sync", "token", "upload", "wifi", "wifi6", nullptr
};

struct BuiltinPredictEntry {
    const char *key;
    const char *candidates[9];
};

static const BuiltinPredictEntry BUILTIN_PREDICT[] = {
    {"我", {"们", "的", "也", "想", "是", "在", "会", "要", nullptr}},
    {"你", {"好", "们", "的", "也", "是", "在", "要", "看", nullptr}},
    {"他", {"们", "的", "也", "是", "在", "说", "会", "要", nullptr}},
    {"她", {"们", "的", "也", "是", "在", "说", "会", "要", nullptr}},
    {"它", {"们", "的", "是", "在", "会", "也", "有", "就", nullptr}},
    {"这", {"个", "样", "里", "些", "是", "种", "么", "次", nullptr}},
    {"那", {"个", "样", "里", "些", "是", "么", "种", "次", nullptr}},
    {"不", {"是", "会", "能", "要", "用", "知道", "过", "太", nullptr}},
    {"没", {"有", "事", "关系", "办法", "必要", "问题", "想到", "看到", nullptr}},
    {"有", {"点", "些", "时候", "一个", "没有", "可能", "什么", "问题", nullptr}},
    {"可", {"以", "能", "是", "爱", "惜", "见", "用", "怕", nullptr}},
    {"会", {"有", "不会", "觉得", "看到", "出现", "影响", "变成", "继续", nullptr}},
    {"想", {"到", "要", "了", "起来", "一下", "办法", "清楚", "知道", nullptr}},
    {"要", {"是", "不要", "把", "做", "看", "写", "用", "说", nullptr}},
    {"在", {"这里", "一起", "里面", "这个", "那边", "做", "看", "写", nullptr}},
    {"就", {"是", "会", "可以", "这样", "不用", "好了", "知道", "开始", nullptr}},
    {"都", {"是", "可以", "有", "没有", "会", "要", "在", "能", nullptr}},
    {"很", {"多", "好", "快", "难", "重要", "舒服", "清楚", "简单", nullptr}},
    {"太", {"多", "好了", "难", "晚", "快", "慢", "重要", "麻烦", nullptr}},
    {"好", {"的", "了", "像", "看", "用", "一点", "起来", "多", nullptr}},
    {"大", {"概", "家", "部分", "概是", "多数", "小", "概念", "量", nullptr}},
    {"小", {"心", "事", "时候", "朋友", "问题", "工具", "一点", "结", nullptr}},
    {"中", {"国", "文", "心", "午", "间", "断", "央", "年", nullptr}},
    {"国", {"内", "外", "家", "语", "际", "人", "民", "庆", nullptr}},
    {"今", {"天", "晚", "年", "后", "日", "早", "下午", "上午", nullptr}},
    {"明", {"天", "白", "年", "确", "显", "亮", "日", "早", nullptr}},
    {"昨", {"天", "晚", "日", "年", "夜", "天下午", "天晚上", "天上午", nullptr}},
    {"时", {"候", "间", "候", "刻", "常", "不时", "而", "差", nullptr}},
    {"日", {"记", "常", "期", "子", "程", "本", "后", "落", nullptr}},
    {"工", {"作", "具", "程", "资", "厂", "位", "人", "业", nullptr}},
    {"学", {"习", "校", "会", "生", "到", "术", "问", "院", nullptr}},
    {"写", {"下", "完", "一下", "出来", "进去", "好", "作", "入", nullptr}},
    {"看", {"看", "到", "一下", "起来", "见", "完", "过", "法", nullptr}},
    {"做", {"好", "完", "到", "一下", "出来", "法", "成", "过", nullptr}},
    {"用", {"来", "了", "一下", "起来", "户", "法", "得", "处", nullptr}},
    {"说", {"明", "了", "一下", "起来", "法", "到", "完", "不定", nullptr}},
    {"输", {"入", "出", "法", "错", "给", "送", "赢", "血", nullptr}},
    {"入", {"法", "口", "门", "手", "职", "睡", "选", "库", nullptr}},
    {"候", {"选", "补", "鸟", "车", "机", "诊", "审", "场", nullptr}},
    {"选", {"择", "项", "中", "候", "取", "词", "出", "举", nullptr}},
    {"词", {"组", "库", "语", "典", "频", "条", "汇", "义", nullptr}},
    {"字", {"符", "体", "词", "段", "节", "数", "幕", "形", nullptr}},
    {"功", {"能", "课", "夫", "耗", "率", "德", "效", "成", nullptr}},
    {"能", {"够", "不能", "力", "用", "看到", "实现", "支持", "继续", nullptr}},
    {"支", {"持", "付", "架", "线", "出", "撑", "配", "点", nullptr}},
    {"修", {"改", "复", "正", "饰", "订", "炼", "理", "行", nullptr}},
    {"优", {"化", "先", "点", "雅", "势", "秀", "惠", "良", nullptr}},
    {"问", {"题", "一下", "候", "问", "答", "号", "清楚", "出来", nullptr}},
    {"题", {"目", "外", "材", "库", "型", "解", "名", "意", nullptr}},
    {"拼", {"音", "写", "起来", "出来", "错", "一下", "读", "接", nullptr}},
    {"码", {"表", "字", "长", "本", "率", "序", "位", "元", nullptr}},
    {"固", {"件", "定", "化", "有", "态", "守", "执", "然", nullptr}},
    {"蓝", {"牙", "色", "图", "本", "屏", "牙键盘", "牙连接", "牙设备", nullptr}},
    {"键", {"盘", "入", "值", "位", "帽", "盘连接", "盘电量", "盘输入", nullptr}},
    {"屏", {"幕", "显", "保", "蔽", "幕显示", "幕刷新", "幕亮度", "幕内容", nullptr}},
    {"设", {"置", "备", "计", "定", "成", "为", "法", "想", nullptr}},
    {"同", {"步", "时", "意", "样", "步失败", "步完成", "步文件", "步数据", nullptr}},
    {"语", {"音", "句", "义", "文", "音输入", "音识别", "音转写", "音文件", nullptr}},
    {"识", {"别", "字", "破", "别结果", "别失败", "别文本", "别内容", nullptr}},
    {"刷", {"机", "新", "写", "卡", "机包", "机命令", "机失败", "机成功", nullptr}},
    {"启", {"动", "用", "发", "示", "动慢", "动界面", "动失败", "动完成", nullptr}},
    {"卡", {"顿", "住", "片", "死", "顿问题", "顿程度", "顿原因", "顿日志", nullptr}},
    {"日", {"记", "常", "期", "子", "程", "本", "后", "落", nullptr}},
    {"笔", {"记", "画", "者", "录", "记本", "记内容", "记文件", "记同步", nullptr}},
    {"灵", {"感", "活", "魂", "敏", "感记录", "感片段", "感整理", "感来源", nullptr}},
    {"大", {"概", "家", "纲", "部分", "多数", "小", "概念", "量", nullptr}},
    {"任", {"务", "何", "凭", "性", "务列表", "务管理", "务完成", "务同步", nullptr}},
    {"待", {"办", "会", "处理", "确定", "续", "命", "机", "选", nullptr}},
    {"保", {"存", "持", "护", "留", "证", "密", "险", "守", nullptr}},
    {"搜", {"索", "寻", "到", "一下", "索结果", "索内容", "索文件", "索词", nullptr}},
    {"输入", {"法", "方式", "模式", "内容", "文字", "拼音", "候选", "中文", nullptr}},
    {"输入法", {"设置", "候选", "词库", "联想", "模式", "优化", "卡顿", "拼音", nullptr}},
    {"候选", {"区", "词", "列表", "排序", "页面", "显示", "选择", "结果", nullptr}},
    {"词库", {"管理", "优化", "同步", "导入", "导出", "更新", "文件", "词条", nullptr}},
    {"联想", {"功能", "词库", "候选", "词", "效果", "设置", "优化", "触发", nullptr}},
    {"蓝牙", {"键盘", "连接", "设备", "电量", "管理", "配对", "断开", "重连", nullptr}},
    {"键盘", {"电量", "输入", "连接", "布局", "按键", "模式", "设备", "状态", nullptr}},
    {"屏幕", {"刷新", "显示", "亮度", "内容", "方向", "界面", "保护", "状态", nullptr}},
    {"语音", {"输入", "识别", "转写", "文件", "内容", "结果", "服务", "设置", nullptr}},
    {"笔记", {"同步", "内容", "文件", "管理", "列表", "搜索", "导出", "整理", nullptr}},
    {"任务", {"管理", "列表", "完成", "同步", "记录", "安排", "提醒", "状态", nullptr}},
    {"搜索", {"结果", "内容", "文件", "词", "页面", "历史", "范围", "匹配", nullptr}},
};

static int pinyinJoinedLength(const std::vector<ime::PinyinToken> &tokens) {
    int len = 0;
    for (auto &t : tokens) len += (int)t.text.length();
    return len;
}

static bool pinyinTokensMatchCodePrefix(const std::vector<ime::PinyinToken> &tokens,
                                        const char *code,
                                        int codeLen) {
    if (!code) return false;
    int pos = 0;
    for (auto &token : tokens) {
        int len = (int)token.text.length();
        if (pos + len > codeLen) return false;
        if (strncmp(code + pos, token.text.c_str(), len) != 0) return false;
        pos += len;
    }
    return true;
}

static bool pinyinSegmentsMatchText(const std::vector<ime::PinyinToken> &typed,
                                    const char *syllables) {
    if (typed.empty() || !syllables) return false;
    const char *p = syllables;
    for (size_t i = 0; i < typed.size(); i++) {
        while (*p == ' ') p++;
        if (*p == '\0') return false;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t entryLen = (size_t)(p - start);
        if (typed[i].text.size() > entryLen) return false;
        if (strncmp(typed[i].text.c_str(), start, typed[i].text.size()) != 0)
            return false;
    }
    return true;
}

static bool syllableTextStartsWithSegments(const char *syllables,
                                           const std::vector<std::string> &segs) {
    if (!syllables || segs.empty()) return false;
    const char *p = syllables;
    for (size_t i = 0; i < segs.size(); i++) {
        while (*p == ' ') p++;
        if (*p == '\0') return false;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t entryLen = (size_t)(p - start);
        if (segs[i].size() > entryLen) return false;
        if (strncmp(segs[i].c_str(), start, segs[i].size()) != 0)
            return false;
    }
    return true;
}

static int segPrefixKey(const char *code, int len) {
    if (!code || len < 1) return 26 * 26;
    int c0 = code[0] - 'a';
    if (c0 < 0 || c0 >= 26) return 26 * 26;
    if (len == 1) return c0 * 26;
    int c1 = code[1] - 'a';
    if (c1 < 0 || c1 >= 26) return 26 * 26;
    return c0 * 26 + c1;
}

static void addSegPrefixCandidates(std::vector<uint16_t> &indices,
                                   const uint16_t *order,
                                   const uint16_t *index,
                                   const char *code,
                                   int len) {
    int k = segPrefixKey(code, len);
    if (k >= 26 * 26) return;
    uint16_t lo = index[k];
    uint16_t hi = (len == 1) ? index[k + 26] : index[k + 1];
    for (uint16_t pos = lo; pos < hi; pos++) {
        uint16_t idx = order[pos];
        if (std::find(indices.begin(), indices.end(), idx) == indices.end())
            indices.push_back(idx);
    }
}

static void addUserPrefixIndexMatches(std::vector<uint16_t> &indices,
                                      const std::unordered_map<int, std::vector<uint16_t>> &index,
                                      const char *code,
                                      int len,
                                      int maxItems = 0) {
    int k = segPrefixKey(code, len);
    auto it = index.find(k);
    if (it == index.end()) return;
    for (uint16_t idx : it->second) {
        if (maxItems > 0 && (int)indices.size() >= maxItems) break;
        if (std::find(indices.begin(), indices.end(), idx) == indices.end())
            indices.push_back(idx);
    }
}

static std::string userPrefixKeyString(const char *code, int len) {
    if (!code || len < 3) return "";
    int n = std::min(len, 4);
    for (int i = 0; i < n; i++) {
        if (code[i] < 'a' || code[i] > 'z') return "";
    }
    return std::string(code, n);
}

static void addUserPrefixIndexMatches(std::vector<uint16_t> &indices,
                                      const std::unordered_map<std::string, std::vector<uint16_t>> &index,
                                      const char *code,
                                      int len,
                                      int maxItems = 0) {
    std::string k = userPrefixKeyString(code, len);
    if (k.empty()) return;
    auto it = index.find(k);
    if (it == index.end()) return;
    for (uint16_t idx : it->second) {
        if (maxItems > 0 && (int)indices.size() >= maxItems) break;
        if (std::find(indices.begin(), indices.end(), idx) == indices.end())
            indices.push_back(idx);
    }
}

static void addUserLookupIndexMatches(std::vector<uint16_t> &indices,
                                      const std::unordered_map<int, std::vector<uint16_t>> &index,
                                      const std::unordered_map<std::string, std::vector<uint16_t>> &prefixIndex,
                                      const char *code,
                                      int len,
                                      int maxItems = 0) {
    if (!code || len <= 0) return;
    if (len <= 2) {
        addUserPrefixIndexMatches(indices, index, code, len, maxItems);
        return;
    }
    addUserPrefixIndexMatches(indices, prefixIndex, code, len, maxItems);
}

static bool userLookupAliasPreciseEnough(const std::string &aliasCode, int typedLen) {
    if (aliasCode.empty()) return false;
    return (int)aliasCode.length() >= std::min(typedLen, 3);
}

static std::vector<std::string> splitSyllableText(const char *text) {
    std::vector<std::string> out;
    std::string cur;
    for (const char *p = text; *p; p++) {
        if (*p == ' ' || *p == '\'') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::string pinyinInitialCodeCompat(const std::string &code) {
    std::string init = ime::PinyinEngine::initialCode(code);
    if (!init.empty()) return init;
    const char *wc = code.c_str();
    int cl = (int)code.length();
    char fallback[13]; int o = 0;
    for (int i = 0; i < cl && o < 12; ) {
        if (strchr("aeiouv", wc[i])) {
            while (i < cl && strchr("aeiouvngr", wc[i]) && o < 12)
                fallback[o++] = wc[i++];
            continue;
        }
        if (i + 1 < cl && (wc[i] == 'z' || wc[i] == 'c' || wc[i] == 's') && wc[i + 1] == 'h') {
            fallback[o++] = wc[i];
            fallback[o++] = wc[i + 1];
            i += 2;
        } else {
            fallback[o++] = wc[i++];
        }
        while (i < cl && strchr("aeiouv", wc[i])) i++;
        if (i < cl && strchr("ngr", wc[i])) {
            int j = i;
            while (j < cl && strchr("ngr", wc[j])) j++;
            if (j >= cl || !strchr("aeiouv", wc[j])) i = j;
        }
    }
    fallback[o] = 0;
    return fallback;
}

static bool pinyinInitialStartsWithCompat(const char *wc, int cl, const char *typed, int typedLen) {
    int o = 0;
    auto pushInit = [&](char ch) -> bool {
        if (o < typedLen && ch != typed[o]) return false;
        o++;
        return true;
    };
    for (int i = 0; i < cl && o < 12; ) {
        if (strchr("aeiouv", wc[i])) {
            while (i < cl && strchr("aeiouvngr", wc[i]) && o < 12) {
                if (!pushInit(wc[i++])) return false;
                if (o >= typedLen) return true;
            }
            continue;
        }
        if (i + 1 < cl && (wc[i] == 'z' || wc[i] == 'c' || wc[i] == 's') && wc[i + 1] == 'h') {
            if (!pushInit(wc[i])) return false;
            if (o >= typedLen) return true;
            if (!pushInit(wc[i + 1])) return false;
            i += 2;
        } else {
            if (!pushInit(wc[i++])) return false;
        }
        if (o >= typedLen) return true;
        while (i < cl && strchr("aeiouv", wc[i])) i++;
        if (i < cl && strchr("ngr", wc[i])) {
            int j = i;
            while (j < cl && strchr("ngr", wc[j])) j++;
            if (j >= cl || !strchr("aeiouv", wc[j])) i = j;
        }
    }
    return o >= typedLen;
}

static int userCandidateScore(const std::string &entryCode, int count, int typedLen) {
    int score = std::min(count, IME_SCORE_USER_COUNT_CAP) * IME_SCORE_USER_COUNT_WEIGHT;
    if ((int)entryCode.length() == typedLen) score += IME_SCORE_EXACT_CODE;
    else score += std::max(0, IME_SCORE_PREFIX_BASE - ((int)entryCode.length() - typedLen));
    return score;
}

static bool pinyinCodeEqualsAny(const std::string &matchedCode, const std::string &primaryCode,
                                const std::vector<std::string> &aliasCodes) {
    if (matchedCode == primaryCode) return true;
    for (auto &aliasCode : aliasCodes) {
        if (matchedCode == aliasCode) return true;
    }
    return false;
}

static int phraseCandidateScore(const std::string &word, int candLen, int typedLen, int syllableCount) {
    int score = std::min(candLen, typedLen) * IME_SCORE_CODE_MATCH_UNIT;
    if (candLen == typedLen) score += IME_SCORE_EXACT_CODE;
    else if (candLen > typedLen)
        score -= std::min(IME_SCORE_PHRASE_OVERMATCH_CAP,
                          (candLen - typedLen) * IME_SCORE_PHRASE_OVERMATCH_UNIT);
    int chars = (int)(word.length() / 3);
    if (syllableCount > 0)
        score += std::max(0, IME_SCORE_SYLLABLE_BASE -
                             std::abs(chars - syllableCount) * IME_SCORE_SYLLABLE_DISTANCE_UNIT);
    if (chars >= 2) score += IME_SCORE_MULTI_CHAR_BONUS;
    return score;
}

static bool phraseMatureForTyped(int fullCodeLen, int typedLen, int charCount) {
    if (typedLen >= fullCodeLen) return true;
    if (charCount <= 2) return typedLen >= 2;
    if (charCount == 3) return typedLen >= std::min(fullCodeLen, 4);
    if (charCount == 4) return typedLen >= std::min(fullCodeLen, 5);
    int minLen = IME_MATURE_LONG_PHRASE_MIN;
    if (charCount >= 7) minLen = IME_MATURE_VERY_LONG_PHRASE_MIN;
    int ratioLen = (fullCodeLen * IME_MATURE_LONG_PHRASE_NUM +
                    (IME_MATURE_LONG_PHRASE_DEN - 1)) / IME_MATURE_LONG_PHRASE_DEN;
    return typedLen >= std::min(fullCodeLen, std::max(minLen, ratioLen));
}

static bool initialPhraseMatureForTyped(int fullInitialLen, int typedLen, int charCount) {
    if (typedLen >= fullInitialLen) return true;
    if (charCount <= 3) return typedLen >= 2;
    if (charCount == 4) return typedLen >= 3;
    int minLen = charCount >= 7 ? IME_INITIAL_MATURE_VERY_LONG_MIN : IME_INITIAL_MATURE_LONG_MIN;
    int ratioLen = (fullInitialLen * IME_INITIAL_MATURE_NUM +
                    (IME_INITIAL_MATURE_DEN - 1)) / IME_INITIAL_MATURE_DEN;
    return typedLen >= std::min(fullInitialLen, std::max(minLen, ratioLen));
}

static int initialPhraseCandidateScoreFromLength(int initLen, int typedLen,
                                                 const std::string &word) {
    if (initLen < typedLen) return -1;
    int score = 0;
    if (initLen == typedLen) score += IME_SCORE_EXACT_CODE;
    else score += std::max(0, IME_INITIAL_SCORE_BASE -
                              (initLen - typedLen) * IME_INITIAL_SCORE_OVERMATCH_UNIT);
    int chars = (int)(word.length() / 3);
    score += std::max(0, IME_INITIAL_SCORE_BASE -
                         std::abs(chars - typedLen) * IME_INITIAL_SCORE_CHAR_DISTANCE_UNIT);
    if (chars == typedLen) score += IME_INITIAL_SCORE_LENGTH_MATCH_BONUS;
    if (chars >= 2) score += IME_SCORE_MULTI_CHAR_BONUS;
    return score;
}

static std::string userInitialForCode(const std::string &code) {
    if (code.find('\'') != std::string::npos) return "";
    return pinyinInitialCodeCompat(code);
}

static std::string zeroInitialAlias(const std::string &code) {
    if (code == "i") return "yi";
    if (code == "ia") return "ya";
    if (code == "ian") return "yan";
    if (code == "iang") return "yang";
    if (code == "iao") return "yao";
    if (code == "ie") return "ye";
    if (code == "in") return "yin";
    if (code == "ing") return "ying";
    if (code == "iong") return "yong";
    if (code == "iu" || code == "iou") return "you";
    if (code == "u") return "wu";
    if (code == "ua") return "wa";
    if (code == "uai") return "wai";
    if (code == "uan") return "wan";
    if (code == "uang") return "wang";
    if (code == "uei" || code == "ui") return "wei";
    if (code == "uen" || code == "un") return "wen";
    if (code == "uo") return "wo";
    if (code == "ve" || code == "ue") return "yue";
    return "";
}

static std::string leadingZeroInitialAliasCode(const std::string &code) {
    int maxLen = std::min<int>(5, code.length());
    for (int len = maxLen; len >= 1; len--) {
        std::string alias = zeroInitialAlias(code.substr(0, len));
        if (!alias.empty()) return alias + code.substr(len);
    }
    return "";
}

static std::string pinyinSpellingAliasCode(const std::string &code) {
    if (code.empty()) return "";
    std::string out = code;
    bool changed = false;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] == 'v' && i > 0 && strchr("jqxy", out[i - 1])) {
            out[i] = 'u';
            changed = true;
        }
    }
    for (size_t i = 0; i + 2 < out.size(); i++) {
        if ((out[i] == 'l' || out[i] == 'n') && out[i + 1] == 'u' && out[i + 2] == 'e') {
            out[i + 1] = 'v';
            changed = true;
        }
    }
    return changed ? out : "";
}

static void addUniqueString(std::vector<std::string> &items, const std::string &value) {
    if (value.empty()) return;
    for (auto &item : items) if (item == value) return;
    items.push_back(value);
}

static bool fuzzyOptionEnabledInConfig(const std::string &cfg, const char *name) {
    if (cfg.empty() || cfg == "0" || cfg == "off" || cfg == "none") return false;
    if (cfg == "1" || cfg == "all") return true;
    std::string needle = name;
    std::string token;
    auto flush = [&]() -> bool {
        if (token == needle) return true;
        if (needle == "zcs" && (token == "z" || token == "c" || token == "s")) return true;
        if (needle == "nl" && (token == "ln" || token == "n/l" || token == "l/n")) return true;
        if (needle == "eneng" && (token == "eng" || token == "en/eng" || token == "eng/en")) return true;
        if (needle == "ining" && (token == "ing" || token == "in/ing" || token == "ing/in")) return true;
        return false;
    };
    for (char ch : cfg) {
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if ((ch >= 'a' && ch <= 'z') || ch == '/') {
            token += ch;
        } else {
            if (!token.empty() && flush()) return true;
            token.clear();
        }
    }
    return !token.empty() && flush();
}

static void addFuzzyPrefixAlias(std::vector<std::string> &out, const std::string &code,
                                const char *from, const char *to) {
    if (code.length() < strlen(from) + 1) return;
    size_t fl = strlen(from);
    if (code.compare(0, fl, from) != 0) return;
    if (!strchr("aeiouv", code[fl])) return;
    addUniqueString(out, std::string(to) + code.substr(fl));
}

static void addFuzzyFinalAlias(std::vector<std::string> &out, const std::string &code,
                               const char *from, const char *to) {
    std::vector<ime::PinyinSplit> splits = ime::PinyinEngine::splitVariants(code, false, 4);
    size_t fromLen = strlen(from);
    for (auto &split : splits) {
        for (size_t i = 0; i < split.tokens.size(); i++) {
            const std::string &s = split.tokens[i].text;
            if (s.length() <= fromLen || s.compare(s.length() - fromLen, fromLen, from) != 0)
                continue;
            std::string alias;
            for (size_t j = 0; j < split.tokens.size(); j++) {
                std::string part = split.tokens[j].text;
                if (j == i)
                    part = part.substr(0, part.length() - fromLen) + to;
                alias += part;
            }
            addUniqueString(out, alias);
        }
    }
}

static void addFuzzyAliasCodes(std::vector<std::string> &out, const std::string &code,
                               const std::string &fuzzyCfg) {
    if (code.length() < 2) return;
    if (fuzzyOptionEnabledInConfig(fuzzyCfg, "zcs")) {
        addFuzzyPrefixAlias(out, code, "zh", "z");
        addFuzzyPrefixAlias(out, code, "ch", "c");
        addFuzzyPrefixAlias(out, code, "sh", "s");
        addFuzzyPrefixAlias(out, code, "z", "zh");
        addFuzzyPrefixAlias(out, code, "c", "ch");
        addFuzzyPrefixAlias(out, code, "s", "sh");
    }
    if (fuzzyOptionEnabledInConfig(fuzzyCfg, "nl")) {
        addFuzzyPrefixAlias(out, code, "n", "l");
        addFuzzyPrefixAlias(out, code, "l", "n");
    }
    if (fuzzyOptionEnabledInConfig(fuzzyCfg, "eneng")) {
        addFuzzyFinalAlias(out, code, "en", "eng");
        addFuzzyFinalAlias(out, code, "eng", "en");
    }
    if (fuzzyOptionEnabledInConfig(fuzzyCfg, "ining")) {
        addFuzzyFinalAlias(out, code, "in", "ing");
        addFuzzyFinalAlias(out, code, "ing", "in");
    }
}

static std::string fuzzyInitialAliasCode(const std::string &code, const std::string &fuzzyCfg) {
    if (code.length() < 2 || !fuzzyOptionEnabledInConfig(fuzzyCfg, "zcs")) return "";
    auto withPrefix = [&](const char *from, const char *to) -> std::string {
        size_t fl = strlen(from);
        if (code.compare(0, fl, from) != 0) return "";
        if (code.length() == fl) return "";
        if (!strchr("aeiouv", code[fl])) return "";
        return std::string(to) + code.substr(fl);
    };
    std::string alias;
    if (!(alias = withPrefix("zh", "z")).empty()) return alias;
    if (!(alias = withPrefix("ch", "c")).empty()) return alias;
    if (!(alias = withPrefix("sh", "s")).empty()) return alias;
    if (!(alias = withPrefix("z", "zh")).empty()) return alias;
    if (!(alias = withPrefix("c", "ch")).empty()) return alias;
    if (!(alias = withPrefix("s", "sh")).empty()) return alias;
    return "";
}

static std::vector<std::string> alternateInputCodes(const std::string &code,
                                                   const std::string &fuzzyCfg) {
    std::vector<std::string> out;
    std::string leading = leadingZeroInitialAliasCode(code);
    addUniqueString(out, leading);
    std::string spelling = pinyinSpellingAliasCode(code);
    addUniqueString(out, spelling);
    if (!spelling.empty()) addUniqueString(out, leadingZeroInitialAliasCode(spelling));
    std::string fuzzy = fuzzyInitialAliasCode(code, fuzzyCfg);
    addUniqueString(out, fuzzy);
    addFuzzyAliasCodes(out, code, fuzzyCfg);
    if (!fuzzy.empty()) {
        addUniqueString(out, leadingZeroInitialAliasCode(fuzzy));
        addUniqueString(out, pinyinSpellingAliasCode(fuzzy));
    }
    return out;
}

static std::vector<std::string> alternateInputCodes(const std::string &code) {
    return alternateInputCodes(code, g_settings.imeFuzzy());
}

static std::vector<std::string> alternateLearningCodes(const std::string &code) {
    std::vector<std::string> out;
    std::string compact = ime::PinyinEngine::removeSplit(ime::PinyinEngine::normalize(code));
    if (!compact.empty() && compact != code) out.push_back(compact);
    for (auto &alias : alternateInputCodes(compact)) {
        if (alias != code && alias != compact) addUniqueString(out, alias);
    }
    return out;
}

static bool userCodeMatchesPrefix(const std::string &entryCode, const char *typed, int typedLen,
                                  std::string *matchedCode = nullptr) {
    if ((int)entryCode.length() >= typedLen && strncmp(entryCode.c_str(), typed, typedLen) == 0) {
        if (matchedCode) *matchedCode = entryCode;
        return true;
    }
    if (entryCode.find('\'') == std::string::npos) return false;
    std::string compact = ime::PinyinEngine::removeSplit(entryCode);
    if ((int)compact.length() >= typedLen && strncmp(compact.c_str(), typed, typedLen) == 0) {
        if (matchedCode) *matchedCode = compact;
        return true;
    }
    return false;
}

struct ImePerfTrace {
    const std::string &code;
    const std::vector<std::string> &candidates;
    int64_t startUs = IME_PERF_NOW();
    int64_t setupUs = 0;
    int64_t userUs = 0;
    int64_t singleUs = 0;
    int64_t segUs = 0;
    int64_t userPhraseUs = 0;
    int64_t phraseUs = 0;
    int64_t phraseSortUs = 0;
    int64_t userInitialUs = 0;
    int64_t initialUs = 0;
    int64_t shorthandUs = 0;
    int64_t partialUs = 0;
    int64_t recentUs = 0;
    int64_t sentenceUs = 0;
    // Sub-timings already contained in setupUs/userUs above. Reported for
    // attribution only, deliberately NOT summed into trackedUs/otherUs.
    int64_t rebuildUs = 0;        // setupUs subset: full user-word index rebuild
    int64_t metaUs = 0;           // setupUs subset: alias/primarySplit recompute
    int64_t userScanFixedUs = 0;  // userUs subset: fixed user-dict scan
    int64_t userScanDynUs = 0;    // userUs subset: dynamic user-dict scan
    int64_t segSplitUs = 0;       // segUs subset: splitVariants (incl. aliases)
    int64_t segMatchUs = 0;       // segUs subset: index lookup + match/score loop
    int64_t segSortUs = 0;        // segUs subset: segMatches sort + append
    int64_t segInitUs = 0;        // userInitUs subset: Phase 4c scan (index + score loop)
    int64_t segInitDedupUs = 0;   // segInitUs subset: addSegPrefixCandidates x2 (dedup)
    int64_t segInitBoostUs = 0;   // segInitUs subset: context/stable boost calls
    int64_t segInitRestUs = 0;    // segInitUs subset: remainder (maturity/score/RankedList)
    int64_t segInitSortUs = 0;    // userInitUs subset: Phase 4c sort + append (disjoint from segInitUs)
    int64_t userInitSortUs = 0;   // userInitUs subset: Phase 5 sort + append
    bool rebuilt = false;
    bool hasVowel = false;
    bool incomplete = false;
    bool fixedPaging = false;
    size_t limit = 0;
    const char *exitName = "end";

    ImePerfTrace(const std::string &c, const std::vector<std::string> &cand)
        : code(c), candidates(cand) {}

    ~ImePerfTrace() {
        int64_t totalUs = IME_PERF_NOW() - startUs;
        if (totalUs < IME_PERF_SLOW_US &&
            setupUs < IME_PERF_SLOW_US &&
            userUs < IME_PERF_SLOW_US &&
            singleUs < IME_PERF_SLOW_US &&
            segUs < IME_PERF_SLOW_US &&
            phraseUs < IME_PERF_SLOW_US &&
            userInitialUs < IME_PERF_SLOW_US &&
            initialUs < IME_PERF_SLOW_US &&
            shorthandUs < IME_PERF_SLOW_US &&
            partialUs < IME_PERF_SLOW_US &&
            recentUs < IME_PERF_SLOW_US &&
            sentenceUs < IME_PERF_SLOW_US)
            return;
        int64_t trackedUs = setupUs + userUs + singleUs + segUs + userPhraseUs +
                            phraseUs + phraseSortUs + userInitialUs + initialUs +
                            shorthandUs + partialUs + recentUs + sentenceUs;
        int64_t otherUs = totalUs > trackedUs ? totalUs - trackedUs : 0;
        ESP_LOGW(IME_TAG,
                 "perf lookup code='%s' exit=%s total=%lldus cand=%u limit=%u hv=%d inc=%d fixed=%d "
                 "setup=%lld[rebuild=%lld r=%d meta=%lld] user=%lld[fixed=%lld dyn=%lld] "
                 "single=%lld seg=%lld[split=%lld match=%lld sort=%lld] userPhrase=%lld "
                 "phrase=%lld sort=%lld userInit=%lld[segInit=%lld dedup=%lld boost=%lld rest=%lld siSort=%lld uiSort=%lld] "
                 "init=%lld shorthand=%lld partial=%lld recent=%lld sentence=%lld other=%lld",
                 code.c_str(), exitName, (long long)totalUs, (unsigned)candidates.size(),
                 (unsigned)limit, hasVowel ? 1 : 0, incomplete ? 1 : 0, fixedPaging ? 1 : 0,
                 (long long)setupUs, (long long)rebuildUs, rebuilt ? 1 : 0, (long long)metaUs,
                 (long long)userUs, (long long)userScanFixedUs, (long long)userScanDynUs,
                 (long long)singleUs,
                 (long long)segUs, (long long)segSplitUs, (long long)segMatchUs, (long long)segSortUs,
                 (long long)userPhraseUs,
                 (long long)phraseUs, (long long)phraseSortUs,
                 (long long)userInitialUs, (long long)segInitUs,
                 (long long)segInitDedupUs, (long long)segInitBoostUs, (long long)segInitRestUs,
                 (long long)segInitSortUs, (long long)userInitSortUs,
                 (long long)initialUs, (long long)shorthandUs, (long long)partialUs,
                 (long long)recentUs, (long long)sentenceUs, (long long)otherUs);
    }
};

static void appendUtf8(uint32_t cp, std::string &out) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static std::string lastUtf8Char(const std::string &text) {
    if (text.empty()) return "";
    size_t pos = text.size() - 1;
    while (pos > 0 && (((unsigned char)text[pos] & 0xC0) == 0x80)) pos--;
    return text.substr(pos);
}

static size_t utf8CharEnd(const std::string &text, size_t pos) {
    if (pos >= text.size()) return text.size();
    size_t next = pos + 1;
    while (next < text.size() && (((unsigned char)text[next] & 0xC0) == 0x80)) next++;
    return next;
}

static std::string utf8CharAt(const std::string &text, size_t pos) {
    if (pos >= text.size()) return "";
    size_t end = utf8CharEnd(text, pos);
    return text.substr(pos, end - pos);
}

static uint32_t utf8CodepointAt(const std::string &text, size_t pos) {
    if (pos >= text.size()) return 0;
    const unsigned char *s = (const unsigned char *)text.data();
    unsigned char c = s[pos];
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && pos + 1 < text.size())
        return ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[pos + 1] & 0x3F);
    if ((c & 0xF0) == 0xE0 && pos + 2 < text.size())
        return ((uint32_t)(c & 0x0F) << 12) |
               ((uint32_t)(s[pos + 1] & 0x3F) << 6) |
               (uint32_t)(s[pos + 2] & 0x3F);
    return 0;
}

static inline bool isCjkCodepoint(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

static bool isCjkChar(const std::string &text) {
    return isCjkCodepoint(utf8CodepointAt(text, 0));
}

static bool validPredictEntry(const std::string &key, const std::string &word) {
    if (!isCjkChar(utf8CharAt(key, 0)) || !isCjkChar(utf8CharAt(word, 0))) return false;
    int keyChars = 0;
    for (size_t pos = 0; pos < key.size() && keyChars <= 4; ) {
        std::string ch = utf8CharAt(key, pos);
        if (!isCjkChar(ch)) return false;
        pos = utf8CharEnd(key, pos);
        keyChars++;
    }
    if (keyChars < 1 || keyChars > 4) return false;
    int chars = 0;
    for (size_t pos = 0; pos < word.size() && chars <= 4; ) {
        std::string ch = utf8CharAt(word, pos);
        if (!isCjkChar(ch)) return false;
        pos = utf8CharEnd(word, pos);
        chars++;
    }
    return chars >= 1 && chars <= 4;
}

static uint32_t candidateHash(const std::string &text) {
    uint32_t h = 2166136261u;
    for (unsigned char c : text) {
        h ^= c;
        h *= 16777619u;
    }
    return h ? h : 1;
}

static std::string userEntryKey(const std::string &code, const std::string &word, bool trad) {
    std::string key;
    key.reserve(code.size() + word.size() + 3);
    key += code;
    key += '\t';
    key += word;
    key += '\t';
    key += trad ? '1' : '0';
    return key;
}

static bool parseUserDictLine(const std::string &raw, std::string &code,
                              std::string &word, int &count, bool &trad) {
    std::string line = str_trim(raw);
    if (line.length() < 3) return false;
    auto sp1 = line.find(' ');
    if (sp1 == std::string::npos || sp1 < 1) return false;
    auto sp2 = line.find(' ', sp1 + 1);
    code = line.substr(0, sp1);
    count = 1;
    trad = false;
    if (sp2 != std::string::npos) {
        word = line.substr(sp1 + 1, sp2 - sp1 - 1);
        std::string tail = line.substr(sp2 + 1);
        auto sp3 = tail.find(' ');
        std::string cntStr = (sp3 == std::string::npos) ? tail : tail.substr(0, sp3);
        bool validCount = !cntStr.empty();
        for (char cc : cntStr)
            if (cc < '0' || cc > '9') { validCount = false; break; }
        if (validCount) {
            count = 0;
            for (char cc : cntStr) count = count * 10 + (cc - '0');
        }
        if (count < 1) count = 1;
        if (sp3 != std::string::npos) {
            std::string fl = tail.substr(sp3 + 1);
            if (fl == "1" || fl == "t" || fl == "T") trad = true;
        }
    } else {
        word = line.substr(sp1 + 1);
    }
    return code.length() >= 1 && word.length() >= 2;
}

static std::string userDictJournalPath(const char *path) {
    return std::string(path) + USERDICT_JOURNAL_SUFFIX;
}

static std::vector<std::string> cjkCharsOf(const std::string &text, int maxChars = 16) {
    std::vector<std::string> chars;
    for (size_t pos = 0; pos < text.size() && (int)chars.size() < maxChars; ) {
        std::string ch = utf8CharAt(text, pos);
        if (!isCjkChar(ch)) {
            chars.clear();
            return chars;
        }
        chars.push_back(ch);
        pos = utf8CharEnd(text, pos);
    }
    return chars;
}

static std::string cjkTailText(const std::string &text, int maxChars) {
    std::vector<std::string> chars = cjkCharsOf(text, 16);
    if (chars.empty()) return "";
    std::string out;
    int start = std::max<int>(0, (int)chars.size() - maxChars);
    for (int i = start; i < (int)chars.size(); i++) out += chars[i];
    return out;
}

static void addContextTailKeys(std::vector<std::string> &keys, const std::string &text) {
    addUniqueString(keys, cjkTailText(text, 4));
    addUniqueString(keys, cjkTailText(text, 3));
    addUniqueString(keys, cjkTailText(text, 2));
    addUniqueString(keys, lastUtf8Char(text));
}

static int utf8TextCharCount(const std::string &text) {
    int count = 0;
    for (size_t pos = 0; pos < text.size(); ) {
        pos = utf8CharEnd(text, pos);
        count++;
    }
    return count;
}

static bool allSameCjkChars(const std::string &text) {
    std::vector<std::string> chars = cjkCharsOf(text, 8);
    if (chars.size() < 2) return false;
    for (size_t i = 1; i < chars.size(); i++) {
        if (chars[i] != chars[0]) return false;
    }
    return true;
}

static bool isWeakSinglePredictTail(const std::string &word) {
    static const char *WEAK[] = {
        "的", "了", "着", "过", "在", "是", "有", "和", "与", "及",
        "就", "都", "也", "还", "很", "更", "最", "把", "被", "给",
        "吗", "呢", "吧", "啊", "呀", "哦", "嗯", nullptr
    };
    if (utf8TextCharCount(word) != 1) return false;
    for (int i = 0; WEAK[i]; i++) {
        if (word == WEAK[i]) return true;
    }
    return false;
}

static bool isWeakAutoPhraseChar(const std::string &word) {
    static const char *WEAK[] = {
        "的", "了", "着", "过", "在", "是", "有", "和", "与", "及",
        "就", "都", "也", "还", "很", "更", "最", "把", "被", "给",
        "我", "你", "他", "她", "它", "这", "那", nullptr
    };
    if (utf8TextCharCount(word) != 1) return false;
    for (int i = 0; WEAK[i]; i++) {
        if (word == WEAK[i]) return true;
    }
    return false;
}

static bool noisyPredictPair(const std::string &key, const std::string &word) {
    if (!validPredictEntry(key, word)) return true;
    if (key == word) return true;
    if (allSameCjkChars(key) || allSameCjkChars(word)) return true;
    if (utf8TextCharCount(key) <= 1 && isWeakSinglePredictTail(word)) return true;
    return false;
}

struct RankedPredictCandidate {
    int score;
    int sourceRank;
    int order;
    std::string key;
    std::string word;
};

static void addRankedPredictCandidate(std::vector<RankedPredictCandidate> &items,
                                      const std::string &key,
                                      const std::string &word,
                                      int score,
                                      int sourceRank,
                                      int order) {
    if (noisyPredictPair(key, word)) return;
    for (auto &item : items) {
        if (item.word == word) {
            if (score > item.score ||
                (score == item.score && sourceRank > item.sourceRank) ||
                (score == item.score && sourceRank == item.sourceRank && order < item.order)) {
                item.score = score;
                item.sourceRank = sourceRank;
                item.order = order;
                item.key = key;
            }
            return;
        }
    }
    items.push_back({score, sourceRank, order, key, word});
}

struct RankedCandidate {
    int score;
    int candLen;
    int sourceRank;
    std::string word;
};

static void sortRankedCandidates(std::vector<RankedCandidate> &items) {
    std::stable_sort(items.begin(), items.end(),
        [](const RankedCandidate &a, const RankedCandidate &b) {
            if (a.score != b.score) return a.score > b.score;
            if (a.sourceRank != b.sourceRank) return a.sourceRank > b.sourceRank;
            return a.word.length() < b.word.length();
        });
}

// Ranked candidate accumulator keyed by word. Replacing the old linear "is this
// word already present" scan keeps the per-record build loops O(n) instead of
// O(n^2) string comparisons, which dominated the wide scan phases.
struct RankedList {
    std::vector<RankedCandidate> items;
    std::unordered_map<std::string, uint16_t> index;

    void add(const std::string &word, int candLen, int score, int sourceRank) {
        auto it = index.find(word);
        if (it != index.end()) {
            RankedCandidate &item = items[it->second];
            if (score > item.score || (score == item.score && sourceRank > item.sourceRank)) {
                item.score = score;
                item.candLen = candLen;
                item.sourceRank = sourceRank;
            }
            return;
        }
        if (items.size() >= UINT16_MAX) return;
        index.emplace(word, (uint16_t)items.size());
        items.push_back({score, candLen, sourceRank, word});
    }
    void sort() { sortRankedCandidates(items); }
    // Keep only the first `keep` entries after sorting. The word index no longer
    // matches the shrunk vector, so drop it -- these lists are only read afterwards.
    void truncate(size_t keep) {
        if (items.size() <= keep) return;
        items.resize(keep);
        index.clear();
    }
    size_t size() const { return items.size(); }
    bool empty() const { return items.empty(); }
};

static std::string predictKeyForCommittedText(const std::string &text) {
    std::string tail = cjkTailText(text, 4);
    if (!tail.empty()) return tail;
    return lastUtf8Char(text);
}

static bool isAsciiPunctKey(int key) {
    return key >= 0x21 && key <= 0x7E &&
           !((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
             (key >= '0' && key <= '9'));
}

static bool isStandardPagePrevKey(int key) {
    return key == IME_KEY_UP || key == '-' || key == ';' || key == ',';
}

static bool isStandardPageNextKey(int key) {
    return key == IME_KEY_DOWN || key == '=' || key == '.';
}

static bool isEnglishPagePrevKey(int key) {
    return key == IME_KEY_UP || key == ';' || key == ',';
}

static bool isEnglishPageNextKey(int key) {
    return key == IME_KEY_DOWN || key == '=';
}

static bool isVModePagePrevKey(int key) {
    return key == IME_KEY_UP || key == ';' || key == ',';
}

static bool isVModePageNextKey(int key) {
    return key == IME_KEY_DOWN || key == '=' || key == '\'';
}

static bool isPredictPagePrevKey(int key) {
    return key == IME_KEY_UP || key == '-';
}

static bool isPredictPageNextKey(int key) {
    return key == IME_KEY_DOWN || key == '=';
}

static std::string imePunctForKey(int key) {
    std::string out;
    switch (key) {
    case ',':  out = "，"; break;
    case '.':  out = "。"; break;
    case '?':  out = "？"; break;
    case ';':  out = "；"; break;
    case ':':  out = "："; break;
    case '!':  out = "！"; break;
    default:
        if (key >= 0x21 && key <= 0x7E) out.assign(1, (char)key);
        break;
    }
    return out;
}

// 词组是否含繁体字形(trad_table.h 位图, U+346E-U+9FD3)。
static bool wordHasTrad(const std::string &w) {
    const char *p = w.c_str();
    while (*p) {
        unsigned char c = (unsigned char)*p;
        uint32_t cp;
        if (c < 0x80) { cp = c; p += 1; }
        else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F); p += 2; }
        else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F); p += 3; }
        else if ((c & 0xF8) == 0xF0) { cp = ((c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) | (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F); p += 4; }
        else { p += 1; continue; }
        if (cp >= kTradLo && cp <= kTradHi) {
            uint32_t off = cp - kTradLo;
            if ((kTradBitmap[off / 8] >> (off % 8)) & 1) return true;
        }
    }
    return false;
}

// 词组在当前显示模式下是否显示: 繁体模式隐藏含简体字形的词组(wf!=0);
// 简体模式隐藏含繁体字形的词组; 单字(≤3字节)不受影响。
static bool wordVisible(bool trad, const std::string &w, uint8_t wf) {
    if (trad) return wf == 0;
    if (w.size() <= 3) return true;
    return !wordHasTrad(w);
}

// Embedded dictionary symbols (from CMakeLists EMBED_FILES "ime/ime_table_pinyin.bin")
extern const uint8_t ime_table_pinyin_bin_start[] asm("_binary_ime_table_pinyin_bin_start");
extern const uint8_t ime_table_pinyin_bin_end[]   asm("_binary_ime_table_pinyin_bin_end");

#if PJOURNAL_IME_ENABLE_LIANGFEN
// Embedded liangfen dictionary
extern const uint8_t liangfen_bin_start[] asm("_binary_liangfen_bin_start");
extern const uint8_t liangfen_bin_end[]   asm("_binary_liangfen_bin_end");
#endif

// Embedded English word list
extern const uint8_t english_words_txt_start[] asm("_binary_english_words_txt_start");
extern const uint8_t english_words_txt_end[]   asm("_binary_english_words_txt_end");

static inline std::string str_trim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool IME::parseHeader(const uint8_t *hdrIndex, size_t total) {
    if (!_dict.parse(hdrIndex, total)) {
        ESP_LOGE(IME_TAG, "bad dictionary magic");
        return false;
    }
    _scheme = (Scheme)_dict.scheme();
    _codeLen = _dict.codeLen();
    _recordSize = _dict.recordSize();
    switch (_scheme) {
    case PINYIN:    _maxCode = 63; break;
    case SHUANGPIN: _maxCode = 2; break;
    case WUBI:
    default:        _maxCode = 4; break;
    }
    _count = _dict.singleCount();
    _recordBase = HEADER_SIZE + (size_t)INDEX_ENTRIES * 4;
    return true;
}

bool IME::begin() {
    if (_loaded) return true;
    _blob = ime_table_pinyin_bin_start;
    _blobSize = (size_t)(ime_table_pinyin_bin_end - ime_table_pinyin_bin_start);
    if (_blobSize < HEADER_SIZE || !parseHeader(_blob, _blobSize)) {
        _blob = nullptr;
        return false;
    }
    _loaded = true;
    _imeDebugLog = g_settings.imeDebug();
    _sentenceMode = g_settings.imeSentence();
    _docCtxMode = g_settings.imeDocContext();
    static const char *NAMES[] = {"Wubi", "Pinyin", "Shuangpin"};
    ESP_LOGI(IME_TAG, "ready: %s, %u records, codeLen %d",
             NAMES[_scheme <= SHUANGPIN ? _scheme : 0], (unsigned)_count, _codeLen);
    return true;
}

bool IME::loadUserDictFile(const char *path, std::vector<UserEntry> &entries,
                           bool &dirty, size_t maxEntries) {
    entries.clear();
    FILE *f = fopen(path, "r");
    if (!f) { dirty = false; return false; }

    // Read the whole file
    std::string allData;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) allData.append(buf, n);
    fclose(f);
    if (allData.empty()) { dirty = false; return true; }

    // Parse lines (same format as before: "code word count" per line). The
    // dynamic dictionaries can grow past 64KB, so parse the whole file that was
    // already read into memory instead of silently truncating at the old cap.
    bool hadDuplicates = false;
    // Hash the (code, word, trad) key instead of rescanning `entries` per line:
    // the dynamic dict allows 5000 entries, where the old linear scan was O(n^2).
    std::unordered_map<std::string, size_t> entryIndex;
    entryIndex.reserve(allData.size() / 12 + 16);
    size_t pos = 0;
    while (pos < allData.length()) {
        size_t nl = allData.find('\n', pos);
        std::string line;
        if (nl == std::string::npos) {
            line = allData.substr(pos);
            pos = allData.length();
        } else {
            line = allData.substr(pos, nl - pos);
            pos = nl + 1;
        }
        std::string code;
        std::string word;
        int count = 1;
        bool trad = false;
        if (parseUserDictLine(line, code, word, count, trad)) {
            auto existingIt = entryIndex.find(userEntryKey(code, word, trad));
            if (existingIt != entryIndex.end()) {
                UserEntry &existing = entries[existingIt->second];
                hadDuplicates = true;
                if (existing.count < count) existing.count = count;
            } else {
                if (entries.size() >= maxEntries) {
                    compactUserEntries(entries, maxEntries);
                    if (entries.size() >= maxEntries) entries.pop_back();
                    hadDuplicates = true;
                    entryIndex.clear();
                    entryIndex.reserve(entries.size() * 2 + 1);
                    for (size_t i = 0; i < entries.size(); i++)
                        entryIndex[userEntryKey(entries[i].code, entries[i].word, entries[i].trad)] = i;
                }
                entries.push_back({code, word, count, trad, userInitialForCode(code)});
                entryIndex[userEntryKey(code, word, trad)] = entries.size() - 1;
            }
        }
    }
    if (compactUserEntries(entries, maxEntries)) hadDuplicates = true;
    if (hadDuplicates) {
        // 只在真正合并了计数时才需要保存
        dirty = true;
        saveUserDictFile(path, entries, dirty);
    } else {
        dirty = false;  // 无重复，无需保存
    }
    if (entries.size() > 0)
        ESP_LOGI(IME_TAG, "loaded %zu user words from %s", entries.size(), path);
    return true;
}

void IME::loadUserDict() {
    loadUserDictFile(USERDICT_FIXED_PATH, _fixedUserWords, _fixedUserDirty, USERDICT_FIXED_LIMIT);
    loadUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty, USERDICT_DYNAMIC_LIMIT);
    loadUserDictFile(USERPREDICT_PATH, _userPredictWords, _userPredictDirty, USERPREDICT_LIMIT);
    loadUserDictFile(USERPREDICT_REJECT_PATH, _userPredictRejectWords, _userPredictRejectDirty, USERPREDICT_REJECT_LIMIT);
    loadUserDictJournal(USERDICT_FIXED_PATH, _fixedUserWords, _fixedUserDirty, USERDICT_FIXED_LIMIT);
    loadUserDictJournal(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty, USERDICT_DYNAMIC_LIMIT);
    loadUserDictJournal(USERPREDICT_PATH, _userPredictWords, _userPredictDirty, USERPREDICT_LIMIT);
    for (auto it = _userPredictWords.begin(); it != _userPredictWords.end(); ) {
        if (!validPredictEntry(it->code, it->word)) {
            it = _userPredictWords.erase(it);
            _userPredictDirty = true;
        } else {
            ++it;
        }
    }
    if (_userPredictDirty) saveUserDictFile(USERPREDICT_PATH, _userPredictWords, _userPredictDirty);
    if (_userPredictRejectDirty) saveUserDictFile(USERPREDICT_REJECT_PATH, _userPredictRejectWords, _userPredictRejectDirty);
    markUserWordIndexesDirty("load");
    _userPredictIndexDirty = true;
    _userDictLoaded = true;
}

void IME::ensureUserDictLoaded() {
    if (!_userDictLoaded) loadUserDict();
}

bool IME::compactUserEntries(std::vector<UserEntry> &entries, size_t limit) {
    bool changed = false;
    std::stable_sort(entries.begin(), entries.end(),
        [](const UserEntry &a, const UserEntry &b) {
            return a.count > b.count;
        });
    if (entries.size() > limit) {
        entries.resize(limit);
        changed = true;
    }
    return changed;
}

void IME::saveUserDictFile(const char *path, std::vector<UserEntry> &entries, bool &dirty) {
    if (!dirty) return;
    mkdir("/sdcard/settings", 0777);
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(IME_TAG, "failed to open userdict file %s", path);
        return;
    }

    // Write in descending-count order, but leave `entries` untouched: the lookup
    // indexes store positional indices, so reordering the vector here would
    // invalidate them and force a full rebuild on the next keystroke. Sort a
    // permutation of indices and walk that instead.
    std::vector<uint32_t> order(entries.size());
    for (uint32_t i = 0; i < (uint32_t)order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return entries[a].count > entries[b].count;
    });
    for (uint32_t i : order) {
        const UserEntry &p = entries[i];
        std::string line = p.code + " " + p.word + " " + std::to_string(p.count)
                         + (p.trad ? " 1" : "") + "\n";
        fwrite(line.data(), 1, line.size(), f);
    }

    if (fclose(f) != 0) {
        ESP_LOGE(IME_TAG, "failed to flush userdict file");
        return;
    }
    clearUserDictJournal(path);
    dirty = false;
}

void IME::loadUserDictJournal(const char *path, std::vector<UserEntry> &entries,
                              bool &dirty, size_t maxEntries) {
    std::string journal = userDictJournalPath(path);
    FILE *f = fopen(journal.c_str(), "r");
    if (!f) return;

    bool changed = false;
    std::unordered_map<std::string, size_t> entryIndex;
    entryIndex.reserve(entries.size() * 2 + 1);
    for (size_t i = 0; i < entries.size(); i++)
        entryIndex[userEntryKey(entries[i].code, entries[i].word, entries[i].trad)] = i;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        std::string code;
        std::string word;
        int count = 1;
        bool trad = false;
        if (!parseUserDictLine(buf, code, word, count, trad)) continue;
        auto existingIt = entryIndex.find(userEntryKey(code, word, trad));
        if (existingIt != entryIndex.end()) {
            UserEntry &existing = entries[existingIt->second];
            if (existing.count < count) {
                existing.count = count;
                changed = true;
            }
        } else {
            if (entries.size() >= maxEntries) {
                compactUserEntries(entries, maxEntries);
                if (entries.size() >= maxEntries) entries.pop_back();
                entryIndex.clear();
                entryIndex.reserve(entries.size() * 2 + 1);
                for (size_t i = 0; i < entries.size(); i++)
                    entryIndex[userEntryKey(entries[i].code, entries[i].word, entries[i].trad)] = i;
            }
            entries.push_back({code, word, count, trad, userInitialForCode(code)});
            entryIndex[userEntryKey(code, word, trad)] = entries.size() - 1;
            changed = true;
        }
    }
    fclose(f);
    if (compactUserEntries(entries, maxEntries)) changed = true;
    if (changed) {
        dirty = true;
        saveUserDictFile(path, entries, dirty);
    } else {
        clearUserDictJournal(path);
    }
}

void IME::appendUserDictJournal(const char *path, const UserEntry &entry) {
    if (!path || !entry.code.length() || !entry.word.length()) return;
    mkdir("/sdcard/settings", 0777);
    std::string journal = userDictJournalPath(path);
    FILE *f = fopen(journal.c_str(), "a");
    if (!f) return;
    std::string line = entry.code + " " + entry.word + " " + std::to_string(entry.count)
                     + (entry.trad ? " 1" : "") + "\n";
    fwrite(line.data(), 1, line.size(), f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

void IME::queueUserDictJournal(const char *path, const UserEntry &entry) {
    if (!path || !entry.code.length() || !entry.word.length()) return;
    _pendingUserDictJournal.push_back({path, entry});
    if (_pendingUserDictJournalSinceUs == 0)
        _pendingUserDictJournalSinceUs = esp_timer_get_time();
    flushUserDictJournal(false);
}

void IME::flushUserDictJournal(bool force) {
    if (_pendingUserDictJournal.empty()) {
        _pendingUserDictJournalSinceUs = 0;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!force && _pendingUserDictJournal.size() < USERDICT_JOURNAL_BATCH_LIMIT &&
        _pendingUserDictJournalSinceUs > 0 &&
        now - _pendingUserDictJournalSinceUs < USERDICT_JOURNAL_DEFER_US)
        return;

    std::vector<std::string> paths;
    for (auto &item : _pendingUserDictJournal) {
        bool seen = false;
        for (auto &path : paths) {
            if (path == item.path) { seen = true; break; }
        }
        if (!seen) paths.push_back(item.path);
    }
    mkdir("/sdcard/settings", 0777);
    for (auto &path : paths) {
        std::string journal = userDictJournalPath(path.c_str());
        FILE *f = fopen(journal.c_str(), "a");
        if (!f) continue;
        for (auto &item : _pendingUserDictJournal) {
            if (item.path != path) continue;
            const UserEntry &entry = item.entry;
            std::string line = entry.code + " " + entry.word + " " + std::to_string(entry.count)
                             + (entry.trad ? " 1" : "") + "\n";
            fwrite(line.data(), 1, line.size(), f);
        }
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    _pendingUserDictJournal.clear();
    _pendingUserDictJournalSinceUs = 0;
}

void IME::clearUserDictJournal(const char *path) {
    if (!path) return;
    std::string journal = userDictJournalPath(path);
    unlink(journal.c_str());
}

void IME::markUserDictDirty(bool &dirty, const char *path, const UserEntry *entry) {
    dirty = true;
    if (path && entry) queueUserDictJournal(path, *entry);
    if (_deferredUserDictSinceUs == 0)
        _deferredUserDictSinceUs = esp_timer_get_time();
}

void IME::flushUserDictSaves(bool force) {
    flushUserDictJournal(force);
    if (!_dynamicUserDirty && !_userPredictDirty && !_fixedUserDirty && !_userPredictRejectDirty) {
        _deferredUserDictSinceUs = 0;
        return;
    }
    if (!force && _deferredUserDictSinceUs > 0 &&
        esp_timer_get_time() - _deferredUserDictSinceUs < USERDICT_DEFER_SAVE_US)
        return;
    saveUserDictFile(USERDICT_FIXED_PATH, _fixedUserWords, _fixedUserDirty);
    saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
    saveUserDictFile(USERPREDICT_PATH, _userPredictWords, _userPredictDirty);
    saveUserDictFile(USERPREDICT_REJECT_PATH, _userPredictRejectWords, _userPredictRejectDirty);
    if (!_dynamicUserDirty && !_userPredictDirty && !_fixedUserDirty && !_userPredictRejectDirty)
        _deferredUserDictSinceUs = 0;
}

void IME::markUserWordIndexesDirty(const char *reason) {
    _userWordIndexesDirty = true;
    _indexDirtyReason = reason;
}

void IME::indexUserWordEntry(uint16_t idx, const UserEntry &p,
                             std::unordered_map<int, std::vector<uint16_t>> &codeIndex,
                             std::unordered_map<int, std::vector<uint16_t>> &initialIndex,
                             std::unordered_map<std::string, std::vector<uint16_t>> &codePrefixIndex,
                             std::unordered_map<std::string, std::vector<uint16_t>> &initialPrefixIndex) {
    int codeKey = segPrefixKey(p.code.c_str(), (int)p.code.length());
    if (codeKey < 26 * 26) codeIndex[codeKey].push_back(idx);
    std::string codePrefixKey = userPrefixKeyString(p.code.c_str(), (int)p.code.length());
    if (!codePrefixKey.empty()) codePrefixIndex[codePrefixKey].push_back(idx);
    if (!p.initial.empty()) {
        int initialKey = segPrefixKey(p.initial.c_str(), (int)p.initial.length());
        if (initialKey < 26 * 26) initialIndex[initialKey].push_back(idx);
        std::string initialPrefixKey = userPrefixKeyString(p.initial.c_str(), (int)p.initial.length());
        if (!initialPrefixKey.empty()) initialPrefixIndex[initialPrefixKey].push_back(idx);
    }
    if (p.code.find('\'') != std::string::npos) {
        std::string compact = ime::PinyinEngine::removeSplit(p.code);
        int compactKey = segPrefixKey(compact.c_str(), (int)compact.length());
        if (compactKey < 26 * 26 && compactKey != codeKey)
            codeIndex[compactKey].push_back(idx);
        std::string compactPrefixKey = userPrefixKeyString(compact.c_str(), (int)compact.length());
        if (!compactPrefixKey.empty() && compactPrefixKey != codePrefixKey)
            codePrefixIndex[compactPrefixKey].push_back(idx);
    }
}

void IME::sortUserIndexMaps(const std::vector<UserEntry> &entries,
                            std::unordered_map<int, std::vector<uint16_t>> &codeIndex,
                            std::unordered_map<int, std::vector<uint16_t>> &initialIndex,
                            std::unordered_map<std::string, std::vector<uint16_t>> &codePrefixIndex,
                            std::unordered_map<std::string, std::vector<uint16_t>> &initialPrefixIndex) {
    auto sortByCount = [&](auto &map) {
        for (auto &kv : map) {
            std::stable_sort(kv.second.begin(), kv.second.end(),
                [&](uint16_t a, uint16_t b) {
                    int ca = a < entries.size() ? entries[a].count : 0;
                    int cb = b < entries.size() ? entries[b].count : 0;
                    return ca > cb;
                });
        }
    };
    sortByCount(codeIndex);
    sortByCount(initialIndex);
    sortByCount(codePrefixIndex);
    sortByCount(initialPrefixIndex);
}

void IME::sortUserIndexMapsAfterAppend(const std::vector<UserEntry> &entries,
                                       std::unordered_map<int, std::vector<uint16_t>> &codeIndex,
                                       std::unordered_map<int, std::vector<uint16_t>> &initialIndex,
                                       std::unordered_map<std::string, std::vector<uint16_t>> &codePrefixIndex,
                                       std::unordered_map<std::string, std::vector<uint16_t>> &initialPrefixIndex) {
    auto countOf = [&entries](uint16_t i) {
        return i < entries.size() ? entries[i].count : 0;
    };
    auto repairIfNeeded = [&](std::vector<uint16_t> &bucket) {
        size_t n = bucket.size();
        if (n < 2) return;
        if (countOf(bucket[n - 2]) >= countOf(bucket[n - 1])) return;
        std::stable_sort(bucket.begin(), bucket.end(),
                         [&](uint16_t a, uint16_t b) { return countOf(a) > countOf(b); });
    };
    auto sweep = [&](auto &map) {
        for (auto &kv : map) repairIfNeeded(kv.second);
    };
    sweep(codeIndex);
    sweep(initialIndex);
    sweep(codePrefixIndex);
    sweep(initialPrefixIndex);
}

void IME::rebuildUserWordIndexes() {
    if (!_userWordIndexesDirty) return;
#if PJOURNAL_IME_PERF_LOG
    int64_t perfStartUs = esp_timer_get_time();
#endif
    auto build = [](const std::vector<UserEntry> &entries,
                    std::unordered_map<int, std::vector<uint16_t>> &codeIndex,
                    std::unordered_map<int, std::vector<uint16_t>> &initialIndex,
                    std::unordered_map<std::string, std::vector<uint16_t>> &codePrefixIndex,
                    std::unordered_map<std::string, std::vector<uint16_t>> &initialPrefixIndex,
                    int64_t &insertUs, int64_t &sortUs) {
        codeIndex.clear();
        initialIndex.clear();
        codePrefixIndex.clear();
        initialPrefixIndex.clear();
        int64_t insertStartUs = IME_PERF_NOW();
        for (size_t i = 0; i < entries.size() && i <= UINT16_MAX; i++)
            indexUserWordEntry((uint16_t)i, entries[i], codeIndex, initialIndex,
                               codePrefixIndex, initialPrefixIndex);
        int64_t sortStartUs = IME_PERF_NOW();
        insertUs += sortStartUs - insertStartUs;
        sortUserIndexMaps(entries, codeIndex, initialIndex, codePrefixIndex, initialPrefixIndex);
        sortUs += IME_PERF_NOW() - sortStartUs;
    };
    int64_t insertUs = 0, sortUs = 0;
    build(_fixedUserWords, _fixedUserCodeIndex, _fixedUserInitialIndex,
          _fixedUserCodePrefixIndex, _fixedUserInitialPrefixIndex, insertUs, sortUs);
    build(_dynamicUserWords, _dynamicUserCodeIndex, _dynamicUserInitialIndex,
          _dynamicUserCodePrefixIndex, _dynamicUserInitialPrefixIndex, insertUs, sortUs);
    _userWordIndexesDirty = false;
#if PJOURNAL_IME_PERF_LOG
    // Logged unconditionally (not behind the slow-lookup threshold): rebuilds are
    // rare -- one per newly learned word -- and the point is to watch this cost as
    // the user dictionary grows, even when the surrounding lookup stays fast.
    ESP_LOGW(IME_TAG, "perf rebuild fixed=%u dyn=%u by=%s insert=%lld sort=%lld total=%lldus",
             (unsigned)_fixedUserWords.size(), (unsigned)_dynamicUserWords.size(),
             _indexDirtyReason ? _indexDirtyReason : "-",
             (long long)insertUs, (long long)sortUs,
             (long long)(esp_timer_get_time() - perfStartUs));
#endif
}

void IME::appendUserWordIndexEntry(size_t entryIdx) {
    // A pending rebuild means the maps are stale (an erase or compaction shifted
    // indices), so appending on top would leave a partial index. Keep the dirty flag
    // set and let the next lookup rebuild everything.
    if (_userWordIndexesDirty || entryIdx > UINT16_MAX) {
        markUserWordIndexesDirty("append-stale");
        return;
    }
#if PJOURNAL_IME_PERF_LOG
    int64_t perfStartUs = esp_timer_get_time();
#endif
    indexUserWordEntry((uint16_t)entryIdx, _dynamicUserWords[entryIdx],
                       _dynamicUserCodeIndex, _dynamicUserInitialIndex,
                       _dynamicUserCodePrefixIndex, _dynamicUserInitialPrefixIndex);
    sortUserIndexMapsAfterAppend(_dynamicUserWords, _dynamicUserCodeIndex, _dynamicUserInitialIndex,
                                _dynamicUserCodePrefixIndex, _dynamicUserInitialPrefixIndex);
#if PJOURNAL_IME_PERF_LOG
    ESP_LOGW(IME_TAG, "perf index-append dyn=%u idx=%u total=%lldus",
             (unsigned)_dynamicUserWords.size(), (unsigned)entryIdx,
             (long long)(esp_timer_get_time() - perfStartUs));
#endif
}

void IME::rebuildUserPredictIndex() {
    if (!_userPredictIndexDirty) return;
    _userPredictIndex.clear();
    for (size_t i = 0; i < _userPredictWords.size() && i <= UINT16_MAX; i++) {
        _userPredictIndex[_userPredictWords[i].code].push_back((uint16_t)i);
    }
    _userPredictIndexDirty = false;
}

void IME::removeUserWord(const std::string &code, const std::string &word) {
    ensureUserDictLoaded();
    for (auto it = _dynamicUserWords.begin(); it != _dynamicUserWords.end(); ++it) {
        if (it->code == code && it->word == word && it->trad == _trad) {
            _dynamicUserWords.erase(it);
            markUserWordIndexesDirty("removeWord");
            _dynamicUserDirty = true;
            saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
            return;
        }
    }
}

void IME::clearUserDict() {
    ensureUserDictLoaded();
    if (_dynamicUserWords.empty()) return;
    _dynamicUserWords.clear();
    markUserWordIndexesDirty("clearAll");
    _dynamicUserDirty = true;
    saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
}

void IME::pruneUserDict(int minCount) {
    ensureUserDictLoaded();
    auto it = _dynamicUserWords.begin();
    while (it != _dynamicUserWords.end()) {
        if (it->count < minCount) {
            it = _dynamicUserWords.erase(it);
            markUserWordIndexesDirty("prune");
            _dynamicUserDirty = true;
        } else ++it;
    }
    if (_dynamicUserDirty) saveUserDictFile(USERDICT_DYNAMIC_PATH, _dynamicUserWords, _dynamicUserDirty);
}

const std::vector<IME::UserEntryView> IME::userDictEntries(UserDictKind kind) const {
    const std::vector<UserEntry> &src =
        (kind == FIXED_DICT) ? _fixedUserWords :
        (kind == PREDICT_DICT) ? _userPredictWords : _dynamicUserWords;
    std::vector<UserEntryView> out;
    out.reserve(src.size());
    for (auto &p : src) out.push_back({p.code, p.word, p.count, p.trad});
    return out;
}

bool IME::addUserDictEntry(UserDictKind kind, const std::string &code, const std::string &word) {
    return addUserDictEntry(kind, code, word, 1, _trad);
}

bool IME::addUserDictEntry(UserDictKind kind, const std::string &code, const std::string &word,
                           int count, bool trad) {
    ensureUserDictLoaded();
    if (word.length() < 3 || code.length() == 0) return false;
    if (kind == PREDICT_DICT && !validPredictEntry(code, word)) return false;
    if (count < 1) count = 1;
    std::vector<UserEntry> &entries =
        (kind == FIXED_DICT) ? _fixedUserWords :
        (kind == PREDICT_DICT) ? _userPredictWords : _dynamicUserWords;
    bool &dirty =
        (kind == FIXED_DICT) ? _fixedUserDirty :
        (kind == PREDICT_DICT) ? _userPredictDirty : _dynamicUserDirty;
    const char *path =
        (kind == FIXED_DICT) ? USERDICT_FIXED_PATH :
        (kind == PREDICT_DICT) ? USERPREDICT_PATH : USERDICT_DYNAMIC_PATH;
    size_t limit =
        (kind == FIXED_DICT) ? USERDICT_FIXED_LIMIT :
        (kind == PREDICT_DICT) ? USERPREDICT_LIMIT : USERDICT_DYNAMIC_LIMIT;
    for (auto &p : entries) {
        if (p.code == code && p.word == word && p.trad == trad) {
            p.count += count;
            dirty = true;
            if (kind == PREDICT_DICT) _userPredictIndexDirty = true;
            else markUserWordIndexesDirty("addEntry");
            saveUserDictFile(path, entries, dirty);
            return true;
        }
    }
    if (entries.size() >= limit) {
        if (kind == FIXED_DICT) return false;
        compactUserEntries(entries, limit);
        if (entries.size() >= limit) entries.pop_back();
        if (kind == PREDICT_DICT) _userPredictIndexDirty = true;
        else markUserWordIndexesDirty("addEntry-compact");
    }
    entries.push_back({code, word, count, trad, userInitialForCode(code)});
    if (kind == PREDICT_DICT) _userPredictIndexDirty = true;
    else markUserWordIndexesDirty("addEntry-new");
    dirty = true;
    saveUserDictFile(path, entries, dirty);
    return true;
}

int IME::addUserDictEntries(UserDictKind kind, const std::vector<UserEntryView> &items,
                            int *skipped) {
    ensureUserDictLoaded();
    int skippedCount = 0;
    int imported = 0;
    if (items.empty()) {
        if (skipped) *skipped = 0;
        return 0;
    }

    std::vector<UserEntry> &entries =
        (kind == FIXED_DICT) ? _fixedUserWords :
        (kind == PREDICT_DICT) ? _userPredictWords : _dynamicUserWords;
    bool &dirty =
        (kind == FIXED_DICT) ? _fixedUserDirty :
        (kind == PREDICT_DICT) ? _userPredictDirty : _dynamicUserDirty;
    const char *path =
        (kind == FIXED_DICT) ? USERDICT_FIXED_PATH :
        (kind == PREDICT_DICT) ? USERPREDICT_PATH : USERDICT_DYNAMIC_PATH;
    size_t limit =
        (kind == FIXED_DICT) ? USERDICT_FIXED_LIMIT :
        (kind == PREDICT_DICT) ? USERPREDICT_LIMIT : USERDICT_DYNAMIC_LIMIT;

    std::unordered_map<std::string, size_t> entryIndex;
    entryIndex.reserve(entries.size() * 2 + items.size() * 2 + 1);
    for (size_t i = 0; i < entries.size(); i++)
        entryIndex[userEntryKey(entries[i].code, entries[i].word, entries[i].trad)] = i;

    bool changed = false;
    for (auto &item : items) {
        if (item.word.length() < 3 || item.code.length() == 0 ||
            (kind == PREDICT_DICT && !validPredictEntry(item.code, item.word))) {
            skippedCount++;
            continue;
        }
        int count = item.count < 1 ? 1 : item.count;
        auto existingIt = entryIndex.find(userEntryKey(item.code, item.word, item.trad));
        if (existingIt != entryIndex.end()) {
            entries[existingIt->second].count += count;
            imported++;
            changed = true;
            continue;
        }
        if (entries.size() >= limit) {
            if (kind == FIXED_DICT) {
                skippedCount++;
                continue;
            }
            compactUserEntries(entries, limit);
            if (entries.size() >= limit) entries.pop_back();
            entryIndex.clear();
            entryIndex.reserve(entries.size() * 2 + items.size() * 2 + 1);
            for (size_t i = 0; i < entries.size(); i++)
                entryIndex[userEntryKey(entries[i].code, entries[i].word, entries[i].trad)] = i;
        }
        entries.push_back({item.code, item.word, count, item.trad, userInitialForCode(item.code)});
        entryIndex[userEntryKey(item.code, item.word, item.trad)] = entries.size() - 1;
        imported++;
        changed = true;
    }

    if (changed) {
        if (kind == PREDICT_DICT) _userPredictIndexDirty = true;
        else markUserWordIndexesDirty("addEntries");
        dirty = true;
        saveUserDictFile(path, entries, dirty);
    }
    if (skipped) *skipped = skippedCount;
    return imported;
}

void IME::removeUserDictEntries(UserDictKind kind, const std::vector<int> &indices) {
    ensureUserDictLoaded();
    std::vector<UserEntry> &entries =
        (kind == FIXED_DICT) ? _fixedUserWords :
        (kind == PREDICT_DICT) ? _userPredictWords : _dynamicUserWords;
    bool &dirty =
        (kind == FIXED_DICT) ? _fixedUserDirty :
        (kind == PREDICT_DICT) ? _userPredictDirty : _dynamicUserDirty;
    const char *path =
        (kind == FIXED_DICT) ? USERDICT_FIXED_PATH :
        (kind == PREDICT_DICT) ? USERPREDICT_PATH : USERDICT_DYNAMIC_PATH;
    for (int n = (int)entries.size() - 1; n >= 0; n--) {
        if (std::find(indices.begin(), indices.end(), n) != indices.end()) {
            entries.erase(entries.begin() + n);
            dirty = true;
            if (kind == PREDICT_DICT) _userPredictIndexDirty = true;
            else markUserWordIndexesDirty("removeEntries");
        }
    }
    if (dirty) saveUserDictFile(path, entries, dirty);
}

void IME::clearUserDict(UserDictKind kind) {
    ensureUserDictLoaded();
    std::vector<UserEntry> &entries =
        (kind == FIXED_DICT) ? _fixedUserWords :
        (kind == PREDICT_DICT) ? _userPredictWords : _dynamicUserWords;
    bool &dirty =
        (kind == FIXED_DICT) ? _fixedUserDirty :
        (kind == PREDICT_DICT) ? _userPredictDirty : _dynamicUserDirty;
    const char *path =
        (kind == FIXED_DICT) ? USERDICT_FIXED_PATH :
        (kind == PREDICT_DICT) ? USERPREDICT_PATH : USERDICT_DYNAMIC_PATH;
    if (entries.empty()) return;
    entries.clear();
    if (kind == PREDICT_DICT) _userPredictIndexDirty = true;
    else markUserWordIndexesDirty("clearKind");
    dirty = true;
    saveUserDictFile(path, entries, dirty);
}

size_t IME::userDictSize(UserDictKind kind) const {
    return (kind == FIXED_DICT) ? _fixedUserWords.size() :
           (kind == PREDICT_DICT) ? _userPredictWords.size() : _dynamicUserWords.size();
}

void IME::loadEnglishDict() {
    if (_englishDictLoaded) return;
    _englishDictLoaded = true;
    _englishWords.clear();

    auto addWord = [&](const std::string &raw) {
        std::string w = str_trim(raw);
        if (w.empty() || _englishWords.size() >= ENGLISHDICT_LIMIT) return;
        bool ok = true;
        for (char c : w) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  c == '\'' || c == '-' || c == '_')) {
                ok = false;
                break;
            }
        }
        if (ok) {
            for (char &c : w) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            _englishWords.push_back(w);
        }
    };

    FILE *f = fopen(ENGLISHDICT_PATH, "r");
    if (f) {
        char buf[128];
        while (fgets(buf, sizeof(buf), f) && _englishWords.size() < ENGLISHDICT_LIMIT) {
            addWord(buf);
        }
        fclose(f);
    }

    const char *embeddedStart = (const char *)english_words_txt_start;
    const char *embeddedEnd = (const char *)english_words_txt_end;
    if (_englishWords.empty() && embeddedEnd > embeddedStart) {
        const char *p = embeddedStart;
        const char *end = embeddedEnd;
        std::string line;
        while (p < end && _englishWords.size() < ENGLISHDICT_LIMIT) {
            char c = *p++;
            if (c == '\n' || c == '\r') {
                addWord(line);
                line.clear();
            } else {
                line += c;
            }
        }
        addWord(line);
    }

    if (_englishWords.empty()) {
        for (auto w : BUILTIN_ENGLISH_WORDS) _englishWords.push_back(w);
    }
    std::sort(_englishWords.begin(), _englishWords.end());
    _englishWords.erase(std::unique(_englishWords.begin(), _englishWords.end()), _englishWords.end());
}

static std::string chineseDigits(uint64_t n, bool financial) {
    static const char *LOW[] = {"零","一","二","三","四","五","六","七","八","九"};
    static const char *FIN[] = {"零","壹","贰","叁","肆","伍","陆","柒","捌","玖"};
    static const char *UNIT_LOW[] = {"","十","百","千"};
    static const char *UNIT_FIN[] = {"","拾","佰","仟"};
    static const char *GROUP[] = {"","万","亿","兆"};
    const char **D = financial ? FIN : LOW;
    const char **U = financial ? UNIT_FIN : UNIT_LOW;
    if (n == 0) return D[0];

    auto groupText = [&](int g) {
        std::string out;
        bool zeroPending = false;
        for (int pos = 3; pos >= 0; pos--) {
            int base = 1;
            for (int i = 0; i < pos; i++) base *= 10;
            int digit = (g / base) % 10;
            if (digit == 0) {
                if (!out.empty()) zeroPending = true;
                continue;
            }
            if (zeroPending) {
                out += D[0];
                zeroPending = false;
            }
            if (!(pos == 1 && digit == 1 && out.empty() && !financial)) out += D[digit];
            out += U[pos];
        }
        return out;
    };

    std::vector<int> groups;
    while (n > 0 && groups.size() < 4) {
        groups.push_back((int)(n % 10000));
        n /= 10000;
    }
    std::string out;
    bool zeroBetween = false;
    for (int i = (int)groups.size() - 1; i >= 0; i--) {
        if (groups[i] == 0) {
            if (!out.empty()) zeroBetween = true;
            continue;
        }
        if (zeroBetween || (!out.empty() && groups[i] < 1000)) {
            out += D[0];
            zeroBetween = false;
        }
        out += groupText(groups[i]);
        out += GROUP[i];
    }
    return out;
}

static std::string romanNumber(int n) {
    struct R { int v; const char *s; };
    static const R MAP[] = {{90,"XC"},{50,"L"},{40,"XL"},{10,"X"},{9,"IX"},{5,"V"},{4,"IV"},{1,"I"}};
    std::string out;
    for (auto &r : MAP) {
        while (n >= r.v) { out += r.s; n -= r.v; }
    }
    return out;
}

static bool parseDateParts(const std::string &s, int &y, int &m, int &d) {
    std::vector<int> nums;
    std::string cur;
    for (char c : s) {
        if (c >= '0' && c <= '9') cur += c;
        else if (c == '.' || c == '-' || c == '/') {
            if (cur.empty()) return false;
            nums.push_back(atoi(cur.c_str()));
            cur.clear();
        } else return false;
    }
    if (!cur.empty()) nums.push_back(atoi(cur.c_str()));
    if (nums.size() == 3) {
        y = nums[0]; m = nums[1]; d = nums[2];
    } else if (nums.size() == 1 && s.length() == 8) {
        y = atoi(s.substr(0, 4).c_str());
        m = atoi(s.substr(4, 2).c_str());
        d = atoi(s.substr(6, 2).c_str());
    } else {
        return false;
    }
    return y >= 1 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

static std::string chineseYear(int y) {
    static const char *D[] = {"零","一","二","三","四","五","六","七","八","九"};
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d", y);
    std::string out;
    // 只遍历数字字符:按数组长度遍历会把 '\0' 和未初始化字节当下标,越界读指针导致崩溃
    for (char *p = buf; *p; p++) out += D[*p - '0'];
    return out;
}

static std::string chineseDayMonth(int n) {
    static const char *D[] = {"零","一","二","三","四","五","六","七","八","九"};
    if (n <= 10) return n == 10 ? "十" : D[n];
    if (n < 20) return std::string("十") + D[n % 10];
    if (n % 10 == 0) return std::string(D[n / 10]) + "十";
    return std::string(D[n / 10]) + "十" + D[n % 10];
}

static std::string capFirst(const std::string &w) {
    if (w.empty() || w[0] < 'a' || w[0] > 'z') return w;
    std::string r = w;
    r[0] = (char)(r[0] - 'a' + 'A');
    return r;
}

// v/t / v/d / v/w 的候选:当前时刻按 纯数字/数字加中文/纯中文(星期为 英文/中文)生成
static std::vector<std::string> vTimeDateWeek(const std::string &body) {
    std::vector<std::string> out;
    time_t now;
    time(&now);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[48];
    if (body == "/t") {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        out.push_back(buf);
        snprintf(buf, sizeof(buf), "%02d时%02d分%02d秒", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        out.push_back(buf);
        snprintf(buf, sizeof(buf), "%s时%s分%s秒", chineseDayMonth(tmv.tm_hour).c_str(),
                 chineseDayMonth(tmv.tm_min).c_str(), chineseDayMonth(tmv.tm_sec).c_str());
        out.push_back(buf);
    } else if (body == "/d") {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        out.push_back(buf);
        snprintf(buf, sizeof(buf), "%d年%d月%d日", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        out.push_back(buf);
        snprintf(buf, sizeof(buf), "%s年%s月%s日", chineseYear(tmv.tm_year + 1900).c_str(),
                 chineseDayMonth(tmv.tm_mon + 1).c_str(), chineseDayMonth(tmv.tm_mday).c_str());
        out.push_back(buf);
    } else if (body == "/w") {
        static const char *EN[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
        static const char *CN[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
        out.push_back(EN[tmv.tm_wday]);
        out.push_back(CN[tmv.tm_wday]);
    }
    return out;
}

#if PJOURNAL_IME_ENABLE_LIANGFEN
void IME::loadLfDict() {
    if (_lfBlob) return;
    _lfBlob = liangfen_bin_start;
    size_t total = (size_t)(liangfen_bin_end - liangfen_bin_start);
    if (total < 1354 + 16) { _lfBlob = nullptr; return; }

    // Parse index: 677 × uint16 LE
    _lfIndex.resize(INDEX_ENTRIES);
    for (int k = 0; k < INDEX_ENTRIES; k++) {
        _lfIndex[k] = (uint16_t)_lfBlob[k * 2] | ((uint16_t)_lfBlob[k * 2 + 1] << 8);
    }

    _lfRecordBase = INDEX_ENTRIES * 2;  // 1354
    _lfCount = (uint32_t)((total - _lfRecordBase) / 16);
    ESP_LOGI(IME_TAG, "Liangfen dict loaded: %u records", (unsigned)_lfCount);
}

void IME::searchLfWindow(const char *code, int len, uint32_t &lo, uint32_t &hi) {
    lo = 0; hi = _lfCount;
    if (_lfIndex.empty() || len < 1) return;
    int c0 = code[0] - 'a'; if (c0 < 0 || c0 >= 26) return;
    if (len == 1) { lo = _lfIndex[c0*26]; hi = _lfIndex[(c0+1)*26]; return; }
    int c1 = code[1] - 'a'; if (c1 < 0 || c1 >= 26) return;
    int k = c0 * 26 + c1;
    lo = _lfIndex[k]; hi = _lfIndex[k + 1];
}

bool IME::readLfCode(uint16_t i, char out[13]) {
    if (!_lfBlob || i >= _lfCount) return false;
    const uint8_t *rec = _lfBlob + _lfRecordBase + i * 16;
    int n = 0;
    for (; n < 12 && rec[n]; n++) out[n] = (char)rec[n];
    out[n] = '\0';
    return true;
}

bool IME::readLfHanzi(uint16_t i, char out[4]) {
    if (!_lfBlob || i >= _lfCount) return false;
    const uint8_t *rec = _lfBlob + _lfRecordBase + i * 16 + 12;
    out[0] = (char)rec[0]; out[1] = (char)rec[1]; out[2] = (char)rec[2]; out[3] = '\0';
    return true;
}
#endif

void IME::bumpFrequency(const std::string &code, const std::string &word, int weight) {
    ensureUserDictLoaded();
    if (recentlyDeletedWord(word)) return;
    if (weight < 1) weight = 1;
    for (auto &p : _fixedUserWords) {
        if (p.code == code && p.word == word && p.trad == _trad) return;
    }
    for (auto it = _dynamicUserWords.begin(); it != _dynamicUserWords.end(); ++it) {
        if (it->code == code && it->word == word && it->trad == _trad) {
            it->count += weight;
            markUserDictDirty(_dynamicUserDirty, USERDICT_DYNAMIC_PATH, &(*it));
            flushUserDictSaves(false);
            return;
        }
    }
    if (word.length() >= 3 && code.length() >= 1) {
        if (_dynamicUserWords.size() >= USERDICT_DYNAMIC_LIMIT) {
            compactUserEntries(_dynamicUserWords, USERDICT_DYNAMIC_LIMIT);
            if (_dynamicUserWords.size() >= USERDICT_DYNAMIC_LIMIT) _dynamicUserWords.pop_back();
            // Compaction drops entries and shifts every following index, so the maps
            // must be rebuilt; leaving the dirty flag set makes the append below a no-op.
            markUserWordIndexesDirty("compact");
        }
        _dynamicUserWords.push_back({code, word, weight, _trad, userInitialForCode(code)});
        appendUserWordIndexEntry(_dynamicUserWords.size() - 1);
        markUserDictDirty(_dynamicUserDirty, USERDICT_DYNAMIC_PATH, &_dynamicUserWords.back());
        flushUserDictSaves(false);
    }
}

// 删掉动态词库下标 i 的条目: 末尾条目顶上(swap-and-pop), 并把索引里指向末尾那条的下标
// 就地改写成 i。penalize 删词原来走 erase + markUserWordIndexesDirty, 动态词到 470 条时
// 整表重建要 40ms(实测 perf rebuild by=penalize insert=22ms sort=12ms), 而退格撤销刚上屏
// 学到的词必然触发它。
// 索引桶不重排: 桶按 count 降序, 从一个有序序列里删掉一个元素、再把另一个元素改个下标
// (它的 count 没变, 原来占的位次本来就正确), 剩下的序列依然有序。
void IME::removeDynamicUserWordAt(size_t i) {
    const size_t last = _dynamicUserWords.size() - 1;
    if (i > last) return;
    // 索引已经过期时不动它: 下标已经整体错位, 局部改写无从下手, 保持 dirty 让下次 lookup
    // 整表重建。USERDICT_DYNAMIC_LIMIT(5000) 小于 UINT16_MAX, 所以有效条目都在索引里。
    if (!_userWordIndexesDirty) {
        const uint16_t dead = (uint16_t)i;
        const uint16_t tail = (uint16_t)last;
        const bool move = (i != last);
        auto fix = [&](auto &map) {
            for (auto it = map.begin(); it != map.end(); ) {
                std::vector<uint16_t> &bucket = it->second;
                bucket.erase(std::remove(bucket.begin(), bucket.end(), dead), bucket.end());
                if (move) {
                    for (uint16_t &v : bucket) {
                        if (v == tail) v = dead;
                    }
                }
                if (bucket.empty()) it = map.erase(it);
                else ++it;
            }
        };
        fix(_dynamicUserCodeIndex);
        fix(_dynamicUserInitialIndex);
        fix(_dynamicUserCodePrefixIndex);
        fix(_dynamicUserInitialPrefixIndex);
    }
    if (i != last) _dynamicUserWords[i] = std::move(_dynamicUserWords[last]);
    _dynamicUserWords.pop_back();
}

bool IME::penalizeUserWord(const std::string &code, const std::string &word, int weight) {
    ensureUserDictLoaded();
    if (word.empty()) return false;
    if (weight < 1) weight = 1;
    bool changed = false;
    size_t i = 0;
    while (i < _dynamicUserWords.size()) {
        UserEntry &e = _dynamicUserWords[i];
        if (e.word == word && e.trad == _trad && (code.empty() || e.code == code)) {
            if (e.count <= weight) {
                // 末尾条目顶到 i, 那一条这个循环还没看过, 下标不能前进。
                removeDynamicUserWordAt(i);
            } else {
                e.count -= weight;
                i++;
            }
            changed = true;
        } else {
            i++;
        }
    }
    if (changed) {
        _dynamicUserDirty = true;
        flushUserDictSaves(false);
    }
    return changed;
}

void IME::bumpPredictFrequency(const std::string &key, const std::string &word, bool saveNow, int weight) {
    ensureUserDictLoaded();
    if (key.empty() || word.empty()) return;
    if (!validPredictEntry(key, word)) return;
    if (weight < 1) weight = 1;
    // learnPredictPairs() calls this up to 12 times per commit, and the old scan
    // walked up to USERPREDICT_LIMIT (2000) entries on every one of them. The
    // key -> entry-index map is kept valid on the append path below, so a repeat is
    // one hash lookup instead of a full-table scan.
    rebuildUserPredictIndex();
    auto bucketIt = _userPredictIndex.find(key);
    if (bucketIt != _userPredictIndex.end()) {
        for (uint16_t entryIdx : bucketIt->second) {
            if (entryIdx >= _userPredictWords.size()) continue;
            auto &p = _userPredictWords[entryIdx];
            if (p.word == word && p.trad == _trad) {
                p.count += weight;
                if (saveNow) {
                    _userPredictDirty = true;
                    saveUserDictFile(USERPREDICT_PATH, _userPredictWords, _userPredictDirty);
                } else {
                    markUserDictDirty(_userPredictDirty, USERPREDICT_PATH, &p);
                    flushUserDictSaves(false);
                }
                return;
            }
        }
    }
    if (_userPredictWords.size() >= USERPREDICT_LIMIT) {
        compactUserEntries(_userPredictWords, USERPREDICT_LIMIT);
        if (_userPredictWords.size() >= USERPREDICT_LIMIT) _userPredictWords.pop_back();
        // Compaction reorders and drops entries, shifting every positional index, so
        // the map cannot be repaired incrementally. Rebuilding here (rather than
        // leaving the dirty flag set) keeps the append below on the fast path.
        _userPredictIndexDirty = true;
        rebuildUserPredictIndex();
    }
    _userPredictWords.push_back({key, word, weight, _trad, ""});
    _userPredictIndex[key].push_back((uint16_t)(_userPredictWords.size() - 1));
    if (saveNow) {
        _userPredictDirty = true;
        saveUserDictFile(USERPREDICT_PATH, _userPredictWords, _userPredictDirty);
    } else {
        markUserDictDirty(_userPredictDirty, USERPREDICT_PATH, &_userPredictWords.back());
        flushUserDictSaves(false);
    }
}

bool IME::penalizePredictWord(const std::string &word, int weight) {
    ensureUserDictLoaded();
    if (word.empty()) return false;
    if (weight < 1) weight = 1;
    bool changed = false;
    for (auto it = _userPredictWords.begin(); it != _userPredictWords.end(); ) {
        if (it->word == word && it->trad == _trad) {
            if (it->count <= weight) {
                it = _userPredictWords.erase(it);
                _userPredictIndexDirty = true;
            } else {
                it->count -= weight;
                ++it;
            }
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        _userPredictDirty = true;
        // 走和其它学习路径同一套延迟落盘, 避免在删除/拒绝按键上做全量 SD 写。
        flushUserDictSaves(false);
    }
    return changed;
}

bool IME::rejectedPredictWord(const std::string &key, const std::string &word) const {
    if (key.empty() || word.empty()) return false;
    for (auto &p : _userPredictRejectWords) {
        if (p.code == key && p.word == word && p.trad == _trad && p.count >= 1)
            return true;
    }
    return false;
}

void IME::rejectPredictWord(const std::string &key, const std::string &word) {
    ensureUserDictLoaded();
    if (!validPredictEntry(key, word)) return;
    penalizePredictWord(word, 6);
    for (auto &p : _userPredictRejectWords) {
        if (p.code == key && p.word == word && p.trad == _trad) {
            p.count += 1;
            _userPredictRejectDirty = true;
            flushUserDictSaves(false);
            return;
        }
    }
    if (_userPredictRejectWords.size() >= USERPREDICT_REJECT_LIMIT) {
        compactUserEntries(_userPredictRejectWords, USERPREDICT_REJECT_LIMIT);
        if (_userPredictRejectWords.size() >= USERPREDICT_REJECT_LIMIT)
            _userPredictRejectWords.pop_back();
    }
    _userPredictRejectWords.push_back({key, word, 1, _trad, ""});
    _userPredictRejectDirty = true;
    flushUserDictSaves(false);
}

void IME::learnPredictPairs(const std::string &text) {
    if (text.size() < 6) return;
    ensureUserDictLoaded();
    std::vector<std::string> chars;
    int learned = 0;
    auto learnSegment = [&]() {
        for (size_t i = 0; i + 1 < chars.size() && learned < 12; i++) {
            std::string key;
            for (size_t k = i; k < chars.size() && k <= i + 2 && learned < 12; k++) {
                key += chars[k];
                if (k + 1 >= chars.size()) break;
                std::string tail;
                for (size_t j = k + 1; j < chars.size() && j <= k + 3 && learned < 12; j++) {
                    tail += chars[j];
                    if (!noisyPredictPair(key, tail))
                        bumpPredictFrequency(key, tail, false);
                    learned++;
                }
            }
        }
        chars.clear();
    };
    size_t pos = 0;
    while (pos < text.size() && learned < 12) {
        std::string ch = utf8CharAt(text, pos);
        pos = utf8CharEnd(text, pos);
        if (!isCjkChar(ch)) {
            if (chars.size() > 1) learnSegment();
            else chars.clear();
            continue;
        }
        chars.push_back(ch);
        if (chars.size() >= 12) learnSegment();
    }
    if (chars.size() > 1 && learned < 12) learnSegment();
    if (_userPredictDirty) flushUserDictSaves(false);
}

void IME::learnAutoPhraseFromSingle(const std::string &code, const std::string &word) {
    if (code.empty() || word.empty() || word.size() > 3 ||
        !isCjkChar(word) || isWeakAutoPhraseChar(word) || recentlyDeletedWord(word)) {
        _recentSingleCommits.clear();
        return;
    }
    _recentSingleCommits.push_back({code, word});
    if (_recentSingleCommits.size() > 4)
        _recentSingleCommits.erase(_recentSingleCommits.begin());
    int n = (int)_recentSingleCommits.size();
    for (int len = 2; len <= 4 && len <= n; len++) {
        std::string phraseCode;
        std::string phraseWord;
        for (int i = n - len; i < n; i++) {
            phraseCode += _recentSingleCommits[i].first;
            phraseWord += _recentSingleCommits[i].second;
        }
        if (phraseWord.size() >= 6 && !recentlyDeletedWord(phraseWord))
            bumpFrequency(phraseCode, phraseWord);
    }
}

void IME::rememberRecentCommit(const std::string &code, const std::string &word) {
    if (word.empty() || code.empty() || recentlyDeletedWord(word)) return;
    for (auto it = _recentCommittedWords.begin(); it != _recentCommittedWords.end(); ++it) {
        if (it->first == code && it->second == word) {
            _recentCommittedWords.erase(it);
            break;
        }
    }
    _recentCommittedWords.insert(_recentCommittedWords.begin(), {code, word});
    if (_recentCommittedWords.size() > 32) _recentCommittedWords.pop_back();
    rebuildRecentBoostIndex();
}

// Indices stay ascending so recentCommitBoost() visits a word's entries in the
// same order the old linear scan did, preserving the exact first-match semantics.
void IME::rebuildRecentBoostIndex() {
    _recentBoostByWord.clear();
    for (size_t i = 0; i < _recentCommittedWords.size(); i++)
        _recentBoostByWord[_recentCommittedWords[i].second].push_back((uint8_t)i);
}

int IME::recentCommitBoost(const std::string &code, const std::string &word) const {
    if (code.empty() || word.empty()) return 0;
    auto it = _recentBoostByWord.find(word);
    if (it == _recentBoostByWord.end()) return 0;
    for (uint8_t idx : it->second) {
        const auto &item = _recentCommittedWords[idx];
        int boost = 6000 - (int)idx * 180;
        if (boost <= 0) return 0;
        if (item.first == code) return boost + 4000;
        if (item.first.size() >= code.size() &&
            strncmp(item.first.c_str(), code.c_str(), code.size()) == 0)
            return boost;
    }
    return 0;
}

void IME::appendRecentCommitCandidates(const std::string &code,
                                       const std::vector<std::string> &aliasCodes,
                                       int typedLen) {
    if (code.empty() || typedLen < 2 || _recentCommittedWords.empty()) return;
    auto matchesInput = [&](const std::string &entryCode) -> bool {
        if ((int)entryCode.size() >= typedLen &&
            strncmp(entryCode.c_str(), code.c_str(), typedLen) == 0)
            return true;
        for (auto &aliasCode : aliasCodes) {
            int aliasLen = (int)aliasCode.length();
            if (aliasLen < 1) continue;
            if ((int)entryCode.size() >= aliasLen &&
                strncmp(entryCode.c_str(), aliasCode.c_str(), aliasLen) == 0)
                return true;
        }
        return false;
    };
    for (auto &item : _recentCommittedWords) {
        if (!matchesInput(item.first)) continue;
        appendCandidate(item.second, (int)item.first.length());
        if (_all.size() >= _candidateLimit) break;
    }
}

bool IME::recentlyDeletedWord(const std::string &word) const {
    if (word.empty()) return false;
    for (auto &w : _recentDeletedWords) {
        if (w == word) return true;
    }
    return false;
}

void IME::rememberDeletedWord(const std::string &word) {
    if (word.empty()) return;
    for (auto it = _recentDeletedWords.begin(); it != _recentDeletedWords.end(); ++it) {
        if (*it == word) {
            _recentDeletedWords.erase(it);
            break;
        }
    }
    _recentDeletedWords.insert(_recentDeletedWords.begin(), word);
    if (_recentDeletedWords.size() > 16) _recentDeletedWords.pop_back();
}

void IME::rememberLastLearning(const std::string &code, const std::string &word) {
    if (code.empty() || word.empty()) return;
    _lastLearningCode = code;
    _lastLearningWord = word;
    _lastLearningContext = _lastCommitText;
    _lastLearningUs = esp_timer_get_time();
}

void IME::rememberReplacementPreference(const std::string &code, const std::string &word) {
    if (code.empty() || word.empty() || _lastRejectedWord.empty() || _lastRejectedUs == 0)
        return;
    int64_t now = esp_timer_get_time();
    if (now - _lastRejectedUs > 8000000) {
        _lastRejectedCode.clear();
        _lastRejectedWord.clear();
        _lastRejectedContext.clear();
        _lastRejectedUs = 0;
        return;
    }
    if (_lastRejectedCode == code && _lastRejectedWord != word) {
        bumpFrequency(code, word, 5);
        penalizeUserWord(code, _lastRejectedWord, 3);
        if (!_lastRejectedContext.empty())
            bumpPredictFrequency(_lastRejectedContext, word, false, 3);
        _lastRejectedCode.clear();
        _lastRejectedWord.clear();
        _lastRejectedContext.clear();
        _lastRejectedUs = 0;
    }
}

// 文档级上下文(#13)。与 contextCandidateBoost 的区别: 那个看的是"上一句结尾预测下一
// 个词", 这个看的是"整篇正文里已经出现过的词"。只加分不压制, 且上限刻意压得很小,
// 否则会变成"写什么就推什么"的自激——正文里写过的词被推到第一, 再上屏又加强它。
static const int IME_DOC_CTX_UNIT = 100;       // 每出现一次加的分
static const int IME_DOC_CTX_MAX_HITS = 4;     // 出现次数封顶, 即最高 400 分
static const int IME_DOC_CTX_PROBE = 16;       // 线性探测步数上限

uint32_t IME::docCtxHash(const char *p, size_t n) {
    // FNV-1a。返回值避开 0, 因为 0 是空槽标记。
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

void IME::rebuildDocumentContext(const std::string &text) {
    memset(_docCtxHashes, 0, sizeof(_docCtxHashes));
    memset(_docCtxCounts, 0, sizeof(_docCtxCounts));
    if (!_docCtxMode || text.empty()) return;
    auto addGram = [&](const char *p, size_t len) {
        uint32_t h = docCtxHash(p, len);
        int base = (int)(h & (uint32_t)(DOC_CTX_SLOTS - 1));
        for (int probe = 0; probe < IME_DOC_CTX_PROBE; probe++) {
            int s = (base + probe) & (DOC_CTX_SLOTS - 1);
            if (_docCtxCounts[s] == 0) {
                _docCtxHashes[s] = h;
                _docCtxCounts[s] = 1;
                return;
            }
            if (_docCtxHashes[s] == h) {
                if (_docCtxCounts[s] < 255) _docCtxCounts[s]++;
                return;
            }
        }
    };
    const size_t n = text.size();
    size_t pos = 0;
    while (pos < n) {
        if (!isCjkCodepoint(utf8CodepointAt(text, pos))) {
            pos = utf8CharEnd(text, pos);
            continue;
        }
        // 片段必须是连续 CJK: 遇到标点/拉丁字母就停, 不跨过去凑字。
        size_t end = pos;
        for (int k = 1; k <= DOC_CTX_MAX_N; k++) {
            end = utf8CharEnd(text, end);
            if (end > n) break;
            if (k >= 2) addGram(text.data() + pos, end - pos);
            if (end < n && !isCjkCodepoint(utf8CodepointAt(text, end))) break;
        }
        pos = utf8CharEnd(text, pos);
    }
}

int IME::documentContextBoost(const std::string &word) const {
    int chars = utf8TextCharCount(word);
    if (chars < 2 || chars > DOC_CTX_MAX_N) return 0;
    uint32_t h = docCtxHash(word.data(), word.size());
    int base = (int)(h & (uint32_t)(DOC_CTX_SLOTS - 1));
    for (int probe = 0; probe < IME_DOC_CTX_PROBE; probe++) {
        int s = (base + probe) & (DOC_CTX_SLOTS - 1);
        if (_docCtxCounts[s] == 0) return 0;
        if (_docCtxHashes[s] == h) {
            int hits = _docCtxCounts[s];
            if (hits > IME_DOC_CTX_MAX_HITS) hits = IME_DOC_CTX_MAX_HITS;
            return hits * IME_DOC_CTX_UNIT;
        }
    }
    return 0;
}

void IME::setDocumentContext(const std::string &text) {
    rebuildDocumentContext(text);
}

void IME::rebuildContextBoostScores() {
    if (_contextBoostScoresContext == _lastCommitText) return;
#if PJOURNAL_IME_PERF_LOG
    int64_t perfStartUs = IME_PERF_NOW();
#endif
    _contextBoostScoresContext = _lastCommitText;
    _contextBoostScores.clear();
    if (_lastCommitText.empty()) return;
    ensureUserDictLoaded();
#if PJOURNAL_IME_PERF_LOG
    int64_t perfEnsureEndUs = IME_PERF_NOW();
#endif
    rebuildUserPredictIndex();
#if PJOURNAL_IME_PERF_LOG
    int64_t perfIdxEndUs = IME_PERF_NOW();
#endif
    std::vector<std::string> keys;
    addContextTailKeys(keys, _lastCommitText);
    auto addScore = [&](const std::string &word, int score) {
        if (word.empty()) return;
        auto it = _contextBoostScores.find(word);
        if (it == _contextBoostScores.end() || it->second < score)
            _contextBoostScores[word] = score;
    };
    for (auto &key : keys) {
        int keyChars = utf8TextCharCount(key);
        if (keyChars <= 0) continue;
        auto indexIt = _userPredictIndex.find(key);
        if (indexIt != _userPredictIndex.end()) {
            for (uint16_t entryIdx : indexIt->second) {
                if (entryIdx >= _userPredictWords.size()) continue;
                const UserEntry &p = _userPredictWords[entryIdx];
                if (p.trad != _trad) continue;
                addScore(p.word, 9000 + keyChars * 900 + std::min(p.count, 1000) * 12);
            }
        }
        if (_dict.hasPredictions()) {
            ime::Im3Dictionary::PredictGroup group;
            if (_dict.findPredictGroup(key, group)) {
                for (size_t i = 0; i < group.candidates.size(); i++) {
                    addScore(group.candidates[i], 5200 + keyChars * 650 - (int)i * 80);
                }
            }
        }
        for (auto &entry : BUILTIN_PREDICT) {
            if (key != entry.key) continue;
            for (int i = 0; entry.candidates[i]; i++) {
                addScore(entry.candidates[i], 3600 + keyChars * 550 - i * 60);
            }
            break;
        }
    }
#if PJOURNAL_IME_PERF_LOG
    int64_t perfTotalUs = IME_PERF_NOW() - perfStartUs;
    if (perfTotalUs >= IME_PERF_SLOW_US)
        ESP_LOGW(IME_TAG,
                 "perf ctx-rebuild predict=%u keys=%u scores=%u ensure=%lld idx=%lld loop=%lld total=%lldus ctx='%s'",
                 (unsigned)_userPredictWords.size(), (unsigned)keys.size(),
                 (unsigned)_contextBoostScores.size(),
                 (long long)(perfEnsureEndUs - perfStartUs),
                 (long long)(perfIdxEndUs - perfEnsureEndUs),
                 (long long)(IME_PERF_NOW() - perfIdxEndUs),
                 (long long)perfTotalUs, _lastCommitText.c_str());
#endif
}

int IME::contextCandidateBoost(const std::string &word) {
    if (word.empty()) return 0;
    // 文档级小加分先算: 它在没有上文(_lastCommitText 为空)时也要生效, 所以不能放到
    // 下面那个 early return 之后。取 max 而不是相加, 保证总分不超过原来那条路的量级。
    const int docBoost = documentContextBoost(word);
    if (_lastCommitText.empty()) return docBoost;
    rebuildContextBoostScores();
    auto it = _contextBoostScores.find(word);
    if (it != _contextBoostScores.end())
        return it->second > docBoost ? it->second : docBoost;
    // 逐前缀回退。用字节偏移判断 CJK, 避免给每个字分配临时 std::string——
    // 本函数在候选扫描循环里逐个调用, 短词命中时分配量很可观。
    int best = 0;
    int chars = 0;
    size_t pos = 0;
    while (pos < word.size() && chars < 4) {
        if (!isCjkCodepoint(utf8CodepointAt(word, pos))) break;
        pos = utf8CharEnd(word, pos);
        chars++;
        auto pit = _contextBoostScores.find(word.substr(0, pos));
        if (pit != _contextBoostScores.end())
            best = std::max(best, pit->second - 300);
    }
    return best > docBoost ? best : docBoost;
}

// Compares candidate hashes rather than strings so this stays allocation-free on
// the per-keystroke path. A hash collision would only grant a small boost to an
// unrelated candidate, which cannot meaningfully reorder the list.
int IME::stableCandidateBoost(const std::string &word) const {
    if (word.empty() || _stableBoostCount == 0) return 0;
    uint32_t h = candidateHash(word);
    for (int i = 0; i < _stableBoostCount; i++) {
        if (_stableBoostHashes[i] == h) return _stableBoostValues[i];
    }
    return 0;
}

void IME::rememberCandidateStability() {
    _stableBoostCount = 0;
    size_t n = std::min(_all.size(), (size_t)STABLE_BOOST_SLOTS);
    for (size_t i = 0; i < n; i++) {
        uint32_t h = candidateHash(_all[i]);
        bool seen = false;
        for (int j = 0; j < _stableBoostCount; j++) {
            if (_stableBoostHashes[j] == h) { seen = true; break; }
        }
        if (seen) continue;  // first occurrence keeps the highest boost, as before
        _stableBoostHashes[_stableBoostCount] = h;
        _stableBoostValues[_stableBoostCount] = (int16_t)(420 - (int)i * 12);
        _stableBoostCount++;
    }
}

void IME::logCandidateDebug(const char *stage, const std::string &word,
                            int score, int context, int stable) const {
    if (!_imeDebugLog) return;
    ESP_LOGI(IME_TAG, "rank %s code='%s' word='%s' score=%d context=%d stable=%d recent=%d",
             stage ? stage : "?", _code.c_str(), word.c_str(), score, context, stable,
             recentCommitBoost(_code, word));
}

void IME::appendEnglishInlineCandidates(const std::string &code) {
    if (code.length() < 2 || _all.size() >= _candidateLimit) return;
    for (char c : code) {
        if (c < 'a' || c > 'z') return;
    }
    bool likelyTech = false;
    for (int i = 0; TECH_INLINE_WORDS[i]; i++) {
        if (std::string(TECH_INLINE_WORDS[i]).find(code) == 0) {
            likelyTech = true;
            break;
        }
    }
    if (!likelyTech && !_englishDictLoaded) return;
    if (!likelyTech && code.length() < 4) return;
    loadEnglishDict();
    int added = 0;
    for (int i = 0; TECH_INLINE_WORDS[i] && added < 8 && _all.size() < _candidateLimit; i++) {
        std::string w = TECH_INLINE_WORDS[i];
        if (w.find(code) == 0 && appendCandidate(w, (int)code.length())) added++;
    }
    auto it = std::lower_bound(_englishWords.begin(), _englishWords.end(), code);
    for (; it != _englishWords.end() && added < 6 && _all.size() < _candidateLimit; ++it) {
        if (it->find(code) != 0) break;
        if (appendCandidate(*it, (int)code.length())) added++;
    }
}

// 无前缀快捷符号表(#14), 由 lookup 的 Phase 0 按精确整码匹配取用。表里只放没有中文
// 读法的短码, 所以"整码命中"不会劫持任何有拼音解释的码。
struct ShortcutSymbol { const char *code; const char *text; };
static const ShortcutSymbol K_SHORTCUT_SYMBOLS[] = {
    {"rmb", "￥"}, {"cny", "￥"}, {"usd", "$"},   {"eur", "€"},
    {"gbp", "£"}, {"jpy", "¥"},  {"hkd", "HK$"}, {"twd", "NT$"},
    {"krw", "₩"}, {"sgd", "S$"}, {"aud", "A$"},  {"cad", "C$"},
    {"chf", "CHF"}, {"inr", "₹"}, {"rub", "₽"}, {"thb", "฿"},
    {"btc", "₿"}, {"copy", "©"}, {"sect", "§"}, {"para", "¶"},
    {nullptr, nullptr},
};

void IME::appendShortcutSymbol(const char *code, int len) {
    for (int i = 0; K_SHORTCUT_SYMBOLS[i].code; i++) {
        if ((int)strlen(K_SHORTCUT_SYMBOLS[i].code) != len) continue;
        if (strncmp(K_SHORTCUT_SYMBOLS[i].code, code, len) != 0) continue;
        appendCandidate(K_SHORTCUT_SYMBOLS[i].text, len);
        return;
    }
}

void IME::handleHostBackspace() {
    if (_lastLearningWord.empty() || _lastLearningUs == 0) return;
    int64_t now = esp_timer_get_time();
    if (now - _lastLearningUs > 4000000) {
        _lastLearningCode.clear();
        _lastLearningWord.clear();
        _lastLearningContext.clear();
        _lastLearningUs = 0;
        return;
    }
    rememberDeletedWord(_lastLearningWord);
    penalizeUserWord(_lastLearningCode, _lastLearningWord, 3);
    penalizePredictWord(_lastLearningWord, 3);
    _lastRejectedCode = _lastLearningCode;
    _lastRejectedWord = _lastLearningWord;
    _lastRejectedContext = _lastLearningContext;
    _lastRejectedUs = now;
    _lastCommitText = _lastLearningContext;
    _lastCommitChar = lastUtf8Char(_lastCommitText);
    _lastLearningCode.clear();
    _lastLearningWord.clear();
    _lastLearningContext.clear();
    _lastLearningUs = 0;
}

void IME::rememberCommittedText(const std::string &text) {
    if (text.empty()) return;
    std::string first = utf8CharAt(text, 0);
    if (!isCjkChar(first)) {
        _lastCommitChar.clear();
        _lastCommitText.clear();
        _lastAsciiCommitUs = esp_timer_get_time();
        return;
    }
    _lastAsciiCommitUs = 0;
    std::string prevKey = !_lastCommitText.empty() ? _lastCommitText : _lastCommitChar;
    if (!prevKey.empty() && first.size() >= 3) {
        if (!noisyPredictPair(prevKey, first))
            bumpPredictFrequency(prevKey, first, false);
        std::string second = utf8CharAt(text, utf8CharEnd(text, 0));
        if (isCjkChar(second) && !noisyPredictPair(prevKey, first + second))
            bumpPredictFrequency(prevKey, first + second, false);
    }
    learnPredictPairs(text);
    if (_userPredictDirty) flushUserDictSaves(false);
    std::string last = lastUtf8Char(text);
    if (isCjkChar(last)) _lastCommitChar = last;
    else _lastCommitChar.clear();
    _lastCommitText = cjkTailText(text, 4);
}

bool IME::readCode(uint32_t i, char out[MAX_CODE_LEN + 1]) {
    return _dict.readSingleCode(i, out);
}

bool IME::readHanzi(uint32_t i, char out[HANZI_SIZE + 1]) {
    return _dict.readSingleText(i, out);
}

uint8_t IME::readRecordFlag(uint32_t i) {
    return _dict.readSingleFlag(i);
}

bool IME::hasCandidate(const std::string &text) const {
    uint32_t h = candidateHash(text);
    if (_candidateHashCount == _all.size()) {
        for (size_t i = 0; i < _candidateHashCount; i++) {
            if (_candidateHashes[i] == h && _all[i] == text) return true;
        }
        return false;
    }
    for (auto &e : _all) {
        if (e == text) return true;
    }
    return false;
}

void IME::clearCandidates() {
    _all.clear();
    _candidateHashCount = 0;
    _candLen.clear();
    _candidateWidths.clear();
    _predictCandidateKeys.clear();
    if (_all.capacity() > MAX_CANDIDATES * 2) _all.shrink_to_fit();
    else if (_all.capacity() < MAX_CANDIDATES / 3) _all.reserve(MAX_CANDIDATES / 3);
}

void IME::rebuildCandidateHashes() {
    if (_candidateHashCount == _all.size()) return;
    _candidateHashCount = 0;
    size_t limit = std::min(_all.size(), (size_t)MAX_CANDIDATES);
    for (size_t i = 0; i < limit; i++)
        _candidateHashes[_candidateHashCount++] = candidateHash(_all[i]);
}

bool IME::appendCandidate(const std::string &text, int candLen) {
    if (_all.size() >= _candidateLimit) return false;
    rebuildCandidateHashes();
    if (hasCandidate(text)) return false;
    _all.push_back(text);
    if (_candidateHashCount < MAX_CANDIDATES)
        _candidateHashes[_candidateHashCount++] = candidateHash(text);
    else
        _candidateHashCount = 0;
    _candLen.push_back(candLen);
    _candidateWidths.push_back(-1);
    _predictCandidateKeys.push_back("");
    return true;
}

#if PJOURNAL_IME_ENABLE_LIANGFEN
uint8_t IME::readLfFlag(uint16_t i) {
    const uint8_t *rec = _lfBlob + _lfRecordBase + (size_t)i * 16;
    return rec[15];
}
#endif

void IME::setActive(bool on) {
    if (!on) flushUserDictSaves(true);
    _active = on;
    if (on) ensureUserDictLoaded();
    reset();
}

std::string IME::modeLabel() const {
    if (_deleteMode) return "[删]";
    if (_lfMode) return "[两]";
    if (_predicting) return "[联]";
    return "";
}

void IME::clearLearningContext() {
    _recentSingleCommits.clear();
    _lastCommitChar.clear();
    _lastCommitText.clear();
}

void IME::reset() {
    _code.clear();
    _predicting = false;
    _predChar.clear();
    _displayCodeDirty = true;
    clearCandidates();
    _page.clear();
    if (_page.capacity() > _pageSize * 2) _page.shrink_to_fit();
    else if (_page.capacity() < _pageSize) _page.reserve(_pageSize);
    _pageStart = 0;
    _curPage = 0;
    _pageAnchor = -1;
    _prefix.clear();
    _remainder.clear();
    _lfMode = false;
    switch (_scheme) {
    case PINYIN:    _maxCode = 63; break;
    case SHUANGPIN: _maxCode = 2; break;
    case WUBI:
    default:        _maxCode = 4; break;
    }
    _deleteMode = false;
    _vMode = false;
    _vSel = 0;
    _fixedCandidatePaging = false;
    _candidateLimit = MAX_CANDIDATES;
    _englishCompose = false;
}

void IME::setDeleteMode(bool on) {
    ensureUserDictLoaded();
    _english = false;
    reset();
    _deleteMode = on;
    if (on) lookup();
}

void IME::toggleDeleteMode() {
    setDeleteMode(!_deleteMode);
}

int IME::pinyinPrefixLen(const std::string &code) {
    int i = 0;
    while (i < (int)code.length() && code[i] >= 'a' && code[i] <= 'z') i++;
    return i;
}

void IME::searchWindow(const char *code, int len, uint32_t &lo, uint32_t &hi) {
    if (code && len > 0 && _singleWindowCacheLen == len &&
        _singleWindowCacheCode.size() == (size_t)len &&
        strncmp(_singleWindowCacheCode.c_str(), code, len) == 0) {
        lo = _singleWindowCacheLo;
        hi = _singleWindowCacheHi;
        return;
    }
    _dict.singleWindow(code, len, lo, hi);
    _singleWindowCacheCode.assign(code ? code : "", code && len > 0 ? len : 0);
    _singleWindowCacheLen = len;
    _singleWindowCacheLo = lo;
    _singleWindowCacheHi = hi;
}

void IME::wordWindowCached(const char *code, int len, size_t &lo, size_t &hi) {
    if (code && len > 0 && _wordWindowCacheLen == len &&
        _wordWindowCacheCode.size() == (size_t)len &&
        strncmp(_wordWindowCacheCode.c_str(), code, len) == 0) {
        lo = _wordWindowCacheLo;
        hi = _wordWindowCacheHi;
        return;
    }
    _dict.wordWindow(code, len, lo, hi);
    _wordWindowCacheCode.assign(code ? code : "", code && len > 0 ? len : 0);
    _wordWindowCacheLen = len;
    _wordWindowCacheLo = lo;
    _wordWindowCacheHi = hi;
}

void IME::lookup() {
    ImePerfTrace perf(_code, _all);
    rememberCandidateStability();
    clearCandidates();
    _pageStart = 0;
    _curPage = 0;
    _pageAnchor = -1;
    _maxMatchLen = 0;
    if (_prefix.length() == 0) _codeOrig = _code;
    static bool dictLoaded = false;
    if (!dictLoaded) {
        dictLoaded = true;
        ensureUserDictLoaded();
    }

    if (!_loaded || (_code.length() == 0 && !_deleteMode)) {
        perf.exitName = "empty";
        buildPage();
        return;
    }

    // 单引号编码分词: 显式按音节分段匹配(词组/补充表/用户词典)
    if (_code.find('\'') != std::string::npos) {
        int64_t t = IME_PERF_NOW();
        lookupSegmented();
        perf.segUs += IME_PERF_NOW() - t;
        perf.exitName = "segmented";
        buildPage();
        return;
    }

    int64_t setupStartUs = IME_PERF_NOW();
    const char *q = _code.c_str();
    int qlen = (int)_code.length();

    int pinyinLen = pinyinPrefixLen(_code);
    std::string pinyinCode = _code.substr(0, pinyinLen);

    // First char uppercase: treat as literal, use partial match for remainder
    if (pinyinLen == 0 && _code.length() > 0) {
        _all.push_back(_code.substr(0, 1));
        _candLen.push_back(0);
        _candidateWidths.push_back(-1);
        _predictCandidateKeys.push_back("");
        _partialStart = 0;
        _remainder = _code.substr(1);
        perf.setupUs += IME_PERF_NOW() - setupStartUs;
        perf.exitName = "literal";
        buildPage();
        return;
    }

    q = pinyinCode.c_str();
    qlen = pinyinLen;
    static std::string cachedMetaCode;
    static std::string cachedMetaFuzzy;
    static std::vector<std::string> cachedAliasCodes;
    static ime::PinyinSplit cachedPrimarySplit;
    int64_t nowUs = esp_timer_get_time();
    if (_fuzzyConfigCacheUs == 0 || nowUs - _fuzzyConfigCacheUs > 2000000) {
        _fuzzyConfigCache = g_settings.imeFuzzy();
        _fuzzyConfigCacheUs = nowUs;
    }
    const std::string &fuzzyCfg = _fuzzyConfigCache;
    int64_t metaStartUs = IME_PERF_NOW();
    if (cachedMetaCode != pinyinCode || cachedMetaFuzzy != fuzzyCfg) {
        cachedMetaCode = pinyinCode;
        cachedMetaFuzzy = fuzzyCfg;
        cachedAliasCodes = alternateInputCodes(pinyinCode, fuzzyCfg);
        cachedPrimarySplit = ime::PinyinEngine::primarySplit(pinyinCode, true);
    }
    const std::vector<std::string> &aliasCodes = cachedAliasCodes;
    perf.metaUs += IME_PERF_NOW() - metaStartUs;
    // Sample the dirty flag before the call: it is rebuildUserWordIndexes()'s own
    // entry condition, so it records whether this keystroke actually paid a rebuild.
    perf.rebuilt = _userWordIndexesDirty;
    int64_t rebuildStartUs = IME_PERF_NOW();
    rebuildUserWordIndexes();
    perf.rebuildUs += IME_PERF_NOW() - rebuildStartUs;
    bool hasVowel = false;
    for (int i = 0; i < qlen; i++) {
        if (strchr("aeiouv", q[i])) { hasVowel = true; break; }
    }
    const ime::PinyinSplit &primarySplit = cachedPrimarySplit;
    bool incompletePinyinInput = false;
    if (!primarySplit.tokens.empty()) {
        incompletePinyinInput = primarySplit.tokens.back().partial;
    }
    _fixedCandidatePaging = (!hasVowel && qlen <= 2) || incompletePinyinInput;
    if (incompletePinyinInput) {
        _candidateLimit = IME_PARTIAL_PINYIN_CANDIDATE_LIMIT;
    } else if (_fixedCandidatePaging) {
        _candidateLimit = IME_SHORT_CONSONANT_CANDIDATE_LIMIT;
    } else {
        _candidateLimit = IME_FAST_CANDIDATE_LIMIT;
    }
    perf.setupUs += IME_PERF_NOW() - setupStartUs;
    perf.hasVowel = hasVowel;
    perf.incomplete = incompletePinyinInput;
    perf.fixedPaging = _fixedCandidatePaging;
    perf.limit = _candidateLimit;

#if PJOURNAL_IME_ENABLE_LIANGFEN
    if (_lfMode && _lfBlob) {
        // 'u' 是两分输入的前缀, 被 handleKey 吞掉而不进 _code, 所以快捷表里唯一以 u
        // 开头的 usd 拿不到精确整码匹配。这里把那个 'u' 补回来再查一次。
        if (qlen >= 2 && qlen <= 3) {
            std::string fullCode = "u";
            fullCode.append(q, qlen);
            appendShortcutSymbol(fullCode.c_str(), (int)fullCode.length());
        }
        uint32_t llo, lhi;
        searchLfWindow(q, qlen, llo, lhi);
        while (llo < lhi) {
            uint32_t mid = llo + (lhi - llo) / 2;
            char code[13]; if (!readLfCode(mid, code)) break;
            if (strncmp(code, q, qlen) < 0) llo = mid + 1;
            else lhi = mid;
        }
        for (uint32_t i = llo; i < _lfCount && _all.size() < IME_FAST_CANDIDATE_LIMIT; i++) {
            char code[13]; if (!readLfCode(i, code)) break;
            if (strncmp(code, q, qlen) != 0) break;
            uint8_t f = readLfFlag(i);
            if (f & (_trad ? 0x01 : 0x02)) continue;
            char hz[4]; if (!readLfHanzi(i, hz)) break;
            appendCandidate(std::string(hz), 0);
        }
        buildPage();
        return;
    }
#endif

    if (_deleteMode) {
        int64_t t = IME_PERF_NOW();
        RankedList userMatches;
        auto codeMatchesDelete = [&](const std::string &entryCode) -> bool {
            if (qlen == 0 || userCodeMatchesPrefix(entryCode, q, qlen)) return true;
            for (auto &aliasCode : aliasCodes) {
                if (!userLookupAliasPreciseEnough(aliasCode, qlen)) continue;
                if (userCodeMatchesPrefix(entryCode, aliasCode.c_str(), (int)aliasCode.length())) return true;
            }
            return false;
        };
        std::vector<uint16_t> deleteIndices;
        if (qlen > 0) {
            addUserLookupIndexMatches(deleteIndices, _dynamicUserCodeIndex,
                                      _dynamicUserCodePrefixIndex, q, qlen);
            for (auto &aliasCode : aliasCodes)
            {
                if (!userLookupAliasPreciseEnough(aliasCode, qlen)) continue;
                addUserLookupIndexMatches(deleteIndices, _dynamicUserCodeIndex,
                                          _dynamicUserCodePrefixIndex,
                                          aliasCode.c_str(), (int)aliasCode.length());
            }
        }
        auto scanDeleteEntry = [&](const UserEntry &p) {
            if (p.trad != _trad) return;
            if (codeMatchesDelete(p.code)) userMatches.add(p.word, 0, p.count, 0);
        };
        if (qlen == 0) {
            for (auto &p : _dynamicUserWords) scanDeleteEntry(p);
        } else {
            for (uint16_t entryIdx : deleteIndices) {
                if (entryIdx >= _dynamicUserWords.size()) continue;
                scanDeleteEntry(_dynamicUserWords[entryIdx]);
            }
        }
        userMatches.sort();
        for (auto &m : userMatches.items) {
            appendCandidate(m.word, 0);
            if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
        }
        perf.userUs += IME_PERF_NOW() - t;
        perf.exitName = "delete";
        buildPage();
        return;
    }

    // Phase 0: 无前缀快捷符号(#14), 精确整码匹配。表里全是没有拼音读法的短码, 所以
    // "整码命中"本身就安全——不会被任何有拼音解释的码触发。必须排在各相位之前: 放在
    // 最后追加时 Phase 8 的逐字匹配已经用首字母的单字占满首屏, 用户看不到符号。
    if (qlen >= 3 && qlen <= 4) appendShortcutSymbol(q, qlen);

    // Phase 0b: 整句覆盖。长全拼串前面的相位只会给出首音节单字, 这里把整句作为首选
    // 候选放在最前, 后面的相位照常追加。
    if (_sentenceMode && _scheme == PINYIN && hasVowel && !incompletePinyinInput &&
        _dict.hasWords() && qlen >= IME_SENTENCE_MIN_LEN && qlen <= IME_SENTENCE_MAX_LEN &&
        (int)primarySplit.tokens.size() >= IME_SENTENCE_MIN_TOKENS) {
        int64_t t = IME_PERF_NOW();
#if PJOURNAL_IME_PERF_LOG
        size_t before = _all.size();
#endif
        appendSentenceCandidates(q, qlen);
        perf.sentenceUs += IME_PERF_NOW() - t;
#if PJOURNAL_IME_PERF_LOG
        // 整句相位单独用低阈值探针: 三五毫秒不会触发 12ms 的总日志, 但那正是
        // 需要盯着看的数字。
        if (perf.sentenceUs >= IME_SENTENCE_PERF_LOG_US) {
            ESP_LOGW(IME_TAG,
                     "perf sentence code='%s' total=%lldus arcs=%u nodes=%u groups=%d added=%u",
                     _code.c_str(), (long long)perf.sentenceUs,
                     (unsigned)_sentenceArcs.size(), (unsigned)_sentenceNodes.size(),
                     _sentenceGroupsScanned, (unsigned)(_all.size() - before));
        }
#endif
    }

    // Phase 1: user dict single chars — highest priority (phrases emitted in Phase 3)
    RankedList userWordFreq;
    RankedList userPrefixWordFreq;
    RankedList userExactWordFreq;
    {
        int64_t t = IME_PERF_NOW();
        RankedList userSingleFreq;
        bool collectUserPhrases = hasVowel && !incompletePinyinInput && qlen >= 3;
        bool skipBroadConsonantUserScan = _fixedCandidatePaging && !hasVowel && qlen <= 2;
        bool skipSingleSyllableUserScan = hasVowel && primarySplit.tokens.size() <= 1 && qlen >= 3;
        auto scanUserWords = [&](const std::vector<UserEntry> &entries,
                                 const std::unordered_map<int, std::vector<uint16_t>> &index,
                                 const std::unordered_map<std::string, std::vector<uint16_t>> &prefixIndex,
                                 int sourceRank) {
        std::vector<uint16_t> indices;
        int scanLimit = _fixedCandidatePaging ? IME_MAX_USER_FIXED_SCAN : IME_MAX_USER_PREFIX_SCAN;
        addUserLookupIndexMatches(indices, index, prefixIndex, q, qlen, scanLimit);
        for (auto &aliasCode : aliasCodes)
        {
            if ((int)indices.size() >= scanLimit) break;
            if (!userLookupAliasPreciseEnough(aliasCode, qlen)) continue;
            addUserLookupIndexMatches(indices, index, prefixIndex,
                                      aliasCode.c_str(), (int)aliasCode.length(),
                                      scanLimit);
        }
        int scanned = 0;
        for (uint16_t entryIdx : indices) {
            if (scanned++ >= scanLimit) break;
            if (entryIdx >= entries.size()) continue;
            auto &p = entries[entryIdx];
            if (p.trad != _trad) continue;
            bool singleWord = p.word.length() <= 3;
            if (!singleWord && !collectUserPhrases) continue;
            std::string matchedCode;
            bool matched = userCodeMatchesPrefix(p.code, q, qlen, &matchedCode);
            for (auto &aliasCode : aliasCodes) {
                if (matched) break;
                if (!userLookupAliasPreciseEnough(aliasCode, qlen)) continue;
                matched = userCodeMatchesPrefix(p.code, aliasCode.c_str(),
                                                (int)aliasCode.length(), &matchedCode);
            }
            if (!matched) continue;
            int score = userCandidateScore(matchedCode, p.count, qlen);
            score += recentCommitBoost(matchedCode, p.word);
            // Single chars get the context boost too: "type a word, then its first
            // letter" is where a learned collocation should re-rank the first page,
            // and the dictionary phases for the same code (Phase 4c) already do.
            int ctxBoost = contextCandidateBoost(p.word);
            int stableBoost = stableCandidateBoost(p.word);
            score += ctxBoost + stableBoost;
            logCandidateDebug("user", p.word, score, ctxBoost, stableBoost);
            bool exactCodeMatch = pinyinCodeEqualsAny(matchedCode, pinyinCode, aliasCodes);
            if (singleWord) {
                // single char
                userSingleFreq.add(p.word, 0, score, sourceRank);
            } else {
                // phrase
                auto &target = exactCodeMatch ? userWordFreq : userPrefixWordFreq;
                target.add(p.word, 0, score, sourceRank);
                if ((int)target.size() > IME_MAX_USER_PHRASE_KEEP) {
                    target.sort();
                    target.truncate(IME_MAX_USER_PHRASE_KEEP);
                }
                if (exactCodeMatch) {
                    userExactWordFreq.add(p.word, qlen, score, sourceRank);
                    if ((int)userExactWordFreq.size() > IME_MAX_USER_PHRASE_KEEP) {
                        userExactWordFreq.sort();
                        userExactWordFreq.truncate(IME_MAX_USER_PHRASE_KEEP);
                    }
                }
            }
        }
        };
        if (!skipBroadConsonantUserScan && !skipSingleSyllableUserScan) {
            int64_t scanT = IME_PERF_NOW();
            scanUserWords(_fixedUserWords, _fixedUserCodeIndex, _fixedUserCodePrefixIndex, 30);
            perf.userScanFixedUs += IME_PERF_NOW() - scanT;
            scanT = IME_PERF_NOW();
            scanUserWords(_dynamicUserWords, _dynamicUserCodeIndex, _dynamicUserCodePrefixIndex, 20);
            perf.userScanDynUs += IME_PERF_NOW() - scanT;
            userSingleFreq.sort();
            for (auto &f : userSingleFreq.items) {
                appendCandidate(f.word, f.candLen);
                if (_all.size() >= _candidateLimit) break;
            }
        }
        perf.userUs += IME_PERF_NOW() - t;
        if (_all.size() >= _candidateLimit) { perf.exitName = "user-single-limit"; buildPage(); return; }
    }

    // Frequently selected full-code phrases should not be buried behind many single chars.
    if (hasVowel && !incompletePinyinInput && primarySplit.tokens.size() > 1 &&
        !userExactWordFreq.empty()) {
        int64_t t = IME_PERF_NOW();
        userExactWordFreq.sort();
        int promoted = 0;
        for (auto &f : userExactWordFreq.items) {
            if (appendCandidate(f.word, f.candLen) && ++promoted >= 4) break;
            if (_all.size() >= _candidateLimit) break;
        }
        perf.userPhraseUs += IME_PERF_NOW() - t;
        if (_all.size() >= _candidateLimit) { perf.exitName = "user-exact-phrase-limit"; buildPage(); return; }
    }

    // Long full-pinyin input benefits from exact phrase promotion before the
    // broad single-character scan. This keeps "shurufa" and longer diary-style
    // phrases from being buried behind many one-character candidates.
    if (hasVowel && !incompletePinyinInput && primarySplit.tokens.size() > 1 &&
        qlen >= 4 && _dict.hasWords() &&
        _all.size() < _candidateLimit) {
        int64_t t = IME_PERF_NOW();
        RankedList exactPhraseMatches;
        const uint8_t *wordData = _dict.wordData();
        auto collectExactPhrase = [&](const char *scanCode, int scanLen, bool aliasScan) {
            if (!scanCode || scanLen < 4) return;
            size_t wlo = 0, whi = _dict.wordDataSize();
            wordWindowCached(scanCode, scanLen, wlo, whi);
            size_t wpos = wlo;
            int safety = 0;
            while (wpos < whi && safety++ < IME_MAX_PHRASE_GROUP_SCAN) {
                uint8_t cl = wordData[wpos];
                if (cl == 0 || wpos + 1 + cl > whi) break;
                const char *wc = (const char *)wordData + wpos + 1;
                size_t next = wpos + 1 + cl;
                if (next >= whi) break;
                uint8_t n = wordData[next++];
                int cmpLen = std::min((int)cl, scanLen);
                int cmp = strncmp(wc, scanCode, cmpLen);
                if (cmp > 0) break;
                bool groupMatch = ((int)cl == scanLen &&
                                   strncmp(wc, scanCode, scanLen) == 0);
                for (uint8_t j = 0; j < n && next < whi; j++) {
                    uint8_t wl = wordData[next++];
                    if (wl == 0 || next + wl + 1 > whi) {
                        next = whi;
                        break;
                    }
                    uint8_t wf = wordData[next + wl];
                    if (groupMatch) {
                        std::string w((const char *)wordData + next, wl);
                        if (wordVisible(_trad, w, wf)) {
                            int consumedLen = aliasScan ? qlen : scanLen;
                            int ctxBoost = contextCandidateBoost(w);
                            int stableBoost = stableCandidateBoost(w);
                            int score = phraseCandidateScore(w, scanLen, scanLen,
                                                             (int)primarySplit.tokens.size()) + 2000
                                      + recentCommitBoost(std::string(scanCode, scanLen), w)
                                      + ctxBoost + stableBoost;
                            logCandidateDebug("exact-phrase", w, score, ctxBoost, stableBoost);
                            exactPhraseMatches.add(w, consumedLen, score, 15);
                        }
                    }
                    next += wl + 1;
                }
                wpos = next;
            }
        };
        collectExactPhrase(q, qlen, false);
        for (auto &aliasCode : aliasCodes)
            collectExactPhrase(aliasCode.c_str(), (int)aliasCode.length(), true);
        exactPhraseMatches.sort();
        int promoted = 0;
        for (auto &m : exactPhraseMatches.items) {
            if (appendCandidate(m.word, m.candLen)) promoted++;
            if (promoted >= 6 || _all.size() >= _candidateLimit) break;
        }
        perf.userPhraseUs += IME_PERF_NOW() - t;
        if (_all.size() >= _candidateLimit) { perf.exitName = "exact-phrase-limit"; buildPage(); return; }
    }

    {
        int64_t t = IME_PERF_NOW();
        appendRecentCommitCandidates(pinyinCode, aliasCodes, qlen);
        perf.recentUs += IME_PERF_NOW() - t;
    }
    if (_all.size() >= _candidateLimit) { perf.exitName = "recent-limit"; buildPage(); return; }

    // Phase 2: single char prefix match (dictionary)
    {
    int64_t t = IME_PERF_NOW();
    if (hasVowel) {
        appendScoredSingleChars(q, qlen, IME_MAX_SINGLE_RECORD_SCAN,
                                /*codeLenFromRecord=*/true, 0, _candidateLimit);
    }
    if (!hasVowel && _all.size() < _candidateLimit) {
        std::string fallback = ime::PinyinEngine::singleKeyFallbackSyllable(pinyinCode);
        if (!fallback.empty()) {
            // zh/ch/sh 的单字展开会占满整个短码预算, 先把槽位留给简码阶段。
            size_t cap = _candidateLimit;
            if (qlen >= 2 && cap > IME_SHORTCUT_RESERVE) cap -= IME_SHORTCUT_RESERVE;
            appendSingleCharCandidates(fallback, qlen, cap);
        }
    }
    if (hasVowel && _all.size() < _candidateLimit) {
        std::string alias = zeroInitialAlias(pinyinCode);
        if (!alias.empty()) appendSingleCharCandidates(alias, qlen);
        for (auto &aliasCode : aliasCodes) {
            if (_all.size() >= _candidateLimit) break;
            appendSingleCharCandidates(aliasCode, qlen);
        }
    }
    perf.singleUs += IME_PERF_NOW() - t;
    }
    if (_all.size() >= _candidateLimit) { perf.exitName = "single-limit"; buildPage(); return; }

    // 补充词典表分段匹配: xian/xi'an/xi'a -> 西安, anguang -> 暗光。
    // 拼音段解析由独立引擎负责, 这里仍只关心候选生成和去重。
    if (!incompletePinyinInput && hasVowel && qlen >= IME_SEG_TABLE_MIN_LEN && _all.size() < IME_FAST_CANDIDATE_LIMIT) {
        int64_t t = IME_PERF_NOW();
        std::vector<ime::PinyinSplit> splits = ime::PinyinEngine::splitVariants(pinyinCode, true, 6);
        std::vector<ime::PinyinSplit> aliasSplits;
        for (auto &aliasCode : aliasCodes) {
            std::vector<ime::PinyinSplit> moreSplits = ime::PinyinEngine::splitVariants(aliasCode, true, 4);
            aliasSplits.insert(aliasSplits.end(), moreSplits.begin(), moreSplits.end());
        }
        perf.segSplitUs += IME_PERF_NOW() - t;
        int64_t matchStartUs = IME_PERF_NOW();
        std::vector<uint16_t> segIndices;
        addSegPrefixCandidates(segIndices, SEG_CODE_ORDER, SEG_CODE_INDEX, q, qlen);
        for (auto &aliasCode : aliasCodes) {
            addSegPrefixCandidates(segIndices, SEG_CODE_ORDER, SEG_CODE_INDEX,
                                   aliasCode.c_str(), (int)aliasCode.length());
        }
        RankedList segMatches;
        for (uint16_t segIdx : segIndices) {
            int i = segIdx;
            int matchedLen = 0;
            auto matchSplits = [&](const std::vector<ime::PinyinSplit> &variants) -> int {
            for (auto &split : variants) {
                int typedCodeLen = pinyinJoinedLength(split.tokens);
                int entryCodeLen = (int)strlen(SEG_TABLE[i].code);
                if (typedCodeLen > entryCodeLen) continue;
                if (!pinyinTokensMatchCodePrefix(split.tokens, SEG_TABLE[i].code, entryCodeLen)) continue;
                if (pinyinSegmentsMatchText(split.tokens, SEG_TABLE[i].syllables))
                    return typedCodeLen;
            }
            return 0;
            };
            matchedLen = matchSplits(splits);
            if (matchedLen == 0 && !aliasSplits.empty())
                matchedLen = matchSplits(aliasSplits);
            if (matchedLen == 0) continue;
            std::string w = SEG_TABLE[i].word;
            int chars = utf8TextCharCount(w);
            int entryCodeLen = (int)strlen(SEG_TABLE[i].code);
            if (!phraseMatureForTyped(entryCodeLen, matchedLen, chars)) continue;
            int score = phraseCandidateScore(w, entryCodeLen, matchedLen, SEG_TABLE[i].syllableCount);
            score += recentCommitBoost(SEG_TABLE[i].code, w);
            int ctxBoost = contextCandidateBoost(w);
            int stableBoost = stableCandidateBoost(w);
            score += ctxBoost + stableBoost;
            logCandidateDebug("seg", w, score, ctxBoost, stableBoost);
            segMatches.add(w, entryCodeLen, score, 10);
        }
        perf.segMatchUs += IME_PERF_NOW() - matchStartUs;
        int64_t sortStartUs = IME_PERF_NOW();
        segMatches.sort();
        for (auto &m : segMatches.items) {
            if (appendCandidate(m.word, m.candLen) && m.candLen > _maxMatchLen)
                _maxMatchLen = m.candLen;
            if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
        }
        perf.segSortUs += IME_PERF_NOW() - sortStartUs;
        perf.segUs += IME_PERF_NOW() - t;
    }
    if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) { perf.exitName = "seg-limit"; buildPage(); return; }

    // Phase 3: user dict phrases — after dictionary single chars, before dictionary phrases
    {
        int64_t t = IME_PERF_NOW();
        userWordFreq.sort();
        for (auto &f : userWordFreq.items) {
            appendCandidate(f.word, f.candLen);
            if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
        }
        perf.userPhraseUs += IME_PERF_NOW() - t;
        if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) { perf.exitName = "user-phrase-limit"; buildPage(); return; }
    }
    size_t p4Start = _all.size();  // 词典词组排序起点(不含用户词组)

    // Phase 4: phrase prefix match (word dictionary)
    if (!incompletePinyinInput && hasVowel && qlen >= IME_PHRASE_PREFIX_MIN_LEN && _dict.hasWords()) {
        int64_t t = IME_PERF_NOW();
        const uint8_t *wordData = _dict.wordData();
        auto scanPhrasePrefix = [&](const char *scanCode, int scanLen, bool aliasScan) {
            if (!scanCode || scanLen < IME_PHRASE_PREFIX_MIN_LEN) return;
            size_t wlo = 0, whi = _dict.wordDataSize();
            wordWindowCached(scanCode, scanLen, wlo, whi);
            size_t wpos = wlo;
            int safety = 0;
            int scanBudget = IME_MAX_PHRASE_GROUP_SCAN;
            if (scanLen >= 10) scanBudget = 20000;
            else if (scanLen >= 8) scanBudget = 10000;
            else if (scanLen >= 6) scanBudget = 3000;
            while (wpos < whi && _all.size() < IME_FAST_CANDIDATE_LIMIT && safety++ < scanBudget) {
                uint8_t cl = wordData[wpos];
                if (cl == 0 || wpos + 1 + cl > whi) break;
                const char *wc = (const char *)wordData + wpos + 1;
                size_t next = wpos + 1 + cl;
                if (next >= whi) break;
                uint8_t n = wordData[next++];
                int cmpLen = std::min((int)cl, scanLen);
                int cmp = strncmp(wc, scanCode, cmpLen);
                if (cmp > 0) break;
                bool groupMatch = (cmp == 0 && (int)cl >= scanLen &&
                                   strncmp(wc, scanCode, scanLen) == 0);
                for (uint8_t j = 0; j < n && next < whi; j++) {
                    uint8_t wl = wordData[next++];
                    if (wl == 0 || next + wl + 1 > whi) {
                        next = whi;
                        break;
                    }
                    uint8_t wf = wordData[next + wl];
                    if (groupMatch) {
                        std::string w((const char *)wordData + next, wl);
                        int chars = utf8TextCharCount(w);
                        if (!phraseMatureForTyped((int)cl, scanLen, chars)) {
                            next += wl + 1;
                            continue;
                        }
                        int consumedLen = aliasScan ? qlen : (int)cl;
                        if (wordVisible(_trad, w, wf) && appendCandidate(w, consumedLen)) {
                            if (cl > _maxMatchLen) _maxMatchLen = cl;
                        }
                    }
                    if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) {
                        next += wl + 1;
                        break;
                    }
                    next += wl + 1;
                }
                wpos = next;
            }
        };
        scanPhrasePrefix(q, qlen, false);
        for (auto &aliasCode : aliasCodes) {
            if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
            scanPhrasePrefix(aliasCode.c_str(), (int)aliasCode.length(), true);
        }
        perf.phraseUs += IME_PERF_NOW() - t;
    }
    // Sort Phase 4 entries by exactness, consumed length, and syllable-count closeness.
    {
        int64_t t = IME_PERF_NOW();
        size_t p4End = _all.size();
        size_t p4Count = p4End - p4Start;
        if (p4Count > 1) {
            std::vector<int> order(p4Count);
            for (size_t i = 0; i < order.size(); i++) order[i] = (int)(p4Start + i);
            std::stable_sort(order.begin(), order.end(),
                [this, qlen, &primarySplit](int a, int b) {
                    int syllables = (int)primarySplit.tokens.size();
                    int scoreA = phraseCandidateScore(_all[a], _candLen[a], qlen, syllables)
                               + contextCandidateBoost(_all[a])
                               + stableCandidateBoost(_all[a]);
                    int scoreB = phraseCandidateScore(_all[b], _candLen[b], qlen, syllables)
                               + contextCandidateBoost(_all[b])
                               + stableCandidateBoost(_all[b]);
                    return scoreA > scoreB;
                });
            std::vector<std::string> sortedAll(_all.begin(), _all.begin() + p4Start);
            std::vector<int> sortedLen(_candLen.begin(), _candLen.begin() + p4Start);
            std::vector<int> sortedWidths(_candidateWidths.begin(), _candidateWidths.begin() + p4Start);
            std::vector<std::string> sortedKeys(_predictCandidateKeys.begin(), _predictCandidateKeys.begin() + p4Start);
            sortedAll.reserve(_all.size());
            sortedLen.reserve(_candLen.size());
            sortedWidths.reserve(_candidateWidths.size());
            sortedKeys.reserve(_predictCandidateKeys.size());
            for (int i : order) {
                sortedAll.push_back(std::move(_all[i]));
                sortedLen.push_back(_candLen[i]);
                sortedWidths.push_back(i < (int)_candidateWidths.size() ? _candidateWidths[i] : -1);
                sortedKeys.push_back(i < (int)_predictCandidateKeys.size() ? std::move(_predictCandidateKeys[i]) : "");
            }
            _all.swap(sortedAll);
            _candLen.swap(sortedLen);
            _candidateWidths.swap(sortedWidths);
            _predictCandidateKeys.swap(sortedKeys);
        }
        perf.phraseSortUs += IME_PERF_NOW() - t;
    }
    if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) { perf.exitName = "phrase-limit"; buildPage(); return; }

    // Phase 4b: longer user phrases that only prefix-match the current input.
    // Exact user phrases stay early; prefix-only phrases must not bury dictionary exact matches.
    if (!userPrefixWordFreq.empty()) {
        int64_t t = IME_PERF_NOW();
        userPrefixWordFreq.sort();
        for (auto &f : userPrefixWordFreq.items) {
            appendCandidate(f.word, f.candLen);
            if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
        }
        perf.userPhraseUs += IME_PERF_NOW() - t;
    if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) { perf.exitName = "user-prefix-phrase-limit"; buildPage(); return; }
    }

    // Phase 4c: curated supplemental phrases for shorthand initials.
    bool curatedInitialFilled = false;
    if (!hasVowel && qlen >= 2 && _all.size() < _candidateLimit) {
        int64_t t = IME_PERF_NOW();
        RankedList segInitFreq;
        std::vector<uint16_t> segIndices;
        addSegPrefixCandidates(segIndices, SEG_INITIAL_ORDER, SEG_INITIAL_INDEX, q, qlen);
        addSegPrefixCandidates(segIndices, SEG_COMPACT_INITIAL_ORDER, SEG_COMPACT_INITIAL_INDEX, q, qlen);
        int64_t dedupEndUs = IME_PERF_NOW();
        int64_t boostUs = 0;
        for (uint16_t segIdx : segIndices) {
            int i = segIdx;
            const char *init = SEG_TABLE[i].initial;
            const char *compactInit = SEG_TABLE[i].compactInitial;
            int initLen = (int)strlen(init);
            int compactInitLen = (int)strlen(compactInit);
            bool match = initLen >= qlen && strncmp(init, q, qlen) == 0;
            bool compactMatch = compactInitLen >= qlen && strncmp(compactInit, q, qlen) == 0;
            if (!match && !compactMatch) continue;
            int matchedInitLen = match ? initLen : compactInitLen;
            int chars = utf8TextCharCount(SEG_TABLE[i].word);
            if (!initialPhraseMatureForTyped(matchedInitLen, qlen, chars)) continue;
            int score = initialPhraseCandidateScoreFromLength(matchedInitLen, qlen, SEG_TABLE[i].word);
            if (matchedInitLen == qlen) score += 5000;
            if (compactMatch) score += 500;
            if (chars >= 2) score += chars;
            if (matchedInitLen > qlen) score -= std::min(6000, (matchedInitLen - qlen) * 900);
            int64_t boostStartUs = IME_PERF_NOW();
            int ctxBoost = contextCandidateBoost(SEG_TABLE[i].word);
            int stableBoost = stableCandidateBoost(SEG_TABLE[i].word);
            boostUs += IME_PERF_NOW() - boostStartUs;
            score += ctxBoost + stableBoost;
            logCandidateDebug("seg-initial", SEG_TABLE[i].word, score, ctxBoost, stableBoost);
            segInitFreq.add(SEG_TABLE[i].word, qlen, score, compactMatch ? 11 : 10);
        }
        int64_t scanEndUs = IME_PERF_NOW();  // Phase 4c scan ends here; sort follows
        perf.segInitDedupUs += dedupEndUs - t;
        perf.segInitBoostUs += boostUs;
        perf.segInitRestUs += (scanEndUs - dedupEndUs) - boostUs;
        perf.segInitUs += scanEndUs - t;
        int64_t sortStartUs = IME_PERF_NOW();
        segInitFreq.sort();
        for (auto &f : segInitFreq.items) {
            if (appendCandidate(f.word, f.candLen)) curatedInitialFilled = true;
            if (_all.size() >= _candidateLimit) break;
        }
        perf.segInitSortUs += IME_PERF_NOW() - sortStartUs;
        perf.userInitialUs += IME_PERF_NOW() - t;
        if (_all.size() >= _candidateLimit) { perf.exitName = "seg-initial-limit"; buildPage(); return; }
    }

    // Phase 5: user dict initial match
    if (!hasVowel && qlen >= IME_USER_INITIAL_MIN_LEN &&
        (_fixedUserWords.size() > 0 || _dynamicUserWords.size() > 0)) {
        int64_t t = IME_PERF_NOW();
        RankedList userInitFreq;
        auto scanInitialWords = [&](const std::vector<UserEntry> &entries,
                                    const std::unordered_map<int, std::vector<uint16_t>> &index,
                                    const std::unordered_map<std::string, std::vector<uint16_t>> &prefixIndex) {
        std::vector<uint16_t> indices;
        addUserLookupIndexMatches(indices, index, prefixIndex, q, qlen);
        for (uint16_t entryIdx : indices) {
            if (entryIdx >= entries.size()) continue;
            auto &p = entries[entryIdx];
            if (p.trad != _trad) continue;
            if (p.code.find('\'') != std::string::npos) continue;  // 撇号码只在分词路径匹配
            const std::string &init = p.initial;
            if ((int)init.length() >= qlen && strncmp(init.c_str(), q, qlen) == 0) {
                int score = p.count * 8 + (((int)init.length() == qlen) ? 100000 : std::max(0, 64 - ((int)init.length() - qlen)));
                score += recentCommitBoost(p.code, p.word);
                int ctxBoost = contextCandidateBoost(p.word);
                int stableBoost = stableCandidateBoost(p.word);
                score += ctxBoost + stableBoost;
                logCandidateDebug("user-initial", p.word, score, ctxBoost, stableBoost);
                userInitFreq.add(p.word, 0, score, 0);
            }
        }
        };
        scanInitialWords(_fixedUserWords, _fixedUserInitialIndex, _fixedUserInitialPrefixIndex);
        scanInitialWords(_dynamicUserWords, _dynamicUserInitialIndex, _dynamicUserInitialPrefixIndex);
        int64_t sortStartUs = IME_PERF_NOW();
        userInitFreq.sort();
        for (auto &f : userInitFreq.items) {
            appendCandidate(f.word, 0);
            if (_all.size() >= _candidateLimit) break;
        }
        perf.userInitSortUs += IME_PERF_NOW() - sortStartUs;
        perf.userInitialUs += IME_PERF_NOW() - t;
        if (_all.size() >= _candidateLimit) { perf.exitName = "user-initial-limit"; buildPage(); return; }
    }

    // Phase 6: initial match (no vowel, consonant-only)
    if (!hasVowel && qlen >= IME_DICT_INITIAL_MIN_LEN && _dict.hasWords() &&
        !(curatedInitialFilled && qlen <= 2)) {
        int64_t t = IME_PERF_NOW();
        struct ScoredPhrase {
            int score;
            int candLen;
            std::string word;
        };
        std::vector<ScoredPhrase> initialCandidates;
        std::unordered_map<std::string, uint16_t> initialIndex;
        int initialCollectLimit = std::min<int>(IME_MAX_INITIAL_COLLECT, (int)_candidateLimit * 3);
        if (_fixedCandidatePaging) initialCollectLimit = std::min<int>(initialCollectLimit, (int)_candidateLimit);
        initialCandidates.reserve(initialCollectLimit);
        auto addInitialCandidate = [&](const std::string &word, int candLen, int score) {
            auto it = initialIndex.find(word);
            if (it != initialIndex.end()) {
                ScoredPhrase &item = initialCandidates[it->second];
                if (item.score < score) {
                    item.score = score;
                    item.candLen = candLen;
                }
                return;
            }
            if ((int)initialCandidates.size() < initialCollectLimit) {
                initialIndex.emplace(word, (uint16_t)initialCandidates.size());
                initialCandidates.push_back({score, candLen, word});
            }
        };
        int scanBudget = IME_MAX_INITIAL_GROUP_SCAN;
        if (qlen <= 2) scanBudget = std::min(IME_MAX_SHORT_INITIAL_GROUP_SCAN, 24);
        else if (qlen == 3) scanBudget = IME_MAX_MEDIUM_INITIAL_GROUP_SCAN;
        else scanBudget = IME_MAX_LONG_INITIAL_GROUP_SCAN;
        const uint8_t *wordData = _dict.wordData();
        size_t slo = 0, shi = _dict.wordDataSize();
        wordWindowCached(q, 1, slo, shi);
        size_t spos = slo;
        int safety = 0;
        while (spos < shi && safety++ < scanBudget &&
               (int)initialCandidates.size() < initialCollectLimit) {
            uint8_t cl = wordData[spos];
            if (cl == 0 || spos + 1 + cl > shi) break;
            const char *wc = (const char *)wordData + spos + 1;
            size_t next = spos + 1 + cl;
            if (next >= shi) break;
            uint8_t n = wordData[next++];
            bool initMatch = pinyinInitialStartsWithCompat(wc, cl, q, qlen);
            std::string groupInit;
            if (initMatch) groupInit = pinyinInitialCodeCompat(std::string(wc, cl));
            for (uint8_t j = 0; j < n && next < shi; j++) {
                uint8_t wl = wordData[next++];
                if (wl == 0 || next + wl + 1 > shi) {
                    next = shi;
                    break;
                }
                uint8_t wf = wordData[next + wl];
                if (initMatch) {
                    std::string w((const char *)wordData + next, wl);
                    if (wordVisible(_trad, w, wf)) {
                        int score = initialPhraseCandidateScoreFromLength((int)groupInit.length(), qlen, w);
                        // _fixedCandidatePaging only picks how the page is sliced, not
                        // how candidates rank; Phase 4c already boosts the same scores
                        // for these codes, so gate on nothing here either.
                        int ctxBoost = contextCandidateBoost(w);
                        int stableBoost = stableCandidateBoost(w);
                        score += ctxBoost + stableBoost;
                        logCandidateDebug("initial", w, score, ctxBoost, stableBoost);
                        if (score >= 0) addInitialCandidate(w, cl, score);
                    }
                }
                next += wl + 1;
            }
            spos = next;
        }
        std::stable_sort(initialCandidates.begin(), initialCandidates.end(),
            [](const ScoredPhrase &a, const ScoredPhrase &b) {
                if (a.score != b.score) return a.score > b.score;
                return a.word.length() < b.word.length();
        });
        for (auto &item : initialCandidates) {
            if (_all.size() >= _candidateLimit) break;
            if (appendCandidate(item.word, item.candLen) && item.candLen > _maxMatchLen)
                _maxMatchLen = item.candLen;
        }
        perf.initialUs += IME_PERF_NOW() - t;
    }

    // Phase 7: shorthand + tail match
    {
        int64_t t = IME_PERF_NOW();
        bool shorthandTail = false;
        std::string typedInit;
        std::string typedTail;
        if (!incompletePinyinInput && primarySplit.tokens.size() > 1 && qlen >= 3 && hasVowel) {
            int lastSylStart = qlen;
            for (int i = qlen - 1; i >= 1; i--) {
                if (strchr("aeiouv", q[i])) {
                    int j = i;
                    while (j > 0 && strchr("aeiouv", q[j-1])) j--;
                    if (j > 0 && strchr("bcdfghjklmnpqrstwxyz", q[j-1])) {
                        lastSylStart = j;
                        break;
                    }
                }
            }
            if (lastSylStart >= 2 && lastSylStart < qlen) {
                bool isPureConsonant = true;
                for (int i = 0; i < lastSylStart; i++) {
                    if (strchr("aeiouv", q[i])) { isPureConsonant = false; break; }
                }
                if (isPureConsonant) {
                    shorthandTail = true;
                    typedInit = std::string(q, lastSylStart);
                    typedTail = std::string(q + lastSylStart, qlen - lastSylStart);
                }
            }
        }
        if (shorthandTail && _dict.hasWords() && _all.size() < IME_FAST_CANDIDATE_LIMIT) {
            size_t slo = 0, shi = _dict.wordDataSize();
            wordWindowCached(typedInit.c_str(), 1, slo, shi);
            size_t spos = slo;
            int safety = 0;
            const uint8_t *wordData = _dict.wordData();
            const char *typedInitChars = typedInit.c_str();
            const char *typedTailChars = typedTail.c_str();
            int typedInitLen = (int)typedInit.length();
            int typedTailLen = (int)typedTail.length();
            while (spos < shi && _all.size() < IME_FAST_CANDIDATE_LIMIT && safety++ < IME_MAX_SHORTHAND_GROUP_SCAN) {
                uint8_t cl = wordData[spos];
                if (cl == 0 || spos + 1 + cl > shi) break;
                const char *wc = (const char *)wordData + spos + 1;
                size_t next = spos + 1 + cl;
                if (next >= shi) break;
                uint8_t n = wordData[next++];
                bool initMatch = pinyinInitialStartsWithCompat(wc, cl, typedInitChars, typedInitLen);
                int lastSylStart = cl;
                for (int i = cl - 1; i >= 0; i--) {
                    if (strchr("aeiouv", wc[i])) {
                        int j = i;
                        while (j > 0 && strchr("aeiouv", wc[j-1])) j--;
                        if (j > 0) { lastSylStart = j; break; }
                    }
                }
                const char *candTailStart = wc + lastSylStart;
                int candTailLen = cl - lastSylStart;
                bool tailMatch = false;
                if (candTailLen >= typedTailLen)
                    tailMatch = (strncmp(candTailStart, typedTailChars, typedTailLen) == 0);
                else
                    tailMatch = (strncmp(typedTailChars, candTailStart, candTailLen) == 0);
                bool groupMatch = initMatch && tailMatch;
                for (uint8_t j = 0; j < n && next < shi; j++) {
                    uint8_t wl = wordData[next++];
                    if (wl == 0 || next + wl + 1 > shi) {
                        next = shi;
                        break;
                    }
                    uint8_t wf = wordData[next + wl];
                    if (groupMatch) {
                        std::string w((const char *)wordData + next, wl);
                        if (wordVisible(_trad, w, wf) && appendCandidate(w, cl)) {
                            if (cl > _maxMatchLen) _maxMatchLen = cl;
                        }
                    }
                    if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) {
                        next += wl + 1;
                        break;
                    }
                    next += wl + 1;
                }
                spos = next;
            }
        }
        perf.shorthandUs += IME_PERF_NOW() - t;
    }

    // Phase 8: partial (逐字) match
    _partialStart = (int)_all.size();
    _remainder.clear();
    if (qlen > 1 && _all.size() < _candidateLimit && (!incompletePinyinInput || _all.empty())) {
        int64_t t = IME_PERF_NOW();
        uint32_t zlo, zhi;
        std::vector<int> tryLens = ime::PinyinEngine::prefixMatchLengths(std::string(q, qlen));
        int maxTry = qlen - 1;
        if (_maxMatchLen > 0 && _maxMatchLen < maxTry) maxTry = _maxMatchLen - 1;
        for (int len = maxTry; len >= 1; len--) {
            if (std::find(tryLens.begin(), tryLens.end(), len) == tryLens.end())
                tryLens.push_back(len);
        }
        for (int tryLen : tryLens) {
            if (tryLen >= qlen || tryLen < 1 || _all.size() >= _candidateLimit) continue;
            searchWindow(q, tryLen, zlo, zhi);
            uint32_t sEnd = zhi;
            int bcount = 0;
            while (zlo < zhi && bcount++ < 200) {
                uint32_t mid = zlo + (zhi - zlo) / 2;
                char code[7]; if (!readCode(mid, code)) break;
                if (strncmp(code, q, tryLen) < 0) zlo = mid + 1;
                else zhi = mid;
            }
            int partialScanned = 0;
            for (uint32_t i = zlo; i < sEnd && _all.size() < _candidateLimit &&
                                  partialScanned++ < IME_MAX_PARTIAL_RECORD_SCAN; i++) {
                char code[7]; if (!readCode(i, code)) break;
                if (strncmp(code, q, tryLen) != 0) break;
                uint8_t f = readRecordFlag(i);
                if (f & (_trad ? 0x01 : 0x02)) continue;
                char hz[4]; if (!readHanzi(i, hz)) break;
                appendCandidate(std::string(hz), 0);
            }
            if (_all.size() > (size_t)_partialStart) {
                _remainder = _code.substr(tryLen);
                break;
            }
        }
        perf.partialUs += IME_PERF_NOW() - t;
    }

    // 低优先级的行内英文候选(中英混排用)。排在所有中文路径之后, 否则会埋掉 Phase 7
    // 的简写/尾码匹配——这个相位对含元音的多音节码是有效的。
    if (hasVowel && !incompletePinyinInput && _all.size() < _candidateLimit) {
        appendEnglishInlineCandidates(pinyinCode);
    }

    perf.exitName = "end";
    buildPage();
}

void IME::lookupEnglishMode() {
    clearCandidates();
    _pageStart = 0;
    _curPage = 0;
    loadEnglishDict();
    std::string q = _code;
    std::string lower = q;
    for (char &c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    // 输入首字母大写时,命中的英文词条首字母跟随大写
    bool upper = !q.empty() && q[0] >= 'A' && q[0] <= 'Z';
    auto it = std::lower_bound(_englishWords.begin(), _englishWords.end(), lower);
    for (; it != _englishWords.end(); ++it) {
        if (_all.size() >= MAX_CANDIDATES) break;
        if (it->find(lower) != 0) break;
        appendCandidate(upper ? capFirst(*it) : *it, (int)q.length());
    }
    bool exact = false;
    for (auto &w : _all) if (w == q) { exact = true; break; }
    if (!exact && !_code.empty()) {
        _all.insert(_all.begin(), q);
        _candLen.insert(_candLen.begin(), (int)q.length());
        _candidateWidths.insert(_candidateWidths.begin(), -1);
        _predictCandidateKeys.insert(_predictCandidateKeys.begin(), "");
        rebuildCandidateHashes();
    }
    buildPage();
}

// v模式颜文字搜索匹配: 编码按音节表贪心切分, 查询串逐音节消费 1..音节长 个
// 字符(全拼前缀或声母缩写均可, 如 k/ka/kai/kx 都命中 kaixin)。音节表按长度
// 降序生成, 首个 strncmp 命中即最长音节。
static bool vKaomojiMatch(const char *q, size_t qlen, const char *code) {
    size_t qi = 0, ci = 0;
    while (code[ci]) {
        if (qi >= qlen) return true;
        size_t slen = 0;
        for (unsigned s = 0; s < K_KAOMOJI_SYLL_COUNT; s++) {
            size_t l = strlen(K_KAOMOJI_SYLLS[s]);
            if (strncmp(code + ci, K_KAOMOJI_SYLLS[s], l) == 0) { slen = l; break; }
        }
        if (slen == 0) slen = 1;
        size_t k = 0;
        while (k < slen && qi + k < qlen && code[ci + k] == q[qi + k]) k++;
        if (k == 0) return false;
        qi += k;
        ci += slen;
    }
    return qi >= qlen;
}

void IME::lookupKaomoji(const std::string &query) {
    if (query.empty()) return;
    for (unsigned i = 0; i < K_KAOMOJI_COUNT && _all.size() < MAX_CANDIDATES; i++) {
        const char *code = K_KAOMOJI_TABLE[i].code;
        if (code[0] == '\0' || code[0] != query[0]) continue;  // 首字符过滤+跳过常用块
        if (!vKaomojiMatch(query.data(), query.size(), code)) continue;
        const char *face = K_KAOMOJI_TABLE[i].face;
        appendCandidate(face, (int)_code.length());
    }
}

void IME::lookupVMode() {
    clearCandidates();
    _pageStart = 0;
    _curPage = 0;
    _vSel = 0;
    std::string body = _code.length() > 1 ? _code.substr(1) : "";
    if (body.empty()) {
        // 裸 v: 常用文字表情(原中文标点候选改由 v/bd/ 搜索)
        for (unsigned i = 0; i < K_KAOMOJI_HOT && i < K_KAOMOJI_COUNT; i++) {
            appendCandidate(K_KAOMOJI_TABLE[i].face, 1);
        }
        buildPage();
        return;
    }

    // 闭合命令(v/t/ v/d/ v/w/,以 / 结尾):出候选,数字键/方向键选择。
    // 未闭合(v/t)不出候选,避免选词歧义。
    if (body == "/t/" || body == "/d/" || body == "/w/") {
        for (auto &s : vTimeDateWeek(body.substr(0, body.size() - 1))) {
            appendCandidate(s, (int)_code.length());
        }
        buildPage();
        return;
    }

    if (body[0] == '/') {
        std::string num = body.substr(1);
        bool allDigits = !num.empty();
        for (char c : num) if (c < '0' || c > '9') { allDigits = false; break; }
        if (allDigits) {
            uint64_t n = 0;
            for (char c : num) n = n * 10 + (uint64_t)(c - '0');
            appendCandidate(chineseDigits(n, false), (int)_code.length());
            appendCandidate(chineseDigits(n, true), (int)_code.length());
            if (n >= 1 && n <= 99) {
                appendCandidate(romanNumber((int)n), (int)_code.length());
            }
        } else {
            // v/编码(闭合 v/编码/ 可数字键直选): 按拼音/声母前缀搜文字表情与标点。
            // 纯字母才进搜索; 数字/混合编码无候选。
            std::string q = num;
            if (!q.empty() && q.back() == '/') q.pop_back();
            bool allAlpha = !q.empty();
            for (char c : q) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { allAlpha = false; break; }
            }
            if (allAlpha) {
                std::string lq = q;
                for (char &c : lq) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                lookupKaomoji(lq);
            }
        }
        buildPage();
        return;
    }

    int y = 0, m = 0, d = 0;
    bool isDate = parseDateParts(body, y, m, d);
    if (isDate) {
        char arabic[32];
        snprintf(arabic, sizeof(arabic), "%d年%d月%d日", y, m, d);
        appendCandidate(arabic, (int)_code.length());
        std::string cn = chineseYear(y) + "年" + chineseDayMonth(m) + "月" + chineseDayMonth(d) + "日";
        appendCandidate(cn, (int)_code.length());
    }

    bool allAlpha = true;
    for (char c : body) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { allAlpha = false; break; }
    }
    if (allAlpha) {
        loadEnglishDict();
        std::string lower = body;
        for (char &c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        // 输入首字母大写时,命中的英文词条首字母跟随大写
        bool upper = body[0] >= 'A' && body[0] <= 'Z';
        auto it = std::lower_bound(_englishWords.begin(), _englishWords.end(), lower);
        for (; it != _englishWords.end(); ++it) {
            if (_all.size() >= MAX_CANDIDATES) break;
            if (it->find(lower) != 0) break;
            std::string w = upper ? capFirst(*it) : *it;
            appendCandidate(w, (int)_code.length());
        }
        bool dup = false;
        for (auto &e : _all) if (e == body) { dup = true; break; }
        if (!dup) {
            _all.insert(_all.begin(), body);
            _candLen.insert(_candLen.begin(), (int)_code.length());
            _candidateWidths.insert(_candidateWidths.begin(), -1);
            _predictCandidateKeys.insert(_predictCandidateKeys.begin(), "");
            rebuildCandidateHashes();
        }
    }

    buildPage();
}

// 单引号分词查词: 编码形如 "xi'an" / "an'guang", 按 ' 切成音节段。
// 1) 用户词典整码/去分隔整码匹配; 2) 补充词典表(seg_table.h) 分段前缀匹配;
// 3) 主词典词组: 拼接码精确匹配且字数=段数;
// 4) 逐字匹配: 首段单字候选, 选中后按 seg0Next 消费跳下一段续拼(见 commit 的 candContinue)。
// 全部视为整码消费。
void IME::lookupSegmented() {
    std::vector<std::string> segs;
    ime::PinyinSplit split = ime::PinyinEngine::primarySplit(_code, true);
    for (auto &token : split.tokens) segs.push_back(token.text);
    if (segs.empty()) segs = splitSyllableText(_code.c_str());
    if (segs.empty()) return;
    std::string q;
    for (auto &s : segs) q += s;
    std::vector<std::string> aliasCodes = alternateInputCodes(q);
    int fullLen = (int)_code.length();
    rebuildUserWordIndexes();

    // 1) 用户词典: 显式分词和去分隔编码共享学习结果
    RankedList userMatches;
    auto scanSegmentedUserWords = [&](const std::vector<UserEntry> &entries,
                                      const std::unordered_map<int, std::vector<uint16_t>> &index,
                                      const std::unordered_map<std::string, std::vector<uint16_t>> &prefixIndex) {
    std::vector<uint16_t> indices;
    addUserPrefixIndexMatches(indices, index, q.c_str(), (int)q.length());
    addUserPrefixIndexMatches(indices, prefixIndex, q.c_str(), (int)q.length());
    for (auto &aliasCode : aliasCodes)
    {
        addUserPrefixIndexMatches(indices, index, aliasCode.c_str(), (int)aliasCode.length());
        addUserPrefixIndexMatches(indices, prefixIndex, aliasCode.c_str(), (int)aliasCode.length());
    }
    for (uint16_t entryIdx : indices) {
        if (entryIdx >= entries.size()) continue;
        auto &p = entries[entryIdx];
        if (p.trad != _trad) continue;
        if (p.word.length() <= 3) continue;
        std::string matchedCode;
        bool matched = userCodeMatchesPrefix(p.code, q.c_str(), (int)q.length(), &matchedCode);
        for (auto &aliasCode : aliasCodes) {
            if (matched) break;
            matched = userCodeMatchesPrefix(p.code, aliasCode.c_str(), (int)aliasCode.length(), &matchedCode);
        }
        if (!matched) continue;
        int score = userCandidateScore(matchedCode, p.count, (int)q.length());
        userMatches.add(p.word, 0, score, 0);
    }
    };
    scanSegmentedUserWords(_fixedUserWords, _fixedUserCodeIndex, _fixedUserCodePrefixIndex);
    scanSegmentedUserWords(_dynamicUserWords, _dynamicUserCodeIndex, _dynamicUserCodePrefixIndex);
    userMatches.sort();
    for (auto &m : userMatches.items) {
        appendCandidate(m.word, fullLen);
        if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
    }

    // 2) 补充词典表
    std::vector<ime::PinyinSplit> aliasSplits;
    for (auto &aliasCode : aliasCodes) {
        std::vector<ime::PinyinSplit> moreSplits = ime::PinyinEngine::splitVariants(aliasCode, true, 4);
        aliasSplits.insert(aliasSplits.end(), moreSplits.begin(), moreSplits.end());
    }
    std::vector<uint16_t> segIndices;
    addSegPrefixCandidates(segIndices, SEG_CODE_ORDER, SEG_CODE_INDEX, q.c_str(), (int)q.length());
    for (auto &aliasCode : aliasCodes) {
        addSegPrefixCandidates(segIndices, SEG_CODE_ORDER, SEG_CODE_INDEX,
                               aliasCode.c_str(), (int)aliasCode.length());
    }
    std::stable_sort(segIndices.begin(), segIndices.end());
    for (uint16_t segIdx : segIndices) {
        if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
        int i = segIdx;
        if (segs.size() > SEG_TABLE[i].syllableCount) continue;
        bool ok = syllableTextStartsWithSegments(SEG_TABLE[i].syllables, segs);
        if (ok) {
            ok = strncmp(q.c_str(), SEG_TABLE[i].code, q.length()) == 0;
        }
        if (!ok && !aliasSplits.empty()) {
            for (auto &split : aliasSplits) {
                int typedCodeLen = pinyinJoinedLength(split.tokens);
                int entryCodeLen = (int)strlen(SEG_TABLE[i].code);
                if (typedCodeLen > entryCodeLen) continue;
                if (!pinyinTokensMatchCodePrefix(split.tokens, SEG_TABLE[i].code, entryCodeLen)) continue;
                if (pinyinSegmentsMatchText(split.tokens, SEG_TABLE[i].syllables)) { ok = true; break; }
            }
        }
        if (!ok) continue;
        appendCandidate(SEG_TABLE[i].word, fullLen);
    }

    // 3) 主词典词组: 拼接码精确匹配 + 字数/3 == 段数
    if (_dict.hasWords() && q.length() >= 2 && _all.size() < IME_FAST_CANDIDATE_LIMIT) {
        const uint8_t *wordData = _dict.wordData();
        int wordTextLen = (int)segs.size() * 3;
        auto scanExactPhrase = [&](const std::string &scanCode) {
            if (scanCode.length() < 2) return;
            size_t wlo = 0, whi = _dict.wordDataSize();
            wordWindowCached(scanCode.c_str(), (int)scanCode.length(), wlo, whi);
            size_t wpos = wlo;
            int safety = 0;
            int scanLen = (int)scanCode.length();
            while (wpos < whi && _all.size() < IME_FAST_CANDIDATE_LIMIT &&
                   safety++ < IME_MAX_PHRASE_GROUP_SCAN) {
                uint8_t cl = wordData[wpos];
                if (cl == 0 || wpos + 1 + cl > whi) break;
                const char *wc = (const char *)wordData + wpos + 1;
                size_t next = wpos + 1 + cl;
                if (next >= whi) break;
                uint8_t n = wordData[next++];
                bool groupMatch = ((int)cl == scanLen && strncmp(wc, scanCode.c_str(), scanCode.length()) == 0);
                for (uint8_t j = 0; j < n && next < whi; j++) {
                    uint8_t wl = wordData[next++];
                    if (wl == 0 || next + wl + 1 > whi) {
                        next = whi;
                        break;
                    }
                    uint8_t wf = wordData[next + wl];
                    if (groupMatch && (int)wl == wordTextLen) {
                        std::string w((const char *)wordData + next, wl);
                        if (wordVisible(_trad, w, wf)) appendCandidate(w, fullLen);
                        if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
                    }
                    next += wl + 1;
                }
                wpos = next;
            }
        };
        scanExactPhrase(q);
        for (auto &aliasCode : aliasCodes) {
            if (_all.size() >= IME_FAST_CANDIDATE_LIMIT) break;
            scanExactPhrase(aliasCode);
        }
    }

    // 4) 逐字匹配: 首段单字前缀候选(词组之后)。选中后消费 seg0Next 字节跳到
    //    下一段续拼(如 xi'an 选"西"后余下 "an" 查字), 由 commit 的 candContinue 推进。
    if (_all.size() < IME_FAST_CANDIDATE_LIMIT) {
        int seg0Next = (segs.size() > 1) ? ((int)segs[0].length() + 1) : fullLen;
        appendSingleCharCandidates(segs[0], seg0Next);
    }

    _partialStart = (int)_all.size();
}

// 主词典单字前缀匹配: 与 lookup() Phase 2 相同扫描, 但消费长度由调用方指定
// (分词逐字续拼时是跳到下一段的字节偏移, 而非词典码长)。
void IME::appendScoredSingleChars(const char *prefix, int qlen, int scanBudget,
                                  bool codeLenFromRecord, int fixedCandLen, size_t cap) {
    if (!prefix || qlen < 1) return;
    if (cap == 0) cap = _candidateLimit;
    if (_all.size() >= cap) return;
    uint32_t lo, hi;
    searchWindow(prefix, qlen, lo, hi);
    uint32_t scanEnd = hi;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        char code[7];
        if (!readCode(mid, code)) break;
        if (strncmp(code, prefix, qlen) < 0) lo = mid + 1;
        else hi = mid;
    }
    _singleScratch.clear();
    int scanned = 0;
    // Scoring is deferred to a sort after the scan, so _all does not grow here and
    // the scan is bounded by scanBudget alone. Duplicates of rows already in _all
    // are dropped later by appendCandidate, so a low-scoring dupe never shadows a
    // higher-scoring row that has not been scanned yet.
    for (uint32_t i = lo; i < scanEnd && scanned++ < scanBudget; i++) {
        char code[7];
        if (!readCode(i, code)) break;
        if (strncmp(code, prefix, qlen) != 0) break;
        uint8_t f = readRecordFlag(i);
        if (f & (_trad ? 0x01 : 0x02)) continue;
        char hz[4];
        if (!readHanzi(i, hz)) break;
        std::string word(hz);
        int score = -(int)_singleScratch.size() * IME_SINGLE_PRIOR_STEP;
        int ctxBoost = contextCandidateBoost(word);
        if (ctxBoost > IME_SINGLE_CTX_BOOST_CAP) ctxBoost = IME_SINGLE_CTX_BOOST_CAP;
        int stableBoost = stableCandidateBoost(word);
        score += ctxBoost + stableBoost;
        logCandidateDebug("single", word, score, ctxBoost, stableBoost);
        _singleScratch.push_back({word, codeLenFromRecord ? (int)strlen(code) : fixedCandLen, score});
    }
    if (_singleScratch.size() > 1) {
        std::stable_sort(_singleScratch.begin(), _singleScratch.end(),
            [](const SingleScratchEntry &a, const SingleScratchEntry &b) {
                return a.score > b.score;
        });
    }
    for (auto &e : _singleScratch) {
        if (_all.size() >= cap) break;
        if (appendCandidate(e.word, e.codeLen) && codeLenFromRecord &&
            e.codeLen > _maxMatchLen)
            _maxMatchLen = e.codeLen;
    }
}

void IME::appendSingleCharCandidates(const std::string &prefix, int candLen, size_t cap) {
    int qlen = (int)prefix.length();
    if (cap == 0) cap = _candidateLimit;
    int scanBudget = (candLen > 0 && candLen <= 2) ?
        IME_MAX_SHORT_CONSONANT_SINGLE_SCAN : IME_MAX_SINGLE_RECORD_SCAN;
    appendScoredSingleChars(prefix.c_str(), qlen, scanBudget,
                            /*codeLenFromRecord=*/false, candLen, cap);
}

// ── 整句覆盖(#12) ────────────────────────────────────────────────────────────
// 长全拼串在词表里没有整串条目, 前面的相位只会给出首音节的单字(Phase 8 逐字匹配),
// 所以这个相位排在 lookup 最前面, 整句候选取代那些单字坐上首屏。
// 这里把编码切成一张词图, 用 beam search 找覆盖整串的若干条最优路径:
//   词弧   词典词组, 其码恰好等于 [pos, pos+cl) 这一段
//   单字弧 该段恰好是一个合法音节, 取单字表里最高频的几个字
// 弧 = (lo, hi, word, score), 用 word 消费掉编码的 [lo, hi) 段。
// 词弧按字数给正分, 单字弧给负分, 每条弧统一扣一个罚项: 少了罚项, DP 一定会把
// "xian" 拆成 "xi"+"an" 去多赚一条弧的分。位次按桶内顺序当词频先验。
void IME::collectSentenceArcs(int pos, const char *code, int len) {
    const int remain = len - pos;
    if (remain <= 0) return;
    const int totalLeft = IME_SENTENCE_TOTAL_SCAN - _sentenceGroupsScanned;
    if (totalLeft <= 0) return;
    const int budget = totalLeft < IME_SENTENCE_BUCKET_SCAN ? totalLeft : IME_SENTENCE_BUCKET_SCAN;
    // 搭配分只给整句首词: 上一次上屏预测的是下一句的起头, 句中词与它没有搭配关系。
    auto ctxBoost = [&](const std::string &w) {
        if (pos != 0) return 0;
        int cb = contextCandidateBoost(w);
        return cb > IME_SENTENCE_CTX_CAP ? IME_SENTENCE_CTX_CAP : cb;
    };
    auto pushWordArc = [&](const std::string &w, int cl, int rank) {
        SentenceArc arc;
        arc.lo = (uint8_t)pos;
        arc.hi = (uint8_t)(pos + cl);
        arc.score = IME_SENTENCE_WORD_UNIT * (utf8TextCharCount(w) - 1)
                  - IME_SENTENCE_RANK_WORD * rank - IME_SENTENCE_ARC_PENALTY
                  + IME_SENTENCE_LEN_BIAS * cl * (len - pos)
                  + ctxBoost(w);
        arc.word = w;
        _sentenceArcs.push_back(std::move(arc));
    };

    // 1) 词典词弧。词表只按前两位分桶, 桶内组记录变长且没有长度字段; 从桶首顺扫时, 要找的
    //    匹配前缀之间还夹着大量不匹配的组(实测 pos 0 就要跨 2314 组, 占整个整句相位七成),
    //    检查点索引也救不了——它只省掉"桶首→首个匹配前缀"这一小段。这里改成对每个前缀长度
    //    各 seek 一次: 命中就取该组的词弧, 再探更长前缀; 一旦"第一个 ≥ 该前缀的组在前 L 字节
    //    内就分歧", 更长的前缀必然也不匹配(否则它会是第一个 ≥ 该前缀的组), 直接停。词弧的
    //    顺序、rank 与顺扫逐条一致, 且完全不扫不匹配的组。
    if (_dict.hasWords()) {
        size_t wlo = 0, whi = 0;
        wordWindowCached(code + pos, remain >= 2 ? 2 : 1, wlo, whi);
        const uint8_t *wordData = _dict.wordData();
        int scanned = 0;
        if (wlo < whi) {
            for (int L = 1; L <= remain && scanned < budget; L++) {
                const size_t off = _dict.wordGroupSeek(wlo, whi, code + pos, L, scanned);
                if (off >= whi) break;  // 桶里没有 ≥ 该前缀的组, 更长前缀也不会有
                int cl = 0;
                const uint8_t *wc = _dict.wordGroupCode(off, cl);
                if (!wc) break;
                if (cl == L && memcmp(wc, code + pos, (size_t)L) == 0) {
                    size_t p = off + 1 + cl;
                    if (p < whi) {
                        uint8_t n = wordData[p++];
                        int taken = 0;
                        for (uint8_t j = 0; j < n; j++) {
                            if (p + 1 > whi) break;
                            uint8_t wl = wordData[p];
                            if (wl == 0 || p + 1 + wl + 1 > whi) break;
                            if (taken < IME_SENTENCE_WORD_ARCS) {
                                std::string w((const char *)wordData + p + 1, wl);
                                if (wordVisible(_trad, w, wordData[p + 1 + wl])) {
                                    pushWordArc(w, cl, taken);
                                    taken++;
                                }
                            }
                            p += 1 + wl + 1;
                        }
                    }
                    continue;  // 命中, 继续探更长前缀
                }
                if (cl <= L || memcmp(wc, code + pos, (size_t)L) != 0) break;
                // 否则 wc 比该前缀长且以它开头, 继续探更长前缀
            }
        }
        _sentenceGroupsScanned += scanned;
    }

    // 2) 单字弧: 段必须恰好是一个合法音节, 否则 "xian" 会被当两个段各出一个字。
    const int maxSyl = remain < MAX_CODE_LEN ? remain : MAX_CODE_LEN;
    for (int cl = 1; cl <= maxSyl; cl++) {
        char syl[MAX_CODE_LEN + 1];
        memcpy(syl, code + pos, cl);
        syl[cl] = '\0';
        if (!ime::PinyinEngine::isValidSyllable(syl)) continue;
        uint32_t slo = 0, shi = 0;
        searchWindow(code + pos, cl, slo, shi);
        uint32_t i = _dict.lowerBoundSingle(code + pos, cl, slo, shi);
        int taken = 0;
        for (; i < shi && taken < IME_SENTENCE_SINGLE_ARCS; i++) {
            char rcode[MAX_CODE_LEN + 1];
            if (!readCode(i, rcode)) break;
            if (strncmp(rcode, code + pos, cl) != 0) break;
            if (readRecordFlag(i) & (_trad ? 0x01 : 0x02)) continue;
            char hz[HANZI_SIZE + 1];
            if (!readHanzi(i, hz)) break;
            SentenceArc arc;
            arc.lo = (uint8_t)pos;
            arc.hi = (uint8_t)(pos + cl);
            arc.word = hz;
            arc.score = IME_SENTENCE_SINGLE - IME_SENTENCE_RANK_SINGLE * taken
                      + IME_SENTENCE_LEN_BIAS * cl * (len - pos) + ctxBoost(arc.word);
            _sentenceArcs.push_back(std::move(arc));
            taken++;
        }
    }

    // 3) 用户词库词弧。桶按前两位分且桶很小, 直接扫桶比走词典便宜; 桶内已按使用
    //    次数降序, 所以位次就是频率先验。
    auto scanUserArcs = [&](const std::vector<UserEntry> &entries,
                            const std::unordered_map<int, std::vector<uint16_t>> &index) {
        auto it = index.find(segPrefixKey(code + pos, remain >= 2 ? 2 : 1));
        if (it == index.end()) return;
        int taken = 0;
        for (uint16_t ei : it->second) {
            if (taken >= IME_SENTENCE_WORD_ARCS) break;
            if (ei >= entries.size()) continue;
            const UserEntry &e = entries[ei];
            if (e.trad != _trad) continue;
            std::string ec = e.code;
            if (ec.find('\'') != std::string::npos) ec = ime::PinyinEngine::removeSplit(ec);
            int cl = (int)ec.length();
            if (cl < 2 || cl > remain) continue;
            if (memcmp(ec.c_str(), code + pos, cl) != 0) continue;
            pushWordArc(e.word, cl, taken);
            taken++;
        }
    };
    scanUserArcs(_fixedUserWords, _fixedUserCodeIndex);
    scanUserArcs(_dynamicUserWords, _dynamicUserCodeIndex);
}

void IME::appendSentenceCandidates(const char *code, int len) {
    if (!code || len < IME_SENTENCE_MIN_LEN || len > IME_SENTENCE_MAX_LEN) return;
    _sentenceArcs.clear();
    _sentenceNodes.clear();
    _sentenceCands.clear();
    _sentenceByHi.clear();
    _sentenceNodeOff.assign(len + 2, 0);
    _sentenceByHiOff.assign(len + 2, 0);
    _sentenceGroupsScanned = 0;

    for (int pos = 0; pos < len; pos++) collectSentenceArcs(pos, code, len);
    if (_sentenceArcs.empty()) return;

    // 弧按终点分桶(计数排序), DP 就能按 hi 升序一次成型: 走到 hi 时所有 lo < hi
    // 的 beam 都已经定型, 不必再维护每位置的待选表。
    int hiCount[IME_SENTENCE_MAX_LEN + 2] = {};
    for (const SentenceArc &arc : _sentenceArcs) {
        if (arc.hi >= 1 && arc.hi <= len) hiCount[arc.hi]++;
    }
    int acc = 0;
    for (int h = 1; h <= len; h++) {
        _sentenceByHiOff[h] = acc;
        acc += hiCount[h];
    }
    _sentenceByHiOff[len + 1] = acc;
    _sentenceByHi.assign(acc, 0);
    int cursor[IME_SENTENCE_MAX_LEN + 2];
    for (int h = 1; h <= len + 1; h++) cursor[h] = _sentenceByHiOff[h];
    for (int ai = 0; ai < (int)_sentenceArcs.size(); ai++) {
        int h = _sentenceArcs[ai].hi;
        if (h >= 1 && h <= len) _sentenceByHi[cursor[h]++] = (int16_t)ai;
    }

    _sentenceNodes.push_back({-1, -1, 0});  // 位置 0 的根
    _sentenceNodeOff[0] = 0;
    auto byScoreDesc = [](const SentenceCand &a, const SentenceCand &b) {
        return a.score > b.score;
    };
    for (int hi = 1; hi <= len; hi++) {
        _sentenceNodeOff[hi] = (int)_sentenceNodes.size();
        _sentenceCands.clear();
        for (int k = _sentenceByHiOff[hi]; k < _sentenceByHiOff[hi + 1]; k++) {
            const SentenceArc &arc = _sentenceArcs[_sentenceByHi[k]];
            int nStart = _sentenceNodeOff[arc.lo];
            int nEnd = _sentenceNodeOff[arc.lo + 1];
            for (int ni = nStart; ni < nEnd; ni++)
                _sentenceCands.push_back({(int16_t)ni, _sentenceByHi[k],
                                          _sentenceNodes[ni].score + arc.score});
        }
        if ((int)_sentenceCands.size() > IME_SENTENCE_BEAM) {
            std::partial_sort(_sentenceCands.begin(),
                              _sentenceCands.begin() + IME_SENTENCE_BEAM,
                              _sentenceCands.end(), byScoreDesc);
            _sentenceCands.resize(IME_SENTENCE_BEAM);
        } else {
            std::sort(_sentenceCands.begin(), _sentenceCands.end(), byScoreDesc);
        }
        for (const SentenceCand &c : _sentenceCands)
            _sentenceNodes.push_back({c.parent, c.arc, c.score});
        _sentenceNodeOff[hi + 1] = (int)_sentenceNodes.size();
    }

    // 位置 len 的节点已按分降序, 顺序回溯父链拼文本。重复(不同切分拼出同一句)
    // 交给 hasCandidate 过滤, 所以多留几个节点凑够 IME_SENTENCE_RESULTS 条。
    std::string text;
    int emitted = 0;
    for (int ni = _sentenceNodeOff[len];
         ni < _sentenceNodeOff[len + 1] && emitted < IME_SENTENCE_RESULTS; ni++) {
        text.clear();
        for (int cur = ni; cur > 0;) {
            const SentenceNode &nd = _sentenceNodes[cur];
            if (nd.arc < 0) break;
            text.insert(0, _sentenceArcs[nd.arc].word);
            cur = nd.parent;
        }
        if (text.empty() || hasCandidate(text)) continue;
        if (appendCandidate(text, len)) emitted++;
        if (_all.size() >= _candidateLimit) break;
    }
}

void IME::beginPredict(const std::string &text, bool afterSpaceCommit) {
    reset();
    std::string mode = g_settings.imePredictMode();
    if (mode == "off") return;
    if (mode == "space" && !afterSpaceCommit) return;
    if (text.empty()) return;
    ensureUserDictLoaded();
    rebuildUserPredictIndex();
    std::vector<std::string> keys;
    addContextTailKeys(keys, text);
    if (keys.empty()) return;

    std::vector<RankedPredictCandidate> ranked;
    int order = 0;
    for (auto &keyText : keys) {
        int keyChars = utf8TextCharCount(keyText);
        auto indexIt = _userPredictIndex.find(keyText);
        if (indexIt != _userPredictIndex.end()) {
            for (uint16_t entryIdx : indexIt->second) {
                if (entryIdx >= _userPredictWords.size()) continue;
                auto &p = _userPredictWords[entryIdx];
                if (p.trad != _trad) continue;
                if (rejectedPredictWord(keyText, p.word)) continue;
                int wordChars = utf8TextCharCount(p.word);
                int score = p.count * 24 + keyChars * 1500 - std::max(0, wordChars - 2) * 40;
                addRankedPredictCandidate(ranked, keyText, p.word, score, 300, order++);
            }
        }
        if (_dict.hasPredictions()) {
            ime::Im3Dictionary::PredictGroup group;
            if (_dict.findPredictGroup(keyText, group))
                for (auto &word : group.candidates) {
                    if (rejectedPredictWord(keyText, word)) continue;
                    int wordChars = utf8TextCharCount(word);
                    int score = keyChars * 1500 + 800 - std::max(0, wordChars - 2) * 50;
                    addRankedPredictCandidate(ranked, keyText, word, score, 200, order++);
                }
        }
        for (auto &entry : BUILTIN_PREDICT) {
            if (keyText == entry.key) {
                for (int i = 0; entry.candidates[i]; i++) {
                    std::string word = entry.candidates[i];
                    if (rejectedPredictWord(keyText, word)) continue;
                    int wordChars = utf8TextCharCount(word);
                    int score = keyChars * 1500 + 500 - i * 8 - std::max(0, wordChars - 2) * 60;
                    addRankedPredictCandidate(ranked, keyText, word, score, 100, order++);
                }
                break;
            }
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(),
        [](const RankedPredictCandidate &a, const RankedPredictCandidate &b) {
            if (a.score != b.score) return a.score > b.score;
            if (a.sourceRank != b.sourceRank) return a.sourceRank > b.sourceRank;
            return a.order < b.order;
        });
    for (auto &item : ranked) {
        size_t before = _all.size();
        if (appendCandidate(item.word, 0) && _all.size() > before)
            _predictCandidateKeys.back() = item.key;
    }
    if (_all.empty()) {
        _predChar.clear();
        reset();
        return;
    }
    _predChar = !_predictCandidateKeys.empty() && !_predictCandidateKeys[0].empty()
        ? _predictCandidateKeys[0] : keys[0];
    _predicting = true;
    buildPage();
}

void IME::buildPage() {
    int64_t pageStartUs = IME_PERF_NOW();
    _page.clear();
    _vSel = 0;  // 换页/重新查词后高亮回到首个候选
    if (_all.empty()) {
        _pageStart = 0;
        _curPage = 0;
        _pageStarts.clear();
        _pageAnchor = -1;
        return;
    }
    if (!_fixedCandidatePaging && _widthFn && _displayWidth > 0) {
        // 按显示宽度分页: 与各界面候选行渲染一致, " 编号." 前缀 + 候选文本,
        // 一行放不下则把该候选归入下一页, 保证候选不被隐藏。
        int anchor = _pageAnchor;
        _pageStarts.clear();
        _pageStarts.push_back(0);
        int lineW = 0;
        int numWidths[10] = {};
        for (int i = 0; i < (int)_all.size(); i++) {
            int pageCount = i - (int)_pageStarts.back();
            char num[16];
            snprintf(num, sizeof(num), " %d.", pageCount + 1);
            if (pageCount + 1 >= 1 && pageCount + 1 <= 9 && numWidths[pageCount + 1] == 0)
                numWidths[pageCount + 1] = _widthFn(num);
            if (i >= (int)_candidateWidths.size()) _candidateWidths.resize(_all.size(), -1);
            if (_candidateWidths[i] < 0) _candidateWidths[i] = _widthFn(_all[i].c_str());
            int partW = ((pageCount + 1 >= 1 && pageCount + 1 <= 9) ? numWidths[pageCount + 1] : _widthFn(num))
                      + _candidateWidths[i];
            if (lineW > 0 && (pageCount >= 9 || lineW + partW > _displayWidth)) {
                _pageStarts.push_back(i);
                lineW = 0;
                snprintf(num, sizeof(num), " 1.");
                if (numWidths[1] == 0) numWidths[1] = _widthFn(num);
                partW = numWidths[1] + _candidateWidths[i];
            }
            lineW += partW;
            if (anchor < 0 && (int)_pageStarts.size() > _curPage + 1) break;
            if (anchor >= 0 && (int)_pageStarts.size() > _curPage + 1 &&
                _pageStarts.back() > anchor) {
                break;
            }
        }
        if (_pageAnchor >= 0) {
            for (int i = 0; i < (int)_pageStarts.size(); i++) {
                int end = (i + 1 < (int)_pageStarts.size()) ? _pageStarts[i + 1] : (int)_all.size();
                if (_pageAnchor >= _pageStarts[i] && _pageAnchor < end) {
                    _curPage = i;
                    break;
                }
            }
            _pageAnchor = -1;
        }
        if (_curPage < 0) _curPage = 0;
        if (_curPage >= (int)_pageStarts.size()) _curPage = (int)_pageStarts.size() - 1;
        _pageStart = _pageStarts[_curPage];
        int end = (_curPage + 1 < (int)_pageStarts.size()) ? _pageStarts[_curPage + 1] : (int)_all.size();
        for (int i = _pageStart; i < end; i++) _page.push_back(_all[i]);
    } else {
        // 退化: 未注册宽度回调时按固定每页数量分页
        _pageStarts.clear();
        for (int i = 0; i < (int)_all.size(); i += _pageSize) _pageStarts.push_back(i);
        if (_pageAnchor >= 0) {
            _curPage = _pageAnchor / std::max(1, _pageSize);
            _pageAnchor = -1;
        }
        if (_curPage < 0) _curPage = 0;
        if (_curPage >= (int)_pageStarts.size()) _curPage = (int)_pageStarts.size() - 1;
        _pageStart = _pageStarts[_curPage];
        for (int i = _pageStart; i < (int)_all.size() && (int)_page.size() < _pageSize; i++)
            _page.push_back(_all[i]);
    }
    int64_t pageUs = IME_PERF_NOW() - pageStartUs;
    if (pageUs >= IME_PERF_SLOW_US) {
        ESP_LOGW(IME_TAG, "perf page code='%s' total=%lldus all=%u page=%u fixed=%d width=%d",
                 _code.c_str(), (long long)pageUs, (unsigned)_all.size(),
                 (unsigned)_page.size(), _fixedCandidatePaging ? 1 : 0, _displayWidth);
    }
}

bool IME::pagePrev() {
    if (_curPage <= 0) return false;
    if (_curPage - 1 < (int)_pageStarts.size()) _pageAnchor = _pageStarts[_curPage - 1];
    _curPage--;
    buildPage();
    return true;
}

bool IME::pageNext() {
    if (_curPage + 1 >= (int)_pageStarts.size()) return false;
    _pageAnchor = _pageStarts[_curPage + 1];
    _curPage++;
    buildPage();
    return true;
}

bool IME::commit(int idx, std::string &out, bool bySpace) {
    if (idx < 0 || idx >= (int)_page.size()) return false;
    out = _page[idx];
    if (_vMode || _englishCompose) {
        _lastCommitChar.clear();
        _lastCommitText.clear();
        reset();
        return true;
    }
    if (_deleteMode) {
        bool deleted = false;
        auto eraseMatch = [&](bool requireCode) -> bool {
            for (size_t i = 0; i < _dynamicUserWords.size(); i++) {
                const UserEntry &e = _dynamicUserWords[i];
                if (e.word == out && e.trad == _trad &&
                    (!requireCode || e.code == _code)) {
                    removeDynamicUserWordAt(i);
                    _dynamicUserDirty = true;
                    flushUserDictSaves(false);
                    penalizePredictWord(out, 3);
                    rememberDeletedWord(out);
                    deleted = true;
                    return true;
                }
            }
            return false;
        };
        if (!eraseMatch(true)) {
            eraseMatch(false);
        }
        _statusMessage = deleted ? ("已删除:" + out) : ("未找到动态词:" + out);
        out.clear();
        reset();
        return true;
    }
    int partialRel = _partialStart - _pageStart;
    bool partial = (_remainder.length() > 0 && idx >= partialRel);
    int pLen = pinyinPrefixLen(_code);
    // 大写后缀才拼进输出; 撇号码("xi'an")剩余部分是纯小写+分隔符, 不能当后缀
    bool hasUpperSuffix = false;
    for (int i = pLen; i < (int)_code.length(); i++) {
        if (_code[i] >= 'A' && _code[i] <= 'Z') { hasUpperSuffix = true; break; }
    }
    // Use per-candidate code length from _candLen for continuation
    int candIdx = idx + _pageStart;
    int consumedLen = (candIdx < (int)_candLen.size()) ? _candLen[candIdx] : 0;
    int learnWeight = 1 + std::min(_curPage, 2);
    bool candContinue = (!partial && consumedLen > 0
                         && consumedLen < (int)_code.length()
                         && consumedLen <= 17);
    if (partial || candContinue) {
        if (!partial)
            _remainder = _code.substr(consumedLen);
        if (_remainder.length() == 0 || _remainder.length() >= _code.length()) {
            _prefix.clear();
            _displayCodeDirty = true;
            _remainder.clear();
            reset();
            return true;
        }
        _prefix += out;
        _displayCodeDirty = true;
        _code = _remainder;
        _remainder.clear();
        _partialStart = 0;
        _maxMatchLen = 0;
        out.clear();
        lookup();
        return false;
    }
    if (_prefix.length() > 0) {
        std::string learnCode = _codeOrig;
        _prefix += out;
        _recentSingleCommits.clear();
        if (hasUpperSuffix) _prefix += _code.substr(pLen);
        _displayCodeDirty = true;
        if (!hasUpperSuffix) {
            bumpFrequency(learnCode, _prefix, learnWeight);
            for (auto &code : alternateLearningCodes(learnCode)) bumpFrequency(code, _prefix);
            rememberReplacementPreference(learnCode, _prefix);
            rememberRecentCommit(learnCode, _prefix);
            rememberLastLearning(learnCode, _prefix);
        }
        out = _prefix;
    } else {
        std::string learnCode = _code;
        if (hasUpperSuffix) out += _code.substr(pLen);
        if (!hasUpperSuffix) {
            bumpFrequency(learnCode, out, learnWeight);
            for (auto &code : alternateLearningCodes(learnCode)) bumpFrequency(code, out);
            learnAutoPhraseFromSingle(learnCode, out);
            rememberReplacementPreference(learnCode, out);
            rememberRecentCommit(learnCode, out);
            rememberLastLearning(learnCode, out);
        } else {
            _recentSingleCommits.clear();
        }
    }
    _prefix.clear();
    _displayCodeDirty = true;
    _codeOrig.clear();
    std::string predictKey = predictKeyForCommittedText(out);
    rememberCommittedText(out);
    reset();
    beginPredict(predictKey, bySpace);
    return true;
}

bool IME::handleFullwidthPunct(int key, std::string &out) {
    // Map ASCII punctuation to fullwidth equivalents when IME is active
    // Only convert specific punctuation, others remain half-width
    auto punctDone = [&]() {
        _recentSingleCommits.clear();
        return true;
    };
    if (_lastAsciiCommitUs > 0 && esp_timer_get_time() - _lastAsciiCommitUs < 8000000 &&
        (key == '.' || key == ',' || key == ':' || key == ';' || key == '!' || key == '?' ||
         key == '-' || key == '/' || key == '@')) {
        out.assign(1, (char)key);
        return punctDone();
    }
    switch (key) {
    case ',':  out = "，"; return punctDone(); // ，
    case '.':  out = "。"; return punctDone(); // 。
    case '?':  out = "？"; return punctDone(); // ？
    case ';':  out = "；"; return punctDone(); // ；
    case ':':  out = "："; return punctDone(); // ：
    case '!':  out = "！"; return punctDone(); // ！
    case '(':  out = "（"; return punctDone(); // （
    case ')':  out = "）"; return punctDone(); // ）
    case '[':  out = "【"; return punctDone(); // 【
    case ']':  out = "】"; return punctDone(); // 】
    case '{':  out = "「"; return punctDone(); // 「
    case '}':  out = "」"; return punctDone(); // 」
    case '\\': out = "、"; return punctDone(); // 、
    case '^':  out = "……"; return punctDone(); // ……
    case '<':  out = "《"; return punctDone(); // 《
    case '>':  out = "》"; return punctDone(); // 》
    case '`':  out = "·"; return punctDone(); // ·
    case '_':  out = "——"; return punctDone(); // ——
    case '$':  out = "¥"; return punctDone(); // ¥
    case '\'':
        // Single quote pairing: first press = ‘, second press = ’
        if (_singleQuoteOpen) {
            out = "’";
            _singleQuoteOpen = false;
        } else {
            out = "‘";
            _singleQuoteOpen = true;
        }
        return punctDone();
    case '"':
        // Double quote pairing: first press = “, second press = ”
        if (_doubleQuoteOpen) {
            out = "”";
            _doubleQuoteOpen = false;
        } else {
            out = "“";
            _doubleQuoteOpen = true;
        }
        return punctDone();
    default:   return false; // Other characters remain half-width
    }
}

bool IME::handleFullwidthChar(int key, std::string &out) {
    // Fullwidth mode: map ASCII letters, digits, space, and remaining symbols to fullwidth
    if (key >= 'A' && key <= 'Z') {
        // U+FF21 = fullwidth A
        uint32_t cp = 0xFF21 + (key - 'A');
        appendUtf8(cp, out);
        return true;
    }
    if (key >= 'a' && key <= 'z') {
        // U+FF41 = fullwidth a
        uint32_t cp = 0xFF41 + (key - 'a');
        appendUtf8(cp, out);
        return true;
    }
    if (key >= '0' && key <= '9') {
        // U+FF10 = fullwidth 0
        uint32_t cp = 0xFF10 + (key - '0');
        appendUtf8(cp, out);
        return true;
    }
    if (key == ' ') {
        // U+3000 = ideographic space (fullwidth space)
        appendUtf8(0x3000, out);
        return true;
    }
    // Remaining printable ASCII not already handled by handleFullwidthPunct
    if (key >= 0x21 && key <= 0x7E) {
        // U+FF01 = fullwidth !, offset from '!' is key - 0x21
        uint32_t cp = 0xFF01 + (key - 0x21);
        appendUtf8(cp, out);
        return true;
    }
    return false;
}

bool IME::handleKey(int key, std::string &out) {
    if (!_active) return false;
    flushUserDictSaves(false);
    if (_english && !_englishCompose) {
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
            _englishCompose = true;
            _code = (char)key;
            _displayCodeDirty = true;
            lookupEnglishMode();
            return true;
        }
        return false;
    }
    if (_englishCompose) {
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
            key == '\'' || key == '-' || key == '_') {
            if ((int)_code.length() < 32) {
                _code += (char)key;
                _displayCodeDirty = true;
                lookupEnglishMode();
            }
            return true;
        }
        if (key >= '1' && key <= '9') { commit(key - '1', out); return true; }
        if (key == ' ') {
            if (_page.size() > 0) commit(0, out);
            else { out = _code; _lastCommitChar.clear(); _lastCommitText.clear(); reset(); }
            return true;
        }
        if (key == '\n') { out = _code; _lastCommitChar.clear(); _lastCommitText.clear(); reset(); return true; }
        if (isAsciiPunctKey(key)) {
            if (_page.size() > 0) commit(0, out);
            else { out = _code; _lastCommitChar.clear(); _lastCommitText.clear(); reset(); }
            out += (char)key;
            return true;
        }
        if (key == '\b') {
            if (_code.length() > 0) _code.erase(_code.length() - 1);
            _displayCodeDirty = true;
            if (_code.empty()) reset();
            else lookupEnglishMode();
            return true;
        }
        if (key == 27) { reset(); return true; }
        if (isEnglishPagePrevKey(key)) { pagePrev(); return true; }
        if (isEnglishPageNextKey(key)) { pageNext(); return true; }
        if (_page.size() > 0) commit(0, out);
        else { out = _code; _lastCommitChar.clear(); _lastCommitText.clear(); }
        reset();
        return true;
    }
    if (_vMode) {
        if (_code == "v" && key >= 0x21 && key <= 0x7E &&
            !((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
              (key >= '0' && key <= '9') || key == '/')) {
            out.assign(1, (char)key);
            _lastCommitChar.clear();
            _lastCommitText.clear();
            reset();
            return true;
        }
        // 数字键直选候选: v/字母编码(含闭合 v/编码/ 与命令 v/t/ v/d/ v/w/)。
        // 裸v与数字/日期编码不拦截, 数字键继续进编码(如 v/5、v2026-9-7)。
        bool vDigitSel = false;
        if (!_page.empty() && _code.length() > 2 && _code[1] == '/') {
            std::string q = _code.substr(2);
            if (!q.empty() && q.back() == '/') q.pop_back();
            vDigitSel = !q.empty();
            for (char c : q) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { vDigitSel = false; break; }
            }
        }
        if (vDigitSel && key >= '1' && key <= '9') { commit(key - '1', out); return true; }
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
            (key >= '0' && key <= '9') || key == '.' || key == '-' ||
            key == '/' || key == '!') {
            if ((int)_code.length() < 32) {
                _code += (char)key;
                _displayCodeDirty = true;
                lookupVMode();
            }
            return true;
        }
        if (key == IME_KEY_LEFT) {
            if (!_page.empty()) _vSel = (_vSel + (int)_page.size() - 1) % (int)_page.size();
            return true;
        }
        if (key == IME_KEY_RIGHT) {
            if (!_page.empty()) _vSel = (_vSel + 1) % (int)_page.size();
            return true;
        }
        if (key == ' ') {
            if (_page.size() > 0) commit(_vSel, out);
            else { out = _code.length() > 1 ? _code.substr(1) : ""; _lastCommitChar.clear(); _lastCommitText.clear(); reset(); }
            return true;
        }
        if (key == '\n') {
            out = _page.size() > 0 ? _page[_vSel] : (_code.length() > 1 ? _code.substr(1) : "");
            _lastCommitChar.clear();
            _lastCommitText.clear();
            reset();
            return true;
        }
        if (key == '\b') {
            if (_code.length() > 1) {
                _code.erase(_code.length() - 1);
                _displayCodeDirty = true;
                lookupVMode();
            } else reset();
            return true;
        }
        if (key == 27) { reset(); return true; }
        if (isVModePagePrevKey(key)) { pagePrev(); return true; }
        if (isVModePageNextKey(key)) { pageNext(); return true; }
        return true;
    }
    if (_predicting) {
        if (key == 0x04) {
            if (!_page.empty()) {
                std::string word = _page[0];
                std::string rejectKey = (_pageStart >= 0 && _pageStart < (int)_predictCandidateKeys.size() &&
                                         !_predictCandidateKeys[_pageStart].empty())
                    ? _predictCandidateKeys[_pageStart] : _predChar;
                rejectPredictWord(rejectKey, word);
                for (int i = (int)_all.size() - 1; i >= 0; i--) {
                    if (_all[i] == word) {
                        _all.erase(_all.begin() + i);
                        if (i < (int)_candLen.size()) _candLen.erase(_candLen.begin() + i);
                        if (i < (int)_candidateWidths.size()) _candidateWidths.erase(_candidateWidths.begin() + i);
                        if (i < (int)_predictCandidateKeys.size())
                            _predictCandidateKeys.erase(_predictCandidateKeys.begin() + i);
                    }
                }
                _candidateHashCount = 0;
                _pageAnchor = std::min(_pageStart, std::max(0, (int)_all.size() - 1));
                if (_all.empty()) reset();
                else buildPage();
            }
            return true;
        }
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
            _predicting = false;
            char cl = (char)tolower(key);
            if (!_deleteMode && !_lfMode && key >= 'A' && key <= 'Z') {
                _englishCompose = true;
                _code = (char)key;
                _displayCodeDirty = true;
                lookupEnglishMode();
                return true;
            }
            if (!_deleteMode && !_lfMode && cl == 'v') {
                _vMode = true;
                _code = "v";
                _displayCodeDirty = true;
                lookupVMode();
                return true;
            }
#if PJOURNAL_IME_ENABLE_LIANGFEN
            if (!_deleteMode && !_lfMode && cl == 'u') {
                loadLfDict();
                if (_lfBlob) { _lfMode = true; _maxCode = 12; return true; }
            }
#endif
            _code = (char)key;
            _displayCodeDirty = true;
            lookup();
            return true;
        }
        if (key >= '1' && key <= '9') {
            int idx = key - '1';
            if (idx < (int)_page.size()) {
                int candIdx = _pageStart + idx;
                std::string predKey = (candIdx >= 0 && candIdx < (int)_predictCandidateKeys.size() &&
                                       !_predictCandidateKeys[candIdx].empty())
                    ? _predictCandidateKeys[candIdx] : _predChar;
                out = _page[idx];
                int learnWeight = 4 + std::min(_curPage, 2);
                _predicting = false;
                bumpPredictFrequency(predKey, out, false, learnWeight);
                rememberReplacementPreference(predKey, out);
                rememberLastLearning(predKey, out);
                rememberCommittedText(out);
                beginPredict(predictKeyForCommittedText(out), false);
            }
            return true;
        }
        if (key == ' ') {
            if (_page.size() > 0) {
                std::string predKey = (_pageStart >= 0 && _pageStart < (int)_predictCandidateKeys.size() &&
                                       !_predictCandidateKeys[_pageStart].empty())
                    ? _predictCandidateKeys[_pageStart] : _predChar;
                out = _page[0];
                int learnWeight = 4 + std::min(_curPage, 2);
                _predicting = false;
                bumpPredictFrequency(predKey, out, false, learnWeight);
                rememberReplacementPreference(predKey, out);
                rememberLastLearning(predKey, out);
                rememberCommittedText(out);
                beginPredict(predictKeyForCommittedText(out), true);
            }
            return true;
        }
        if (isAsciiPunctKey(key) && key != '-' && key != '=') {
            out = imePunctForKey(key);
            _predicting = false;
            clearLearningContext();
            return true;
        }
        if (isPredictPagePrevKey(key)) { pagePrev(); return true; }
        if (isPredictPageNextKey(key)) { pageNext(); return true; }
        if (key == '\b' || key == 27 || key == '\n') {
            _predicting = false;
            clearLearningContext();
            return true;
        }
        _predicting = false;
        return false;
    }
    // In fullwidth mode with no composition, output letters/digits/space as fullwidth
    if (_fullwidth && _code.length() == 0 && !_deleteMode && !_lfMode && !_vMode) {
        if (((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) ||
            (key >= '0' && key <= '9') || key == ' ') {
            bool handled = handleFullwidthChar(key, out);
            if (handled) clearLearningContext();
            return handled;
        }
    }
    if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
        char cl = (char)tolower(key);
        char c = (char)key;
        if (_code.length() == 0 && !_deleteMode && !_lfMode && key >= 'A' && key <= 'Z') {
            _englishCompose = true;
            _code = (char)key;
            _displayCodeDirty = true;
            lookupEnglishMode();
            return true;
        }
        if (_code.length() == 0 && !_deleteMode && !_lfMode && cl == 'v') {
            _vMode = true;
            _code = "v";
            _displayCodeDirty = true;
            lookupVMode();
            return true;
        }
#if PJOURNAL_IME_ENABLE_LIANGFEN
        if (_code.length() == 0 && !_deleteMode && !_lfMode && cl == 'u') {
            loadLfDict();
            if (_lfBlob) { _lfMode = true; _maxCode = 12; return true; }
        }
#endif
        if ((int)_code.length() < _maxCode) {
            _code += c;
            _displayCodeDirty = true;
            lookup();
        }
        return true;
    }
    if (_code.length() == 0) {
        if (handleFullwidthPunct(key, out)) { clearLearningContext(); return true; }
        if (_fullwidth && handleFullwidthChar(key, out)) { clearLearningContext(); return true; }
        return false;
    }
    // 单引号编码分词: 拼音模式下 ' 显式分隔音节(如 xi'an); 两分模式保留翻页
    if (key == '\'' && !_lfMode) {
        if (_code.back() == '\'') return true;
        if ((int)_code.length() < _maxCode) {
            _code += '\'';
            _displayCodeDirty = true;
            lookup();
        }
        return true;
    }
    if (key >= '1' && key <= '9') {
        commit(key - '1', out);
        return true;
    }
    if (key == ' ') {
        if (_page.size() > 0) commit(0, out, true);
        else reset();
        return true;
    }
    if (key == '\n') {
        out = _code;
        clearLearningContext();
        reset();
        return true;
    }
    if (key == '\b') {
        _recentSingleCommits.clear();
        if (_prefix.length() > 0) {
            _code = _codeOrig;
            if (_code.length() > 0) {
                _code.erase(_code.length() - 1);
            }
            _prefix.clear();
            _remainder.clear();
            _partialStart = 0;
            _maxMatchLen = 0;
            _displayCodeDirty = true;
            if (_code.length() == 0) reset();
            else lookup();
        } else if (_code.length() > 0) {
            _code.erase(_code.length() - 1);
            _displayCodeDirty = true;
            if (_code.length() == 0) reset();
            else lookup();
        }
        return true;
    }
    if (key == 27) {
        clearLearningContext();
        reset();
        return true;
    }
    if (isStandardPagePrevKey(key)) { pagePrev(); return true; }
    if (isStandardPageNextKey(key)) { pageNext(); return true; }
    if (_page.size() > 0) {
        commit(0, out);
        return true;
    }
    reset();
    return true;
}
