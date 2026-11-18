#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

// 网络收音机：支持 HLS（m3u8 + ts 分片）和明文 MP3 流。
// 电台列表放卡上 /sdcard/radio/radio.txt，一行一个「名称|URL」，
// 换源改这个文件即可，不用重编译固件。
class SdRadio {
public:
    static SdRadio& GetInstance();

    bool Scan();                      // 读 radio.txt
    bool WriteDefaultList();          // 把内置默认列表写到卡上（用户不用拔卡）
    int Count() const { return (int)stations_.size(); }
    const char* NameAt(int idx) const;

    bool Play(int idx);
    void Stop();
    // 释放常驻 PSRAM 缓冲（相册解码 JPEG 需要连续大内存时先调它）
    void ReleaseBuffers();
    void TogglePause();
    bool IsPlaying() const { return playing_.load(); }
    bool IsPaused() const { return paused_.load(); }
    int CurrentIndex() const { return index_.load(); }
    const char* NowPlaying() const { return now_playing_; }
    const char* StatusText() const { return status_; }
    const char* LastError() const { return error_; }

    static constexpr const char* kListFile = "/sdcard/radio/radio.txt";

private:
    SdRadio() = default;
    static void TaskEntry(void* arg);
    void PlayerLoop(int idx, std::string url);
    bool PlayHls(const std::string& url);
    bool PlayMp3Stream(const std::string& url);
    void SetError(const char* msg);
    void SetStatus(const char* s);

    struct Station {
        std::string name;
        std::string url;
    };
    std::vector<Station> stations_;
    std::atomic<int> index_{-1};
    std::atomic<bool> playing_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_request_{false};
    char now_playing_[64] = {0};
    char status_[32] = {0};
    char error_[64] = {0};
    std::string pending_url_;
    int pending_idx_ = -1;
};