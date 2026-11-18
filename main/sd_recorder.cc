#include "sd_recorder.h"
#include "sd_card_manager.h"
#include "application.h"
#include "audio_service.h"
#include "board.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_heap_caps.h>

static const char* TAG = "SdRec";

namespace {

constexpr size_t kRingSamples = (size_t)SdRecorder::kSampleRate * SdRecorder::kRingSeconds;
constexpr size_t kWriteChunkSamples = 2048;
constexpr size_t kHeaderBytes = 44;

int16_t* s_ring = nullptr;
size_t s_head = 0;
size_t s_tail = 0;
portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;

struct __attribute__((packed)) WavHeader {
    char     riff[4];
    uint32_t riff_size;
    char     wave[4];
    char     fmt_id[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data_id[4];
    uint32_t data_size;
};

WavHeader MakeHeader(uint32_t data_bytes) {
    WavHeader h = {};
    memcpy(h.riff, "RIFF", 4);
    h.riff_size = 36 + data_bytes;
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt_id, "fmt ", 4);
    h.fmt_size = 16;
    h.audio_format = 1;
    h.channels = 1;
    h.sample_rate = (uint32_t)SdRecorder::kSampleRate;
    h.byte_rate = (uint32_t)SdRecorder::kSampleRate * 2;
    h.block_align = 2;
    h.bits_per_sample = 16;
    memcpy(h.data_id, "data", 4);
    h.data_size = data_bytes;
    return h;
}

size_t RingTake(int16_t* dst, size_t max) {
    size_t n = 0;
    portENTER_CRITICAL(&s_ring_lock);
    while (s_tail != s_head && n < max) {
        dst[n++] = s_ring[s_tail];
        s_tail = (s_tail + 1) % kRingSamples;
    }
    portEXIT_CRITICAL(&s_ring_lock);
    return n;
}

}  // namespace

SdRecorder& SdRecorder::GetInstance() {
    static SdRecorder instance;
    return instance;
}

size_t SdRecorder::InputChannels() {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr && codec->input_channels() > 0) {
        return (size_t)codec->input_channels();
    }
    return 1;
}

void SdRecorder::SetError(const char* msg) {
    strncpy(last_error_, msg, sizeof(last_error_) - 1);
    last_error_[sizeof(last_error_) - 1] = '\0';
    ESP_LOGE(TAG, "%s", msg);
}

void SdRecorder::EnsureTapInstalled() {
    if (tap_installed_.exchange(true)) return;
    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetInputTap([](const std::vector<int16_t>& data) {
        SdRecorder::GetInstance().OnPcm(data.data(), data.size());
    });
    ESP_LOGI(TAG, "已挂上音频输入 tap");
}

