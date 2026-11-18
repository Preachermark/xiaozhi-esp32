#include "reader_ui.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "display/lvgl_display/lvgl_theme.h"

static const char* TAG = "Reader";

namespace {

constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
constexpr uint32_t kBgColor = 0x0E0E16;
constexpr uint32_t kBtnColor = 0x2A2A3A;
constexpr uint32_t kTextColor = 0xE8E8F0;
constexpr uint32_t kDimColor = 0x707080;

lv_obj_t* MakeButton(lv_obj_t* parent, const char* text, int w, int h, int x, int y) {
    lv_obj_t* btn = lv_button_create(parent);
    if (btn == nullptr) return nullptr;
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 10, 0);
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

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, uint32_t color, lv_text_align_t align,
                    int w, int x, int y) {
    lv_obj_t* lbl = lv_label_create(parent);
    if (lbl == nullptr) return nullptr;
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    if (align != LV_TEXT_ALIGN_LEFT) {
        lv_obj_set_width(lbl, w);
        lv_obj_set_style_text_align(lbl, align, 0);
    }
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

}  // namespace

ReaderUi& ReaderUi::GetInstance() {
    static ReaderUi instance;
    return instance;
}

void ReaderUi::ApplyFont(lv_obj_t* obj) {
    if (obj == nullptr) return;
    auto theme = LvglThemeManager::GetInstance().GetTheme("light");
    if (theme == nullptr) return;
    auto font = theme->text_font();
    if (font == nullptr) return;
    const lv_font_t* raw = font->font();
    if (raw == nullptr) return;
    lv_obj_set_style_text_font(obj, raw, 0);
}

