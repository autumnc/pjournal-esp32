#pragma once
// 拼音分词补充词典：主词典缺失或需显式音节分界的词。
// 单引号编码分词时（如 xi'an、an'guang）与普通编码全码匹配时（xian、anguang）
// 都会查这张表。syllables 用空格分隔音节，word 为对应汉字。
struct SegEntry {
    const char *syllables;  // e.g. "an guang"
    const char *word;       // e.g. "暗光"
};

static const SegEntry SEG_TABLE[] = {
    { "an guang", "暗光" },      // 主词典缺失
    { "xi an", "西安" },          // 主词典仅存联想数据，无词组词条
    { "fang an", "方案" },
    { "dang an", "档案" },
    { "ti an", "提案" },
    { "di an", "堤岸" },
    { "ming an", "命案" },
    { "ping an", "平安" },
    { "xue an", "血案" },
    { "li an", "立案" },          // li'an 与 连/脸(lian) 冲突
    { "yi an", "议案" },
    { "yu an", "预案" },          // yu'an 与 元/愿(yuan) 冲突
    { "bi an", "彼岸" },          // bi'an 与 边/变(bian) 冲突
    { "yan an", "延安" },
    { "xin an", "心安" },
    { "xin ai", "心爱" },
    { "qin ai", "亲爱" },
    { "guan ai", "关爱" },
    { "pi ao", "皮袄" },
    { "chang e", "嫦娥" },
    { "ji e", "饥饿" },           // ji'e 与 借(jie) 冲突
    { "qi e", "企鹅" },           // qi'e 与 且/切(qie) 冲突
    { "ke e", "可恶" },
    { "ji ang", "激昂" },         // ji'ang 与 江(jiang) 冲突
    { "ku ai", "酷爱" },          // ku'ai 与 快(kuai) 冲突
    { "jiao ao", "骄傲" },
    { "tian an men", "天安门" },
    { "dang an guan", "档案馆" },
    { "shu ru", "输入" },
    { "shu ru fa", "输入法" },
    { "hou xuan", "候选" },
    { "hou xuan ci", "候选词" },
    { "yong hu ci", "用户词" },
    { "yong hu ci ku", "用户词库" },
    { "lian xiang", "联想" },
    { "lian xiang ci", "联想词" },
    { "lian xiang ci ku", "联想词库" },
    { "ci ku", "词库" },
    { "ci pin", "词频" },
    { "ci zu", "词组" },
    { "jian pin", "简拼" },
    { "quan pin", "全拼" },
    { "pin yin", "拼音" },
    { "ma biao", "码表" },
    { "bian ma", "编码" },
    { "yin jie", "音节" },
    { "fen ci", "分词" },
    { "gu jian", "固件" },
    { "shua ji", "刷机" },
    { "qi dong", "启动" },
    { "chong qi", "重启" },
    { "ka dun", "卡顿" },
    { "you hua", "优化" },
    { "ban ben", "版本" },
    { "lan ya", "蓝牙" },
    { "jian pan", "键盘" },
    { "dian liang", "电量" },
    { "ping mu", "屏幕" },
    { "she zhi", "设置" },
    { "ri ji", "日记" },
    { "tong bu", "同步" },
    { "wen ben", "文本" },
    { "bian ji", "编辑" },
    { "biao dian", "标点" },
    { "quan jiao", "全角" },
    { "ban jiao", "半角" },
    { "ying wen", "英文" },
    { "wen jian", "文件" },
    { "mu lu", "目录" },
    { "dao chu", "导出" },
    { "dao ru", "导入" },
    { "yu yin", "语音" },
    { "shi bie", "识别" },
    { "wang luo", "网络" },
    { "mi ma", "密码" },
    { "ling gan", "灵感" },
    { "da gang", "大纲" },
    { "ren wu", "任务" },
    { "dai ban", "待办" },
    { "bi ji", "笔记" },
    { "xiu gai", "修改" },
    { "bao cun", "保存" },
    { "sou suo", "搜索" },
};

static const int SEG_TABLE_COUNT = (int)(sizeof(SEG_TABLE) / sizeof(SEG_TABLE[0]));
