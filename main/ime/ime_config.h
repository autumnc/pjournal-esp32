#pragma once

// Compile-time switches for IME modules. Defaults keep the current firmware
// behavior intact while leaving room for optional table modules.
#ifndef PJOURNAL_IME_ENABLE_PINYIN
#define PJOURNAL_IME_ENABLE_PINYIN 1
#endif

#ifndef PJOURNAL_IME_ENABLE_LIANGFEN
#define PJOURNAL_IME_ENABLE_LIANGFEN 1
#endif

#ifndef PJOURNAL_IME_ENABLE_WUBI
#define PJOURNAL_IME_ENABLE_WUBI 0
#endif

#ifndef PJOURNAL_IME_ENABLE_SHUANGPIN
#define PJOURNAL_IME_ENABLE_SHUANGPIN 0
#endif

#ifndef PJOURNAL_IME_FAST_LOOKUP
#define PJOURNAL_IME_FAST_LOOKUP 1
#endif

#ifndef PJOURNAL_IME_PERF_LOG
#define PJOURNAL_IME_PERF_LOG 0
#endif
