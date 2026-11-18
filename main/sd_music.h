#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

// TF 卡音乐播放器：扫 /sdcard/music/*.mp3，用 esp_audio_codec 的 MP3 解码器解成 PCM，
// 重采样到设备统一的 24kHz 单声道，塞进 AudioService 的播放队列。
// 不直接碰 I2S —— 那样会把语音链路搞死；走播放队列，音量和回声参考都自动生效。
class SdMusic {
public:
    static SdMusic& GetInstance();

    bool Scan();                        // 扫目录，返回是否有歌
    int Count() const { return (int)files_.size(); }
    const char* TitleAt(int idx) const;

    bool Play(int idx);                 // 从指定序号开始播
    void TogglePause();                 // 播放 <-> 暂停
    void Next();
    void Prev();
    void Stop();

    // 循环模式：列表循环 / 单曲循环 / 随机
    enum LoopMode { kLoopList = 0, kLoopSingle, kLoopShuffle };
    void CycleLoopMode();
    LoopMode GetLoopMode() const { return (LoopMode)loop_mode_.load(); }
    const char* LoopModeName() const;

    bool IsPlaying() const { return playing_.load(); }
    bool IsPaused() const { return paused_.load(); }
    int CurrentIndex() const { return index_.load(); }
    uint32_t PositionSec() const { return position_sec_.load(); }
    const char* NowPlaying() const;
    const char* LastError() const { return error_; }

private:
    SdMusic() = default;
    static void TaskEntry(void* arg);
    void PlayerLoop(int idx, std::string path);
    void SetError(const char* msg);
    void LoadPlaylist();      // 读 playlist.txt：一行「文件名|中文显示名」
    std::map<std::string, std::string> titles_;

    std::vector<std::string> files_;
    std::atomic<int> index_{-1};
    std::atomic<int> loop_mode_{kLoopList};
    std::atomic<bool> playing_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_request_{false};
    std::atomic<uint32_t> position_sec_{0};
    char error_[64] = {0};
    char now_playing_[128] = {0};
    std::string pending_path_;
    int pending_idx_ = -1;
};