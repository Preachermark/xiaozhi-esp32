#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// 极简 TS(MPEG-TS) 解复用：从 188 字节包里抽出音频 PES 的 ADTS 帧。
// 库自带的 esp_ts_dec 在 CNR 这类流上解不出来（读 3 个包就 FAIL），所以自己来。
class TsDemux {
public:
    // 回调：拿到一段连续的 ADTS 数据（可能不是完整帧，调用方负责按帧切）
    using FrameSink = void (*)(void* ctx, const uint8_t* data, size_t len);

    void Reset();
    // 喂一段 TS 数据（长度最好是 188 的整数倍，不是也会尽量处理）
    void Feed(const uint8_t* data, size_t len, FrameSink sink, void* ctx);

    uint16_t AudioPid() const { return audio_pid_; }
    bool PmtParsed() const { return pmt_parsed_; }

private:
    void ParsePat(const uint8_t* p, int len);
    void ParsePmt(const uint8_t* p, int len);

    uint16_t pmt_pid_ = 0xFFFF;
    uint16_t audio_pid_ = 0xFFFF;
    bool pmt_parsed_ = false;
};
