#include "sd_radio.h"
#include "ts_demux.h"
#include "sd_card_manager.h"
#include "application.h"
#include "audio_service.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include "esp_aac_dec.h"
#include "esp_mp3_dec.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"
#include "esp_ae_rate_cvt.h"

static const char* TAG = "SdRadio";

constexpr size_t kTsPacket = 188;   // MPEG-TS 固定包长




namespace {

constexpr int kOutRate = 24000;
constexpr size_t kSegMax = 512 * 1024;      // 单个 ts 分片最大
constexpr size_t kM3u8Max = 64 * 1024;
constexpr size_t kOutBufStart = 32 * 1024;
constexpr size_t kOutBufMax = 256 * 1024;
constexpr size_t kPushSamples = 1024;
constexpr int kMaxSegsPerFetch = 6;         // 一次列表最多连播几个分片，然后刷新列表

bool s_registered = false;

uint8_t* PsramAlloc(size_t n) {
    uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p != nullptr ? p : (uint8_t*)malloc(n);
}


// ---- HLS/AAC 解码上下文：TsDemux 的回调是函数指针，状态只能放静态 ----
struct HlsAac {
    void* aac = nullptr;   // esp_aac_dec 专用句柄（不走 simple_dec 封装）
    std::vector<uint8_t> pending;
    uint8_t* outbuf = nullptr;
    size_t out_cap = 0;
    esp_ae_rate_cvt_handle_t rsv = nullptr;
    int src_rate = 0;
    std::vector<int16_t> mono;
    int16_t* rsvbuf = nullptr;
    size_t rsv_cap = 0;
    uint32_t frames_ok = 0;
    uint32_t pushed_ok = 0;
    uint32_t pushed_drop = 0;
    uint32_t fail_streak = 0;   // 连续解码失败次数，超阈值就重开解码器
};
static HlsAac g_hls;

// 下载/解码中断标志：Stop() 置 true。HttpGet 的读循环和拆包循环都要看它，
// 否则旧任务会卡在 read（15 秒超时）或闷头塞满整片音频，切台切不干净。
static volatile bool g_abort_download = false;

// 收音机常驻的 PSRAM 大缓冲（约 760KB）。原来放在 PlayHls 内部当 static 局部，
// 外部无法释放；相册解 JPEG 需要连续大内存，常因这几块被长期占用而失败。
// 提到文件级后，停播时可以主动还给系统。
static uint8_t* g_m3u8buf = nullptr;   // m3u8 列表缓冲
static uint8_t* g_tsbuf = nullptr;     // 分片缓冲
static uint8_t* g_ls_buf = nullptr;    // 列表预拉缓冲
static uint8_t* g_pf_buf = nullptr;    // 预取缓冲

// ---- HLS 分片预取（单片）----
// 串行的"下载→解码→下载"会让分片边界出现下载空窗，用独立任务在解码期间
// 把下一片下好。（两片深度那版引入了状态机竞态，实测 abort() 崩溃，已退回。）
static volatile size_t s_pf_len = 0;
static volatile bool s_pf_done = false;
static volatile bool s_pf_running = false;
static std::string s_pf_url;

// 预取任务要用 HttpGet，而它定义在后面，这里先声明
bool HttpGet(const std::string& url, uint8_t* buf, size_t max, size_t* out_len);

static void HlsPrefetchEntry(void*) {
    size_t n = 0;
    if (g_pf_buf != nullptr && !s_pf_url.empty()) {
        HttpGet(s_pf_url, g_pf_buf, kSegMax, &n);
    }
    if (g_abort_download) n = 0;
    s_pf_len = n;
    s_pf_done = (n > 0);
    s_pf_running = false;
    vTaskDelete(nullptr);
}

// ---- HLS 列表提前刷新 ----
// 兰州这类台列表里只有 5 个分片、每片 4 秒，播满 20 秒就会耗尽，
// 只能停下来同步拉一次 m3u8 —— 那一停就是周期性卡顿（实测 18~20 秒一次）。
// 所以在还剩两片时就让后台把新列表拉回来，真正播完时新分片已到手。
static volatile size_t s_ls_len = 0;
static volatile bool s_ls_done = false;
static volatile bool s_ls_running = false;
static std::string s_ls_url;

static void HlsListFetchEntry(void*) {
    size_t n = 0;
    if (g_ls_buf != nullptr && !s_ls_url.empty()) {
        HttpGet(s_ls_url, g_ls_buf, kM3u8Max, &n);
    }
    if (g_abort_download) n = 0;
    s_ls_len = n;
    s_ls_done = (n > 0);
    s_ls_running = false;
    vTaskDelete(nullptr);
}

// 播放互斥锁：切台时新旧播放任务绝不能同时碰解码器/缓冲，否则旧任务手里
// 的句柄会被新任务关掉，紧接着访问就是 LoadProhibited 崩溃。
static SemaphoreHandle_t g_play_lock = nullptr;
struct PlayLockGuard {
    bool held = false;
    PlayLockGuard() {
        if (g_play_lock == nullptr) g_play_lock = xSemaphoreCreateMutex();
        // 等旧任务彻底退场，最多 12 秒
        held = (g_play_lock != nullptr) &&
               (xSemaphoreTake(g_play_lock, pdMS_TO_TICKS(12000)) == pdTRUE);
    }
    ~PlayLockGuard() {
        if (held) xSemaphoreGive(g_play_lock);
    }
    bool ok() const { return held; }
};

static void HlsDecodeOneFrame(const uint8_t* frame, size_t flen) {
    HlsAac& h = g_hls;
    if (h.aac == nullptr || h.outbuf == nullptr) return;

    // 跟 MP3 那条能出声的路径完全同构：填 in_raw → 收 out_frame → info 一起回来
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t*>(frame);
    raw.len = (uint32_t)flen;
    esp_audio_dec_out_frame_t out = {};
    out.buffer = h.outbuf;
    out.len = (uint32_t)h.out_cap;
    esp_audio_dec_info_t dinfo = {};
    esp_audio_err_t r = esp_aac_dec_decode(h.aac, &raw, &out, &dinfo);

    if (r != ESP_AUDIO_ERR_OK) {
        static int ea = 0;
        if (ea++ < 5) ESP_LOGW(TAG, "aac_dec_decode 返回 0x%x (输入 %u 字节)", (int)r, (unsigned)flen);
        h.fail_streak++;
        return;
    }
    if (out.decoded_size == 0) {
        h.fail_streak++;
        return;
    }
    h.fail_streak = 0;
    h.frames_ok++;

    int srate = (int)dinfo.sample_rate;
    int chs = (int)dinfo.channel;
    int bps = (int)dinfo.bits_per_sample;
    if (srate == 0 || chs == 0) { srate = 48000; chs = 2; }
    if (bps == 0) bps = 16;

    // 按位深转单声道 int16
    size_t nfr = 0;
    h.mono.clear();
    if (bps == 32) {
        const int32_t* p32 = (const int32_t*)h.outbuf;
        size_t tot = out.decoded_size / sizeof(int32_t);
        nfr = tot / (size_t)chs;
        if (nfr == 0) return;
        h.mono.resize(nfr);
        for (size_t k = 0; k < nfr; k++) {
            int64_t sum = 0;
            for (int cc = 0; cc < chs; cc++) sum += p32[k * chs + cc];
            h.mono[k] = (int16_t)((sum / chs) >> 16);
        }
    } else {
        const int16_t* p16 = (const int16_t*)h.outbuf;
        size_t tot = out.decoded_size / sizeof(int16_t);
        nfr = tot / (size_t)chs;
        if (nfr == 0) return;
        h.mono.resize(nfr);
        if (chs >= 2) {
            for (size_t k = 0; k < nfr; k++) {
                int32_t sum = 0;
                for (int cc = 0; cc < chs; cc++) sum += p16[k * chs + cc];
                h.mono[k] = (int16_t)(sum / chs);
            }
        } else {
            memcpy(h.mono.data(), p16, nfr * sizeof(int16_t));
        }
    }

    const int16_t* outp = h.mono.data();
    size_t outn = nfr;
    if (srate != (int)kOutRate) {
        if (h.rsv == nullptr || h.src_rate != srate) {
            if (h.rsv != nullptr) esp_ae_rate_cvt_close(h.rsv);
            esp_ae_rate_cvt_cfg_t rc = {};
            rc.src_rate = (uint32_t)srate;
            rc.dest_rate = kOutRate;
            rc.channel = 1;
            rc.bits_per_sample = 16;
            rc.complexity = 2;
            rc.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
            if (esp_ae_rate_cvt_open(&rc, &h.rsv) != ESP_AE_ERR_OK || h.rsv == nullptr) {
                h.rsv = nullptr;
                return;
            }
            h.src_rate = srate;
            ESP_LOGW(TAG, "AAC 重采样 %d -> %u", srate, (unsigned)kOutRate);
        }
        uint32_t need = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(h.rsv, (uint32_t)nfr, &need);
        if (h.rsvbuf == nullptr || h.rsv_cap < need) {
            if (h.rsvbuf != nullptr) heap_caps_free(h.rsvbuf);
            h.rsv_cap = need + 64;
            h.rsvbuf = (int16_t*)PsramAlloc(h.rsv_cap * sizeof(int16_t));
            if (h.rsvbuf == nullptr) return;
        }
        uint32_t got = (uint32_t)h.rsv_cap;
        if (esp_ae_rate_cvt_process(h.rsv, (esp_ae_sample_t)h.mono.data(), (uint32_t)nfr,
                                    (esp_ae_sample_t)h.rsvbuf, &got) != ESP_AE_ERR_OK || got == 0) {
            return;
        }
        outp = h.rsvbuf;
        outn = got;
    }

    auto& audio = Application::GetInstance().GetAudioService();
    static std::vector<int16_t> s_pushbuf;
    // 队列只有 2 个任务位的容量，解码却是瞬间完成的。若用非阻塞推，绝大多数
    // PCM 会被直接丢弃（实测丢 87%），听起来就是"快好几倍"的怪声。
    // 这里改成"满了就等一会儿再试"，但每轮都检查停止标志，保证切台能立刻响应。
    s_pushbuf.assign(outp, outp + outn);
    int retry = 0;
    while (!audio.PushPcmToPlaybackQueue(std::move(s_pushbuf), false)) {
        if (g_abort_download) return;   // 切台/退出优先（Stop() 会置这个标志）
        // 短分片（兰州 4 秒、甘肃更短）下解码产出的速度会持续快于播放，
        // 3 秒的等待窗口不够，会被迫丢帧造成"吃字"。放宽到 30 秒，
        // 停止/切台仍然靠 g_abort_download 立刻退出，不会卡死。
        if (++retry > 3000) {
            h.pushed_drop++;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        s_pushbuf.assign(outp, outp + outn);
    }
    h.pushed_ok++;
}

static void HlsAdtsSink(void* /*ctx*/, const uint8_t* data, size_t len) {
    HlsAac& h = g_hls;
    // 连续失败太多：解码器状态已经坏了，重开一个 + 清掉残留
    if (h.fail_streak >= 24) {
        ESP_LOGW(TAG, "[恢复] 连续解码失败 %u 次，重开 AAC 解码器", (unsigned)h.fail_streak);
        if (h.aac != nullptr) esp_aac_dec_close(h.aac);
        h.aac = nullptr;
        static esp_aac_dec_cfg_t aac_cfg2 = {};
        aac_cfg2.aac_plus_enable = true;
        (void)esp_aac_dec_open(&aac_cfg2, sizeof(aac_cfg2), &h.aac);
        h.pending.clear();
        h.fail_streak = 0;
    }
    h.pending.insert(h.pending.end(), data, data + len);
    if (h.pending.size() > 256 * 1024) {
        h.pending.erase(h.pending.begin(), h.pending.begin() + 128 * 1024);
    }
    while (h.pending.size() >= 7) {
        if (g_abort_download) return;   // 已被叫停：立刻停止切帧解码，别继续往队列里塞
        if (h.pending[0] != 0xFF || (h.pending[1] & 0xF0) != 0xF0) {
            size_t k = 1;
            while (k + 1 < h.pending.size() &&
                   !(h.pending[k] == 0xFF && (h.pending[k + 1] & 0xF0) == 0xF0)) {
                k++;
            }
            h.pending.erase(h.pending.begin(), h.pending.begin() + k);
            if (h.pending.size() < 7) return;
        }
        int flen = ((h.pending[3] & 0x03) << 11) | (h.pending[4] << 3) | ((h.pending[5] >> 5) & 0x07);
        if (flen < 7) {
            static int bl = 0;
            if (bl++ < 5) ESP_LOGW(TAG, "[切帧] 坏帧长 %d，丢 1 字节", flen);
            h.pending.erase(h.pending.begin());
            continue;
        }
        if (h.pending.size() < (size_t)flen) return;
        HlsDecodeOneFrame(h.pending.data(), (size_t)flen);
        h.pending.erase(h.pending.begin(), h.pending.begin() + flen);
    }
}

}  // namespace

SdRadio& SdRadio::GetInstance() {
    static SdRadio instance;
    return instance;
}

void SdRadio::SetError(const char* msg) {
    strncpy(error_, msg, sizeof(error_) - 1);
    error_[sizeof(error_) - 1] = '\0';
    ESP_LOGE(TAG, "%s", msg);
}

void SdRadio::SetStatus(const char* s) {
    strncpy(status_, s, sizeof(status_) - 1);
    status_[sizeof(status_) - 1] = '\0';
}

bool SdRadio::Scan() {
    stations_.clear();
    if (!SdCardManager::GetInstance().IsMounted()) {
        ESP_LOGW(TAG, "TF卡没挂上，读不到电台列表");
        return false;
    }
    // 目录不存在就自动建，免得还要手动去建
    struct stat st = {};
    if (stat("/sdcard/radio", &st) != 0) {
        if (SdCardManager::GetInstance().LockFs()) {
            mkdir("/sdcard/radio", 0775);
            SdCardManager::GetInstance().UnlockFs();
        }
    }
    if (!SdCardManager::GetInstance().LockFs()) return false;
    FILE* f = fopen(kListFile, "r");
    if (f != nullptr) {
        static char line[512];
        while (fgets(line, sizeof(line), f) != nullptr) {
            char* nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            if (line[0] == '#' || line[0] == '\0') continue;
            char* bar = strchr(line, '|');
            if (bar == nullptr) continue;
            *bar = '\0';
            Station st;
            st.name = line;
            st.url = bar + 1;
            // 去掉名字两端空格
            while (!st.name.empty() && st.name.back() == ' ') st.name.pop_back();
            while (!st.url.empty() && st.url.front() == ' ') st.url.erase(0, 1);
            if (!st.name.empty() && !st.url.empty()) {
                stations_.push_back(std::move(st));
            }
        }
        fclose(f);
    }
    SdCardManager::GetInstance().UnlockFs();
    // 重读后校正索引：新列表可能比原来短（用户删了台），index_ 不能越界
    if (index_.load() >= (int)stations_.size()) index_ = 0;
    if (index_.load() < 0) index_ = 0;
    ESP_LOGI(TAG, "电台列表 %u 个（当前索引 %d）", (unsigned)stations_.size(), index_.load());
    return !stations_.empty();
}

bool SdRadio::WriteDefaultList() {
    static const char* kDefault =
        "# 网络收音机列表：一行一个台，格式 名称|URL\n"
        "# CNR 与甘肃走 TS 封装的 HLS；兰州台是裸 ADTS 分片\n"
        "01 中国之声|http://ngcdn002.cnr.cn/live/zgzs/index.m3u8\n"
        "02 经济之声|http://ngcdn002.cnr.cn/live/jjzs/index.m3u8\n"
        "03 音乐之声|http://ngcdn002.cnr.cn/live/yyzs/index.m3u8\n"
        "04 香港之声|http://ngcdn002.cnr.cn/live/xgzs/index.m3u8\n"
        "05 华夏之声|http://ngcdn002.cnr.cn/live/hxzs/index.m3u8\n"
        "06 甘肃新闻综合|https://rscdn.ajmide.com/r_1249/nglive_1249.m3u8\n"
        "07 甘肃交通广播|https://rscdn.ajmide.com/r_1224/nglive_1224.m3u8\n"
        "08 甘肃农村广播|https://rscdn.ajmide.com/r_1246/nglive_1246.m3u8\n"
        "09 中国交通甘肃|https://rscdn.ajmide.com/r_2865/nglive_2865.m3u8\n"
        "10 兰州综合广播|https://liveplus.lzr.com.cn/aac_zhgb/HD/live.m3u8\n"
        "11 兰州交通音乐|https://liveplus.lzr.com.cn/aac_jtyy/HD/live.m3u8\n"
        "12 兰州生活文艺|https://liveplus.lzr.com.cn/aac_shwy/HD/live.m3u8\n";

    if (!SdCardManager::GetInstance().IsMounted()) return false;
    struct stat st = {};
    if (stat("/sdcard/radio", &st) != 0) {
        if (SdCardManager::GetInstance().LockFs()) {
            mkdir("/sdcard/radio", 0775);
            SdCardManager::GetInstance().UnlockFs();
        }
    }
    if (!SdCardManager::GetInstance().LockFs()) return false;
    FILE* f = fopen(kListFile, "w");
    bool ok = false;
    if (f != nullptr) {
        size_t n = strlen(kDefault);
        ok = fwrite(kDefault, 1, n, f) == n;
        fclose(f);
    }
    SdCardManager::GetInstance().UnlockFs();
    ESP_LOGW(TAG, "写入默认电台列表: %s", ok ? "成功" : "失败");
    if (ok) Scan();
    return ok;
}

const char* SdRadio::NameAt(int idx) const {
    if (idx < 0 || idx >= (int)stations_.size()) return "";
    return stations_[idx].name.c_str();
}

bool SdRadio::Play(int idx) {
    if (stations_.empty() && !Scan()) {
        SetError("卡上没有电台列表");
        return false;
    }
    if (idx < 0 || idx >= (int)stations_.size()) idx = 0;

    if (playing_.load()) {
        // 关键：只设 stop_request_ 不够 —— 下载读循环、拆包循环、推队列重试
        // 看的都是 g_abort_download。不设这个，旧任务根本不会中断。
        g_abort_download = true;
        stop_request_ = true;
        // 等旧任务真正退出（它的退出路径会把 playing_ 置 false），最多 10 秒
        for (int i = 0; i < 200 && playing_.load(); i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (playing_.load()) {
            ESP_LOGE(TAG, "旧播放任务 10 秒未退出，强行切换（可能出现双任务并存）");
        }
    }
    error_[0] = '\0';
    SetStatus("连接中");
    index_ = idx;
    snprintf(now_playing_, sizeof(now_playing_), "%s", stations_[idx].name.c_str());
    stop_request_ = false;
    paused_ = false;
    playing_ = true;
    pending_url_ = stations_[idx].url;
    pending_idx_ = idx;

    BaseType_t ret = xTaskCreate(TaskEntry, "sd_radio", 6144, this, 3, nullptr);
    ESP_LOGW(TAG, "播放任务创建: %d, 空闲内部RAM %u 字节", (int)ret,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (ret != pdPASS) {
        playing_ = false;
        SetError("播放任务创建失败（内存不足）");
        return false;
    }
    ESP_LOGW(TAG, "开始收听 #%d %s | %s", idx, now_playing_, pending_url_.c_str());
    return true;
}

void SdRadio::TaskEntry(void* arg) {
    auto* self = static_cast<SdRadio*>(arg);
    std::string url = self->pending_url_;
    int idx = self->pending_idx_;
    self->PlayerLoop(idx, url);
    vTaskDelete(nullptr);
}

void SdRadio::ReleaseBuffers() {
    if (playing_.load()) return;      // 正在播就别动
    if (s_pf_running || s_ls_running) return;
    if (g_tsbuf != nullptr) { heap_caps_free(g_tsbuf); g_tsbuf = nullptr; }
    if (g_pf_buf != nullptr) { heap_caps_free(g_pf_buf); g_pf_buf = nullptr; }
    if (g_m3u8buf != nullptr) { heap_caps_free(g_m3u8buf); g_m3u8buf = nullptr; }
    if (g_ls_buf != nullptr) { heap_caps_free(g_ls_buf); g_ls_buf = nullptr; }
    s_pf_len = 0;
    s_pf_done = false;
    s_ls_len = 0;
    s_ls_done = false;
    ESP_LOGI(TAG, "收音机缓冲已释放（让给相册解码等大块需求）");
}

void SdRadio::Stop() {
    g_abort_download = true;   // 打断正在进行的 HTTP 读
    stop_request_ = true;
    // 必须等旧任务真正退出再让上层起新任务：强行置 playing_=false 会导致
    // 新旧两个播放任务并存，撞在 PSRAM/解码器上就是堆损坏崩溃。
    for (int i = 0; i < 200 && playing_.load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));   // 最多等 10 秒
    }
    if (playing_.load()) {
        ESP_LOGE(TAG, "旧播放任务 10 秒仍未退出，强制停止（可能内存已被污染）");
        playing_ = false;
    }
    paused_ = false;
    SetStatus("已停止");
}

void SdRadio::TogglePause() {
    if (!playing_.load()) return;
    paused_ = !paused_.load();
    SetStatus(paused_.load() ? "已暂停" : "播放中");
}

namespace {

// HTTP GET 到内存。返回是否拿到数据。
bool HttpGet(const std::string& url, uint8_t* buf, size_t max, size_t* out_len) {
    *out_len = 0;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 8192;
    cfg.buffer_size_tx = 512;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = false;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == nullptr) return false;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open 失败 (%s): %s", esp_err_to_name(err), url.c_str());
        esp_http_client_cleanup(c);
        return false;
    }
    int64_t clen = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status >= 400) {
        ESP_LOGW(TAG, "HTTP %d: %s", status, url.c_str());
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return false;
    }
    size_t total = 0;
    while (total < max) {
        if (g_abort_download) {   // 被叫停，别再读了
            break;
        }
        int r = esp_http_client_read(c, (char*)buf + total, (int)(max - total));
        if (r <= 0) break;
        total += (size_t)r;
        if (clen > 0 && total >= (size_t)clen) break;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    *out_len = total;
    return total > 0;
}

