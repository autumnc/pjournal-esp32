#pragma once

// 打字机模式的按键/上屏音效。18 种机械/静电容轴体的真实采样,素材来自 Mechvibes
// 的内置音色包(见 click_samples_data.inc),每次敲击随机变调/变音量/左右微差。
//
// play() 只把请求塞进队列就返回,渲染与写 I2S 由一个后台任务做,UI 线程不会被
// DMA 阻塞。懒加载 ES8311 DAC,闲置 30s 自动拆音频;仅
// g_settings.inputMode()=="typewriter" 且打字音效打开时发声,否则为 no-op。
void typingClickPlay(int count = 1);

// 立刻停掉放音、丢掉排队的声,并等后台任务真正拆完音频再返回
// (关掉打字音效、离开打字机模式、开始录音前调用——调用方随后要独占 I2S 控制器)。
void typingClickRelease();
