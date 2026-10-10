#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include "ime_config.h"
#include "yong_dict.h"

class IME {
public:
    enum Scheme { WUBI = 0, PINYIN = 1, SHUANGPIN = 2 };

    bool begin();
    bool loaded() const { return _loaded; }
    Scheme scheme() const { return _scheme; }

    bool active() const { return _active; }
    void setActive(bool on);
    void toggle() { setActive(!_active); }

    bool fullwidth() const { return _fullwidth; }
    void toggleFullwidth() { _fullwidth = !_fullwidth; }
    void setFullwidth(bool on) { _fullwidth = on; }

    bool trad() const { return _trad; }
    void toggleTrad() { _trad = !_trad; buildPage(); }
    void setTrad(bool on) { _trad = on; buildPage(); }

    bool english() const { return _english; }
    void toggleEnglish() { setEnglish(!_english); }
    void setEnglish(bool on) { _english = on; if (on) reset(); }

    bool handleKey(int key, std::string &out);

    std::string displayCode() const {
        if (_displayCodeDirty) {
            _displayCodeCache = _prefix + _code;
            _displayCodeDirty = false;
        }
        return _displayCodeCache;
    }
    const std::string &composition() const { return _code; }
    const std::vector<std::string> &candidates() const { return _page; }
    // 页内高亮候选下标(v 模式始终可用; 普通候选由设置控制)
    int highlightIdx() const {
        return (_vMode || (_highlightSelectMode && !_englishCompose && !_predicting)) ? _sel : -1;
    }
    bool composing() const { return _code.length() > 0 || _predicting || _lfMode || _deleteMode || _vMode || _englishCompose; }

    bool isLfMode() const { return _lfMode; }
    bool isDeleteMode() const { return _deleteMode; }
    std::string modeLabel() const;
    void clearLearningContext();
    // 文档级上下文: 编辑器喂入正文尾部(约 200 字), 已出现在正文里的词在小范围加分
    void setDocumentContext(const std::string &text);
    void toggleDeleteMode();
    void setDeleteMode(bool on);
    std::string takeStatusMessage() {
        std::string msg = _statusMessage;
        _statusMessage.clear();
        return msg;
    }

    void beginPredict(const std::string &text, bool afterSpaceCommit = false);
    void endPredict() { _predicting = false; _predChar = ""; }
    bool predicting() const { return _predicting; }
    void handleHostBackspace();
    void cancelComposition() { reset(); }
    void flushUserDictSavesNow() { flushUserDictSaves(true); }
    // 空闲落盘: 按键路径只标脏不写 SD, 由主循环在用户静默后调用(见 IME.cpp 说明)。
    // 现在它只做序列化(纯 CPU)并入队, SD 写由写盘任务完成 —— 不再阻塞主循环。
    void flushUserDictSavesIdle();
    // 等待异步写盘队列排空。强制落盘路径(休眠/退出/关闭输入法)必须调它:
    // 那些时刻之后进程可能就不再调度了, 队列里没写完的任务要等它写完。
    // 超时返回 false(数据仍在队列里, 下次启动会从 journal 恢复)。
    bool waitUserDictWritesDrained(int timeout_ms);
    // 是否有待落盘的用户词典改动。主循环用它避免空闲时白调一次。
    bool hasPendingUserDictWrites() const {
        return _dynamicUserDirty || _userPredictDirty || _fixedUserDirty ||
               _userPredictRejectDirty || !_pendingUserDictJournal.empty();
    }

    enum UserDictKind { FIXED_DICT = 0, DYNAMIC_DICT = 1, PREDICT_DICT = 2 };
    struct UserEntryView { std::string code; std::string word; int count; bool trad = false; };
    const std::vector<UserEntryView> userDictEntries(UserDictKind kind) const;
    bool addUserDictEntry(UserDictKind kind, const std::string &code, const std::string &word);
    bool addUserDictEntry(UserDictKind kind, const std::string &code, const std::string &word,
                          int count, bool trad);
    int addUserDictEntries(UserDictKind kind, const std::vector<UserEntryView> &items,
                           int *skipped = nullptr);
    void removeUserDictEntries(UserDictKind kind, const std::vector<int> &indices);
    void clearUserDict(UserDictKind kind);
    size_t userDictSize(UserDictKind kind) const;
    void ensureUserDictLoaded();

    void removeUserWord(const std::string &code, const std::string &word);
    void clearUserDict();
    void pruneUserDict(int minCount = 0);
    size_t userDictSize() const { return _dynamicUserWords.size(); }

    static IME &getInstance() {
        static IME instance;
        return instance;
    }
    IME(const IME &) = delete;
    IME &operator=(const IME &) = delete;