// 解析 m3u8：把相对路径按列表地址补全
int ParseM3u8(const std::string& base_url, const char* content, size_t len,
              std::vector<std::string>& segs) {
    segs.clear();
    std::string dir;
    size_t slash = base_url.find_last_of('/');
    if (slash != std::string::npos && base_url.rfind("http", 0) == 0) {
        dir = base_url.substr(0, slash + 1);
    }

    size_t pos = 0;
    while (pos < len) {
        size_t eol = pos;
        while (eol < len && content[eol] != '\n') eol++;
        size_t llen = eol - pos;
        if (llen > 0 && content[pos + llen - 1] == '\r') llen--;
        if (llen > 0 && content[pos] != '#') {
            std::string seg(content + pos, llen);
            if (seg.rfind("http", 0) == 0) {
                segs.push_back(seg);
            } else if (!seg.empty() && seg[0] == '/') {
                // 绝对路径：/aac_zhgb_HD/xxx.aac → 补全成 scheme://host/path
                size_t sch = base_url.find("://");
                if (sch != std::string::npos) {
                    size_t host_end = base_url.find('/', sch + 3);
                    std::string origin = (host_end == std::string::npos)
                                             ? base_url
                                             : base_url.substr(0, host_end);
                    segs.push_back(origin + seg);
                }
            } else if (!dir.empty()) {
                segs.push_back(dir + seg);
            }
        }
        pos = eol + 1;
    }
    return (int)segs.size();
}

}  // namespace

