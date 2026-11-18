#include "radio_ui.h"
#include "sd_card_manager.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "sd_radio.h"
#include "board.h"
#include "audio/audio_codec.h"
#include "display/lvgl_display/lvgl_theme.h"

static const char* TAG = "RadioUi";

namespace {

constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
constexpr uint32_t kBgColor = 0x0A0A12;
constexpr uint32_t kBtnColor = 0x2A2A3A;
constexpr uint32_t kTextColor = 0xE8E8F0;
constexpr uint32_t kDimColor = 0x707080;

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

RadioUi& RadioUi::GetInstance() {
    static RadioUi instance;
    return instance;
}

void RadioUi::ApplyFont(lv_obj_t* obj) {
    if (obj == nullptr) return;
    auto theme = LvglThemeManager::GetInstance().GetTheme("light");
    if (theme == nullptr) return;
    auto font = theme->text_font();
    if (font == nullptr) return;
    const lv_font_t* raw = font->font();
    if (raw == nullptr) return;
    lv_obj_set_style_text_font(obj, raw, 0);
}

void RadioUi::Create(lv_obj_t* parent, std::function<void()> on_home) {
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

    // ---- 台列表页 ----
    list_page_ = lv_obj_create(root_);
    lv_obj_set_size(list_page_, kScreenW, kScreenH);
    lv_obj_set_pos(list_page_, 0, 0);
    lv_obj_set_style_bg_opa(list_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_page_, 0, 0);
    lv_obj_set_style_pad_all(list_page_, 0, 0);
    lv_obj_clear_flag(list_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list_page_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(list_page_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* home = MakeButton(list_page_, "首页", 52, 26, 4, 4);
    if (home != nullptr) lv_obj_add_event_cb(home, HomeCb, LV_EVENT_CLICKED, this);
    lv_obj_t* reset_btn = MakeButton(list_page_, "重置", 52, 26, 60, 4);
    if (reset_btn != nullptr) lv_obj_add_event_cb(reset_btn, ResetBtnCb, LV_EVENT_CLICKED, this);

    list_header_ = lv_label_create(list_page_);
    if (list_header_ != nullptr) {
        lv_label_set_text(list_header_, "收音机");
        lv_obj_set_width(list_header_, 140);
        lv_obj_set_style_text_align(list_header_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(list_header_, lv_color_hex(kTextColor), 0);
        lv_obj_set_pos(list_header_, 118, 6);
    }

    list_box_ = lv_obj_create(list_page_);
    lv_obj_set_size(list_box_, kScreenW - 8, kScreenH - 36);
    lv_obj_set_pos(list_box_, 4, 32);
    lv_obj_set_style_bg_opa(list_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box_, 0, 0);
    lv_obj_set_style_pad_all(list_box_, 2, 0);
    lv_obj_set_style_pad_row(list_box_, 3, 0);
    lv_obj_set_flex_flow(list_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_box_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(list_box_, LV_SCROLLBAR_MODE_AUTO);

    // ---- 播放页 ----
    play_page_ = lv_obj_create(root_);
    lv_obj_set_size(play_page_, kScreenW, kScreenH);
    lv_obj_set_pos(play_page_, 0, 0);
    lv_obj_set_style_bg_opa(play_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(play_page_, 0, 0);
    lv_obj_set_style_pad_all(play_page_, 0, 0);
    lv_obj_clear_flag(play_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(play_page_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(play_page_, LV_OBJ_FLAG_HIDDEN);

    // 左上角：首页 + 列表（和音乐播放器同一套位置）
    lv_obj_t* home_btn = MakeButton(play_page_, "首页", 56, 26, 6, 4);
    if (home_btn != nullptr) lv_obj_add_event_cb(home_btn, HomeCb, LV_EVENT_CLICKED, this);
    lv_obj_t* back = MakeButton(play_page_, "列表", 56, 26, 62, 4);
    if (back != nullptr) lv_obj_add_event_cb(back, BackCb, LV_EVENT_CLICKED, this);

    lv_obj_t* head = lv_label_create(play_page_);
    if (head != nullptr) {
        lv_label_set_text(head, "正在收听");
        lv_obj_set_width(head, kScreenW);
        lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(head, lv_color_hex(kDimColor), 0);
        lv_obj_set_pos(head, 0, 6);
    }

    play_title_ = lv_label_create(play_page_);
    if (play_title_ != nullptr) {
        lv_label_set_text(play_title_, "");
        lv_label_set_long_mode(play_title_, LV_LABEL_LONG_DOT);
        lv_obj_set_width(play_title_, 290);
        lv_obj_set_style_text_align(play_title_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(play_title_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_pos(play_title_, 15, 44);
    }

    play_status_ = lv_label_create(play_page_);
    if (play_status_ != nullptr) {
        lv_label_set_text(play_status_, "");
        lv_obj_set_width(play_status_, kScreenW);
        lv_obj_set_style_text_align(play_status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(play_status_, lv_color_hex(0x9AA0B0), 0);
        lv_obj_set_pos(play_status_, 0, 84);
    }

    // 音量行：音量- / 当前值 / 音量+
    lv_obj_t* vd = MakeButton(play_page_, "音量-", 76, 28, 40, 138);
    if (vd != nullptr) {
        lv_obj_set_user_data(vd, (void*)4L);
        lv_obj_add_event_cb(vd, CtrlCb, LV_EVENT_CLICKED, this);
    }
    play_vol_label_ = lv_label_create(play_page_);
    if (play_vol_label_ != nullptr) {
        lv_label_set_text(play_vol_label_, "--");
        lv_obj_set_width(play_vol_label_, 50);
        lv_obj_set_style_text_align(play_vol_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(play_vol_label_, lv_color_hex(kDimColor), 0);
        lv_obj_set_pos(play_vol_label_, 135, 142);
    }
    lv_obj_t* vu = MakeButton(play_page_, "音量+", 76, 28, 204, 138);
    if (vu != nullptr) {
        lv_obj_set_user_data(vu, (void*)5L);
        lv_obj_add_event_cb(vu, CtrlCb, LV_EVENT_CLICKED, this);
    }

    // 控制按钮：上一台 / 暂停 / 下一台
    lv_obj_t* prev = MakeButton(play_page_, "上一台", 84, 42, 12, 180);
    if (prev != nullptr) {
        lv_obj_set_user_data(prev, (void*)1L);
        lv_obj_add_event_cb(prev, CtrlCb, LV_EVENT_CLICKED, this);
    }
    lv_obj_t* toggle = MakeButton(play_page_, "停止", 84, 42, 118, 180);
    if (toggle != nullptr) {
        lv_obj_set_user_data(toggle, (void*)2L);
        lv_obj_add_event_cb(toggle, CtrlCb, LV_EVENT_CLICKED, this);
        play_toggle_label_ = lv_obj_get_child(toggle, 0);
    }
    lv_obj_t* next = MakeButton(play_page_, "下一台", 84, 42, 224, 180);
    if (next != nullptr) {
        lv_obj_set_user_data(next, (void*)3L);
        lv_obj_add_event_cb(next, CtrlCb, LV_EVENT_CLICKED, this);
    }
}

void RadioUi::SetMode(Mode m) {
    mode_ = m;
    if (list_page_ != nullptr) {
        if (m == kList) lv_obj_clear_flag(list_page_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(list_page_, LV_OBJ_FLAG_HIDDEN);
    }
    if (play_page_ != nullptr) {
        if (m == kPlaying) lv_obj_clear_flag(play_page_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(play_page_, LV_OBJ_FLAG_HIDDEN);
    }
    if (m == kOff && root_ != nullptr) {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void RadioUi::Show() {
    if (root_ == nullptr) return;
    ApplyFont(root_);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    ShowList();
    ESP_LOGI(TAG, "收音机界面打开");
}

void RadioUi::Hide() {
    SdRadio::GetInstance().Stop();
    SetMode(kOff);
    ESP_LOGI(TAG, "收音机界面关闭");
}

void RadioUi::ShowList() {
    auto& radio = SdRadio::GetInstance();
    // 每次进收音机都重读卡上的台表，这样拔卡改完 radio.txt 再插回来进界面就生效，
    // 不用点「重置」也不用重启。（原来是 Count()==0 才扫，改了文件永远读不到）
    // 拔插过卡的情况：FATFS 那层已经失效，读出来是"没有 radio.txt"，
    // 这时强制重挂一次再读，避免用户必须按 R 键复位。
    if (!radio.Scan()) {
        if (SdCardManager::GetInstance().ForceRemount()) {
            radio.Scan();
        }
    }

    if (list_box_ != nullptr) lv_obj_clean(list_box_);
    static char buf[128];
    if (list_header_ != nullptr) {
        snprintf(buf, sizeof(buf), "收音机 (%d)", radio.Count());
        lv_label_set_text(list_header_, buf);
    }

    for (int i = 0; i < radio.Count(); i++) {
        lv_obj_t* item = MakeButton(list_box_, "", kScreenW - 24, 32, 0, 0);
        if (item == nullptr) continue;
        lv_obj_set_style_bg_color(item, lv_color_hex(0x1E1E2A), 0);
        lv_obj_set_user_data(item, (void*)(long)i);
        lv_obj_add_event_cb(item, ItemCb, LV_EVENT_CLICKED, this);
        lv_obj_t* lbl = lv_label_create(item);
        if (lbl != nullptr) {
            snprintf(buf, sizeof(buf), "%02d  %s", i + 1, radio.NameAt(i));
            lv_label_set_text(lbl, buf);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(lbl, kScreenW - 60);
            lv_obj_set_style_text_color(lbl, lv_color_hex(kTextColor), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);
        }
    }
    if (radio.Count() == 0 && list_box_ != nullptr) {
        lv_obj_t* lbl = lv_label_create(list_box_);
        if (lbl != nullptr) {
            lv_label_set_text(lbl, "卡上没有 radio/radio.txt");
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF8080), 0);
        }
    }
    SetMode(kList);
}

void RadioUi::ShowPlaying() {
    auto& radio = SdRadio::GetInstance();
    if (play_title_ != nullptr) lv_label_set_text(play_title_, radio.NowPlaying());
    if (play_vol_label_ != nullptr) {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr) {
            static char vb2[16];
            snprintf(vb2, sizeof(vb2), "%d", codec->output_volume());
            lv_label_set_text(play_vol_label_, vb2);
        }
    }
    RefreshStatus();
    SetMode(kPlaying);
}

void RadioUi::RefreshStatus() {
    if (play_status_ == nullptr) return;
    auto& radio = SdRadio::GetInstance();
    static char buf[96];
    const char* st = radio.StatusText();
    if (radio.LastError()[0] != '\0' && !radio.IsPlaying()) {
        snprintf(buf, sizeof(buf), "出错：%s", radio.LastError());
        lv_obj_set_style_text_color(play_status_, lv_color_hex(0xFF8080), 0);
    } else {
        snprintf(buf, sizeof(buf), "%s", st);
        lv_obj_set_style_text_color(play_status_, lv_color_hex(0x9AA0B0), 0);
    }
    lv_label_set_text(play_status_, buf);
    if (play_toggle_label_ != nullptr) {
        lv_label_set_text(play_toggle_label_, radio.IsPlaying() ? "停止" : "重播");
    }
}

bool RadioUi::HandleTap(const lv_point_t& p) {
    if (p.x > kScreenW - 60 && p.y < 26) {
        return false;       // 右上角留给菜单
    }
    // 两个页面的点击都交给各自的按钮，这里只吞掉事件，不让它触发对话
    return true;
}

void RadioUi::HomeCb(lv_event_t* e) {
    auto* self = static_cast<RadioUi*>(lv_event_get_user_data(e));
    if (self == nullptr) return;
    if (self->on_home_) self->on_home_();
    else self->Hide();
}

void RadioUi::ItemCb(lv_event_t* e) {
    auto* self = static_cast<RadioUi*>(lv_event_get_user_data(e));
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (self == nullptr || obj == nullptr) return;
    int idx = (int)(long)lv_obj_get_user_data(obj);
    self->cur_ = idx;
    if (SdRadio::GetInstance().Play(idx)) {
        self->ShowPlaying();
    } else {
        self->ShowPlaying();
        self->RefreshStatus();
    }
}

void RadioUi::BackCb(lv_event_t* e) {
    auto* self = static_cast<RadioUi*>(lv_event_get_user_data(e));
    if (self == nullptr) return;
    self->ShowList();
}

void RadioUi::CtrlCb(lv_event_t* e) {
    auto* self = static_cast<RadioUi*>(lv_event_get_user_data(e));
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (self == nullptr || obj == nullptr) return;
    auto& radio = SdRadio::GetInstance();
    int action = (int)(long)lv_obj_get_user_data(obj);
    if (action == 1) {
        if (radio.Count() > 0) {
            int n = radio.Count();
            int idx = (radio.CurrentIndex() - 1 + n) % n;
            self->cur_ = idx;
            radio.Play(idx);
            self->ShowPlaying();
        }
    } else if (action == 2) {
        if (radio.IsPlaying()) {
            radio.Stop();
        } else {
            radio.Play(self->cur_);
        }
        self->RefreshStatus();
    } else if (action == 4 || action == 5) {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr) {
            auto& radio = SdRadio::GetInstance();
            (void)radio;
            codec->SetOutputVolume(codec->output_volume() + (action == 5 ? 10 : -10));
            if (self->play_vol_label_ != nullptr) {
                static char vb[16];
                snprintf(vb, sizeof(vb), "%d", codec->output_volume());
                lv_label_set_text(self->play_vol_label_, vb);
            }
        }
    } else if (action == 3) {
        if (radio.Count() > 0) {
            int n = radio.Count();
            int idx = (radio.CurrentIndex() + 1) % n;
            self->cur_ = idx;
            radio.Play(idx);
            self->ShowPlaying();
        }
    }
}

void RadioUi::ResetBtnCb(lv_event_t* e) {
    auto* self = static_cast<RadioUi*>(lv_event_get_user_data(e));
    if (self == nullptr) return;
    SdRadio::GetInstance().WriteDefaultList();
    self->ShowList();
}