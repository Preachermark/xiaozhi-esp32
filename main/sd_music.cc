#include "sd_music.h"
#include "sd_card_manager.h"
#include "application.h"
#include "audio_service.h"
#include "board.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <strings.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_log.h>

#include "esp_audio_dec.h"
#include "esp_mp3_dec.h"
#include "esp_ae_rate_cvt.h"

static const char* TAG = "SdMusic";

namespace {

constexpr const char* kMusicDir = "/sdcard/music";
constexpr int kOutRate = 24000;
constexpr size_t kInBufSize = 64 * 1024;    // 输入缓冲（PSRAM），够放几十帧
constexpr size_t kLowWater = 16 * 1024;     // 低于这个就补数据
constexpr size_t kOutBufStart = 32 * 1024;
constexpr size_t kOutBufMax = 256 * 1024;
constexpr size_t kPushSamples = 1024;       // 每次推 1024 样本 = 42ms @24kHz
constexpr int kMaxFrameBytes = 4096;

bool s_mp3_registered = false;

std::string FileStem(const char* name) {
    std::string s(name);
    size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    return s;
}

uint8_t* PsramAlloc(size_t n) {
    uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p != nullptr ? p : (uint8_t*)malloc(n);
}

struct Mp3Frame {
    uint32_t frame_len;
    uint32_t sample_rate;
    uint32_t channels;
};

// MPEG1/2/2.5 Layer III 帧头解析。这个库的 simple decoder 不支持 MP3 容器
// （Kconfig 只有 WAV/M4A/TS/OGG），所以帧边界得自己切。
bool ParseMp3Frame(const uint8_t* p, size_t avail, Mp3Frame* out) {
    if (avail < 4) return false;
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;

    const int ver = (p[1] >> 3) & 0x03;      // 3=MPEG1 2=MPEG2 0=MPEG2.5
    const int layer = (p[1] >> 1) & 0x03;    // 1=Layer III
    const int br_idx = (p[2] >> 4) & 0x0F;
    const int sr_idx = (p[2] >> 2) & 0x03;
    const int pad = (p[2] >> 1) & 0x01;
    const int ch_mode = (p[3] >> 6) & 0x03;

    if (ver == 1 || layer != 1) return false;
    if (br_idx == 0 || br_idx == 15 || sr_idx == 3) return false;

    static const uint16_t kBrV1[16] = {0,  32, 40, 48, 56, 64, 80,  96,
                                       112,128,160,192,224,256,320, 0};
    static const uint16_t kBrV2[16] = {0,  8,  16, 24, 32, 40, 48, 56,
                                       64, 80, 96, 112,128,144,160, 0};
    static const uint32_t kSrV1[4] = {44100, 48000, 32000, 0};

    uint32_t bitrate = (uint32_t)(ver == 3 ? kBrV1[br_idx] : kBrV2[br_idx]) * 1000u;
    uint32_t sr = kSrV1[sr_idx];
    if (ver == 2) {
        sr /= 2;
    } else if (ver == 0) {
        sr /= 4;
    }
    if (bitrate == 0 || sr == 0) return false;

    uint32_t frame_len = (ver == 3) ? (144u * bitrate / sr + (uint32_t)pad)
                                    : (72u * bitrate / sr + (uint32_t)pad);
    if (frame_len < 24 || frame_len > (uint32_t)kMaxFrameBytes) return false;

    out->frame_len = frame_len;
    out->sample_rate = sr;
    out->channels = (ch_mode == 3) ? 1 : 2;
    return true;
}

}  // namespace

SdMusic& SdMusic::GetInstance() {
    static SdMusic instance;
    return instance;
}

void SdMusic::SetError(const char* msg) {
    strncpy(error_, msg, sizeof(error_) - 1);
    error_[sizeof(error_) - 1] = '\0';
    ESP_LOGE(TAG, "%s", msg);
}

void SdMusic::LoadPlaylist() {
    titles_.clear();
    if (!SdCardManager::GetInstance().LockFs()) return;
    FILE* f = fopen("/sdcard/music/playlist.txt", "r");
    if (f != nullptr) {
        char line[256];
        while (fgets(line, sizeof(line), f) != nullptr) {
            char* nl = strpbrk(line, "\r\n");
            if (nl != nullptr) *nl = '\0';
            char* bar = strchr(line, '|');
            if (bar == nullptr) continue;
            *bar = '\0';
            std::string key(line);
            std::string val(bar + 1);
            if (!key.empty() && !val.empty()) {
                titles_[key] = val;
            }
        }
        fclose(f);
    }
    SdCardManager::GetInstance().UnlockFs();
}