bool SdRadio::PlayHls(const std::string& url) {
    // 持有播放锁直到本函数返回：保证旧任务完全退出后才动解码器和静态缓冲
    PlayLockGuard lock;
    g_abort_download = false;
    // 严格照 esp_audio_codec 官方测试示例的写法：
    // 1) 必须传 esp_ts_dec_cfg_t + cfg_size，否则返回 NOT_SUPPORT(-7)
    // 2) 小块喂数据（官方 512 字节一次），不要整块灌
    // 3) 用 while (raw.len) 循环，process 内部会递减 raw.len
    // 4) eos 只在数据末尾设一次
    if (!lock.ok()) {
        SetError("上一个播放还没退出，稍后再试");
        ESP_LOGE(TAG, "拿不到播放锁（旧任务未退出）");
        return false;
    }
    // 等上一次的预取任务收尾，否则它会继续往 g_pf_buf 里写
    for (int i = 0; i < 150 && s_pf_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_pf_done = false;
    s_pf_len = 0;
    s_pf_url.clear();
    // 等上一次的列表预拉收尾，再复位
    for (int i = 0; i < 150 && s_ls_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_ls_done = false;
    s_ls_len = 0;
    s_ls_url.clear();
    if (!s_registered) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        s_registered = true;
        ESP_LOGW(TAG, "已注册帧解码器 + 容器解析器");
    }
    // 库自带的 TS 解析器在 CNR 这类流上解不出帧（process 一律 FAIL），
    // 改成自己拆 TS + 裸 AAC 帧解码：use_frame_dec=true 表示"跳过解析，直接帧解码"
    // 走专用 AAC 解码器（跟 MP3 那条能出声的路径同构），绕开 simple_dec 封装
    // 注意：必须先关掉上一次的句柄。之前不关，切一台漏一个，切到第九台就
    // "aac 解码器打不开"，之后所有台全废。
    if (g_hls.aac != nullptr) {
        esp_aac_dec_close(g_hls.aac);
        g_hls.aac = nullptr;
    }
    static esp_aac_dec_cfg_t aac_cfg = {};
    aac_cfg.aac_plus_enable = true;
    esp_audio_err_t derr = esp_aac_dec_open(&aac_cfg, sizeof(aac_cfg), &g_hls.aac);
    ESP_LOGW(TAG, "AAC 专用解码器 open 返回 0x%x（cfg_sz=%u）", (int)derr,
             (unsigned)sizeof(aac_cfg));
    if (derr != ESP_AUDIO_ERR_OK || g_hls.aac == nullptr) {
        ESP_LOGE(TAG, "aac open 失败 0x%x，空闲内部RAM=%u 字节", (int)derr,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        SetError("AAC 解码器打不开");
        return false;
    }
    if (g_hls.outbuf == nullptr) {
        g_hls.out_cap = 8192;
        g_hls.outbuf = (uint8_t*)heap_caps_malloc(g_hls.out_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (g_hls.outbuf == nullptr) {
            SetError("解码缓冲分配失败");
            return false;
        }
    }
    g_hls.pending.clear();

    // 这两块大缓冲改成进程内静态复用：切台时新旧任务会短暂并存，
    // 一旦其中一方 free、另一方还在用，TLSF 堆就会 assert（block_merge_prev）。
    // 静态复用 + 永不释放，从根上避免 double free。
    if (g_m3u8buf == nullptr) g_m3u8buf = PsramAlloc(kM3u8Max);
    if (g_tsbuf == nullptr) g_tsbuf = PsramAlloc(kSegMax);
    uint8_t* m3u8buf = g_m3u8buf;
    uint8_t* tsbuf = g_tsbuf;

    // 重采样已搬进 HlsDecodeOneFrame（用 g_hls 里的状态），这里不再需要
    uint32_t total_samples = 0;
    uint32_t pushed_ok = 0, pushed_drop = 0;
    TsDemux demux;              // 自己拆 TS（库自带的在 CNR 流上不可用）
    std::vector<std::string> segs;
    std::string last_seg;
    bool ok = true;

    if (m3u8buf == nullptr || tsbuf == nullptr || g_hls.outbuf == nullptr) {
        SetError("PSRAM 分配失败");
        return false;   // 静态缓冲不释放，交给下次复用
    }

    ESP_LOGW(TAG, "串行播放开始，等第一个分片");
    int empty_rounds = 0;
    while (!stop_request_.load()) {
        // 1) 需要时拉列表
        if (segs.empty()) {
            size_t mlen = 0;
            SetStatus("取列表");
            if (!HttpGet(url, m3u8buf, kM3u8Max, &mlen)) {
                if (++empty_rounds > 5) {
                    SetError("拿不到 m3u8 列表");
                    ok = false;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            int n = ParseM3u8(url, (const char*)m3u8buf, mlen, segs);
            ESP_LOGW(TAG, "列表 %d 个分片", n);
            if (n == 0) {
                if (++empty_rounds > 5) {
                    SetError("列表里没有分片");
                    ok = false;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            empty_rounds = 0;
        }

        // 1.5) 列表快用完了就提前拉新的（异步，不打断播放）
        if (!s_ls_running && !s_ls_done && segs.size() <= 2 && !s_pf_running) {
            s_ls_url = url;
            s_ls_len = 0;
            s_ls_running = true;
            s_ls_done = false;
            if (g_ls_buf == nullptr) g_ls_buf = PsramAlloc(kM3u8Max);
            if (g_ls_buf != nullptr) {
                xTaskCreate(HlsListFetchEntry, "hls_ls", 4096, nullptr, 1, nullptr);
            } else {
                s_ls_running = false;
            }
        }
        // 拉回来了就并进队列（按 URL 去重，避免重播同一片）
        if (s_ls_done && s_ls_len > 0) {
            std::vector<std::string> fresh;
            ParseM3u8(url, (const char*)g_ls_buf, s_ls_len, fresh);
            int added = 0;
            for (size_t fi = 0; fi < fresh.size(); fi++) {
                bool dup = false;
                for (size_t ei = 0; ei < segs.size(); ei++) {
                    if (segs[ei] == fresh[fi]) {
                        dup = true;
                        break;
                    }
                }
                if (!dup && fresh[fi] != last_seg) {
                    segs.push_back(fresh[fi]);
                    added++;
                }
            }
            if (added > 0) ESP_LOGI(TAG, "[列表] 提前补进 %d 片", added);
            s_ls_done = false;
            s_ls_len = 0;
        }

        // 2) 取一个没播过的分片
        std::string seg;
        while (!segs.empty()) {
            seg = segs.front();
            segs.erase(segs.begin());
            if (seg != last_seg) break;
            seg.clear();
        }
        if (seg.empty()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        while (paused_.load() && !stop_request_.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (stop_request_.load()) break;

        // 3) 下载
        SetStatus("缓冲中");
        size_t tlen = 0;
        bool dl_ok = false;
        uint8_t* curbuf = tsbuf;
        if (s_pf_done && s_pf_len > 0) {
            // 拷贝而不是交换指针：交换会让预取缓冲指向正在解码的数据，
            // 下一轮预取任务直接覆盖，实测 abort() 崩溃。
            memcpy(tsbuf, g_pf_buf, s_pf_len);
            curbuf = tsbuf;
            tlen = s_pf_len;
            dl_ok = true;
            s_pf_len = 0;
            s_pf_done = false;
            s_pf_url.clear();
        } else {
            dl_ok = HttpGet(seg, tsbuf, kSegMax, &tlen);
        }
        if (!dl_ok) {
            ESP_LOGW(TAG, "分片下载失败，跳过");
            continue;
        }
        last_seg = seg;
        SetStatus("播放中");

        // 4) 自己解 TS：抽出 ADTS 帧交给 AAC 解码器（库的 TS 解析器在这类流上不可用）
        SetStatus("播放中");
        if (g_abort_download || stop_request_.load()) break;
        // 先给下一片开个后台下载任务：紧接着的解码会阻塞好几秒（推 PCM 等队列），
        // 正好让下载并行跑完，下轮循环直接命中预取结果。
        if (!s_pf_running && !s_pf_done && !segs.empty()) {
            std::string next;
            for (size_t qi = 0; qi < segs.size(); qi++) {
                if (!segs[qi].empty() && segs[qi] != last_seg) {
                    next = segs[qi];
                    break;
                }
            }
            if (!next.empty()) {
                s_pf_url = next;
                s_pf_len = 0;
                s_pf_done = false;
                s_pf_running = true;
                if (g_pf_buf == nullptr) g_pf_buf = PsramAlloc(kSegMax);
                if (g_pf_buf != nullptr) {
                    xTaskCreate(HlsPrefetchEntry, "hls_pf", 4096, nullptr, 2, nullptr);
                } else {
                    s_pf_running = false;
                }
            }
        }
        uint32_t f0 = g_hls.frames_ok, p0 = g_hls.pushed_ok, d0 = g_hls.pushed_drop;
        if (tlen > (size_t)kTsPacket && curbuf[0] == 0x47) {
            demux.Feed(curbuf, tlen, HlsAdtsSink, nullptr);   // TS 封装（CNR / 甘肃）
        } else {
            HlsAdtsSink(nullptr, curbuf, tlen);               // 裸 ADTS（兰州台，前面可能带 ID3）
        }
        int frames_ok = (int)(g_hls.frames_ok - f0);
        pushed_ok = g_hls.pushed_ok - p0;
        pushed_drop = g_hls.pushed_drop - d0;
        total_samples += pushed_ok;

        static int log_cnt = 0;
        if (log_cnt++ < 20) {
            ESP_LOGW(TAG, "这片：解出 %d 帧，送入 %u 次丢 %u 次，累计 %u 秒",
                     frames_ok, (unsigned)pushed_ok, (unsigned)pushed_drop,
                     (unsigned)(total_samples / kOutRate));
        }
    }

    // 重采样状态归 g_hls 管（静态复用），这里只需清掉残留帧数据
    g_hls.pending.clear();
    // 静态缓冲不释放（下次复用），但解码器要关，否则句柄泄漏
    if (g_hls.aac != nullptr) {
        esp_aac_dec_close(g_hls.aac);
        g_hls.aac = nullptr;
    }
    ESP_LOGW(TAG, "HLS 结束，累计播放 %u 秒", (unsigned)(total_samples / kOutRate));
    return ok;
}

// MP3 直播流：HTTP 边下边解，帧解析和本地音乐用的是同一套逻辑
bool SdRadio::PlayMp3Stream(const std::string& url) {
    PlayLockGuard lock;   // 与 HLS 共用一把锁，防止新旧任务并存
    g_abort_download = false;
    if (!lock.ok()) {
        SetError("上一个播放还没退出，稍后再试");
        return false;
    }
    // MP3 帧头解析（和 sd_music 同一套规则）
    struct FrameInfo {
        uint32_t len;
        uint32_t rate;
        uint32_t ch;
    };
    auto parse_frame = [](const uint8_t* p, size_t avail, FrameInfo* out) -> bool {
        if (avail < 4) return false;
        if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
        int ver = (p[1] >> 3) & 0x03;
        int layer = (p[1] >> 1) & 0x03;
        int br_idx = (p[2] >> 4) & 0x0F;
        int sr_idx = (p[2] >> 2) & 0x03;
        int pad = (p[2] >> 1) & 0x01;
        int ch_mode = (p[3] >> 6) & 0x03;
        if (ver == 1 || layer != 1) return false;
        if (br_idx == 0 || br_idx == 15 || sr_idx == 3) return false;
        static const uint16_t kBrV1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160,
                                           192, 224, 256, 320, 0};
        static const uint16_t kBrV2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96,
                                           112, 128, 144, 160, 0};
        static const uint32_t kSrV1[4] = {44100, 48000, 32000, 0};
        uint32_t bitrate = (uint32_t)(ver == 3 ? kBrV1[br_idx] : kBrV2[br_idx]) * 1000u;
        uint32_t sr = kSrV1[sr_idx];
        if (ver == 2) sr /= 2;
        else if (ver == 0) sr /= 4;
        if (bitrate == 0 || sr == 0) return false;
        uint32_t flen = (ver == 3) ? (144u * bitrate / sr + (uint32_t)pad)
                                   : (72u * bitrate / sr + (uint32_t)pad);
        if (flen < 24 || flen > 4096) return false;
        out->len = flen;
        out->rate = sr;
        out->ch = (ch_mode == 3) ? 1 : 2;
        return true;
    };

    if (!s_registered) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        s_registered = true;
    }
    void* mp3 = nullptr;
    esp_audio_err_t oerr = esp_mp3_dec_open(nullptr, 0, &mp3);
    ESP_LOGW(TAG, "MP3 解码器 open 返回 0x%x", (int)oerr);
    if (oerr != ESP_AUDIO_ERR_OK || mp3 == nullptr) {
        SetError("MP3 解码器打不开");
        return false;
    }

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 8192;
    cfg.buffer_size_tx = 512;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = false;

    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h == nullptr) {
        esp_mp3_dec_close(mp3);
        SetError("HTTP 初始化失败");
        return false;
    }
    if (esp_http_client_open(h, 0) != ESP_OK) {
        esp_http_client_cleanup(h);
        esp_mp3_dec_close(mp3);
        SetError("连不上电台");
        return false;
    }
    esp_http_client_fetch_headers(h);
    int status = esp_http_client_get_status_code(h);
    ESP_LOGW(TAG, "MP3 流 HTTP %d", status);
    if (status >= 400) {
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        esp_mp3_dec_close(mp3);
        SetError("电台返回错误");
        return false;
    }
    SetStatus("播放中");

    constexpr size_t kInMax = 64 * 1024;
    uint8_t* inbuf = PsramAlloc(kInMax);
    size_t out_cap = kOutBufStart;
    uint8_t* outbuf = PsramAlloc(out_cap);
    uint8_t* rsvbuf = nullptr;
    size_t rsv_cap = 0;
    std::vector<int16_t> mono;
    esp_ae_rate_cvt_handle_t rsv = nullptr;
    int src_rate = 0;
    uint32_t total = 0, pushed_ok = 0, pushed_drop = 0;
    size_t in_len = 0;
    bool ok = true;
    int frame_ok = 0, read_fail = 0;

    if (inbuf == nullptr || outbuf == nullptr) {
        SetError("PSRAM 分配失败");
        ok = false;
    }

    while (ok && !stop_request_.load()) {
        // 补数据
        if (in_len < kInMax / 2) {
            int r = esp_http_client_read(h, (char*)inbuf + in_len, (int)(kInMax - in_len));
            if (r > 0) {
                in_len += (size_t)r;
                read_fail = 0;
            } else {
                if (++read_fail > 20) {
                    SetError("流断了（20 次读不到数据）");
                    ok = false;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        // 切帧解码
        size_t pos = 0;
        while (pos + 4 <= in_len && !stop_request_.load()) {
            FrameInfo fi;
            if (!parse_frame(inbuf + pos, in_len - pos, &fi)) {
                pos++;
                continue;
            }
            if (pos + fi.len > in_len) break;
            esp_audio_dec_in_raw_t raw = {};
            raw.buffer = inbuf + pos;
            raw.len = fi.len;
            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = outbuf;
            frame.len = (uint32_t)out_cap;
            esp_audio_dec_info_t dinfo = {};
            esp_audio_err_t rr = esp_mp3_dec_decode(mp3, &raw, &frame, &dinfo);
            pos += fi.len;
            if (rr != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) continue;
            frame_ok++;

            while (paused_.load() && !stop_request_.load()) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            const int16_t* pcm = (const int16_t*)outbuf;
            size_t tot = frame.decoded_size / sizeof(int16_t);
            size_t frames = tot / fi.ch;
            if (frames == 0) continue;
            mono.resize(frames);
            if (fi.ch >= 2) {
                for (size_t k = 0; k < frames; k++) {
                    int32_t sum = 0;
                    for (size_t cc = 0; cc < fi.ch; cc++) sum += pcm[k * fi.ch + cc];
                    mono[k] = (int16_t)(sum / (int32_t)fi.ch);
                }
            } else {
                memcpy(mono.data(), pcm, frames * sizeof(int16_t));
            }

            const int16_t* outp = mono.data();
            size_t outn = frames;
            if ((int)fi.rate != kOutRate) {
                if (rsv == nullptr || src_rate != (int)fi.rate) {
                    if (rsv != nullptr) esp_ae_rate_cvt_close(rsv);
                    esp_ae_rate_cvt_cfg_t rc = {};
                    rc.src_rate = fi.rate;
                    rc.dest_rate = kOutRate;
                    rc.channel = 1;
                    rc.bits_per_sample = 16;
                    rc.complexity = 2;
                    rc.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                    if (esp_ae_rate_cvt_open(&rc, &rsv) != ESP_AE_ERR_OK || rsv == nullptr) {
                        ok = false;
                        break;
                    }
                    src_rate = (int)fi.rate;
                    ESP_LOGW(TAG, "MP3 流重采样 %d -> %d", src_rate, kOutRate);
                }
                uint32_t need = 0;
                esp_ae_rate_cvt_get_max_out_sample_num(rsv, (uint32_t)frames, &need);
                if (rsvbuf == nullptr || rsv_cap < need) {
                    if (rsvbuf != nullptr) heap_caps_free(rsvbuf);
                    rsv_cap = need + 64;
                    rsvbuf = PsramAlloc(rsv_cap * sizeof(int16_t));
                    if (rsvbuf == nullptr) {
                        ok = false;
                        break;
                    }
                }
                uint32_t got = (uint32_t)rsv_cap;
                if (esp_ae_rate_cvt_process(rsv, (esp_ae_sample_t)mono.data(), (uint32_t)frames,
                                            (esp_ae_sample_t)rsvbuf, &got) != ESP_AE_ERR_OK ||
                    got == 0) {
                    continue;
                }
                outp = (const int16_t*)rsvbuf;
                outn = got;
            }

            auto& audio = Application::GetInstance().GetAudioService();
            size_t o = 0;
            while (o < outn && !stop_request_.load()) {
                size_t k = std::min(kPushSamples, outn - o);
                std::vector<int16_t> chunk(outp + o, outp + o + k);
                if (!audio.PushPcmToPlaybackQueue(std::move(chunk), false)) {
                    pushed_drop++;
                    vTaskDelay(pdMS_TO_TICKS(15));
                    continue;
                }
                o += k;
                total += (uint32_t)k;
                pushed_ok++;
            }
        }
        if (pos > 0) {
            memmove(inbuf, inbuf + pos, in_len - pos);
            in_len -= pos;
        }
        static int lg = 0;
        if (lg++ < 10) {
            ESP_LOGW(TAG, "[MP3流] 帧 %d，送入 %u 次丢 %u 次，累计 %u 秒", frame_ok,
                     (unsigned)pushed_ok, (unsigned)pushed_drop, (unsigned)(total / kOutRate));
        }
    }

    if (rsv != nullptr) esp_ae_rate_cvt_close(rsv);
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    esp_mp3_dec_close(mp3);
    if (inbuf) heap_caps_free(inbuf);
    g_hls.pending.clear();
    if (rsvbuf) heap_caps_free(rsvbuf);
    ESP_LOGW(TAG, "MP3 流结束，累计 %u 秒", (unsigned)(total / kOutRate));
    return ok;
}

void SdRadio::PlayerLoop(int idx, std::string url) {
    (void)idx;
    bool is_hls = url.find(".m3u8") != std::string::npos;
    bool ok = is_hls ? PlayHls(url) : PlayMp3Stream(url);
    playing_ = false;
    paused_ = false;
    if (ok) {
        SetStatus("已停止");
    }
}