    void setPageSize(int n) { _pageSize = n; }
    // 返回当前页实际候选数量(界面用 (i % pageSize)+1 编号, 页内从 1 起)
    int pageSize() const { int n = (int)_page.size(); return n >= 1 ? n : 1; }
    int totalCandidates() const { return (int)_all.size(); }
    int totalPages() const { return _pageStarts.empty() ? 1 : (int)_pageStarts.size(); }
    int currentPage() const { return _curPage + 1; }

    using WidthFn = int (*)(const char *text);
    void setWidthFn(WidthFn fn) { _widthFn = fn; }
    // 候选行可用像素宽度(与各界面渲染 curW+partW+8>SCREEN_W 的 8px 余量一致)
    void setDisplayWidth(int w) { _displayWidth = w; }

private:
    IME() {}

    static const int HEADER_SIZE = 12;
    static const int HANZI_SIZE = 3;
    static const int FLAG_SIZE = 1;
    int _codeLen = 6;
    int _recordSize = 6 + HANZI_SIZE + FLAG_SIZE;
    int _maxCode = 4;
    Scheme _scheme = WUBI;

    static const int INDEX_ENTRIES = 26 * 26 + 1; // 677
    static const int MAX_CODE_LEN = 6;
    static const int MAX_CANDIDATES = 300;

    bool _loaded = false;
    bool _active = false;

    const uint8_t *_blob = nullptr;
    size_t _blobSize = 0;
    ime::Im3Dictionary _dict;
    uint32_t _count = 0;
    size_t _recordBase = HEADER_SIZE + INDEX_ENTRIES * 4;

    bool _predicting = false;
    std::string _predChar;
    std::string _lastCommitChar;
    std::string _lastCommitText;
    std::string _statusMessage;
    int _partialStart = 0;
    int _maxMatchLen = 0;
    std::string _prefix;
    std::string _remainder;
    std::string _codeOrig;

