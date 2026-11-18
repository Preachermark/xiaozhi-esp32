#pragma once

#include <cstddef>
#include <functional>

#include <lvgl.h>

// 收音机界面：台列表（滚动）→ 播放页（台名 + 状态 + 控制）。
// 界面开着时接管触摸，避免点一下屏幕就触发"开始对话"把音频抢走。
class RadioUi {
public:
    enum Mode { kOff = 0, kList, kPlaying };

    static RadioUi& GetInstance();

    void Create(lv_obj_t* parent, std::function<void()> on_home = nullptr);
    void Show();
    void Hide();
    bool IsVisible() const { return mode_ != kOff; }
    Mode GetMode() const { return mode_; }

    bool HandleTap(const lv_point_t& p);
    void RefreshStatus();          // 播放页状态文字（缓冲中/播放中）
    void ApplyFont(lv_obj_t* obj);

private:
    RadioUi() = default;
    void ShowList();
    void ShowPlaying();
    void SetMode(Mode m);

    static void HomeCb(lv_event_t* e);
    static void ItemCb(lv_event_t* e);
    static void BackCb(lv_event_t* e);
    static void CtrlCb(lv_event_t* e);
    static void ResetBtnCb(lv_event_t* e);

    Mode mode_ = kOff;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* list_page_ = nullptr;
    lv_obj_t* play_page_ = nullptr;
    lv_obj_t* list_header_ = nullptr;
    lv_obj_t* list_box_ = nullptr;
    lv_obj_t* play_title_ = nullptr;
    lv_obj_t* play_status_ = nullptr;
    lv_obj_t* play_toggle_label_ = nullptr;
    lv_obj_t* play_vol_label_ = nullptr;
    int cur_ = 0;
    std::function<void()> on_home_;
};