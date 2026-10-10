#include "io_probe.h"

#if PJOURNAL_IO_PERF_LOG

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>

static const char *TAG = "IOProbe";

// 小于这个耗时不打印。默认 0 = 全打, 因为诊断固件要的是分布和次数, 不只是慢的。
// 真嫌刷屏就把它调大(单位 us)。
static const int64_t IO_PERF_MIN_US = 0;

IoProbeScope::IoProbeScope(const char *what, const char *detail)
    : what_(what), detail_(detail), t0_(esp_timer_get_time()) {}

IoProbeScope::~IoProbeScope() {
    int64_t us = esp_timer_get_time() - t0_;
    if (us < IO_PERF_MIN_US) return;
    ESP_LOGI(TAG, "PROBE|%-24s|%8lld us|%s", what_, (long long)us,
             detail_ ? detail_ : "");
}

// ── 基准测试 ────────────────────────────────────────────────────────────
//
// 目的: 同一份数据(32KB, 与 predict 词典/一篇日记同量级)分别走 SD 和内部 flash
// 裸分区, 横向比一次。"内部 flash 是不是快得多" 这问题不能靠推 —— NOR 要先擦后写,
// 且擦写期间 cache 关闭、两个核一起停, 代价结构和 SD 完全不同。
//
// 注意: 内部 flash 那组是**裸区读写**, 不含文件系统的元数据和磨损均衡开销, 所以它
// 是内部方案的**下界**(实际用 LittleFS 只会更慢)。SD 那组是真实文件操作, 含 FAT
// 目录项更新与 rename, 是**上界以内的真实值**。两组不在同一抽象层, 看趋势别看绝对值。