    struct UserEntry { std::string code; std::string word; int count; bool trad = false; std::string initial; };
    struct PendingJournalEntry { std::string path; UserEntry entry; };
    std::vector<UserEntry> _fixedUserWords;
    std::vector<UserEntry> _dynamicUserWords;
    std::vector<UserEntry> _userPredictWords;
    std::vector<UserEntry> _userPredictRejectWords;
    std::vector<PendingJournalEntry> _pendingUserDictJournal;
    std::unordered_map<int, std::vector<uint16_t>> _fixedUserCodeIndex;
    std::unordered_map<int, std::vector<uint16_t>> _dynamicUserCodeIndex;
    std::unordered_map<int, std::vector<uint16_t>> _fixedUserInitialIndex;
    std::unordered_map<int, std::vector<uint16_t>> _dynamicUserInitialIndex;
    std::unordered_map<std::string, std::vector<uint16_t>> _fixedUserCodePrefixIndex;
    std::unordered_map<std::string, std::vector<uint16_t>> _dynamicUserCodePrefixIndex;
    std::unordered_map<std::string, std::vector<uint16_t>> _fixedUserInitialPrefixIndex;
    std::unordered_map<std::string, std::vector<uint16_t>> _dynamicUserInitialPrefixIndex;
    std::unordered_map<std::string, std::vector<uint16_t>> _userPredictIndex;
    bool _userWordIndexesDirty = true;
    // Last caller to invalidate the positional lookup indexes; reported by the
    // "perf rebuild" log so a stray invalidation is attributable. PERF_LOG only.
    const char *_indexDirtyReason = nullptr;
    bool _userPredictIndexDirty = true;
    bool _fixedUserDirty = false;
    bool _dynamicUserDirty = false;
    bool _userPredictDirty = false;
    bool _userPredictRejectDirty = false;
    bool _userDictLoaded = false;
    int64_t _deferredUserDictSinceUs = 0;
    int64_t _pendingUserDictJournalSinceUs = 0;
    // 空闲落盘失败后的退避截止时刻。见 flushUserDictSavesIdle 的说明。
    int64_t _idleFlushBackoffUntilUs = 0;
    void loadUserDict();
    bool loadUserDictFile(const char *path, std::vector<UserEntry> &entries, bool &dirty, size_t maxEntries);
    void saveUserDictFile(const char *path, std::vector<UserEntry> &entries, bool &dirty);
    void loadUserDictJournal(const char *path, std::vector<UserEntry> &entries,
                             bool &dirty, size_t maxEntries);
    void appendUserDictJournal(const char *path, const UserEntry &entry);
    void queueUserDictJournal(const char *path, const UserEntry &entry);
    void flushUserDictJournal(bool force);
    void clearUserDictJournal(const char *path);
    void markUserDictDirty(bool &dirty, const char *path = nullptr, const UserEntry *entry = nullptr);
    void flushUserDictSaves(bool force);
    void markUserWordIndexesDirty(const char *reason = nullptr);
    void rebuildUserWordIndexes();
    void rebuildUserPredictIndex();
    // Push one entry's keys into the four lookup maps (push-only, no sort/clear).
    static void indexUserWordEntry(uint16_t idx, const UserEntry &entry,
                                   std::unordered_map<int, std::vector<uint16_t>> &codeIndex,
                                   std::unordered_map<int, std::vector<uint16_t>> &initialIndex,
                                   std::unordered_map<std::string, std::vector<uint16_t>> &codePrefixIndex,
                                   std::unordered_map<std::string, std::vector<uint16_t>> &initialPrefixIndex);
    static void sortUserIndexMaps(const std::vector<UserEntry> &entries,
                                  std::unordered_map<int, std::vector<uint16_t>> &codeIndex,
                                  std::unordered_map<int, std::vector<uint16_t>> &initialIndex,
                                  std::unordered_map<std::string, std::vector<uint16_t>> &codePrefixIndex,
                                  std::unordered_map<std::string, std::vector<uint16_t>> &initialPrefixIndex);
    // Append-path variant of the above. A bucket is count-sorted after the last full
    // rebuild and appends only ever push onto its tail, so the only possible disorder
    // is that tail pair. Re-sorting just those buckets skips a std::stable_sort (which
    // allocates a temporary buffer even for 2-element buckets) on every other bucket.
    static void sortUserIndexMapsAfterAppend(const std::vector<UserEntry> &entries,
                                             std::unordered_map<int, std::vector<uint16_t>> &codeIndex,
                                             std::unordered_map<int, std::vector<uint16_t>> &initialIndex,
                                             std::unordered_map<std::string, std::vector<uint16_t>> &codePrefixIndex,
                                             std::unordered_map<std::string, std::vector<uint16_t>> &initialPrefixIndex);
    // Index one already-appended dynamic entry without a full rebuild. Falls back to
    // a full rebuild (leaving the dirty flag set) if the maps are stale.
    void appendUserWordIndexEntry(size_t entryIdx);
    void bumpFrequency(const std::string &code, const std::string &word, int weight = 1);
    bool penalizeUserWord(const std::string &code, const std::string &word, int weight = 2);
    // 删掉动态词库下标 i 的条目: 末尾条目顶上来(swap-and-pop)并把索引里的下标就地改写,
    // 不做整表重建。penalize 与 delete 模式删词都走这里。
    void removeDynamicUserWordAt(size_t i);
    void bumpPredictFrequency(const std::string &key, const std::string &word, bool saveNow = true, int weight = 1);
    bool penalizePredictWord(const std::string &word, int weight = 2);
    bool rejectedPredictWord(const std::string &key, const std::string &word) const;
    void rejectPredictWord(const std::string &key, const std::string &word);
    void learnPredictPairs(const std::string &text);
    void learnAutoPhraseFromSingle(const std::string &code, const std::string &word);
    void rememberCommittedText(const std::string &text);
    void rememberRecentCommit(const std::string &code, const std::string &word);
    int recentCommitBoost(const std::string &code, const std::string &word) const;
    void appendRecentCommitCandidates(const std::string &code,
                                      const std::vector<std::string> &aliasCodes,
                                      int typedLen);
    bool recentlyDeletedWord(const std::string &word) const;
    bool recentlyDeletedWordHash(uint32_t hash) const;
    void rememberDeletedWord(const std::string &word);
    bool confirmNewUserWordLearning(const std::string &code, const std::string &word, int weight);
    void rememberLastLearning(const std::string &code, const std::string &word);
    void rememberReplacementPreference(const std::string &code, const std::string &word);
    int contextCandidateBoost(const std::string &word);
    void rebuildContextBoostScores();
    void appendShortcutSymbol(const char *code, int len);
    int stableCandidateBoost(const std::string &word) const;
    void rememberCandidateStability();
    void rebuildRecentBoostIndex();
    void logCandidateDebug(const char *stage, const std::string &word, int score, int context, int stable) const;
    static bool compactUserEntries(std::vector<UserEntry> &entries, size_t limit);

