#include "sd_log.h"
#include "sd_card_manager.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <atomic>

#include <sys/stat.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_app_desc.h>

#include "system_info.h"

static const char* TAG = "SdLog";

void SdLogBootBanner();   // 由 flush 任务在挂载成功后调用

namespace {

constexpr size_t kRingSize = 16 * 1024;
constexpr size_t kFlushChunk = 8192;
constexpr size_t kLogFileLimit = 256 * 1024;
constexpr const char* kLogPaths[2] = {"/sdcard/log/log0.txt", "/sdcard/log/log1.txt"};
constexpr const char* kNoteDir = "/sdcard/notes";

char* s_ring = nullptr;
size_t s_head = 0;
size_t s_tail = 0;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t s_prev_vprintf = nullptr;
std::atomic<int> s_cur_file{0};
std::atomic<bool> s_installed{false};
std::atomic<bool> s_notes_always{false};   // 记录模式：每句话都落盘
time_t s_last_stamp = 0;

// 跳过 ANSI 颜色码，取等级字母（E/W/I/D/V）
char LevelOf(const char* line) {
    const char* p = line;
    while (p[0] == '\033') {
        while (*p && *p != 'm') p++;
        if (*p == 'm') p++;
    }
    if (p[0] != '\0' && p[1] == ' ') {
        return p[0];
    }
    return '\0';
}

void RingPush(const char* data, size_t len) {
    if (s_ring == nullptr || len == 0) return;
    if (len > kRingSize / 2) len = kRingSize / 2;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < len; i++) {
        size_t next = (s_head + 1) % kRingSize;
        if (next == s_tail) break;      // 满了就丢新的，保老证据
        s_ring[s_head] = data[i];
        s_head = next;
    }
    portEXIT_CRITICAL(&s_lock);
}

size_t RingPop(char* dst, size_t max) {
    size_t n = 0;
    portENTER_CRITICAL(&s_lock);
    while (s_tail != s_head && n < max) {
        dst[n++] = s_ring[s_tail];
        s_tail = (s_tail + 1) % kRingSize;
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

bool WriteRaw(const char* path, const char* data, size_t len, const char* mode) {
    if (!SdCardManager::GetInstance().LockFs()) return false;
    FILE* f = fopen(path, mode);
    bool ok = false;
    if (f != nullptr) {
        ok = fwrite(data, 1, len, f) == len;
        fclose(f);
    }
    SdCardManager::GetInstance().UnlockFs();
    return ok;
}

long FileSize(const char* path) {
    struct stat st = {};
    if (!SdCardManager::GetInstance().LockFs()) return 0;
    int r = stat(path, &st);
    SdCardManager::GetInstance().UnlockFs();
    return (r == 0) ? (long)st.st_size : 0;
}

const char* ResetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "上电";
        case ESP_RST_EXT:      return "外部复位";
        case ESP_RST_SW:       return "软重启";
        case ESP_RST_PANIC:    return "崩溃PANIC";
        case ESP_RST_INT_WDT:  return "中断看门狗";
        case ESP_RST_TASK_WDT: return "任务看门狗";
        case ESP_RST_WDT:      return "看门狗";
        case ESP_RST_BROWNOUT: return "掉电BROWNOUT";
        case ESP_RST_DEEPSLEEP:return "深睡唤醒";
        default:               return "其他";
    }
}

// 自检：把卡上日志文件的大小和开头读回来打到串口，没有读卡器也能确认落盘成功
void SelfCheck() {
    struct stat st = {};
    if (!SdCardManager::GetInstance().LockFs()) return;
    int r = stat(kLogPaths[0], &st);
    SdCardManager::GetInstance().UnlockFs();
    if (r != 0) {
        ESP_LOGW(TAG, "[自检] %s 还不存在（首次开机属正常）", kLogPaths[0]);
        return;
    }

    char head[192] = {0};
    if (!SdCardManager::GetInstance().LockFs()) return;
    FILE* f = fopen(kLogPaths[0], "r");
    size_t n = 0;
    if (f != nullptr) {
        n = fread(head, 1, sizeof(head) - 1, f);
        fclose(f);
    }
    SdCardManager::GetInstance().UnlockFs();

    for (size_t i = 0; i < n; i++) {
        if (head[i] == '\n' || head[i] == '\r') head[i] = '|';
    }
    ESP_LOGI(TAG, "[自检] %s 大小=%ld 字节 开头%u字节: %s",
             kLogPaths[0], (long)st.st_size, (unsigned)n, head);
}

void FlushTask(void*) {
    char* buf = (char*)heap_caps_malloc(kFlushChunk, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) {
        buf = (char*)malloc(kFlushChunk);
    }
    if (buf == nullptr) {
        ESP_LOGE(TAG, "日志缓冲分配失败，日志功能停用");
        vTaskDelete(nullptr);
        return;
    }

    bool bannered = false;
    bool self_checked = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(300));
        if (!SdCardManager::GetInstance().IsMounted()) continue;

        if (!bannered) {
            bannered = true;
            SdLogBootBanner();
        }

        size_t n = RingPop(buf, kFlushChunk);
        if (n == 0) continue;

        int idx = s_cur_file.load();
        const char* path = kLogPaths[idx];
        if (FileSize(path) > (long)kLogFileLimit) {
            idx = 1 - idx;
            s_cur_file = idx;
            path = kLogPaths[idx];
            WriteRaw(path, "", 0, "w");     // 轮转时清空新文件
        }
        WriteRaw(path, buf, n, "a");

        if (!self_checked) {
            self_checked = true;
            SelfCheck();   // 第一次真正落盘后自证：文件存在、大小、开头内容
        }
    }
}

