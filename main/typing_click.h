#pragma once

// 打字机模式的按键/上屏音效。懒加载 ES8311 DAC 播放合成"嗒"声;
// 仅 g_settings.inputMode()=="typewriter" 时发声,正常模式调用为 no-op。
void typingClickPlay(int count = 1);
void typingClickRelease();
