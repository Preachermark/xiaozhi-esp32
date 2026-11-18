#include "sd_emoji.h"
#include "sd_card_manager.h"

#include "emoji_collection.h"
#include "lvgl_image.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

static const char* TAG = "SdEmoji";

namespace {

constexpr const char* kEmojiDir = "/sdcard/assets/emoji";
constexpr size_t kMaxFiles = 32;
constexpr size_t kMaxFileBytes = 256 * 1024;      // 单张上限 256KB（240x240 PNG 约 100KB）
constexpr size_t kTotalBudgetBytes = 3 * 1024 * 1024;   // 全部表情的 PSRAM 预算 3MB

int s_loaded_count = 0;
size_t s_loaded_bytes = 0;

std::string ToLowerStem(const char* filename) {
    std::string s(filename);
    size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) {
        s = s.substr(0, dot);
    }
    for (char& c : s) {
        c = (char)tolower((unsigned char)c);
    }
    return s;
}

bool ReadFileToPsram(const char* path, uint8_t** out, size_t* out_len) {
    *out = nullptr;
    *out_len = 0;

    struct stat st = {};
    if (!SdCardManager::GetInstance().LockFs()) return false;
    int r = stat(path, &st);
    SdCardManager::GetInstance().UnlockFs();
    if (r != 0 || st.st_size <= 0 || (size_t)st.st_size > kMaxFileBytes) {
        return false;
    }

    if (!SdCardManager::GetInstance().LockFs()) return false;
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        SdCardManager::GetInstance().UnlockFs();
        return false;
    }
    // 表情数据必须常驻（LvglRawImage 只包指针、不复制），放 PSRAM
    uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM);
    size_t got = 0;
    if (buf != nullptr) {
        got = fread(buf, 1, (size_t)st.st_size, f);
    }
    fclose(f);
    SdCardManager::GetInstance().UnlockFs();

    if (buf == nullptr || got != (size_t)st.st_size) {
        if (buf != nullptr) heap_caps_free(buf);
        return false;
    }
    *out = buf;
    *out_len = got;
    return true;
}

}  // namespace

int SdEmojiLoadedCount() {
    return s_loaded_count;
}

size_t SdEmojiLoadedBytes() {
    return s_loaded_bytes;
}

std::shared_ptr<EmojiCollection> SdEmojiLoad() {
    s_loaded_count = 0;
    s_loaded_bytes = 0;

    if (!SdCardManager::GetInstance().IsMounted()) {
        ESP_LOGI(TAG, "TF卡没挂上，表情继续用 flash 里的");
        return nullptr;
    }

    // 1. 先收文件名（不持锁读文件，避免长时间占着文件系统）
    std::vector<std::string> files;
    if (!SdCardManager::GetInstance().LockFs()) return nullptr;
    DIR* d = opendir(kEmojiDir);
    if (d != nullptr) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            size_t len = strlen(e->d_name);
            if (len < 5) continue;
            if (strcasecmp(e->d_name + len - 4, ".png") != 0) continue;
            files.emplace_back(e->d_name);
            if (files.size() >= kMaxFiles) break;
        }
        closedir(d);
    }
    SdCardManager::GetInstance().UnlockFs();

    if (files.empty()) {
        ESP_LOGI(TAG, "%s 里没有 png，表情继续用 flash 里的", kEmojiDir);
        return nullptr;
    }
    std::sort(files.begin(), files.end());

    // 2. 逐张读进 PSRAM 并塞进集合
    auto collection = std::make_shared<EmojiCollection>();
    static char path[128];
    for (const auto& name : files) {
        if (s_loaded_bytes + kMaxFileBytes > kTotalBudgetBytes && s_loaded_count > 0) {
            ESP_LOGW(TAG, "表情总预算用完，剩余 %u 张未加载",
                     (unsigned)(files.size() - s_loaded_count));
            break;
        }
        snprintf(path, sizeof(path), "%s/%s", kEmojiDir, name.c_str());
        uint8_t* buf = nullptr;
        size_t len = 0;
        if (!ReadFileToPsram(path, &buf, &len)) {
            ESP_LOGW(TAG, "读不了 %s（超 256KB 或卡出错），跳过", name.c_str());
            continue;
        }
        collection->AddEmoji(ToLowerStem(name.c_str()), new LvglRawImage(buf, len));
        s_loaded_count++;
        s_loaded_bytes += len;
    }

    if (s_loaded_count == 0) {
        ESP_LOGW(TAG, "一张表情都没读成功，继续用 flash 里的");
        return nullptr;
    }

    ESP_LOGI(TAG, "已从 TF 卡加载 %d 张表情，占用 PSRAM %u KB",
             s_loaded_count, (unsigned)(s_loaded_bytes / 1024));
    return collection;
}