// SD / 内部 flash 写盘诊断探针。
//
// 背景: 用户词典、日记正文、恢复草稿、历史版本全部走 SD(FATFS, SDMMC 1-bit),
// 都跑在主任务上。"输入词组偶尔卡顿"的怀疑对象就是这些写盘。这个模块只在
// PJOURNAL_IO_PERF_LOG=ON 的诊断固件里存在(shipping 版编译出来一个字节都没有),
// 干两件事:
//   1) IO_PROBE(name, detail) 给具体写盘点掐表, 结束即打印耗时
//   2) ioProbeRunBenchmark() 启动时跑一次基准, 同一份数据分别写 SD 和内部 flash
//      的裸分区, 量出各自真实的写入耗时 —— 用来回答"词库换到内部 flash 值不值"
#pragma once

#include <cstdint>

#ifndef PJOURNAL_IO_PERF_LOG
#define PJOURNAL_IO_PERF_LOG 0
#endif

#if PJOURNAL_IO_PERF_LOG

class IoProbeScope {
public:
    IoProbeScope(const char *what, const char *detail);
    ~IoProbeScope();

private:
    const char *what_;
    const char *detail_;
    int64_t t0_;
};

#define IO_PROBE(what, detail) IoProbeScope _io_probe_scope_(what, detail)

// 一次性基准: SD 新建写 / 覆盖写 / 带 fsync 写 / 读, 以及内部 flash 裸分区的
// 擦除 + 写入。只在 SD 挂载成功后调用。
void ioProbeRunBenchmark();

// 打一行内部 RAM 体检。背景: 这台机器的症状是"PSRAM 空闲 8MB, 内部 RAM 却干了",
// 而 SDMMC 的 DMA 缓冲、任务栈、以及所有 <=2KB 的小分配(SPIRAM_MALLOC_ALWAYSINTERNAL)
// 都只认内部 RAM。光看 esp_get_free_heap_size() 会把 PSRAM 算进去, 完全掩盖这个问题,
// 所以这里分开打 INTERNAL / DMA / PSRAM 各自的 free 和 largest block。
//
// 用在: 开机、周期性、以及每一条分配失败路径上(SD 打开失败、xTaskCreate 失败)。
void ioProbeLogHeap(const char *where);

#define IO_PROBE_HEAP(where) ioProbeLogHeap(where)

#else

// 关掉时退化成空对象, 调用点不用改(不能只是 do{}while(0), 否则接不上构造参数)
struct IoProbeNoop {
    IoProbeNoop(const char *, const char *) {}
};
#define IO_PROBE(what, detail) \
    IoProbeNoop _io_probe_scope_ __attribute__((unused))((what), (detail))

inline void ioProbeRunBenchmark() {}
inline void ioProbeLogHeap(const char *) {}

#define IO_PROBE_HEAP(where) ((void)0)

#endif
