#pragma once

#include <cstdint>
#include <atomic>

// TF 卡（1 线 SDMMC）管理。
// 设计原则：卡不在就当没有——绝不阻塞启动、绝不影响对话、绝不格式化用户的卡。
class SdCardManager {
public:
    static SdCardManager& GetInstance();

    void InitAsync();                       // 起后台任务挂载，立即返回
    bool IsMounted() const { return mounted_.load(); }
    uint64_t CapacityMb() const { return capacity_mb_.load(); }
    bool MountFailed() const { return failed_.load(); }

    // 热插拔：卡槽没有卡检测引脚，只能定时探测后自行挂载/卸载
    void Unmount();
    bool ForceRemount();   // 强制卸载再挂载（拔插卡后文件系统失效时用）
    bool CardPresent();

    // FATFS 不支持多任务并发访问同一卷，所有文件操作必须包在这对里
    bool LockFs(uint32_t timeout_ms = 2000);
    void UnlockFs();

    static bool SystemTimeValid();          // OTA 校时后系统时间才可信
    static int64_t BootMs();
    static const char* MountPoint() { return "/sdcard"; }

private:
    SdCardManager() = default;
    static void TaskEntry(void* arg);
    static void MonitorEntry(void* arg);
    void MonitorLoop();
    void MountTask();
    bool Mount();
    void PrepareDirs();

    std::atomic<bool> mounted_{false};
    std::atomic<bool> failed_{false};
    std::atomic<uint64_t> capacity_mb_{0};
    void* fs_mutex_{nullptr};
    void* card_{nullptr};      // sdmmc_card_t*
};