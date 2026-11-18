#include "sd_card_manager.h"

#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>

#include "board.h"
#include "display.h"
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>

static const char* TAG = "SdCard";

// 立创实战派 ESP32-S3：TF 卡走 1 线 SDMMC
// 引脚来自官方例程 lcsc-shizhanpi-esp32s3-examples/03-micro_sd/main/main.c
// 与板级 config.h 里那个从没被实例化的 BUILTIN_LED_GPIO(48) 冲突，已在 config.h 里改成 NC
#define SD_PIN_CLK 47
#define SD_PIN_CMD 48
#define SD_PIN_D0  21

SdCardManager& SdCardManager::GetInstance() {
    static SdCardManager instance;
    return instance;
}

void SdCardManager::InitAsync() {
    if (fs_mutex_ == nullptr) {
        fs_mutex_ = (void*)xSemaphoreCreateMutex();
    }
    // 栈 4096：挂载会碰 FATFS，别放在 1536 的默认链路里
    xTaskCreate(TaskEntry, "sd_mount", 4096, this, 2, nullptr);
}

void SdCardManager::TaskEntry(void* arg) {
    auto* self = static_cast<SdCardManager*>(arg);
    self->MountTask();
    // 无论首次挂载成没成功，都起一个监控任务盯着插拔
    xTaskCreate(MonitorEntry, "sd_monitor", 6144, self, 1, nullptr);
    vTaskDelete(nullptr);
}

void SdCardManager::MonitorEntry(void* arg) {
    static_cast<SdCardManager*>(arg)->MonitorLoop();
    vTaskDelete(nullptr);
}

// 探测卡是否还在：直接读一个扇区最真实。sdmmc_get_status 在卡已拔掉时
// 仍可能返回 OK（它只是问控制器状态），必须真发一次读命令。
bool SdCardManager::CardPresent() {
    if (card_ == nullptr) return false;
    static uint8_t probe[512];
    esp_err_t err = sdmmc_read_sectors((sdmmc_card_t*)card_, probe, 0, 1);
    return err == ESP_OK;
}

void SdCardManager::Unmount() {
    if (LockFs(3000)) {
        if (card_ != nullptr) {
            esp_vfs_fat_sdcard_unmount(MountPoint(), (sdmmc_card_t*)card_);
            card_ = nullptr;
        }
        mounted_ = false;
        UnlockFs();
    } else {
        mounted_ = false;
    }
}


// 强制重新挂载：SD 卡只在开机挂载一次，热插拔后 FATFS 那层已经失效，
// 读文件会返回"不存在"，写也会失败。这里先卸干净再挂一次。
bool SdCardManager::ForceRemount() {
    ESP_LOGW(TAG, "强制重新挂载 TF 卡");
    Unmount();
    vTaskDelay(pdMS_TO_TICKS(300));
    if (Mount()) {
        PrepareDirs();
        ESP_LOGW(TAG, "强制重挂成功: %llu MB", (unsigned long long)capacity_mb_.load());
        return true;
    }
    ESP_LOGE(TAG, "强制重挂失败");
    return false;
}

void SdCardManager::MonitorLoop() {
    ESP_LOGW(TAG, "TF 卡热插拔监控已启动（每 2.5 秒探测）");
    int tick = 0;
    bool last_present = true;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2500));
        bool present = CardPresent();
        bool is_mounted = mounted_.load();
        if (present != last_present) {
            ESP_LOGW(TAG, "TF 卡状态变化: %s -> %s", last_present ? "在位" : "不在位",
                     present ? "在位" : "不在位");
            last_present = present;
            // 屏幕上也给一句，肉眼能确认热插拔生效
            auto* disp = Board::GetInstance().GetDisplay();
            if (disp != nullptr) {
                disp->ShowNotification(present ? "TF卡已插入" : "TF卡已拔出", 2000);
            }
        }
        if (++tick % 12 == 0) {   // 每 30 秒报一次心跳（用 W 级，会写进卡上日志）
            ESP_LOGW(TAG, "热插拔心跳: 卡%s, 挂载%s", present ? "在位" : "不在位",
                     is_mounted ? "中" : "已卸载");
        }
        if (present && !is_mounted) {
            ESP_LOGW(TAG, "检测到 TF 卡插入，重新挂载");
            if (Mount()) {
                PrepareDirs();
                ESP_LOGI(TAG, "重新挂载成功: %llu MB", (unsigned long long)capacity_mb_.load());
            }
        } else if (!present && is_mounted) {
            ESP_LOGW(TAG, "TF 卡已拔出，卸载文件系统");
            Unmount();
        }
    }
}

void SdCardManager::MountTask() {
    if (Mount()) {
        PrepareDirs();
        ESP_LOGI(TAG, "TF卡挂载成功: %llu MB @ %s",
                 (unsigned long long)capacity_mb_.load(), MountPoint());
    } else {
        failed_ = true;
    }
}

bool SdCardManager::Mount() {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 20000;   // 1 线模式实测上限

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = (gpio_num_t)SD_PIN_CLK;
    slot.cmd = (gpio_num_t)SD_PIN_CMD;
    slot.d0  = (gpio_num_t)SD_PIN_D0;
    slot.d1  = GPIO_NUM_NC;
    slot.d2  = GPIO_NUM_NC;
    slot.d3  = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;   // 红线：绝不格式化用户的卡
    mcfg.max_files = 6;
    mcfg.allocation_unit_size = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MountPoint(), &host, &slot, &mcfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TF卡未挂载(%s)，按无卡模式运行", esp_err_to_name(err));
        return false;
    }

    uint64_t bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
    capacity_mb_ = bytes / (1024ULL * 1024ULL);
    card_ = card;
    mounted_ = true;
    return true;
}

void SdCardManager::PrepareDirs() {
    const char* dirs[] = {"/sdcard/log", "/sdcard/notes", "/sdcard/record", "/sdcard/assets"};
    for (const char* d : dirs) {
        struct stat st = {};
        if (stat(d, &st) != 0) {
            mkdir(d, 0775);
        }
    }
}

bool SdCardManager::LockFs(uint32_t timeout_ms) {
    if (fs_mutex_ == nullptr) {
        return false;
    }
    return xSemaphoreTake((SemaphoreHandle_t)fs_mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void SdCardManager::UnlockFs() {
    if (fs_mutex_ != nullptr) {
        xSemaphoreGive((SemaphoreHandle_t)fs_mutex_);
    }
}

bool SdCardManager::SystemTimeValid() {
    // 2025-01-01 之后才算校时成功
    return time(nullptr) > 1735689600;
}

int64_t SdCardManager::BootMs() {
    return esp_timer_get_time() / 1000;
}