bool SdRecorder::Start() {
    last_error_[0] = '\0';
    if (recording_.load()) {
        SetError("已经在录了");
        return false;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        SetError("TF卡没挂上");
        return false;
    }

    EnsureTapInstalled();

    if (s_ring == nullptr) {
        s_ring = (int16_t*)heap_caps_malloc(kRingSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (s_ring == nullptr) {
            SetError("PSRAM 缓冲分配失败");
            return false;
        }
        ESP_LOGI(TAG, "录音缓冲: %u KB PSRAM (%u 秒)",
                 (unsigned)(kRingSamples * sizeof(int16_t) / 1024), (unsigned)kRingSeconds);
    }
    s_head = 0;
    s_tail = 0;
    channels_ = InputChannels();

    time_t now = time(nullptr);
    char stamp[32];
    if (now > 1735689600) {
        struct tm tm_now = {};
        localtime_r(&now, &tm_now);
        snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    } else {
        snprintf(stamp, sizeof(stamp), "boot%llds", (long long)(SdCardManager::BootMs() / 1000));
    }
    snprintf(path_, sizeof(path_), "/sdcard/record/%s.wav", stamp);

    // 目录兜底：万一挂载时没建成
    struct stat st = {};
    if (stat("/sdcard/record", &st) != 0) {
        if (SdCardManager::GetInstance().LockFs()) {
            mkdir("/sdcard/record", 0775);
            SdCardManager::GetInstance().UnlockFs();
        }
    }

    if (!SdCardManager::GetInstance().LockFs()) {
        SetError("文件系统锁超时");
        return false;
    }
    FILE* f = fopen(path_, "wb");
    bool ok = false;
    if (f != nullptr) {
        WavHeader hd = MakeHeader(0);
        ok = fwrite(&hd, sizeof(hd), 1, f) == 1;
        fclose(f);
    }
    SdCardManager::GetInstance().UnlockFs();
    if (!ok) {
        SetError("建文件失败（查卡写保护/满）");
        return false;
    }

    samples_captured_ = 0;
    samples_written_ = 0;
    stop_requested_ = false;
    recording_ = true;
    if (xTaskCreate(WriterTaskEntry, "sd_rec", 4096, this, 3, nullptr) != pdPASS) {
        recording_ = false;
        SetError("写盘任务创建失败");
        return false;
    }
    ESP_LOGI(TAG, "开始录音 -> %s (源 %u 通道，取通道0)", path_, (unsigned)InputChannels());
    return true;
}

bool SdRecorder::Stop(uint32_t* out_seconds, char* out_path, size_t path_len) {
    if (!recording_.load()) return false;
    stop_requested_ = true;
    for (int i = 0; i < 80 && recording_.load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (out_seconds != nullptr) {
        *out_seconds = samples_written_.load() / (uint32_t)kSampleRate;
    }
    if (out_path != nullptr && path_len > 0) {
        strncpy(out_path, path_, path_len - 1);
        out_path[path_len - 1] = '\0';
    }
    return true;
}

void SdRecorder::OnPcm(const int16_t* data, size_t samples) {
    // 只有音频输入任务会走到这里，首个标志不需要原子
    static bool first_seen = false;
    if (!first_seen) {
        first_seen = true;
        ESP_LOGI(TAG, "音频输入 tap 已接通：首帧 %u 样本/16kHz 单声道", (unsigned)samples);
    }
    if (!recording_.load() || data == nullptr || samples == 0 || s_ring == nullptr) return;

    // tap 拿到的是多通道交错数据（本板 2 路：0=麦克风，1=喇叭回采参考），只留麦克风那一路
    const size_t ch = channels_ > 0 ? channels_ : 1;
    const size_t frames = samples / ch;
    if (frames == 0) return;

    uint32_t captured = samples_captured_.load();
    if (captured + frames > kMaxSeconds * (uint32_t)kSampleRate) {
        stop_requested_ = true;     // 到 5 分钟自动停
        return;
    }

    portENTER_CRITICAL(&s_ring_lock);
    for (size_t i = 0; i < frames; i++) {
        size_t next = (s_head + 1) % kRingSamples;
        if (next == s_tail) break;      // 满了丢新数据，绝不阻塞音频任务
        s_ring[s_head] = data[i * ch];  // 通道 0
        s_head = next;
    }
    portEXIT_CRITICAL(&s_ring_lock);
    samples_captured_ = captured + (uint32_t)frames;
}

void SdRecorder::WriterTaskEntry(void* arg) {
    static_cast<SdRecorder*>(arg)->WriterLoop();
    vTaskDelete(nullptr);
}

void SdRecorder::WriterLoop() {
    int16_t* buf = (int16_t*)heap_caps_malloc(kWriteChunkSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (buf == nullptr) buf = (int16_t*)malloc(kWriteChunkSamples * sizeof(int16_t));
    if (buf == nullptr) {
        SetError("写盘缓冲分配失败");
        recording_ = false;
        return;
    }

    FILE* f = nullptr;
    if (SdCardManager::GetInstance().LockFs()) {
        f = fopen(path_, "r+b");
        if (f != nullptr) fseek(f, (long)kHeaderBytes, SEEK_SET);
        SdCardManager::GetInstance().UnlockFs();
    }
    if (f == nullptr) {
        SetError("重打开文件失败");
        free(buf);
        recording_ = false;
        return;
    }

    uint32_t total = 0;
    while (!stop_requested_.load()) {
        vTaskDelay(pdMS_TO_TICKS(80));
        size_t got;
        while ((got = RingTake(buf, kWriteChunkSamples)) > 0) {
            if (SdCardManager::GetInstance().LockFs()) {
                fwrite(buf, sizeof(int16_t), got, f);
                SdCardManager::GetInstance().UnlockFs();
                total += (uint32_t)got;
                samples_written_ = total;
            }
        }
    }

    // 收尾：把 ring 里剩下的写完
    size_t got;
    while ((got = RingTake(buf, kWriteChunkSamples)) > 0) {
        if (SdCardManager::GetInstance().LockFs()) {
            fwrite(buf, sizeof(int16_t), got, f);
            SdCardManager::GetInstance().UnlockFs();
            total += (uint32_t)got;
            samples_written_ = total;
        }
    }

    uint32_t data_bytes = total * (uint32_t)sizeof(int16_t);
    WavHeader hd = MakeHeader(data_bytes);
    if (SdCardManager::GetInstance().LockFs()) {
        fseek(f, 0, SEEK_SET);
        fwrite(&hd, sizeof(hd), 1, f);
        fflush(f);
        fclose(f);
        SdCardManager::GetInstance().UnlockFs();
    }

    free(buf);
    recording_ = false;
    ESP_LOGI(TAG, "录音结束: %s, %u 秒, %u 字节", path_,
             (unsigned)(total / (uint32_t)kSampleRate), (unsigned)data_bytes);
}

// ============ 诊断用：把卡上最新一个 wav 的头和开头数据从串口吐出来 ============
// 直接在串口打印（printf 绕开日志钩子），供远端解析实际采样率/通道数/数据特征
void SdRecorder::DumpDiagnosis() {
    if (!SdCardManager::GetInstance().IsMounted()) {
        printf("[DIAG] TF卡没挂上，无法诊断\n");
        return;
    }

    // 找 /sdcard/record 下文件名最大的 .wav（时间戳命名 = 最新）
    static char latest[288];
    memset(latest, 0, sizeof(latest));
    int count = 0;
    if (SdCardManager::GetInstance().LockFs()) {
        DIR* d = opendir("/sdcard/record");
        if (d != nullptr) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                size_t len = strlen(e->d_name);
                if (len < 5 || strcasecmp(e->d_name + len - 4, ".wav") != 0) continue;
                count++;
                if (strcmp(e->d_name, latest) > 0) {
                    snprintf(latest, sizeof(latest), "%s", e->d_name);
                }
            }
            closedir(d);
        }
        SdCardManager::GetInstance().UnlockFs();
    }

    if (latest[0] == '\0') {
        printf("[DIAG] /sdcard/record 里没有 wav 文件\n");
        return;
    }

    static char path[352];
    snprintf(path, sizeof(path), "/sdcard/record/%s", latest);

    struct stat st = {};
    if (!SdCardManager::GetInstance().LockFs()) return;
    int sr = stat(path, &st);
    SdCardManager::GetInstance().UnlockFs();
    printf("[DIAG] 文件数=%d 最新=%s 大小=%ld 字节\n", count, latest, sr == 0 ? (long)st.st_size : -1L);

    // 读头 44 字节 + 前 1024 字节数据
    static uint8_t head[44];
    static uint8_t data[1024];
    memset(head, 0, sizeof(head));
    memset(data, 0, sizeof(data));
    if (SdCardManager::GetInstance().LockFs()) {
        FILE* f = fopen(path, "rb");
        if (f != nullptr) {
            fread(head, 1, sizeof(head), f);
            fread(data, 1, sizeof(data), f);
            fclose(f);
        }
        SdCardManager::GetInstance().UnlockFs();
    }

    // 头字段解码
    uint32_t riff_size = (uint32_t)head[4] | ((uint32_t)head[5] << 8) | ((uint32_t)head[6] << 16) | ((uint32_t)head[7] << 24);
    uint16_t fmt = (uint16_t)head[20] | ((uint16_t)head[21] << 8);
    uint16_t ch = (uint16_t)head[22] | ((uint16_t)head[23] << 8);
    uint32_t srate = (uint32_t)head[24] | ((uint32_t)head[25] << 8) | ((uint32_t)head[26] << 16) | ((uint32_t)head[27] << 24);
    uint32_t brate = (uint32_t)head[28] | ((uint32_t)head[29] << 8) | ((uint32_t)head[30] << 16) | ((uint32_t)head[31] << 24);
    uint16_t align = (uint16_t)head[32] | ((uint16_t)head[33] << 8);
    uint16_t bits = (uint16_t)head[34] | ((uint16_t)head[35] << 8);
    uint32_t dsize = (uint32_t)head[40] | ((uint32_t)head[41] << 8) | ((uint32_t)head[42] << 16) | ((uint32_t)head[43] << 24);

    printf("[DIAG] HEAD riff_size=%lu fmt=%u channels=%u sample_rate=%lu byte_rate=%lu block_align=%u bits=%u data_size=%lu\n",
           (unsigned long)riff_size, (unsigned)fmt, (unsigned)ch, (unsigned long)srate,
           (unsigned long)brate, (unsigned)align, (unsigned)bits, (unsigned long)dsize);
    if (srate > 0 && ch > 0) {
        uint32_t frames = dsize / (align > 0 ? align : 2);
        printf("[DIAG] 头声称时长=%.2f 秒 (%lu 帧)\n", (double)frames / (double)srate, (unsigned long)frames);
    }

    // 头 44 字节 hex
    printf("[DIAG] HEADHEX ");
    for (size_t i = 0; i < sizeof(head); i++) printf("%02x", head[i]);
    printf("\n");

    // 前 1024 字节数据 hex，每行 64 字符 = 32 字节
    for (size_t off = 0; off < sizeof(data); off += 32) {
        printf("[DIAG] D%04u ", (unsigned)off);
        for (size_t i = 0; i < 32 && off + i < sizeof(data); i++) printf("%02x", data[off + i]);
        printf("\n");
    }

    // 前 32 个样本的十进制值，方便肉眼判断是否交错/是否有静音
    printf("[DIAG] SAMPLES");
    for (size_t i = 0; i + 1 < 64; i += 2) {
        int16_t v = (int16_t)((uint16_t)data[i] | ((uint16_t)data[i + 1] << 8));
        printf(" %d", v);
    }
    printf("\n[DIAG] END\n");
}