    bool _deleteMode = false;
    bool _vMode = false;
    std::vector<std::pair<std::string, std::string>> _pendingUserWordLearns;
    std::vector<std::pair<std::string, std::string>> _recentSingleCommits;
    std::vector<std::pair<std::string, std::string>> _recentCommittedWords;
    // word -> indices into _recentCommittedWords, ascending. Rebuilt only when the
    // list changes (on commit), so recentCommitBoost() is an O(1) lookup per candidate.
    std::unordered_map<std::string, std::vector<uint8_t>> _recentBoostByWord;
    std::vector<std::string> _recentDeletedWords;
    std::vector<uint32_t> _recentDeletedHashes;
    std::string _lastLearningCode;
    std::string _lastLearningWord;
    std::string _lastLearningContext;
    int64_t _lastLearningUs = 0;
    std::string _lastRejectedCode;
    std::string _lastRejectedWord;
    std::string _lastRejectedContext;
    int64_t _lastRejectedUs = 0;
    // Previous screen candidates, kept as hashes so per-keystroke rebuild does not
    // churn the heap (a map/vector of strings would malloc+free every lookup).
    static const int STABLE_BOOST_SLOTS = 24;
    uint32_t _stableBoostHashes[STABLE_BOOST_SLOTS] = {};
    int16_t _stableBoostValues[STABLE_BOOST_SLOTS] = {};
    int _stableBoostCount = 0;
    int64_t _lastAsciiCommitUs = 0;
    int _sel = 0;  // 页内高亮候选(左右键移动)
    bool _englishCompose = false;
    bool _englishDictLoaded = false;
    std::vector<std::string> _englishWords;
    // Snapshot of the hidden ime_debug setting, taken once in begin() while the SD
    // card is already mounted. logCandidateDebug() runs per candidate inside the
    // hottest scan loops, where a settings mutex + map lookup is measurable (and its
    // first call otherwise paid three stat() plus a failed fopen on the SD card).
    // Enabling the setting therefore requires a reboot.
    bool _imeDebugLog = false;
    // Snapshot of the ime_sentence toggle, taken in begin() for the same reason as
    // _imeDebugLog: the phase runs inside the hot lookup path.
    bool _sentenceMode = true;
    bool _highlightSelectMode = false;  // begin() 快照的 ime_candidate_highlight
#if PJOURNAL_IME_ENABLE_LIANGFEN
    bool _lfMode = false;
    const uint8_t *_lfBlob = nullptr;
    uint32_t _lfCount = 0;
    size_t _lfRecordBase = 0;
    std::vector<uint16_t> _lfIndex;
    void loadLfDict();
    void searchLfWindow(const char *code, int len, uint32_t &lo, uint32_t &hi);
    bool readLfCode(uint16_t i, char out[13]);
    bool readLfHanzi(uint16_t i, char out[4]);
#else
    bool _lfMode = false;
    void loadLfDict() {}
#endif

    void searchWindow(const char *code, int len, uint32_t &lo, uint32_t &hi);
    void wordWindowCached(const char *code, int len, size_t &lo, size_t &hi);
    static int pinyinPrefixLen(const std::string &code);
    bool parseHeader(const uint8_t *hdrIndex, size_t total);
    bool readCode(uint32_t i, char out[MAX_CODE_LEN + 1]);
    bool readHanzi(uint32_t i, char out[HANZI_SIZE + 1]);
    uint8_t readRecordFlag(uint32_t i);
#if PJOURNAL_IME_ENABLE_LIANGFEN
    uint8_t readLfFlag(uint16_t i);
#endif

    std::string _code;
    std::vector<std::string> _all;
    uint32_t _candidateHashes[MAX_CANDIDATES] = {};
    size_t _candidateHashCount = 0;
    std::vector<int> _candLen;  // code length per candidate in _all
    std::vector<int> _candidateWidths;  // cached text width parallel to _all (-1=unknown)
    std::vector<std::string> _predictCandidateKeys;  // prediction key parallel to _all in predict mode
    // Scratch for scoring dictionary single chars before they are appended. Held as
    // a member so the pass adds no per-lookup heap traffic (capacity survives clear).
    struct SingleScratchEntry { std::string word; int codeLen = 0; int score = 0; };
    std::vector<SingleScratchEntry> _singleScratch;
    std::vector<std::string> _page;
    int _pageStart = 0;
    int _pageSize = 9;
    int _curPage = 0;                    // 当前页索引(第 _curPage+1 页)
    std::vector<int> _pageStarts;        // 每页起始候选索引; 按实测宽度分页时由 buildPage 重建
    int _pageAnchor = -1;                // 翻页时锚定目标候选, 宽度分页重建后仍停在包含它的页
    WidthFn _widthFn = nullptr;          // 候选文本宽度测量回调
    int _displayWidth = 0;               // 候选行可用像素宽度(0=退化为固定 _pageSize 分页)
    bool _fixedCandidatePaging = false;  // 短辅音输入走固定分页, 避免热路径反复测字宽
    size_t _candidateLimit = MAX_CANDIDATES;
    std::string _singleWindowCacheCode;
    int _singleWindowCacheLen = 0;
    uint32_t _singleWindowCacheLo = 0;
    uint32_t _singleWindowCacheHi = 0;
    std::string _wordWindowCacheCode;
    int _wordWindowCacheLen = 0;
    size_t _wordWindowCacheLo = 0;
    size_t _wordWindowCacheHi = 0;
    std::string _fuzzyConfigCache;
    int64_t _fuzzyConfigCacheUs = 0;
    std::string _contextBoostScoresContext;
    std::unordered_map<std::string, int> _contextBoostScores;