int LogHook(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int ret = s_prev_vprintf ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);

    // 栈帧受 -Werror=frame-larger-than=1536 约束，这里只放 512
    char line[512];
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (n > 0 && s_ring != nullptr) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        char lvl = LevelOf(line);
        bool keep = (lvl == 'E' || lvl == 'W');
        if (!keep && lvl == 'I') {
            // 自己的模块信息全留，方便验证
            keep = (strstr(line, "SdCard") != nullptr) || (strstr(line, "SdLog") != nullptr) ||
                   (strstr(line, "Application: Startup") != nullptr);
        }
        if (keep) {
            RingPush(line, len);
        }
    }
    return ret;
}

}  // namespace

void SdLogInitEarly() {
    if (s_installed.exchange(true)) return;

    s_ring = (char*)heap_caps_malloc(kRingSize, MALLOC_CAP_SPIRAM);
    if (s_ring == nullptr) {
        s_ring = (char*)malloc(kRingSize);
    }
    if (s_ring == nullptr) {
        ESP_LOGE(TAG, "环形缓冲分配失败，日志不写入卡");
        return;
    }

    s_prev_vprintf = esp_log_set_vprintf(LogHook);
    xTaskCreate(FlushTask, "sd_log", 4096, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "日志黑匣子就绪 (ring=%u KB, 单文件上限=%u KB)",
             (unsigned)(kRingSize / 1024), (unsigned)(kLogFileLimit / 1024));
}

void SdLogBootBanner() {
    if (!SdCardManager::GetInstance().IsMounted()) return;

    const esp_app_desc_t* app = esp_app_get_description();
    char line[320];
    int n = snprintf(line, sizeof(line),
                     "=== 小瑞开机 | 固件 %s | MAC %s | TF卡 %llu MB | 复位原因: %s ===\n",
                     app != nullptr ? app->version : "?",
                     SystemInfo::GetMacAddress().c_str(),
                     (unsigned long long)SdCardManager::GetInstance().CapacityMb(),
                     ResetReasonStr(esp_reset_reason()));
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        WriteRaw(kLogPaths[s_cur_file.load()], line, len, "a");
    }

    const esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
        r == ESP_RST_WDT || r == ESP_RST_BROWNOUT) {
        int m = snprintf(line, sizeof(line), "!!! 上次运行不是正常重启: %s\n", ResetReasonStr(r));
        if (m > 0) {
            size_t len = (size_t)m < sizeof(line) ? (size_t)m : sizeof(line) - 1;
            WriteRaw(kLogPaths[s_cur_file.load()], line, len, "a");
        }
    }
}

void SdNotesSetAlways(bool on) {
    s_notes_always = on;
    ESP_LOGI(TAG, "记录模式: %s", on ? "开" : "关");
}

bool SdNotesGetAlways() {
    return s_notes_always.load();
}

void SdNotesOnUserText(const char* text) {
    if (text == nullptr || text[0] == '\0') return;
    if (!SdCardManager::GetInstance().IsMounted()) return;

    // 服务器偶尔会把唤醒词本身当识别结果下发（实测出现 "Ni3Hao3Xiao3Rui4"），
    // 这类纯唤醒噪声绝不能被记进备忘
    static const char* kNoise[] = {"Xiao3Rui4", "xiao3rui4", "Ni3Hao3", "ni3hao3",
                                   "XiaoRui", "xiaorui", "NihaoXiaorui"};
    for (const char* noise : kNoise) {
        if (strstr(text, noise) != nullptr) return;
    }

    static const char* kTriggers[] = {"记一下", "帮我记", "记录一下", "备忘", "记下来", "记下"};
    bool hit = false;
    for (const char* t : kTriggers) {
        if (strstr(text, t) != nullptr) {
            hit = true;
            break;
        }
    }
    if (!hit && !s_notes_always.load()) return;

    time_t now = time(nullptr);
    struct tm tm_now = {};
    char path[64];
    char stamp[32];
    if (now > 1735689600) {
        localtime_r(&now, &tm_now);
        snprintf(path, sizeof(path), "%s/%04d%02d%02d.md", kNoteDir,
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
        snprintf(stamp, sizeof(stamp), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    } else {
        snprintf(path, sizeof(path), "%s/undated.md", kNoteDir);
        snprintf(stamp, sizeof(stamp), "开机%llds", (long long)(SdCardManager::BootMs() / 1000));
    }

    char line[512];
    int n;
    if (FileSize(path) == 0) {
        n = snprintf(line, sizeof(line), "# 语音备忘 %04d-%02d-%02d\n\n- %s %s\n",
                     tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, stamp, text);
    } else {
        n = snprintf(line, sizeof(line), "- %s %s\n", stamp, text);
    }
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        if (WriteRaw(path, line, len, "a")) {
            ESP_LOGI(TAG, "备忘已写入 %s", path);
        }
    }
}