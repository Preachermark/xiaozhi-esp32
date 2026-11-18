#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <vector>

// 语音备忘录音：从 AudioService 的输入 tap 抓 PCM（16kHz/16bit/单声道）写成 wav。
// 缓冲放在 PSRAM（内部 RAM 只剩 ~120KB，绝不能拿来开大队列）。
class SdRecorder {
public:
    static SdRecorder& GetInstance();

    void EnsureTapInstalled();
    bool Start();
    bool Stop(uint32_t* out_seconds, char* out_path, size_t path_len);
    bool IsRecording() const { return recording_.load(); }
    const char* LastError() const { return last_error_; }

    void OnPcm(const int16_t* data, size_t samples);

    // 诊断：把最新 wav 的头与前 1KB 数据打到串口
    void DumpDiagnosis();

    static constexpr int kSampleRate = 16000;
    static constexpr uint32_t kMaxSeconds = 300;    // 5 分钟自动停
    static constexpr size_t kRingSeconds = 2;       // PSRAM 缓冲秒数

private:
    SdRecorder() = default;
    static void WriterTaskEntry(void* arg);
    void WriterLoop();
    void SetError(const char* msg);
    static size_t InputChannels();

    std::atomic<bool> recording_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> tap_installed_{false};
    std::atomic<uint32_t> samples_written_{0};
    std::atomic<uint32_t> samples_captured_{0};
    size_t channels_ = 1;   // tap 来源的通道数（本板为 2：麦克风 + 喇叭回采）
    char path_[64] = {0};
    char last_error_[64] = {0};
};