bool SdMusic::Scan() {
    files_.clear();
    if (!SdCardManager::GetInstance().IsMounted()) {
        ESP_LOGW(TAG, "TF卡没挂上，放不了歌");
        return false;
    }

    std::vector<std::string> names;
    if (!SdCardManager::GetInstance().LockFs()) return false;
    DIR* d = opendir(kMusicDir);
    if (d != nullptr) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            size_t len = strlen(e->d_name);
            if (len < 5) continue;
            if (strcasecmp(e->d_name + len - 4, ".mp3") != 0) continue;
            names.emplace_back(e->d_name);
        }
        closedir(d);
    }
    SdCardManager::GetInstance().UnlockFs();

    std::sort(names.begin(), names.end());
    files_ = std::move(names);
    LoadPlaylist();
    ESP_LOGI(TAG, "%s 里找到 %u 首 mp3，歌单 %u 条", kMusicDir,
             (unsigned)files_.size(), (unsigned)titles_.size());
    return !files_.empty();
}

const char* SdMusic::TitleAt(int idx) const {
    if (idx < 0 || idx >= (int)files_.size()) return "";
    auto it = titles_.find(files_[idx]);
    if (it != titles_.end()) return it->second.c_str();
    static char title[128];
    std::string stem = FileStem(files_[idx].c_str());
    snprintf(title, sizeof(title), "%s", stem.c_str());
    return title;
}

const char* SdMusic::NowPlaying() const {
    return now_playing_;
}

