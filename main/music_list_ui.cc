#include "music_list_ui.h"

#include <cstdio>

#include <esp_log.h>

#include "sd_music.h"

static const char* TAG = "MusicList";

namespace {
constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
constexpr int kRowH = 32;
constexpr int kRowGap = 3;
constexpr int kTopH = 28;
constexpr uint32_t kBgColor = 0x101018;
constexpr uint32_t kRowColor = 0x1E1E2A;
constexpr uint32_t kTextColor = 0xE0E0E0;
constexpr uint32_t kHeaderColor = 0x7FD4FF;
}  // namespace

std::function<void()> MusicListUi::s_on_home;
std::function<void(int)> MusicListUi::s_on_pick;

MusicListUi& MusicListUi::GetInstance() {
    static MusicListUi instance;
    return instance;
}

void MusicListUi::Create(lv_obj_t* parent, std::function<void()> on_home,
                         std::function<void(int)> on_pick) {
    if (root_ != nullptr) {
        // 已经建过：只更新回调，不重复建对象
        s_on_home = std::move(on_home);
        s_on_pick = std::move(on_pick);
        return;
    }
    s_on_home = std::move(on_home);
    s_on_pick = std::move(on_pick);

    root_ = lv_obj_create(parent);
    if (root_ == nullptr) return;
    lv_obj_set_size(root_, kScreenW, kScreenH);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kBgColor), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    // 顶部：标题 + 返回
    header_ = lv_label_create(root_);
    if (header_ != nullptr) {
        lv_label_set_text(header_, "歌曲");
        lv_obj_set_style_text_color(header_, lv_color_hex(kHeaderColor), 0);
        lv_obj_set_pos(header_, 10, 6);
    }

    lv_obj_t* home = lv_button_create(root_);
    if (home != nullptr) {
        lv_obj_set_size(home, 60, kTopH - 6);
        lv_obj_set_pos(home, kScreenW - 66, 3);
        lv_obj_set_style_radius(home, 6, 0);
        lv_obj_set_style_bg_color(home, lv_color_hex(kRowColor), 0);
        lv_obj_add_event_cb(home, HomeCb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* hl = lv_label_create(home);
        if (hl != nullptr) {
            // 用 ASCII 箭头：本模块的 label 没套用板级中文字体，写中文会渲染成空白
            lv_label_set_text(hl, "<");
            lv_obj_center(hl);
        }
    }

    // 滚动列表容器
    list_box_ = lv_obj_create(root_);
    if (list_box_ != nullptr) {
        lv_obj_set_size(list_box_, kScreenW, kScreenH - kTopH);
        lv_obj_set_pos(list_box_, 0, kTopH);
        lv_obj_set_style_bg_opa(list_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(list_box_, 0, 0);
        lv_obj_set_style_pad_all(list_box_, 6, 0);
        lv_obj_set_style_pad_row(list_box_, kRowGap, 0);
        lv_obj_set_flex_flow(list_box_, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(list_box_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(list_box_, LV_DIR_VER);
        // 滚动条给一点提示，否则看不出来还能往下翻
        lv_obj_set_scrollbar_mode(list_box_, LV_SCROLLBAR_MODE_ACTIVE);
    }
    ESP_LOGI(TAG, "歌曲列表已创建");
}

void MusicListUi::Refresh() {
    if (list_box_ == nullptr) return;
    auto& music = SdMusic::GetInstance();
    if (!music.Scan()) {
        ESP_LOGW(TAG, "扫不到歌单");
    }
    lv_obj_clean(list_box_);

    static char buf[128];
    int n = music.Count();
    if (header_ != nullptr) {
        snprintf(buf, sizeof(buf), "歌曲 (%d)", n);
        lv_label_set_text(header_, buf);
    }

    for (int i = 0; i < n; i++) {
        lv_obj_t* row = lv_button_create(list_box_);
        if (row == nullptr) break;
        lv_obj_set_size(row, kScreenW - 24, kRowH);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(kRowColor), 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_user_data(row, (void*)(long)i);
        lv_obj_add_event_cb(row, ItemCb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* lbl = lv_label_create(row);
        if (lbl != nullptr) {
            snprintf(buf, sizeof(buf), "%02d  %s", i + 1, music.TitleAt(i));
            lv_label_set_text(lbl, buf);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(lbl, kScreenW - 70);
            lv_obj_set_style_text_color(lbl, lv_color_hex(kTextColor), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
        }
        // 正在播的那首高亮
        if (i == music.CurrentIndex()) {
            lv_obj_set_style_bg_color(row, lv_color_hex(0x2A3A4A), 0);
        }
    }
    ESP_LOGI(TAG, "列表刷新：%d 首", n);
}

void MusicListUi::Show() {
    if (root_ == nullptr) return;
    Refresh();
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
    visible_ = true;
    ESP_LOGI(TAG, "歌曲列表打开");
}

void MusicListUi::Hide() {
    if (root_ == nullptr) return;
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    visible_ = false;
    ESP_LOGI(TAG, "歌曲列表关闭");
}

void MusicListUi::HomeCb(lv_event_t* e) {
    (void)e;
    MusicListUi::GetInstance().Hide();
    if (s_on_home) s_on_home();
}

void MusicListUi::ItemCb(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (obj == nullptr) return;
    int idx = (int)(long)lv_obj_get_user_data(obj);
    auto& ui = MusicListUi::GetInstance();
    ui.Hide();
    if (s_on_pick) s_on_pick(idx);
}
