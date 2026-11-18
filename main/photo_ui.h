#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <lvgl.h>

#include "display/lvgl_display/lvgl_image.h"

// 照片查看器：/sdcard/photos 下的 JPEG，2×2 缩略图网格 → 全屏浏览。
// 画布 320×240 横屏：状态条 20px + 两行两列，每格 158×108。
class PhotoUi {
public:
    enum Mode { kOff = 0, kGrid, kFull, kSlideshow };

    static PhotoUi& GetInstance();

    void Create(lv_obj_t* parent, std::function<void()> on_home = nullptr);
    void Show();
    void Hide();
    bool IsVisible() const { return mode_ != kOff; }
    Mode GetMode() const { return mode_; }

    // 触摸入口：返回 true 表示这次点击已被查看器消费
    bool HandleTap(const lv_point_t& p);
    void HandleSwipe(int dir);   // LV_DIR_LEFT / RIGHT，网格翻页或全屏切图

    void ApplyFont(lv_obj_t* obj);

    static constexpr size_t kMaxFileBytes = 1200 * 1024;   // 单张上限，PSRAM 余量有限
    static constexpr int kPerPage = 4;
    int TotalPages() const { return ((int)files_.size() + kPerPage - 1) / kPerPage; }

private:
    PhotoUi() = default;
    void Scan();
    void ShowGrid();
    void LoadThumbs();
    void ShowFull();
    void LoadFull();
    void ReleaseAll();
    bool DecodeTo(const std::string& name, int max_w, int max_h,
                  std::unique_ptr<LvglAllocatedImage>* out);
    void UpdateStatus();

    static void HomeCb(lv_event_t* e);
    static void SlideBtnCb(lv_event_t* e);
    static void SlideshowTimerCb(lv_timer_t* t);
    void StartSlideshow();
    void StopSlideshow();

    Mode mode_ = kOff;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* grid_page_ = nullptr;
    lv_obj_t* full_page_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* cells_[kPerPage] = {nullptr};
    lv_obj_t* cell_labels_[kPerPage] = {nullptr};   // 打不开时显示原因
    lv_obj_t* full_img_ = nullptr;
    lv_obj_t* full_hint_ = nullptr;

    std::vector<std::string> files_;
    int page_ = 0;
    int cur_ = 0;
    std::unique_ptr<LvglAllocatedImage> thumbs_[kPerPage];
    std::unique_ptr<LvglAllocatedImage> full_;
    std::function<void()> on_home_;
    lv_timer_t* slide_timer_ = nullptr;
};