#include "photo_ui.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_log.h>

#include "sd_card_manager.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "jpg/jpeg_to_image.h"

static const char* TAG = "PhotoUi";

namespace {

constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
constexpr int kStatusH = 26;
constexpr int kCellW = 152;
constexpr int kCellH = 102;
constexpr int kColX[2] = {4, 164};
constexpr int kRowY[2] = {28, 134};
constexpr uint32_t kBgColor = 0x0A0A12;
constexpr uint32_t kBtnColor = 0x2A2A3A;
constexpr uint32_t kTextColor = 0xE8E8F0;
constexpr uint32_t kDimColor = 0x707080;

// 自然排序：数字段按数值比较，和 Windows 资源管理器一致
// （普通字符串排序下 10.jpg 会排在 2.jpg 前面）
bool NaturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[j];
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && a[i2] >= '0' && a[i2] <= '9') i2++;
            while (j2 < b.size() && b[j2] >= '0' && b[j2] <= '9') j2++;
            unsigned long va = strtoul(a.substr(i, i2 - i).c_str(), nullptr, 10);
            unsigned long vb = strtoul(b.substr(j, j2 - j).c_str(), nullptr, 10);
            if (va != vb) return va < vb;
            i = i2;
            j = j2;
        } else {
            if (ca != cb) return ca < cb;
            i++;
            j++;
        }
    }
    return (a.size() - i) < (b.size() - j);
}

bool HasJpegExt(const char* name) {
    size_t len = strlen(name);
    return (len > 4 && strcasecmp(name + len - 4, ".jpg") == 0) ||
           (len > 5 && strcasecmp(name + len - 5, ".jpeg") == 0);
}

lv_obj_t* MakeButton(lv_obj_t* parent, const char* text, int w, int h, int x, int y) {
    lv_obj_t* btn = lv_button_create(parent);
    if (btn == nullptr) return nullptr;
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kBtnColor), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x50506A), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    if (lbl != nullptr) {
        lv_label_set_text(lbl, text);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl, w);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kTextColor), 0);
        lv_obj_center(lbl);
    }
    return btn;
}

}  // namespace

PhotoUi& PhotoUi::GetInstance() {
    static PhotoUi instance;
    return instance;
}

void PhotoUi::ApplyFont(lv_obj_t* obj) {
    if (obj == nullptr) return;
    auto theme = LvglThemeManager::GetInstance().GetTheme("light");
    if (theme == nullptr) return;
    auto font = theme->text_font();
    if (font == nullptr) return;
    const lv_font_t* raw = font->font();
    if (raw == nullptr) return;
    lv_obj_set_style_text_font(obj, raw, 0);
}