namespace {

int64_t nowUs() { return esp_timer_get_time(); }

struct BenchResult {
    const char *what;
    int64_t us;
    bool ok;
};

std::vector<BenchResult> g_bench;

void record(const char *what, int64_t us, bool ok) {
    g_bench.push_back({what, us, ok});
}

// 找一个安全的裸 flash 擦写区域: 所有分区之后、对齐 64KB、且距 flash 末尾留足余量。
// 这块区域不属于任何分区, 应用和 idf.py flash 都不会碰它, 写坏也无副作用。
uint32_t scratchOffset() {
    uint32_t maxEnd = 0;
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    for (; it != nullptr; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        uint32_t end = p->address + p->size;
        if (end > maxEnd) maxEnd = end;
    }
    esp_partition_iterator_release(it);

    uint32_t off = (maxEnd + 0xFFFFu) & ~0xFFFFu;
    uint32_t flashSize = 0;
    if (esp_flash_get_size(nullptr, &flashSize) != ESP_OK || flashSize == 0) return 0;
    if (off + 0x10000u > flashSize) return 0;
    return off;
}

void benchSd(const std::vector<char> &buf) {
    const char *path = "/sdcard/.ioprobe_bench.bin";
    const size_t sz = buf.size();
    char detail[96];

    remove(path);
    // 1) 新建写(要分配新簇) + fsync
    int64_t t0 = nowUs();
    FILE *f = fopen(path, "w");
    bool ok = false;
    if (f) {
        size_t n = fwrite(buf.data(), 1, sz, f);
        fflush(f);
        fsync(fileno(f));
        ok = (n == sz) && (fclose(f) == 0);
    }
    snprintf(detail, sizeof(detail), "%u B 新建 fopen+fwrite+fsync+fclose", (unsigned)sz);
    record("SD 新建写", nowUs() - t0, ok);
    ESP_LOGI(TAG, "BENCH|SD 新建写|%8lld us|%s%s", (long long)g_bench.back().us, detail,
             ok ? "" : "  [失败]");

    // 2) 覆盖写(簇已分配, 只改数据) + fsync —— 和词典整表重写的形态一致
    t0 = nowUs();
    f = fopen(path, "w");
    ok = false;
    if (f) {
        size_t n = fwrite(buf.data(), 1, sz, f);
        fflush(f);
        fsync(fileno(f));
        ok = (n == sz) && (fclose(f) == 0);
    }
    snprintf(detail, sizeof(detail), "%u B 覆盖 fopen+fwrite+fsync+fclose", (unsigned)sz);
    record("SD 覆盖写", nowUs() - t0, ok);
    ESP_LOGI(TAG, "BENCH|SD 覆盖写|%8lld us|%s%s", (long long)g_bench.back().us, detail,
             ok ? "" : "  [失败]");

    // 3) 覆盖写但不 fsync: 隔离出 fsync 本身占多少
    t0 = nowUs();
    f = fopen(path, "w");
    ok = false;
    if (f) {
        size_t n = fwrite(buf.data(), 1, sz, f);
        fflush(f);
        ok = (n == sz) && (fclose(f) == 0);
    }
    record("SD 覆盖写(无fsync)", nowUs() - t0, ok);
    ESP_LOGI(TAG, "BENCH|SD 覆盖写(无fsync)|%8lld us|同上去掉 fsync",
             (long long)g_bench.back().us);

    // 4) 读回
    t0 = nowUs();
    f = fopen(path, "r");
    ok = false;
    if (f) {
        std::vector<char> rb(sz);
        size_t n = fread(rb.data(), 1, sz, f);
        ok = (n == sz) && (fclose(f) == 0);
    }
    record("SD 读", nowUs() - t0, ok);
    ESP_LOGI(TAG, "BENCH|SD 读|%8lld us|%u B 顺序读", (long long)g_bench.back().us,
             (unsigned)sz);

    remove(path);
}

void benchInternalFlash(const std::vector<char> &buf) {
    uint32_t off = scratchOffset();
    if (off == 0) {
        ESP_LOGW(TAG, "BENCH|内部 flash|    跳过|分区表之后没有足够空间做裸写基准");
        return;
    }
    const size_t sz = buf.size();
    const uint32_t eraseLen = 0x10000;  // 64KB, esp_flash 最小擦除粒度是 4KB

    int64_t t0 = nowUs();
    esp_err_t e = esp_flash_erase_region(nullptr, off, eraseLen);
    int64_t eraseUs = nowUs() - t0;
    ESP_LOGI(TAG, "BENCH|内部 flash 擦除|%8lld us|0x%06x %uKB %s", (long long)eraseUs,
             (unsigned)off, (unsigned)(eraseLen / 1024),
             e == ESP_OK ? "" : "  [失败]");

    t0 = nowUs();
    esp_err_t w = esp_flash_write(nullptr, buf.data(), off, sz);
    int64_t writeUs = nowUs() - t0;
    ESP_LOGI(TAG, "BENCH|内部 flash 写|%8lld us|%u B 裸写(不含文件系统元数据)",
             (long long)writeUs, (unsigned)sz);

    ESP_LOGI(TAG, "BENCH|内部 flash 合计|%8lld us|擦 %uKB + 写 %uKB; 这是内部方案的下界, "
                  "LittleFS 还要叠元数据与磨损均衡",
             (long long)(eraseUs + writeUs), (unsigned)(eraseLen / 1024), (unsigned)(sz / 1024));
    (void)w;
    // 基准数据留在无人区, 不影响任何分区; 下次完整刷机会把它抹掉
}

}  // namespace

void ioProbeLogHeap(const char *where) {
    // 分开打, 因为合并后的 esp_get_free_heap_size() 会把 8MB PSRAM 算进去,
    // 完全掩盖"内部 RAM 已干"这个真相。
    // largest 比 free 更要紧: SDMMC 的 DMA 缓冲要的是**连续**一块, 碎片化时空有
    // free 也分配不出来(日志里的 allocate_dma_buf err=0x101 就是这种)。
    ESP_LOGI(TAG, "HEAP|%-22s|内部 free %6u largest %6u | DMA free %6u largest %6u | "
                  "PSRAM free %8u largest %8u",
             where ? where : "",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

void ioProbeRunBenchmark() {
    const size_t kSize = 32 * 1024;
    std::vector<char> buf(kSize);
    for (size_t i = 0; i < kSize; i++) buf[i] = (char)('a' + (i % 26));

    uint32_t flashSize = 0;
    esp_flash_get_size(nullptr, &flashSize);
    ESP_LOGI(TAG, "==== 写盘基准开始: %u B, flash %u KB ====", (unsigned)kSize,
             (unsigned)(flashSize / 1024));

    benchSd(buf);
    benchInternalFlash(buf);

    ESP_LOGI(TAG, "==== 写盘基准结束 ====");
}

#endif  // PJOURNAL_IO_PERF_LOG