    // 文档级上下文(#13): 只保留正文里 2-3 字 CJK 片段的出现次数, 用固定槽位哈希表
    // (线性探测)存。候选打分循环里按候选调用, 所以查找必须是零分配的——冲突只意味着
    // 某个无关词多拿一点小加分, 与 _stableBoostHashes 的处理同理。
    // 槽位数按 200 字正文最坏 ~400 个片段(2 字 + 3 字各一遍)取 512, 装载因子 0.78,
    // 16 步探测足够(平均成功探测 ~2 步)。只收 2-3 字: 中文词绝大多数是这两个长度。
    static const int DOC_CTX_SLOTS = 512;   // 2 的幂, 掩码即取模
    static const int DOC_CTX_MAX_N = 3;
    uint32_t _docCtxHashes[DOC_CTX_SLOTS] = {};
    uint8_t _docCtxCounts[DOC_CTX_SLOTS] = {};
    bool _docCtxMode = true;  // begin() 快照的 ime_doc_context; 改设置要重启才生效
    void rebuildDocumentContext(const std::string &text);
    int documentContextBoost(const std::string &word) const;
    static uint32_t docCtxHash(const char *p, size_t n);

    // 整句覆盖词图的暂存区。全部做成长期成员, 容量在多次 lookup 间复用, 整句路径
    // 本身就不再产生查找期堆分配(除每条弧的候选文本字符串)。
    struct SentenceArc { uint8_t lo = 0; uint8_t hi = 0; int32_t score = 0; std::string word; };
    struct SentenceNode { int16_t parent = -1; int16_t arc = -1; int32_t score = 0; };
    struct SentenceCand { int16_t parent = -1; int16_t arc = -1; int32_t score = 0; };
    std::vector<SentenceArc> _sentenceArcs;    // 按 lo 升序(每个起点收集一遍); DP 另按 hi 分桶
    std::vector<SentenceNode> _sentenceNodes;  // 已定型的 beam 节点, 按位置切分
    std::vector<int> _sentenceNodeOff;
    std::vector<SentenceCand> _sentenceCands;  // 单个位置收候选时的临时表
    std::vector<int16_t> _sentenceByHi;        // 弧下标按终点分桶(计数排序)
    std::vector<int> _sentenceByHiOff;
    int _sentenceGroupsScanned = 0;

    mutable std::string _displayCodeCache;
    mutable bool _displayCodeDirty = true;

    bool _singleQuoteOpen = false;  // Track single quote pairing state
    bool _doubleQuoteOpen = false;  // Track double quote pairing state
    bool _fullwidth = false;        // Fullwidth character mode
    bool _trad = false;             // Traditional mode: hide simplified-only, show trad counterparts
    bool _english = false;          // Temp English mode: pass keys through as ASCII

    void reset();
    void lookup();
    void lookupSegmented();  // 单引号分词编码的查词路径
    void lookupVMode();
    void lookupKaomoji(const std::string &query);  // v/编码 拼音/声母搜索文字表情
    void lookupEnglishMode();
    void loadEnglishDict();
    bool hasCandidate(const std::string &text, uint32_t hash) const;
    void clearCandidates();
    void rebuildCandidateHashes();
    bool appendCandidate(const std::string &text, int candLen);
    void appendScoredSingleChars(const char *prefix, int qlen, int scanBudget,
                                 bool codeLenFromRecord, int fixedCandLen, size_t cap);
    void appendSingleCharCandidates(const std::string &prefix, int candLen,
                                    size_t cap = 0);  // cap=0 用 _candidateLimit
    void collectSentenceArcs(int pos, const char *code, int len);
    void appendSentenceCandidates(const char *code, int len);
    void buildPage();
    bool pagePrev();
    bool pageNext();
    bool commit(int idx, std::string &out, bool bySpace = false);
    bool handleFullwidthPunct(int key, std::string &out);
    bool handleFullwidthChar(int key, std::string &out);
};
