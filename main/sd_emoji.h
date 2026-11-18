#pragma once

#include <memory>

class EmojiCollection;

// 从 /sdcard/assets/emoji/ 加载表情集合。
// 文件名（不含扩展名、不区分大小写）= 情绪名，例如 happy.png -> "happy"。
// 卡不在、目录空、一张都没读成功 → 返回 nullptr，调用方回落到 flash 里的表情。
std::shared_ptr<EmojiCollection> SdEmojiLoad();

// 供诊断用：上次加载了多少张、用了多少 PSRAM
int SdEmojiLoadedCount();
size_t SdEmojiLoadedBytes();