bool SdMusic::Play(int idx) {
    if (files_.empty() && !Scan()) {
        SetError("卡上没有 mp3");
        return false;
    }
    if (idx < 0 || idx >= (int)files_.size()) idx = 0;

    if (playing_.load()) {
        stop_request_ = true;
        for (int i = 0; i < 40 && playing_.load(); i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    error_[0] = '\0';
    std::string path = std::string(kMusicDir) + "/" + files_[idx];
    snprintf(now_playing_, sizeof(now_playing_), "%s", TitleAt(idx));
    index_ = idx;
    position_sec_ = 0;
    stop_request_ = false;
    paused_ = false;
    playing_ = true;
    pending_path_ = path;
    pending_idx_ = idx;

    if (xTaskCreate(TaskEntry, "sd_music", 8192, this, 3, nullptr) != pdPASS) {
        playing_ = false;
        SetError("播放任务创建失败");
        return false;
    }
    ESP_LOGW(TAG, "[诊断] 开始播放 #%d/%u %s | %s", idx, (unsigned)files_.size(),
             path.c_str(), TitleAt(idx));
    return true;
}

void SdMusic::TaskEntry(void* arg) {
    auto* self = static_cast<SdMusic*>(arg);
    std::string path = self->pending_path_;
    int idx = self->pending_idx_;
    self->PlayerLoop(idx, path);
    vTaskDelete(nullptr);
}

void SdMusic::PlayerLoop(int idx, std::string path) {
    (void)idx;
    FILE* f = nullptr;
    if (SdCardManager::GetInstance().LockFs()) {
        f = fopen(path.c_str(), "rb");
        // 跳过 ID3v2 标签：里面的随机字节可能撞上 0xFF Ex 被当成帧头，产生刺啦声
        if (f != nullptr) {
            uint8_t id3[10] = {0};
            if (fread(id3, 1, 10, f) == 10 && id3[0] == 'I' && id3[1] == 'D' && id3[2] == '3') {
                size_t tag_size = ((size_t)(id3[6] & 0x7F) << 21) |
                                  ((size_t)(id3[7] & 0x7F) << 14) |
                                  ((size_t)(id3[8] & 0x7F) << 7) |
                                  ((size_t)(id3[9] & 0x7F));
                fseek(f, (long)(10 + tag_size), SEEK_SET);
                ESP_LOGI(TAG, "%s 跳过 ID3v2 标签 %u 字节", path.c_str(),
                         (unsigned)(10 + tag_size));
            } else {
                fseek(f, 0, SEEK_SET);
            }
        }
        SdCardManager::GetInstance().UnlockFs();
    }
    if (f == nullptr) {
        SetError("打不开音乐文件");
        playing_ = false;
        return;
    }

    // 帧解码器（不是 simple decoder —— 那个不支持 MP3 容器）
    if (!s_mp3_registered) {
        esp_mp3_dec_register();
        s_mp3_registered = true;
    }
    void* mp3 = nullptr;
    esp_audio_err_t oerr = esp_mp3_dec_open(nullptr, 0, &mp3);
    ESP_LOGW(TAG, "[诊断] mp3_dec_open 返回 0x%x handle=%p", (int)oerr, mp3);
    if (oerr != ESP_AUDIO_ERR_OK || mp3 == nullptr) {
        SetError("MP3 帧解码器打不开");
        fclose(f);
        playing_ = false;
        return;
    }

    uint8_t* inbuf = PsramAlloc(kInBufSize);
    size_t out_cap = kOutBufStart;
    uint8_t* outbuf = PsramAlloc(out_cap);
    uint8_t* rsvbuf = nullptr;
    size_t rsv_cap = 0;
    std::vector<int16_t> mono;

    esp_ae_rate_cvt_handle_t rsv = nullptr;
    int src_rate = 0;
    uint32_t pushed = 0;
    size_t buf_len = 0;
    bool eos = false;
    bool failed = false;
    int frame_ok = 0;
    int frame_bad = 0;
    bool codec_logged = false;

    if (inbuf == nullptr || outbuf == nullptr) {
        SetError("PSRAM 缓冲分配失败");
        failed = true;
    }

    while (!stop_request_.load() && !failed) {
        if (paused_.load()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 补数据
        if (buf_len < kLowWater && !eos) {
            size_t n = 0;
            if (SdCardManager::GetInstance().LockFs()) {
                n = fread(inbuf + buf_len, 1, kInBufSize - buf_len, f);
                SdCardManager::GetInstance().UnlockFs();
            }
            if (n == 0) {
                eos = true;
            } else {
                buf_len += n;
            }
        }

        // 逐帧解码
        size_t pos = 0;
        while (pos + 4 <= buf_len && !stop_request_.load()) {
            Mp3Frame fi;
            if (!ParseMp3Frame(inbuf + pos, buf_len - pos, &fi)) {
                pos++;                      // 不是帧头，往下挪一个字节再找
                continue;
            }
            if (pos + fi.frame_len > buf_len) {
                break;                      // 帧不完整，等更多数据
            }

            esp_audio_dec_in_raw_t raw = {};
            raw.buffer = inbuf + pos;
            raw.len = fi.frame_len;

            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = outbuf;
            frame.len = (uint32_t)out_cap;

            esp_audio_dec_info_t dinfo = {};
            esp_audio_err_t r = esp_mp3_dec_decode(mp3, &raw, &frame, &dinfo);
            pos += fi.frame_len;

            if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out_cap < kOutBufMax) {
                size_t new_cap = out_cap * 2;
                uint8_t* bigger = PsramAlloc(new_cap);
                if (bigger != nullptr) {
                    heap_caps_free(outbuf);
                    outbuf = bigger;
                    out_cap = new_cap;
                    pos -= fi.frame_len;    // 重解这一帧
                    break;
                }
                failed = true;
                break;
            }
            if (r != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) {
                frame_bad++;
                continue;
            }
            frame_ok++;

            if (!codec_logged) {
                codec_logged = true;
                auto* cdc = Board::GetInstance().GetAudioCodec();
                ESP_LOGW(TAG, "[诊断] 首帧 OK | rate=%u ch=%u | codec output_enabled=%d",
                         (unsigned)fi.sample_rate, (unsigned)fi.channels,
                         cdc != nullptr ? (int)cdc->output_enabled() : -1);
            }

            // 混成单声道
            const int16_t* pcm = (const int16_t*)outbuf;
            size_t total = frame.decoded_size / sizeof(int16_t);
            size_t ch = fi.channels > 0 ? fi.channels : 1;
            size_t frames = total / ch;
            if (frames == 0) continue;
            mono.resize(frames);
            if (ch >= 2) {
                for (size_t i = 0; i < frames; i++) {
                    int32_t sum = 0;
                    for (size_t c = 0; c < ch; c++) sum += pcm[i * ch + c];
                    mono[i] = (int16_t)(sum / (int32_t)ch);
                }
            } else {
                memcpy(mono.data(), pcm, frames * sizeof(int16_t));
            }

            // 重采样到 24kHz
            const int16_t* out_pcm = mono.data();
            size_t out_n = frames;
            if ((int)fi.sample_rate != kOutRate) {
                if (rsv == nullptr || src_rate != (int)fi.sample_rate) {
                    if (rsv != nullptr) esp_ae_rate_cvt_close(rsv);
                    esp_ae_rate_cvt_cfg_t rcfg = {};
                    rcfg.src_rate = fi.sample_rate;
                    rcfg.dest_rate = kOutRate;
                    rcfg.channel = 1;
                    rcfg.bits_per_sample = 16;
                    rcfg.complexity = 2;
                    rcfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                    if (esp_ae_rate_cvt_open(&rcfg, &rsv) != ESP_AE_ERR_OK || rsv == nullptr) {
                        SetError("重采样器打开失败");
                        failed = true;
                        break;
                    }
                    src_rate = (int)fi.sample_rate;
                    ESP_LOGW(TAG, "[诊断] 重采样 %d -> %d Hz", src_rate, kOutRate);
                }
                uint32_t need = 0;
                esp_ae_rate_cvt_get_max_out_sample_num(rsv, (uint32_t)frames, &need);
                if (rsvbuf == nullptr || rsv_cap < need) {
                    if (rsvbuf != nullptr) heap_caps_free(rsvbuf);
                    rsv_cap = need + 64;
                    rsvbuf = PsramAlloc(rsv_cap * sizeof(int16_t));
                    if (rsvbuf == nullptr) {
                        SetError("重采样缓冲分配失败");
                        failed = true;
                        break;
                    }
                }
                uint32_t got = (uint32_t)rsv_cap;
                if (esp_ae_rate_cvt_process(rsv, (esp_ae_sample_t)mono.data(), (uint32_t)frames,
                                            (esp_ae_sample_t)rsvbuf, &got) != ESP_AE_ERR_OK ||
                    got == 0) {
                    continue;
                }
                out_pcm = (const int16_t*)rsvbuf;
                out_n = got;
            }

            // 推播放队列（wait=true 天然限速）
            auto& audio = Application::GetInstance().GetAudioService();
            size_t off = 0;
            while (off < out_n && !stop_request_.load()) {
                while (paused_.load() && !stop_request_.load()) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                size_t n = std::min(kPushSamples, out_n - off);
                std::vector<int16_t> chunk(out_pcm + off, out_pcm + off + n);
                if (!audio.PushPcmToPlaybackQueue(std::move(chunk), true)) break;
                off += n;
                pushed += (uint32_t)n;
                position_sec_ = pushed / (uint32_t)kOutRate;
            }
        }

        // 把已处理数据移走
        if (pos > 0) {
            if (pos >= buf_len) {
                buf_len = 0;
            } else {
                memmove(inbuf, inbuf + pos, buf_len - pos);
                buf_len -= pos;
            }
        } else if (buf_len >= kInBufSize) {
            ESP_LOGW(TAG, "[诊断] 缓冲满且找不到帧头，放弃本曲");
            break;
        }

        if (eos && buf_len < 4) break;      // 播完了
        if (eos && pos == 0 && buf_len > 0) break;
    }

    if (rsv != nullptr) esp_ae_rate_cvt_close(rsv);
    esp_mp3_dec_close(mp3);
    if (SdCardManager::GetInstance().LockFs()) {
        fclose(f);
        SdCardManager::GetInstance().UnlockFs();
    }
    if (inbuf) heap_caps_free(inbuf);
    if (outbuf) heap_caps_free(outbuf);
    if (rsvbuf) heap_caps_free(rsvbuf);

    playing_ = false;
    paused_ = false;
    ESP_LOGI(TAG, "播放结束: %s | 好帧 %d 坏帧 %d | %u 秒 | failed=%d stop=%d",
             now_playing_, frame_ok, frame_bad,
             (unsigned)position_sec_.load(), (int)failed, (int)stop_request_.load());

    // 自然播完（不是被叫停的）→ 按循环模式续播
    bool natural_end = !stop_request_.load() && !failed;
    if (natural_end && !files_.empty()) {
        int n = (int)files_.size();
        int curbak = idx;
        int next_idx = curbak;
        switch (loop_mode_.load()) {
            case kLoopSingle:
                next_idx = curbak;
                break;
            case kLoopShuffle: {
                if (n > 1) {
                    for (int tries = 0; tries < 8; tries++) {
                        next_idx = (int)(esp_random() % (uint32_t)n);
                        if (next_idx != curbak) break;
                    }
                } else {
                    next_idx = 0;
                }
                break;
            }
            default:
                next_idx = (curbak + 1) % n;
                break;
        }
        ESP_LOGI(TAG, "循环模式(%s) 续播 #%d", LoopModeName(), next_idx);
        Play(next_idx);   // playing_ 已置 false，Play 不会等自己
    }
}

void SdMusic::CycleLoopMode() {
    int m = loop_mode_.load();
    m = (m + 1) % 3;
    loop_mode_ = m;
    ESP_LOGI(TAG, "循环模式切换为 %s", LoopModeName());
}

const char* SdMusic::LoopModeName() const {
    switch (loop_mode_.load()) {
        case kLoopSingle:  return "单曲";
        case kLoopShuffle: return "随机";
        default:           return "列表";
    }
}

void SdMusic::TogglePause() {
    if (!playing_.load()) return;
    paused_ = !paused_.load();
    ESP_LOGI(TAG, "%s", paused_.load() ? "已暂停" : "继续播放");
}

void SdMusic::Stop() {
    stop_request_ = true;
    for (int i = 0; i < 40 && playing_.load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    playing_ = false;
    paused_ = false;
}

void SdMusic::Next() {
    if (files_.empty()) return;
    int n = (int)files_.size();
    Play((index_.load() + 1 + n) % n);
}

void SdMusic::Prev() {
    if (files_.empty()) return;
    int n = (int)files_.size();
    Play((index_.load() - 1 + n) % n);
}