void PhotoUi::Create(lv_obj_t* parent, std::function<void()> on_home) {
    if (root_ != nullptr) return;
    on_home_ = std::move(on_home);

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, kScreenW, kScreenH);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kBgColor), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_CLICKABLE);   // 点击穿到下层叫菜单
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    // ---- 缩略图网格页 ----
    grid_page_ = lv_obj_create(root_);
    lv_obj_set_size(grid_page_, kScreenW, kScreenH);
    lv_obj_set_pos(grid_page_, 0, 0);
    lv_obj_set_style_bg_opa(grid_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid_page_, 0, 0);
    lv_obj_set_style_pad_all(grid_page_, 0, 0);
    lv_obj_clear_flag(grid_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(grid_page_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(grid_page_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* home_btn = MakeButton(grid_page_, "首页", 52, 26, 4, 4);
    if (home_btn != nullptr) {
        lv_obj_add_event_cb(home_btn, HomeCb, LV_EVENT_CLICKED, this);
    }
    lv_obj_t* slide_btn = MakeButton(grid_page_, "幻灯", 52, 26, 60, 4);
    if (slide_btn != nullptr) {
        lv_obj_add_event_cb(slide_btn, SlideBtnCb, LV_EVENT_CLICKED, this);
    }

    status_ = lv_label_create(grid_page_);
    if (status_ != nullptr) {
        lv_label_set_text(status_, "");
        lv_obj_set_style_text_color(status_, lv_color_hex(kDimColor), 0);
        lv_obj_set_width(status_, 140);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(status_, 118, 6);
    }

    for (int i = 0; i < kPerPage; i++) {
        lv_obj_t* cell = lv_obj_create(grid_page_);
        lv_obj_set_size(cell, kCellW, kCellH);
        lv_obj_set_pos(cell, kColX[i % 2], kRowY[i / 2]);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x1A1A26), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);   // 点击由 HandleTap 统一处理

        cells_[i] = lv_image_create(cell);
        if (cells_[i] != nullptr) {
            lv_obj_set_size(cells_[i], kCellW, kCellH);
            lv_obj_center(cells_[i]);
        }
        cell_labels_[i] = lv_label_create(cell);
        if (cell_labels_[i] != nullptr) {
            lv_label_set_text(cell_labels_[i], "");
            lv_obj_set_style_text_color(cell_labels_[i], lv_color_hex(0xFF8080), 0);
            lv_obj_center(cell_labels_[i]);
        }
    }

    // ---- 全屏页 ----
    full_page_ = lv_obj_create(root_);
    lv_obj_set_size(full_page_, kScreenW, kScreenH);
    lv_obj_set_pos(full_page_, 0, 0);
    lv_obj_set_style_bg_color(full_page_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(full_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(full_page_, 0, 0);
    lv_obj_set_style_pad_all(full_page_, 0, 0);
    lv_obj_clear_flag(full_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(full_page_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(full_page_, LV_OBJ_FLAG_HIDDEN);

    full_img_ = lv_image_create(full_page_);
    if (full_img_ != nullptr) {
        lv_obj_center(full_img_);
    }

    full_hint_ = lv_label_create(full_page_);
    if (full_hint_ != nullptr) {
        lv_label_set_text(full_hint_, "");
        lv_obj_set_style_text_color(full_hint_, lv_color_hex(0xC0C0D0), 0);
        lv_obj_set_style_bg_color(full_hint_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(full_hint_, LV_OPA_50, 0);
        lv_obj_align(full_hint_, LV_ALIGN_BOTTOM_MID, 0, -2);
    }
}

void PhotoUi::Scan() {
    files_.clear();
    if (!SdCardManager::GetInstance().IsMounted()) return;
    if (!SdCardManager::GetInstance().LockFs()) return;
    DIR* d = opendir("/sdcard/photos");
    if (d != nullptr) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_name[0] == '.') continue;
            if (HasJpegExt(e->d_name)) files_.emplace_back(e->d_name);
        }
        closedir(d);
    }
    SdCardManager::GetInstance().UnlockFs();
    std::sort(files_.begin(), files_.end(), NaturalLess);
    ESP_LOGI(TAG, "/sdcard/photos: %u 张 JPEG（自然排序）", (unsigned)files_.size());
    for (size_t i = 0; i < files_.size(); i++) {
    }
}

void PhotoUi::ReleaseAll() {
    for (int i = 0; i < kPerPage; i++) {
        if (cells_[i] != nullptr) {
            lv_image_set_src(cells_[i], nullptr);
        }
        thumbs_[i].reset();
    }
    if (full_img_ != nullptr) {
        lv_image_set_src(full_img_, nullptr);
    }
    full_.reset();
}

bool PhotoUi::DecodeTo(const std::string& name, int max_w, int max_h,
                       std::unique_ptr<LvglAllocatedImage>* out) {
    out->reset();
    char path[160];
    snprintf(path, sizeof(path), "/sdcard/photos/%s", name.c_str());

    size_t fsize = 0;
    uint8_t* buf = nullptr;
    if (!SdCardManager::GetInstance().LockFs()) return false;
    // 用 stat 取大小：FATFS 上 ftell 有时返回 -1，会导致大文件被判成"超过上限"而跳过
    struct stat fst = {};
    long sz = -1;
    if (stat(path, &fst) == 0) {
        sz = (long)fst.st_size;
    }
    FILE* f = fopen(path, "rb");
    if (f != nullptr) {
        if (sz <= 0) {   // stat 失败时才退回 ftell
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fseek(f, 0, SEEK_SET);
        }
        if (sz > 0 && (size_t)sz <= kMaxFileBytes) {
            fsize = (size_t)sz;
            buf = (uint8_t*)heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
            if (buf != nullptr && fread(buf, 1, fsize, f) != fsize) {
                heap_caps_free(buf);
                buf = nullptr;
            }
        }
        fclose(f);
    }
    SdCardManager::GetInstance().UnlockFs();

    if (buf == nullptr) {
        if (fsize == 0) {
            ESP_LOGW(TAG, "[照片] %s: 打不开或空文件", name.c_str());
        } else {
            char path2[160];
            snprintf(path2, sizeof(path2), "/sdcard/photos/%s", name.c_str());
            struct stat st2 = {};
            long real_sz = 0;
            if (SdCardManager::GetInstance().LockFs()) {
                if (stat(path2, &st2) == 0) real_sz = (long)st2.st_size;
                SdCardManager::GetInstance().UnlockFs();
            }
            ESP_LOGW(TAG, "[照片] %s: %ld 字节 超过上限 %u，跳过", name.c_str(), real_sz,
                     (unsigned)kMaxFileBytes);
        }
        return false;
    }

    uint8_t* out_data = nullptr;
    size_t out_len = 0, out_w = 0, out_h = 0, out_stride = 0;
    esp_err_t ret = jpeg_to_image_fit(buf, fsize, (size_t)max_w, (size_t)max_h, &out_data,
                                      &out_len, &out_w, &out_h, &out_stride);
    heap_caps_free(buf);
    if (ret != ESP_OK || out_data == nullptr || out_len == 0) {
        ESP_LOGW(TAG, "[照片] %s: %u 字节 解码失败 ret=%d（多为内存不足，暂存 PSRAM 需要一块连续大内存）", name.c_str(),
                 (unsigned)fsize, (int)ret);
        return false;
    }
    ESP_LOGI(TAG, "[照片] %s: %u 字节 -> %ux%u OK", name.c_str(), (unsigned)fsize,
             (unsigned)out_w, (unsigned)out_h);

    auto img = std::make_unique<LvglAllocatedImage>(out_data, out_len, (int)out_w, (int)out_h,
                                                    (int)out_stride, LV_COLOR_FORMAT_RGB565);
    if (!img->IsValid()) {
        heap_caps_free(out_data);
        return false;
    }
    *out = std::move(img);
    return true;
}

void PhotoUi::UpdateStatus() {
    if (status_ == nullptr) return;
    static char buf[64];
    int total = (int)files_.size();
    if (total == 0) {
        lv_label_set_text(status_, "photos 里没有 jpg");
        return;
    }
    int first = page_ * kPerPage + 1;
    int last = std::min(first + kPerPage - 1, total);
    snprintf(buf, sizeof(buf), "%d-%d / %d", first, last, total);
    lv_label_set_text(status_, buf);
}

void PhotoUi::LoadThumbs() {
    for (int i = 0; i < kPerPage; i++) {
        if (cells_[i] != nullptr) lv_image_set_src(cells_[i], nullptr);
        if (cell_labels_[i] != nullptr) lv_label_set_text(cell_labels_[i], "");
        thumbs_[i].reset();
    }
    int base = page_ * kPerPage;
    for (int i = 0; i < kPerPage; i++) {
        int idx = base + i;
        if (idx >= (int)files_.size()) break;
        // 每解一张让出 CPU 一下：语音调用时主线程还要喂看门狗、发协议响应，
        // 连续硬解会把看门狗喂爆（实测 task_wdt 触发后界面白着不出来）。
        if (i > 0) vTaskDelay(pdMS_TO_TICKS(30));
        if (DecodeTo(files_[idx], kCellW, kCellH, &thumbs_[i]) && cells_[i] != nullptr) {
            lv_image_set_src(cells_[i], thumbs_[i]->image_dsc());
        } else if (cell_labels_[i] != nullptr) {
            // 明确告诉是哪张打不开，别让人猜
            static char why[48];
            snprintf(why, sizeof(why), "打不开\n%s", files_[idx].c_str());
            lv_label_set_text(cell_labels_[i], why);
            lv_obj_set_style_text_align(cell_labels_[i], LV_TEXT_ALIGN_CENTER, 0);
        }
    }
    UpdateStatus();
}

void PhotoUi::ShowGrid() {
    ReleaseAll();
    if (files_.empty()) Scan();
    if (page_ * kPerPage >= (int)files_.size()) page_ = 0;
    lv_obj_add_flag(full_page_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(grid_page_, LV_OBJ_FLAG_HIDDEN);
    mode_ = kGrid;
    LoadThumbs();
}

void PhotoUi::LoadFull() {
    if (full_img_ != nullptr) lv_image_set_src(full_img_, nullptr);
    full_.reset();
    if (cur_ < 0 || cur_ >= (int)files_.size()) return;
    if (DecodeTo(files_[cur_], kScreenW, kScreenH, &full_) && full_img_ != nullptr) {
        lv_image_set_src(full_img_, full_->image_dsc());
        lv_obj_center(full_img_);
    }
    // 全屏不显示任何提示和张数（用户要求）
}

void PhotoUi::ShowFull() {
    if (files_.empty()) return;
    lv_obj_add_flag(grid_page_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(full_page_, LV_OBJ_FLAG_HIDDEN);
    mode_ = kFull;
    LoadFull();
}

void PhotoUi::Show() {
    if (root_ == nullptr) return;
    ApplyFont(root_);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    page_ = 0;
    cur_ = 0;
    Scan();
    ShowGrid();
    ESP_LOGI(TAG, "照片查看器打开");
}

void PhotoUi::Hide() {
    StopSlideshow();
    ReleaseAll();
    mode_ = kOff;
    if (root_ != nullptr) lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "照片查看器关闭");
}

bool PhotoUi::HandleTap(const lv_point_t& p) {
    if (p.x > kScreenW - 60 && p.y < 24) {
        return false;       // 右上角留给菜单
    }
    if (mode_ == kGrid) {
        if (p.y < kStatusH) {
            if (p.x < 60) {          // 左上角首页
                if (on_home_) on_home_();
                return true;
            }
            return true;
        }
        // 点格子 → 全屏
        int col = (p.x >= kColX[1]) ? 1 : 0;
        int row = (p.y >= kRowY[1]) ? 1 : 0;
        int idx = page_ * kPerPage + row * 2 + col;
        if (idx < (int)files_.size()) {
            cur_ = idx;
            ShowFull();
        }
        return true;
    }
    if (mode_ == kSlideshow) {
        // 幻灯片里点一下就退出回网格
        StopSlideshow();
        page_ = cur_ / kPerPage;
        ShowGrid();
        return true;
    }
    if (mode_ == kFull) {
        if (p.x < kScreenW / 3) {
            if (cur_ > 0) {
                cur_--;
                LoadFull();
            }
        } else if (p.x > kScreenW * 2 / 3) {
            if (cur_ + 1 < (int)files_.size()) {
                cur_++;
                LoadFull();
            }
        } else {
            // 中间：回缩略图，并把当前页对准这张
            page_ = cur_ / kPerPage;
            ShowGrid();
        }
        return true;
    }
    return false;
}

void PhotoUi::HandleSwipe(int dir) {
    if (mode_ == kGrid) {
        int pages = TotalPages();
        if (pages <= 1) return;
        if (dir == LV_DIR_LEFT && page_ + 1 < pages) {
            page_++;
            LoadThumbs();
            ESP_LOGI(TAG, "网格翻到第 %d/%d 页", page_ + 1, pages);
        } else if (dir == LV_DIR_RIGHT && page_ > 0) {
            page_--;
            LoadThumbs();
            ESP_LOGI(TAG, "网格翻到第 %d/%d 页", page_ + 1, pages);
        }
    } else if (mode_ == kFull) {
        if (dir == LV_DIR_LEFT && cur_ + 1 < (int)files_.size()) {
            cur_++;
            LoadFull();
        } else if (dir == LV_DIR_RIGHT && cur_ > 0) {
            cur_--;
            LoadFull();
        }
    }
}

void PhotoUi::StartSlideshow() {
    if (files_.empty()) {
        Scan();
        if (files_.empty()) return;
    }
    mode_ = kSlideshow;
    lv_obj_add_flag(grid_page_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(full_page_, LV_OBJ_FLAG_HIDDEN);
    cur_ = (int)(esp_random() % (uint32_t)files_.size());
    LoadFull();
    if (slide_timer_ == nullptr) {
        slide_timer_ = lv_timer_create(SlideshowTimerCb, 15000, this);   // 每 15 秒一张
    }
    ESP_LOGI(TAG, "幻灯片开始，%d 张，15 秒一张", (int)files_.size());
}

void PhotoUi::StopSlideshow() {
    if (slide_timer_ != nullptr) {
        lv_timer_delete(slide_timer_);
        slide_timer_ = nullptr;
    }
    ESP_LOGI(TAG, "幻灯片结束");
}

void PhotoUi::SlideBtnCb(lv_event_t* e) {
    auto* self = static_cast<PhotoUi*>(lv_event_get_user_data(e));
    if (self == nullptr) return;
    self->StartSlideshow();
}

void PhotoUi::SlideshowTimerCb(lv_timer_t* t) {
    auto* self = static_cast<PhotoUi*>(lv_timer_get_user_data(t));
    if (self == nullptr || self->mode_ != kSlideshow) return;
    if (self->files_.size() <= 1) return;
    // 随机换一张，别连续放同一张
    int next = self->cur_;
    for (int i = 0; i < 8; i++) {
        next = (int)(esp_random() % (uint32_t)self->files_.size());
        if (next != self->cur_) break;
    }
    self->cur_ = next;
    self->LoadFull();
}

void PhotoUi::HomeCb(lv_event_t* e) {
    auto* self = static_cast<PhotoUi*>(lv_event_get_user_data(e));
    if (self == nullptr) return;
    if (self->on_home_) {
        self->on_home_();
    } else {
        self->Hide();
    }
}