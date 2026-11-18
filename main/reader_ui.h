#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <lvgl.h>

#include "sd_books.h"

// 电子书阅读界面：目录选择 → 文件列表 → 正文分页。
// 状态机：kDir（选 books/notes）→ kList（选文件）→ kPage（读正文）
class ReaderUi {
public:
    enum Mode { kOff = 0, kDir, kList, kPage };

    static ReaderUi& GetInstance();

    // on_home：点左上角「首页」时回调，由 board 负责收起阅读器并回对话界面
    void Create(lv_obj_t* parent, std::function<void()> on_home = nullptr);
    void Show();                       // 打开到目录页
    void Hide();
    bool IsVisible() const { return mode_ != kOff; }
    Mode GetMode() const { return mode_; }

    // 触摸入口：返回 true 表示这次点击已被阅读器消费
    bool HandleTap(const lv_point_t& p);

    void ApplyFont(lv_obj_t* obj);

private:
    ReaderUi() = default;
    void ShowDir();
    void ShowList(SdBooks::Kind kind);
    void ShowPage(size_t offset);
    void RefreshPage();
    void GotoPrevPage();
    void GotoNextPage();
    void SetMode(Mode m);

    static void DirBtnCb(lv_event_t* e);
    static void ItemCb(lv_event_t* e);
    static void BackCb(lv_event_t* e);
    static void HomeCb(lv_event_t* e);
    static void PageBtnCb(lv_event_t* e);   // 上一页/返回/下一页

    Mode mode_ = kOff;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* dir_page_ = nullptr;
    lv_obj_t* list_page_ = nullptr;
    lv_obj_t* page_page_ = nullptr;
    lv_obj_t* list_header_ = nullptr;
    lv_obj_t* list_box_ = nullptr;
    lv_obj_t* page_top_ = nullptr;
    lv_obj_t* page_text_ = nullptr;
    lv_obj_t* page_bottom_ = nullptr;

    size_t cur_offset_ = 0;
    size_t next_offset_ = 0;
    std::vector<size_t> history_;
    std::function<void()> on_home_;
};