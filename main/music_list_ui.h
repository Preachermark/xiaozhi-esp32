#pragma once

#include <functional>

#include "lvgl.h"

// TF 卡音乐的歌曲列表：仿收音机列表的交互（一屏多行、可上下滑、点行播放）。
// 只有纯文字，无缩略图解码，几十首歌不会卡。
class MusicListUi {
public:
    static MusicListUi& GetInstance();

    // on_home：点左上角返回时回调（回音乐播放页）
    // on_pick：点了某一行，参数是歌曲序号
    void Create(lv_obj_t* parent, std::function<void()> on_home, std::function<void(int)> on_pick);
    void Show();
    void Hide();
    void Refresh();   // 重新读歌单并重建行

    bool IsVisible() const { return visible_; }

private:
    MusicListUi() = default;
    static void HomeCb(lv_event_t* e);
    static void ItemCb(lv_event_t* e);

    lv_obj_t* root_ = nullptr;
    lv_obj_t* list_box_ = nullptr;
    lv_obj_t* header_ = nullptr;
    bool visible_ = false;

    // 静态回调里要用，不能捕获
    static std::function<void()> s_on_home;
    static std::function<void(int)> s_on_pick;
};