void ReaderUi::Create(lv_obj_t* parent, std::function<void()> on_home) {
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
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_CLICKABLE);   // 点击要能穿到下层叫菜单
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    // ---- 目录页：书籍 / 备忘 ----
    dir_page_ = lv_obj_create(root_);
    lv_obj_set_size(dir_page_, kScreenW, kScreenH);
    lv_obj_set_pos(dir_page_, 0, 0);
    lv_obj_set_style_bg_opa(dir_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dir_page_, 0, 0);
    lv_obj_set_style_pad_all(dir_page_, 0, 0);
    lv_obj_clear_flag(dir_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dir_page_, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* home_btn = MakeButton(dir_page_, "首页", 56, 26, 6, 4);
    if (home_btn != nullptr) {
        lv_obj_add_event_cb(home_btn, HomeCb, LV_EVENT_CLICKED, this);
    }
    MakeLabel(dir_page_, "电子书", kDimColor, LV_TEXT_ALIGN_CENTER, kScreenW, 0, 8);

    lv_obj_t* b1 = MakeButton(dir_page_, "书　籍", 130, 110, 20, 60);
    if (b1 != nullptr) {
        lv_obj_set_user_data(b1, (void*)(long)SdBooks::kBooks);
        lv_obj_add_event_cb(b1, DirBtnCb, LV_EVENT_CLICKED, this);
    }
    lv_obj_t* b2 = MakeButton(dir_page_, "备　忘", 130, 110, 170, 60);
    if (b2 != nullptr) {
        lv_obj_set_user_data(b2, (void*)(long)SdBooks::kNotes);
        lv_obj_add_event_cb(b2, DirBtnCb, LV_EVENT_CLICKED, this);
    }
    MakeLabel(dir_page_, "左边是 books 目录，右边是每日备忘", kDimColor, LV_TEXT_ALIGN_CENTER,
              kScreenW, 0, 190);

    // ---- 列表页 ----
    list_page_ = lv_obj_create(root_);
    lv_obj_set_size(list_page_, kScreenW, kScreenH);
    lv_obj_set_pos(list_page_, 0, 0);
    lv_obj_set_style_bg_opa(list_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_page_, 0, 0);
    lv_obj_set_style_pad_all(list_page_, 0, 0);
    lv_obj_clear_flag(list_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list_page_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(list_page_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* back = MakeButton(list_page_, "返回", 60, 28, 6, 6);
    if (back != nullptr) {
        lv_obj_add_event_cb(back, BackCb, LV_EVENT_CLICKED, this);
    }
    list_header_ = MakeLabel(list_page_, "", kTextColor, LV_TEXT_ALIGN_CENTER, 180, 70, 10);

    list_box_ = lv_obj_create(list_page_);
    lv_obj_set_size(list_box_, kScreenW - 8, kScreenH - 46);
    lv_obj_set_pos(list_box_, 4, 40);
    lv_obj_set_style_bg_opa(list_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box_, 0, 0);
    lv_obj_set_style_pad_all(list_box_, 2, 0);
    lv_obj_set_style_pad_row(list_box_, 4, 0);
    lv_obj_set_flex_flow(list_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_box_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(list_box_, LV_SCROLLBAR_MODE_AUTO);

    // ---- 正文页 ----
    page_page_ = lv_obj_create(root_);
    lv_obj_set_size(page_page_, kScreenW, kScreenH);
    lv_obj_set_pos(page_page_, 0, 0);
    lv_obj_set_style_bg_opa(page_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page_page_, 0, 0);
    lv_obj_set_style_pad_all(page_page_, 0, 0);
    lv_obj_clear_flag(page_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(page_page_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(page_page_, LV_OBJ_FLAG_HIDDEN);

    page_top_ = MakeLabel(page_page_, "", kDimColor, LV_TEXT_ALIGN_LEFT, 240, 8, 4);

    page_text_ = lv_label_create(page_page_);
    if (page_text_ != nullptr) {
        lv_label_set_text(page_text_, "");
        lv_label_set_long_mode(page_text_, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(page_text_, 304, 176);
        lv_obj_set_pos(page_text_, 8, 28);
        lv_obj_set_style_text_color(page_text_, lv_color_hex(kTextColor), 0);
        lv_obj_set_style_text_line_space(page_text_, 6, 0);
    }

    // 底部三个按钮：上一页 / 返回 / 下一页（小号文字，各占 1/3）
    lv_obj_t* pb = MakeButton(page_page_, "上页", 76, 22, 6, 214);
    if (pb != nullptr) {
        lv_obj_set_user_data(pb, (void*)1L);
        lv_obj_add_event_cb(pb, PageBtnCb, LV_EVENT_CLICKED, this);
    }
    lv_obj_t* bb = MakeButton(page_page_, "返回", 76, 22, 122, 214);
    if (bb != nullptr) {
        lv_obj_set_user_data(bb, (void*)2L);
        lv_obj_add_event_cb(bb, PageBtnCb, LV_EVENT_CLICKED, this);
    }
    lv_obj_t* nb = MakeButton(page_page_, "下页", 76, 22, 238, 214);
    if (nb != nullptr) {
        lv_obj_set_user_data(nb, (void*)3L);
        lv_obj_add_event_cb(nb, PageBtnCb, LV_EVENT_CLICKED, this);
    }
}

void ReaderUi::SetMode(Mode m) {
    mode_ = m;
    if (dir_page_ != nullptr) {
        if (m == kDir) lv_obj_clear_flag(dir_page_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(dir_page_, LV_OBJ_FLAG_HIDDEN);
    }
    if (list_page_ != nullptr) {
        if (m == kList) lv_obj_clear_flag(list_page_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(list_page_, LV_OBJ_FLAG_HIDDEN);
    }
    if (page_page_ != nullptr) {
        if (m == kPage) lv_obj_clear_flag(page_page_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(page_page_, LV_OBJ_FLAG_HIDDEN);
    }
    if (m == kOff && root_ != nullptr) {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ReaderUi::Show() {
    if (root_ == nullptr) return;
    ApplyFont(root_);
    history_.clear();
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    ShowDir();
    ESP_LOGI(TAG, "阅读器打开");
}

void ReaderUi::Hide() {
    SdBooks::GetInstance().Close();
    SetMode(kOff);
    ESP_LOGI(TAG, "阅读器关闭");
}

void ReaderUi::ShowDir() {
    SdBooks::GetInstance().Close();
    SetMode(kDir);
}

void ReaderUi::ShowList(SdBooks::Kind kind) {
    auto& books = SdBooks::GetInstance();
    books.Scan(kind);

    if (list_box_ != nullptr) {
        lv_obj_clean(list_box_);
    }
    static char buf[96];
    if (list_header_ != nullptr) {
        snprintf(buf, sizeof(buf), "%s (%d)", kind == SdBooks::kBooks ? "书籍" : "备忘",
                 books.Count());
        lv_label_set_text(list_header_, buf);
    }

    for (int i = 0; i < books.Count(); i++) {
        const auto* ent = books.At(i);
        if (ent == nullptr) continue;
        lv_obj_t* item = MakeButton(list_box_, "", kScreenW - 24, 34, 0, 0);
        if (item == nullptr) continue;
        lv_obj_set_style_bg_color(item, lv_color_hex(0x1E1E2A), 0);
        lv_obj_set_user_data(item, (void*)(long)i);
        lv_obj_add_event_cb(item, ItemCb, LV_EVENT_CLICKED, this);
        lv_obj_t* lbl = lv_label_create(item);
        if (lbl != nullptr) {
            snprintf(buf, sizeof(buf), "%02d  %s", i + 1, ent->title.c_str());
            lv_label_set_text(lbl, buf);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(lbl, kScreenW - 60);
            lv_obj_set_style_text_color(lbl, lv_color_hex(kTextColor), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);
        }
    }
    if (books.Count() == 0) {
        if (list_box_ != nullptr) {
            lv_obj_t* lbl = lv_label_create(list_box_);
            if (lbl != nullptr) {
                lv_label_set_text(lbl, kind == SdBooks::kBooks ? "books 目录里没有 txt/md"
                                                               : "notes 目录里还没有备忘");
                lv_obj_set_style_text_color(lbl, lv_color_hex(kDimColor), 0);
            }
        }
    }
    SetMode(kList);
}

void ReaderUi::ShowPage(size_t offset) {
    auto& books = SdBooks::GetInstance();
    if (!books.IsOpen()) return;
    if (page_text_ == nullptr) return;

    std::string text;
    size_t next = offset;
    bool is_end = false;
    if (!books.LoadPage(offset, text, next, is_end)) {
        lv_label_set_text(page_text_, books.LastError());
        return;
    }
    cur_offset_ = offset;
    next_offset_ = next;

    lv_label_set_text(page_text_, text.c_str());

    static char buf[128];
    const auto* ent = books.CurrentEntry();
    if (page_top_ != nullptr) {
        snprintf(buf, sizeof(buf), "%s", ent != nullptr ? ent->title.c_str() : "");
        lv_label_set_text(page_top_, buf);
    }
    // 记进度，下次接着看
    if (ent != nullptr) {
        books.SaveProgress(ent->name, offset);
    }
    if (page_top_ != nullptr && ent != nullptr) {
        snprintf(buf, sizeof(buf), "%s   %d%%", ent->title.c_str(), books.Percent());
        lv_label_set_text(page_top_, buf);
    }
    SetMode(kPage);
}

void ReaderUi::RefreshPage() {
    ShowPage(cur_offset_);
}

void ReaderUi::GotoNextPage() {
    auto& books = SdBooks::GetInstance();
    if (!books.IsOpen()) return;
    if (next_offset_ <= cur_offset_) return;      // 到底了
    history_.push_back(cur_offset_);
    if (history_.size() > 200) history_.erase(history_.begin());
    ShowPage(next_offset_);
}

void ReaderUi::GotoPrevPage() {
    auto& books = SdBooks::GetInstance();
    if (!books.IsOpen()) return;
    if (history_.empty()) return;
    size_t prev = history_.back();
    history_.pop_back();
    ShowPage(prev);
}

bool ReaderUi::HandleTap(const lv_point_t& p) {
    const int w = kScreenW;

    if (p.x > w - 60 && p.y < 50) {
        return false;       // 右上角留给菜单
    }

    switch (mode_) {
        case kDir:
            return true;    // 目录页只认按钮
        case kList:
            return true;    // 列表页只认条目和返回
        case kPage:
            if (p.x < w / 3) {
                GotoPrevPage();
            } else if (p.x > w * 2 / 3) {
                GotoNextPage();
            } else {
                // 中间：回列表（进度已在翻页时存好）
                auto kind = SdBooks::GetInstance().CurrentKind();
                SdBooks::GetInstance().Close();
                ShowList(kind);
            }
            return true;
        default:
            return false;
    }
}

void ReaderUi::DirBtnCb(lv_event_t* e) {
    auto* self = static_cast<ReaderUi*>(lv_event_get_user_data(e));
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (self == nullptr || obj == nullptr) return;
    auto kind = (SdBooks::Kind)(int)(long)lv_obj_get_user_data(obj);
    self->ShowList(kind);
}

void ReaderUi::ItemCb(lv_event_t* e) {
    auto* self = static_cast<ReaderUi*>(lv_event_get_user_data(e));
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (self == nullptr || obj == nullptr) return;
    int idx = (int)(long)lv_obj_get_user_data(obj);
    auto& books = SdBooks::GetInstance();
    if (!books.Open(idx)) {
        ESP_LOGE(TAG, "打开失败: %s", books.LastError());
        return;
    }
    self->history_.clear();
    self->ShowPage(books.CurrentOffset());
}

void ReaderUi::BackCb(lv_event_t* e) {
    auto* self = static_cast<ReaderUi*>(lv_event_get_user_data(e));
    if (self == nullptr) return;
    self->ShowDir();
}

void ReaderUi::HomeCb(lv_event_t* e) {
    auto* self = static_cast<ReaderUi*>(lv_event_get_user_data(e));
    if (self == nullptr) return;
    if (self->on_home_) {
        self->on_home_();       // board 负责收起界面并回对话
    } else {
        self->Hide();
    }
}

void ReaderUi::PageBtnCb(lv_event_t* e) {
    auto* self = static_cast<ReaderUi*>(lv_event_get_user_data(e));
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (self == nullptr || obj == nullptr) return;
    int action = (int)(long)lv_obj_get_user_data(obj);
    if (action == 1) {
        self->GotoPrevPage();
    } else if (action == 2) {
        auto kind = SdBooks::GetInstance().CurrentKind();
        SdBooks::GetInstance().Close();
        self->ShowList(kind);
    } else if (action == 3) {
        self->GotoNextPage();
    }
}
