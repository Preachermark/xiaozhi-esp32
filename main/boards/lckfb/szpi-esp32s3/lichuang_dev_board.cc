#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "esp32_camera.h"
#include "mcp_server.h"
#include "press_to_talk_mcp_tool.h"
#include "power_save_timer.h"   // [screen-off patch]
#include "sd_card_manager.h"   // [TF卡]
#include "sd_log.h"           // [TF卡]
#include "sd_recorder.h"      // [TF卡] 录音
#include "sd_music.h"
#include "music_list_ui.h"         // [TF卡] 音乐
#include "reader_ui.h"        // [TF卡] 电子书
#include "photo_ui.h"         // [TF卡] 照片
#include "sd_radio.h"         // [TF卡] 收音机
#include "radio_ui.h"         // [TF卡] 收音机界面
#include "display/lvgl_display/lvgl_display.h"   // [截图]
#include "jpg/image_to_jpeg.h"                 // [截图] image_to_jpeg_cb
#include <cstdio>
#include <cstring>
#include "display/lvgl_display/lvgl_theme.h"   // [screen-off patch v7] LvglThemeManager

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#define TAG "LichuangDevBoard"

class Pca9557 : public I2cDevice {
public:
    Pca9557(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x01, 0x03);
        WriteReg(0x03, 0xf8);
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint8_t data = ReadReg(0x01);
        data = (data & ~(1 << bit)) | (level << bit);
        WriteReg(0x01, data);
    }
};

class CustomAudioCodec : public BoxAudioCodec {
private:
    Pca9557* pca9557_;

public:
    CustomAudioCodec(i2c_master_bus_handle_t i2c_bus, Pca9557* pca9557)
        : BoxAudioCodec(i2c_bus, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                        AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                        AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, GPIO_NUM_NC,
                        AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE,
                        28.0f,  // Physical MIC1 gain
                        2,      // Physical MIC3 is the playback reference input
                        0.0f),
          pca9557_(pca9557) {}

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        if (enable) {
            pca9557_->SetOutputState(1, 1);
        } else {
            pca9557_->SetOutputState(1, 0);
        }
    }
};

class LichuangDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t pca9557_handle_;
    Button boot_button_;
    Display* display_;
    Pca9557* pca9557_;
    Esp32Camera* camera_;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;   // [screen-off patch]
    bool screen_sleeping_ = false;                 // [screen-off patch]
    // [screen-off v11] 手动熄屏锁：为 true 时锁住触控，只有长按 B 键能点亮。
    // 自动息屏(60s)不会设这个标志，所以暗屏状态下碰屏幕仍然能唤醒。
    bool manual_screen_off_ = false;
    // [v11c] 熄屏时刻（lv_tick 毫秒）。背光是渐变的，熄屏后几百毫秒内
    // brightness() 还在从旧值往 0 掉，不能在这段窗口里判断「有人点亮了」。
    uint32_t manual_off_tick_ = 0;
    // [screen-off patch v7] 分区点击与下拉菜单
    lv_obj_t* menu_panel_ = nullptr;
    bool menu_visible_ = false;
    // [TF卡] 音乐播放界面
    lv_obj_t* music_ui_ = nullptr;
    lv_timer_t* chat_clear_timer_ = nullptr;
    lv_obj_t* music_title_ = nullptr;
    lv_obj_t* music_time_ = nullptr;
    lv_obj_t* music_toggle_label_ = nullptr;
    lv_obj_t* music_loop_label_ = nullptr;
    lv_obj_t* music_vol_label_ = nullptr;   // 当前音量数值
    bool music_ui_visible_ = false;
    lv_point_t tap_press_pt_{0, 0};   // 按下位置，用于自己算滑动
    bool tap_swiped_ = false;
    enum MusicUiAction {
        kMusicPrev = 1, kMusicToggle, kMusicNext,
        kMusicVolDown, kMusicVolUp, kMusicLoop, kMusicList, kMusicHome,
    };
    int brightness_level_ = 50;
    int volume_level_ = 50;
    enum MenuAction {
        kMenuBrightnessDown = 1,
        kMenuBrightnessUp,
        kMenuVolumeDown,
        kMenuVolumeUp,
        kMenuTheme,
        kMenuScreenOff,
        kMenuRecord,       // [TF卡] 录音备忘
        kMenuNotesMode,    // [TF卡] 记录模式
        kMenuMusic,        // [TF卡] 音乐播放
        kMenuPhotos,       // [TF卡] 图片查看器（待实现）
        kMenuBook,         // [TF卡] 电子书（待实现）
        kMenuHome,         // 逃生按钮：退出音乐/录音/查看器，回语音对话
        kMenuRadio,        // [TF卡] 网络收音机
        kMenuClose,        // 收起菜单栏
    };

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // Initialize PCA9557
        pca9557_ = new Pca9557(i2c_bus_, 0x19);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_40;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_41;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (music_ui_visible_) {
                // B 键 = 收起界面并停止播放：进了聆听状态还放着歌，麦克风会灌进音乐
                HideMusicUi();
                SdMusic::GetInstance().Stop();
            }
            if (ReaderUi::GetInstance().IsVisible()) {
                ReaderUi::GetInstance().Hide();          // B 键也退出阅读器
            }
            if (PhotoUi::GetInstance().IsVisible()) {
                PhotoUi::GetInstance().Hide();           // B 键也退出照片查看器
            }
            if (RadioUi::GetInstance().IsVisible()) {
                RadioUi::GetInstance().Hide();           // B 键退出收音机界面
            }
            // During startup (before connected), pressing BOOT button enters Wi-Fi config mode without reboot
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            // In press-to-talk mode the click event is handled by press down/up
            if (!press_to_talk_tool_ || !press_to_talk_tool_->IsPressToTalkEnabled()) {
                app.ToggleChatState();
            }
        });

        boot_button_.OnPressDown([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif

        // [screen-off patch] 长按 B 键切换息屏 / 亮屏
        boot_button_.OnLongPress([this]() {
            screen_sleeping_ = !screen_sleeping_;
            // [v11] 只有 B 键能改这个锁：熄屏时锁住触控，亮屏时解锁
            manual_screen_off_ = screen_sleeping_;
            GetDisplay()->SetPowerSaveMode(screen_sleeping_);
            if (screen_sleeping_) {
                manual_off_tick_ = lv_tick_get();   // [v11c] 渐变开始，记时刻
                GetBacklight()->SetBrightness(0);
            } else {
                GetBacklight()->RestoreBrightness();
            }
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
            ESP_LOGI(TAG, "Screen toggled by long press: %s",
                     screen_sleeping_ ? "off" : "on");
        });
    }

    // [screen-off patch] 自动息屏：接 PowerSaveTimer，时间从配网页面读
    // 配网页面 sleep_dim_minutes -> NVS 键 sleep_dim_min（PowerSaveTimer 内部重读）
    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            // [screen-off patch v10] B 键手动熄屏时亮度被设成 0，而这一级原来是
            // 无条件 SetBrightness(10)，于是过 60 秒屏幕从全黑变成暗亮，看起来像
            // "自己亮起来了"。已手动熄屏就保持全黑，直接早退。
            if (screen_sleeping_) {
                GetBacklight()->SetBrightness(0);
                ESP_LOGI(TAG, "Auto sleep: keep fully off (manual)");
                return;
            }
            screen_sleeping_ = true;
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(10);
            ESP_LOGI(TAG, "Auto sleep: dimming screen");
        });
        power_save_timer_->OnExitSleepMode([this]() {
            // [v11] 手动熄屏锁生效期间，任何非用户触发的退出睡眠都不许点屏。
            // （触摸已被锁住，所以走到这里的只可能是 ApplySettings / SetEnabled 那类内部调用）
            if (manual_screen_off_) {
                ESP_LOGI(TAG, "Auto sleep: keep off (manual lock)");
                return;
            }
            screen_sleeping_ = false;
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
            ESP_LOGI(TAG, "Auto sleep: restoring screen");
        });
        // [screen-off patch v2] 第二级：全黑。本板无 PMIC 无法真正关机，
        // 借 shutdown 级关掉背光；时间由配网页面 sleep_off_min 覆盖。
        power_save_timer_->OnShutdownRequest([this]() {
            GetBacklight()->SetBrightness(0);
            ESP_LOGI(TAG, "Auto sleep: screen fully off");
        });
        power_save_timer_->SetEnabled(true);
    }

    // [screen-off patch v2] 左右滑动切换表情（21 个固件标准情绪名，换表情包仍有效）
    void CycleEmotionOnSwipe(int delta) {
        static const char* kEmotionNames[] = {
            "angry", "confident", "confused", "cool", "crying", "delicious",
            "embarrassed", "funny", "happy", "kissy", "laughing", "loving",
            "neutral", "relaxed", "sad", "shocked", "silly", "sleepy",
            "surprised", "thinking", "winking"
        };
        constexpr int kCount = static_cast<int>(sizeof(kEmotionNames) / sizeof(kEmotionNames[0]));
        static int index = 8;  // 从 happy 起
        index = ((index + delta) % kCount + kCount) % kCount;
        GetDisplay()->SetEmotion(kEmotionNames[index]);
        ESP_LOGI(TAG, "Swipe emoji -> %s", kEmotionNames[index]);
    }

    // [screen-off patch v7] 菜单按钮：标准 lv_button，自己处理自己的点击
    void AddMenuButton(lv_obj_t* parent, const char* text, int action,
                       int width = 70, int height = 34) {
        lv_obj_t* btn = lv_button_create(parent);
        if (btn == nullptr) {
            return;
        }
        // [screen-off patch v8] 深色卡片风格：圆角 + 按下高亮，无边框
        lv_obj_set_size(btn, width, height);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E2E2E), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x4A4A4A), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_user_data(btn, (void*)(long)action);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<LichuangDevBoard*>(lv_event_get_user_data(e));
            auto* btn_obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
            if (self == nullptr || btn_obj == nullptr) {
                return;
            }
            self->HandleMenuAction((int)(long)lv_obj_get_user_data(btn_obj));
        }, LV_EVENT_CLICKED, this);

        lv_obj_t* label = lv_label_create(btn);
        if (label != nullptr) {
            lv_label_set_text(label, text);
            // [screen-off patch v9] 这里不设字体：创建时(约423ms)assets/主题尚未就绪，
            // 字体指针为 nullptr，直接设会导致渲染时 panic。改在 ShowMenu() 里统一设。
            lv_obj_set_style_text_color(label, lv_color_hex(0xF0F0F0), 0);
            lv_obj_center(label);
        }
    }

    // [screen-off patch v7] 创建下拉菜单面板（默认隐藏）
    void CreateTouchMenu(lv_obj_t* parent) {
        menu_panel_ = lv_obj_create(parent);
        if (menu_panel_ == nullptr) {
            return;
        }
        // [screen-off patch v8] 深色卡片面板：圆角 + 细边框 + 阴影
        lv_obj_set_size(menu_panel_, LV_PCT(100), 240);   // 6 行紧凑排布，铺满画布高
        lv_obj_align(menu_panel_, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(menu_panel_, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(menu_panel_, LV_OPA_90, 0);
        lv_obj_set_style_border_width(menu_panel_, 1, 0);
        lv_obj_set_style_border_color(menu_panel_, lv_color_hex(0x3A3A3A), 0);
        lv_obj_set_style_radius(menu_panel_, 14, 0);
        lv_obj_set_style_shadow_width(menu_panel_, 16, 0);
        lv_obj_set_style_shadow_opa(menu_panel_, LV_OPA_40, 0);
        lv_obj_set_style_pad_all(menu_panel_, 4, 0);
        lv_obj_set_style_pad_row(menu_panel_, 3, 0);
        lv_obj_set_style_pad_column(menu_panel_, 6, 0);
        lv_obj_set_flex_flow(menu_panel_, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(menu_panel_, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(menu_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(menu_panel_, LV_OBJ_FLAG_HIDDEN);

        // 第 1 行：亮度 / 音量
        AddMenuButton(menu_panel_, "亮度-", kMenuBrightnessDown);
        AddMenuButton(menu_panel_, "亮度+", kMenuBrightnessUp);
        AddMenuButton(menu_panel_, "音量-", kMenuVolumeDown);
        // 第 2 行
        AddMenuButton(menu_panel_, "音量+", kMenuVolumeUp);
        AddMenuButton(menu_panel_, "主题", kMenuTheme);
        AddMenuButton(menu_panel_, "息屏", kMenuScreenOff);
        // 第 3 行：录音 / 记录
        AddMenuButton(menu_panel_, "录音", kMenuRecord);
        AddMenuButton(menu_panel_, "记录", kMenuNotesMode);
        // 第 4 行：音乐 / 图片（各占两格）
        AddMenuButton(menu_panel_, "音乐", kMenuMusic, 111, 34);
        AddMenuButton(menu_panel_, "图片", kMenuPhotos, 111, 34);
        // 第 5 行：电子书 / 收音机
        AddMenuButton(menu_panel_, "电子书", kMenuBook, 111, 34);
        AddMenuButton(menu_panel_, "收音机", kMenuRadio, 111, 34);
        // 第 6 行：首页（退出所有功能回对话）+ 返回（只收起菜单）
        AddMenuButton(menu_panel_, "首页", kMenuHome, 100, 34);
        AddMenuButton(menu_panel_, "返回", kMenuClose, 100, 34);
    }

    void ShowMenu() {
        if (menu_panel_ == nullptr) {
            return;
        }
        // [screen-off patch v9] 显示时才设中文字体：此刻 assets 与主题必然已就绪。
        // 设在面板上，子 label 自动继承；三层判空，任一为空都不设，避免野指针。
        auto theme = LvglThemeManager::GetInstance().GetTheme("light");
        if (theme != nullptr) {
            auto font = theme->text_font();
            if (font != nullptr) {
                const lv_font_t* raw = font->font();
                if (raw != nullptr) {
                    lv_obj_set_style_text_font(menu_panel_, raw, 0);
                }
            }
        }
        menu_visible_ = true;
        lv_obj_clear_flag(menu_panel_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "Menu shown");
    }

    void HideMenu() {
        if (menu_panel_ == nullptr) {
            return;
        }
        menu_visible_ = false;
        lv_obj_add_flag(menu_panel_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "Menu hidden");
    }

    // ===== [截图] 把当前屏幕 base64 打到串口，供远端核对真实布局 =====

    // ===== [TF卡] 音乐播放界面 =====
    void ApplyUiFont(lv_obj_t* obj) {
        // 字体必须等 assets 与主题就绪后再设，否则渲染时 panic（之前踩过）
        auto theme = LvglThemeManager::GetInstance().GetTheme("light");
        if (theme != nullptr) {
            auto font = theme->text_font();
            if (font != nullptr) {
                const lv_font_t* raw = font->font();
                if (raw != nullptr) {
                    lv_obj_set_style_text_font(obj, raw, 0);
                }
            }
        }
    }

    lv_obj_t* AddMusicButton(lv_obj_t* parent, const char* text, int action, int w, int h,
                             int radius, int x, int y, lv_obj_t** out_label) {
        lv_obj_t* btn = lv_button_create(parent);
        if (btn == nullptr) return nullptr;
        lv_obj_set_size(btn, w, h);
        // 关键：默认内边距会把 60px 宽的文字挤到换行，把按钮外观彻底搞乱
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_radius(btn, radius, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2A3A), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x50506A), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_user_data(btn, (void*)(long)action);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<LichuangDevBoard*>(lv_event_get_user_data(e));
            auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
            if (self == nullptr || obj == nullptr) return;
            self->HandleMusicAction((int)(long)lv_obj_get_user_data(obj));
        }, LV_EVENT_CLICKED, this);
        if (x >= 0 && y >= 0) {
            lv_obj_set_pos(btn, x, y);   // 传负数则由父容器的 flex 排版
        }

        lv_obj_t* label = lv_label_create(btn);
        if (label != nullptr) {
            lv_label_set_text(label, text);
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);   // 绝不换行
            lv_obj_set_width(label, w);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0xF0F0F0), 0);
            lv_obj_center(label);
        }
        if (out_label != nullptr) *out_label = label;
        return btn;
    }

    // 透明横向容器：子按钮由 flex 平均排开，不受字体宽度影响
    lv_obj_t* CreateMusicRow(lv_obj_t* parent, int y, int h) {
        lv_obj_t* row = lv_obj_create(parent);
        if (row == nullptr) return nullptr;
        lv_obj_set_size(row, 320, h);   // 画布实宽 320（之前误按 240）
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        return row;
    }

    void CreateMusicUi(lv_obj_t* parent) {
        music_ui_ = lv_obj_create(parent);
        if (music_ui_ == nullptr) return;
        lv_obj_set_size(music_ui_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(music_ui_, 0, 0);
        lv_obj_set_style_bg_color(music_ui_, lv_color_hex(0x0E0E16), 0);
        lv_obj_set_style_bg_opa(music_ui_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(music_ui_, 0, 0);
        lv_obj_set_style_radius(music_ui_, 0, 0);
        lv_obj_set_style_pad_all(music_ui_, 0, 0);
        lv_obj_clear_flag(music_ui_, LV_OBJ_FLAG_SCROLLABLE);
        // 关键：容器本身不能吃掉点击，否则下层（叫菜单的那层）永远收不到触摸，
        // 右上角菜单就"失效"了。里面的按钮仍然可点。
        lv_obj_clear_flag(music_ui_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(music_ui_, LV_OBJ_FLAG_HIDDEN);

        // 布局按 240x320 排死坐标，避免 align 叠在一起
        // 画布实际 320x240（横屏），所有 y 必须 < 240
        lv_obj_t* head = lv_label_create(music_ui_);
        if (head != nullptr) {
            lv_label_set_text(head, "正在播放");
            lv_obj_set_style_text_color(head, lv_color_hex(0x707080), 0);
            lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 6);
        }

        music_title_ = lv_label_create(music_ui_);
        if (music_title_ != nullptr) {
            lv_label_set_text(music_title_, "");
            lv_label_set_long_mode(music_title_, LV_LABEL_LONG_DOT);
            lv_obj_set_width(music_title_, 300);
            lv_obj_set_style_text_align(music_title_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(music_title_, lv_color_hex(0xFFFFFF), 0);
            lv_obj_align(music_title_, LV_ALIGN_TOP_MID, 0, 44);
        }

        music_time_ = lv_label_create(music_ui_);
        if (music_time_ != nullptr) {
            lv_label_set_text(music_time_, "0:00");
            lv_obj_set_style_text_color(music_time_, lv_color_hex(0x9AA0B0), 0);
            lv_obj_align(music_time_, LV_ALIGN_TOP_MID, 0, 92);
        }

        // 左上角：回小瑞对话界面
        // 左上角：回小瑞对话界面
        AddMusicButton(music_ui_, "首页", kMusicHome, 56, 26, 8, 6, 4, nullptr);

        // 循环模式（居中）
        AddMusicButton(music_ui_, "列表循环", kMusicLoop, 120, 28, 8, 100, 118, &music_loop_label_);
        // 歌曲列表：仿收音机列表，纯文字无解码，几十首也不卡。
        // 文案就两个字，紧挨「首页」放在左上角，不要横在屏幕中间挡视线。
        AddMusicButton(music_ui_, "列表", kMusicList, 56, 26, 8, 62, 4, nullptr);

        // 音量行
        lv_obj_t* vol_row = CreateMusicRow(music_ui_, 150, 30);
        if (vol_row != nullptr) {
            AddMusicButton(vol_row, "音量-", kMusicVolDown, 88, 28, 8, -1, -1, nullptr);
            // 中间显示当前音量（纯数字，默认字体就认，不依赖中文字体）
            music_vol_label_ = lv_label_create(vol_row);
            if (music_vol_label_ != nullptr) {
                lv_label_set_text(music_vol_label_, "--");
                lv_obj_set_width(music_vol_label_, 56);
                lv_obj_set_style_text_align(music_vol_label_, LV_TEXT_ALIGN_CENTER, 0);
            }
            AddMusicButton(vol_row, "音量+", kMusicVolUp, 88, 28, 8, -1, -1, nullptr);
        }

        // 播放控制行：178 + 54 = 232，画布 240 内
        lv_obj_t* ctl_row = CreateMusicRow(music_ui_, 184, 52);
        if (ctl_row != nullptr) {
            AddMusicButton(ctl_row, "上一首", kMusicPrev, 88, 44, 10, -1, -1, nullptr);
            AddMusicButton(ctl_row, "暂停", kMusicToggle, 88, 44, 10, -1, -1, &music_toggle_label_);
            AddMusicButton(ctl_row, "下一首", kMusicNext, 88, 44, 10, -1, -1, nullptr);
        }
    }

    void UpdateMusicUi() {
        if (music_ui_ == nullptr) return;
        auto& music = SdMusic::GetInstance();
        static char buf[48];
        if (music_title_ != nullptr) {
            lv_label_set_text(music_title_, music.NowPlaying());
        }
        if (music_time_ != nullptr) {
            uint32_t s = music.PositionSec();
            snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
            lv_label_set_text(music_time_, buf);
        }
        if (music_toggle_label_ != nullptr) {
            lv_label_set_text(music_toggle_label_, music.IsPaused() ? "继续" : "暂停");
        }
        if (music_vol_label_ != nullptr) {
            auto codec = Board::GetInstance().GetAudioCodec();
            if (codec != nullptr) {
                static char vb[8];
                snprintf(vb, sizeof(vb), "%d", codec->output_volume());
                lv_label_set_text(music_vol_label_, vb);
            }
        }
        if (music_loop_label_ != nullptr) {
            snprintf(buf, sizeof(buf), "%s循环", music.LoopModeName());
            lv_label_set_text(music_loop_label_, buf);
        }
    }

    void HandleMusicAction(int action) {
        auto& music = SdMusic::GetInstance();
        switch (action) {
            case kMusicPrev:
                music.Prev();
                break;
            case kMusicToggle:
                music.TogglePause();
                break;
            case kMusicNext:
                music.Next();
                break;
            case kMusicVolDown: {
                auto* codec = GetAudioCodec();
                if (codec != nullptr) {
                    volume_level_ -= 10;
                    if (volume_level_ < 0) volume_level_ = 0;
                    codec->SetOutputVolume(volume_level_);
                }
                break;
            }
            case kMusicVolUp: {
                auto* codec = GetAudioCodec();
                if (codec != nullptr) {
                    volume_level_ += 10;
                    if (volume_level_ > 100) volume_level_ = 100;
                    codec->SetOutputVolume(volume_level_);
                }
                break;
            }
            case kMusicList: {
                // 列表要有内容：先确保歌单读进来了
                if (SdMusic::GetInstance().Count() == 0) {
                    SdMusic::GetInstance().Scan();
                }
                MusicListUi::GetInstance().Show();
                break;
            }
            case kMusicLoop:
                music.CycleLoopMode();
                break;
            case kMusicHome:
                // 回对话界面：收界面 + 停播
                HideMusicUi();
                SdMusic::GetInstance().Stop();
                GetDisplay()->ShowNotification("已回到主页", 1200);
                break;
                        default:
                return;
        }
        UpdateMusicUi();
    }

    void ShowMusicUi() {
        if (music_ui_ == nullptr) return;
        ApplyUiFont(music_ui_);
        music_ui_visible_ = true;
        UpdateMusicUi();
        lv_obj_clear_flag(music_ui_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "Music UI shown | screen=%dx%d", (int)lv_obj_get_width(music_ui_),
                 (int)lv_obj_get_height(music_ui_));
    }

    void HideMusicUi() {
        if (music_ui_ == nullptr) return;
        music_ui_visible_ = false;
        lv_obj_add_flag(music_ui_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "Music UI hidden");
    }

    static void MusicUiTimerCb(lv_timer_t* timer) {
        auto* self = static_cast<LichuangDevBoard*>(lv_timer_get_user_data(timer));
        if (self == nullptr || !self->music_ui_visible_) return;
        self->UpdateMusicUi();
    }

    // 进任何功能前先把别的停掉，避免音乐、收音机、阅读器同时跑
    // 四个功能界面有任何一个开着，就不许自动息屏
    void UpdatePowerSave() {
        if (power_save_timer_ == nullptr) return;
        bool any_open = music_ui_visible_ ||
                        RadioUi::GetInstance().IsVisible() ||
                        ReaderUi::GetInstance().IsVisible() ||
                        PhotoUi::GetInstance().IsVisible();
        power_save_timer_->SetEnabled(!any_open);
    }

    void StopOtherFeatures(int keep) {
        if (keep != 1) SdMusic::GetInstance().Stop();
        if (keep != 2) RadioUi::GetInstance().Hide();
        if (keep != 3) ReaderUi::GetInstance().Hide();
        if (keep != 4) PhotoUi::GetInstance().Hide();
        UpdatePowerSave();
    }

    void HandleMenuAction(int action) {
        switch (action) {
            case kMenuBrightnessDown:
                brightness_level_ -= 10;
                if (brightness_level_ < 10) {
                    brightness_level_ = 10;
                }
                GetBacklight()->SetBrightness(brightness_level_);
                break;
            case kMenuBrightnessUp:
                brightness_level_ += 10;
                if (brightness_level_ > 100) {
                    brightness_level_ = 100;
                }
                GetBacklight()->SetBrightness(brightness_level_);
                break;
            case kMenuVolumeDown: {
                auto* codec = GetAudioCodec();
                if (codec != nullptr) {
                    volume_level_ -= 10;
                    if (volume_level_ < 0) {
                        volume_level_ = 0;
                    }
                    codec->SetOutputVolume(volume_level_);
                }
                break;
            }
            case kMenuVolumeUp: {
                auto* codec = GetAudioCodec();
                if (codec != nullptr) {
                    volume_level_ += 10;
                    if (volume_level_ > 100) {
                        volume_level_ = 100;
                    }
                    codec->SetOutputVolume(volume_level_);
                }
                break;
            }
            case kMenuTheme: {
                static bool use_dark = false;
                use_dark = !use_dark;
                auto theme = LvglThemeManager::GetInstance().GetTheme(use_dark ? "dark" : "light");
                if (theme != nullptr) {
                    GetDisplay()->SetTheme(theme);
                }
                break;
            }
            case kMenuRecord: {
                auto& rec = SdRecorder::GetInstance();
                static char rec_msg[128];
                if (rec.IsRecording()) {
                    uint32_t secs = 0;
                    static char rec_path[64];
                    rec.Stop(&secs, rec_path, sizeof(rec_path));
                    const char* slash = strrchr(rec_path, '/');
                    snprintf(rec_msg, sizeof(rec_msg), "已存 %lu:%02lu  %s",
                             (unsigned long)(secs / 60), (unsigned long)(secs % 60),
                             slash ? slash + 1 : rec_path);
                } else if (rec.Start()) {
                    snprintf(rec_msg, sizeof(rec_msg), "录音中…再点一次结束");
                } else {
                    const char* why = rec.LastError();
                    snprintf(rec_msg, sizeof(rec_msg), "录音失败：%s",
                             (why != nullptr && why[0] != '\0') ? why : "未知原因");
                }
                GetDisplay()->ShowNotification(rec_msg, 3000);
                HideMenu();
                break;
            }
            case kMenuNotesMode: {
                static char note_msg[96];
                bool on = !SdNotesGetAlways();
                SdNotesSetAlways(on);
                snprintf(note_msg, sizeof(note_msg),
                         on ? "记录模式：开" : "记录模式：关");
                GetDisplay()->ShowNotification(note_msg, 3000);
                HideMenu();
                break;
            }
            case kMenuMusic: {
                auto& music = SdMusic::GetInstance();
                HideMenu();
                StopOtherFeatures(1);
                if (music_ui_visible_) {
                    HideMusicUi();        // 界面开着再点一次 = 收起（音乐继续）
                    break;
                }
                if (!music.IsPlaying()) {
                    if (!music.Play(0)) {
                        static char music_msg[128];
                        const char* why = music.LastError();
                        snprintf(music_msg, sizeof(music_msg), "放不了：%s",
                                 (why != nullptr && why[0] != '\0') ? why : "未知");
                        GetDisplay()->ShowNotification(music_msg, 3000);
                        break;
                    }
                }
                ShowMusicUi();
                break;
            }
            case kMenuPhotos:
                HideMenu();
                // 相册要解 JPEG，需要一块连续大内存。收音机常驻约 760KB，
                // 开机后第一次开相册常因此解码失败（重启就好，因为内存变干净）。
                SdRadio::GetInstance().ReleaseBuffers();
                StopOtherFeatures(4);
                if (PhotoUi::GetInstance().IsVisible()) {
                    PhotoUi::GetInstance().Hide();       // 再点一次 = 关闭
                } else {
                    PhotoUi::GetInstance().Show();
                }
                break;
            case kMenuRadio:
                HideMenu();
                StopOtherFeatures(2);
                if (RadioUi::GetInstance().IsVisible()) {
                    RadioUi::GetInstance().Hide();
                } else {
                    RadioUi::GetInstance().Show();
                }
                break;
            case kMenuBook:
                HideMenu();
                StopOtherFeatures(3);
                if (ReaderUi::GetInstance().IsVisible()) {
                    ReaderUi::GetInstance().Hide();      // 再点一次 = 关闭
                } else {
                    ReaderUi::GetInstance().Show();
                }
                break;
            case kMenuClose:
                HideMenu();
                break;
            case kMenuHome: {
                // 逃生出口：不管卡在什么状态，一律停掉并回语音对话页
                HideMusicUi();
                SdMusic::GetInstance().Stop();
                ReaderUi::GetInstance().Hide();
                PhotoUi::GetInstance().Hide();
                RadioUi::GetInstance().Hide();
                if (SdRecorder::GetInstance().IsRecording()) {
                    uint32_t secs = 0;
                    SdRecorder::GetInstance().Stop(&secs, nullptr, 0);
                    ESP_LOGW(TAG, "返回主页时中止了录音（%u 秒已保存）", (unsigned)secs);
                }
                HideMenu();
                GetDisplay()->ShowNotification("已回到主页", 1500);
                ESP_LOGI(TAG, "Home: 音乐与录音已停止");
                break;
            }
            case kMenuScreenOff:
                screen_sleeping_ = true;
                manual_screen_off_ = true;   // [v11] 菜单息屏同样锁触控
                manual_off_tick_ = lv_tick_get();
                GetDisplay()->SetPowerSaveMode(true);
                GetBacklight()->SetBrightness(0);
                HideMenu();
                break;
            default:
                break;
        }
        ESP_LOGI(TAG, "Menu action %d, brightness=%d volume=%d", action,
                 brightness_level_, volume_level_);
    }

    // [screen-off patch v7] 按点击坐标分区
    void HandleScreenTap(const lv_point_t& p) {
        const int w = LV_HOR_RES;
        UpdatePowerSave();



        // [TF卡] 收音机界面：开着时接管点击，别让它触发对话
        if (RadioUi::GetInstance().IsVisible()) {
            if (RadioUi::GetInstance().HandleTap(p)) {
                return;
            }
            ShowMenu();
            return;
        }

        // [TF卡] 收音机界面：开着时接管点击，别让它触发对话
        if (RadioUi::GetInstance().IsVisible()) {
            if (RadioUi::GetInstance().HandleTap(p)) {
                return;
            }
            ShowMenu();
            return;
        }

        // [TF卡] 照片查看器：开着时优先接管点击（右上角仍留给菜单）
        if (PhotoUi::GetInstance().IsVisible()) {
            if (PhotoUi::GetInstance().HandleTap(p)) {
                return;
            }
            ShowMenu();
            return;
        }

        // [TF卡] 电子书：阅读器开着时优先接管点击（右上角仍留给菜单）
        if (ReaderUi::GetInstance().IsVisible()) {
            if (ReaderUi::GetInstance().HandleTap(p)) {
                return;
            }
            ShowMenu();
            return;
        }

        // [TF卡] 音乐界面开着时，点击归它管：右上角叫菜单，其余交给界面按钮，
        // 不再去触发"开始对话"（那会把播放链路夺走、音乐就断了）
        if (music_ui_visible_) {
            if (p.x > w - 60 && p.y < 50) {
                ShowMenu();
            }
            return;
        }

        if (menu_visible_) {
            HideMenu();
            return;
        }
        if (p.x > w - 60 && p.y < 50) {
            ShowMenu();
            return;
        }
        // [screen-off patch v8] 边缘热区 50 → 35，减少误触
        if (p.x < 35) {
            CycleEmotionOnSwipe(-1);
            return;
        }
        if (p.x > w - 35) {
            CycleEmotionOnSwipe(1);
            return;
        }
        Application::GetInstance().ToggleChatState();
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = GPIO_NUM_39;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        pca9557_->SetOutputState(0, 0);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

    // 功能界面开着的时候，持续把底部那句对话文字清掉。
    // 只清一次不够：服务端的回复是在我们 clear 之后才到达并画上去的。
    void StartChatClearWatch() {
        if (chat_clear_timer_ != nullptr) return;
        chat_clear_timer_ = lv_timer_create([](lv_timer_t* t) {
            auto* self = static_cast<LichuangDevBoard*>(lv_timer_get_user_data(t));
            if (self == nullptr) return;
            // [screen-off v11b] 手动熄屏锁生效期间，若背光被外部力量点亮（最典型是
            // 用户让她调 self.screen.set_brightness），立刻解锁触控，否则会卡在
            // 「屏幕亮着但点不动」的怪状态。
            // [v12] 语音唤醒亮屏：被叫醒（connecting/listening/speaking/notifying）
            // 就点亮并解锁。触摸误触是另一条路，仍被锁挡着，不会亮。
            if (self->manual_screen_off_ &&
                (lv_tick_get() - self->manual_off_tick_) > 3000) {
                auto st = Application::GetInstance().GetDeviceState();
                if (st == kDeviceStateConnecting || st == kDeviceStateListening ||
                    st == kDeviceStateSpeaking || st == kDeviceStateNotifying) {
                    self->manual_screen_off_ = false;
                    self->screen_sleeping_ = false;
                    if (auto* d = Board::GetInstance().GetDisplay()) {
                        d->SetPowerSaveMode(false);
                    }
                    if (auto* bl2 = Board::GetInstance().GetBacklight()) {
                        bl2->RestoreBrightness();
                    }
                    ESP_LOGI(TAG, "Voice wake: screen on (state=%d)", (int)st);
                }
            }
            // [v11c] 熄屏后 3 秒内不检查：那段时间亮的是渐变尾巴，不是有人点亮
            if (self->manual_screen_off_ &&
                (lv_tick_get() - self->manual_off_tick_) > 3000) {
                auto* bl = Board::GetInstance().GetBacklight();
                if (bl != nullptr && bl->brightness() > 0) {
                    self->manual_screen_off_ = false;
                    self->screen_sleeping_ = false;
                    if (auto* d = Board::GetInstance().GetDisplay()) {
                        d->SetPowerSaveMode(false);
                    }
                    ESP_LOGI(TAG, "Manual lock released: backlight=%d", bl->brightness());
                }
            }
            bool any_open = self->music_ui_visible_ ||
                            RadioUi::GetInstance().IsVisible() ||
                            ReaderUi::GetInstance().IsVisible() ||
                            PhotoUi::GetInstance().IsVisible() ||
                            MusicListUi::GetInstance().IsVisible();
            if (!any_open) return;
            if (auto disp = Board::GetInstance().GetDisplay()) {
                disp->ClearChatMessages();
            }
        }, 500, this);
        ESP_LOGI(TAG, "清屏 timer 已启动");
    }

    void InitializeTouch()
    {
        esp_lcd_touch_handle_t tp = nullptr;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_HEIGHT,
            .y_max = DISPLAY_WIDTH,
            .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
            .int_gpio_num = GPIO_NUM_NC, 
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 1,
                .mirror_x = 1,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .flags =
            {
                .disable_control_phase = 1,
            }
        };
        tp_io_config.scl_speed_hz = 400000;

        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create touch panel IO, continuing without touch");
            return;
        }
        if (esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp) != ESP_OK || tp == nullptr) {
            ESP_LOGW(TAG, "FT5x06 touch controller not found, continuing without touch");
            esp_lcd_panel_io_del(tp_io_handle);
            return;
        }

        /* Add touch input (for selected screen) */
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(), 
            .handle = tp,
        };

        if(touch_cfg.disp) {
            lvgl_port_add_touch(&touch_cfg);
        } else {
            ESP_LOGE(TAG, "Touch display is not initialized");
        }

        // [screen-off patch] 点屏幕 = 开始 / 停止对话
        if (touch_cfg.disp) {
            lvgl_port_lock(0);
            lv_obj_t* scr = lv_screen_active();
            if (scr != nullptr) {
                lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
                // [screen-off patch v4] 点击事件会被 SetupUI() 建的子对象消费掉（它们默认
                // clickable），而手势事件是输入设备级的、能到 screen。因此在 lv_layer_top()
                // 盖一层透明全屏对象，两个事件都绑在它上面 —— 它在 screen 之上，子对象挡不住。
                lv_obj_t* overlay = lv_obj_create(lv_layer_top());
                if (overlay != nullptr) {
                    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
                    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
                    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_width(overlay, 0, 0);
                    lv_obj_set_style_radius(overlay, 0, 0);
                    lv_obj_set_style_pad_all(overlay, 0, 0);
                    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
                    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
                    // [screen-off patch v7] 不再用手势：所有交互都由 CLICKED + 坐标判断完成，
                    // 彻底绕开 LVGL 手势事件的对象层级与速度阈值问题。
                    lv_obj_add_event_cb(overlay, [](lv_event_t* e) {
                        auto* self = static_cast<LichuangDevBoard*>(lv_event_get_user_data(e));
                        if (self == nullptr) {
                            return;
                        }
                        // [v11] 手动熄屏期间：触摸既不唤醒屏幕，也不处理点击
                        if (self->manual_screen_off_) {
                            return;
                        }
                        if (self->power_save_timer_ != nullptr) {
                            self->power_save_timer_->WakeUp();
                        }
                        if (self->tap_swiped_) {
                            self->tap_swiped_ = false;   // 刚才是滑动，别再当点击处理
                            return;
                        }
                        lv_point_t p = {0, 0};
                        lv_indev_get_point(lv_indev_active(), &p);
                        self->HandleScreenTap(p);
                    }, LV_EVENT_CLICKED, this);
                    // 滑动检测：自己算按下→抬起的水平位移，不依赖 LVGL 手势阈值
                    lv_obj_add_event_cb(overlay, [](lv_event_t* e) {
                        auto* self = static_cast<LichuangDevBoard*>(lv_event_get_user_data(e));
                        if (self == nullptr) return;
                        // [v11] 手动熄屏期间不记录按下点，避免残留状态干扰点亮后的第一次点击
                        if (self->manual_screen_off_) return;
                        lv_indev_get_point(lv_indev_active(), &self->tap_press_pt_);
                        self->tap_swiped_ = false;
                        if (self->power_save_timer_ != nullptr) {
                            self->power_save_timer_->WakeUp();
                        }
                    }, LV_EVENT_PRESSED, this);

                    lv_obj_add_event_cb(overlay, [](lv_event_t* e) {
                        auto* self = static_cast<LichuangDevBoard*>(lv_event_get_user_data(e));
                        if (self == nullptr) return;
                        lv_point_t p = {0, 0};
                        lv_indev_get_point(lv_indev_active(), &p);
                        int dx = p.x - self->tap_press_pt_.x;
                        int dy = p.y - self->tap_press_pt_.y;
                        if (PhotoUi::GetInstance().IsVisible() && abs(dx) > 40 && abs(dx) > abs(dy)) {
                            self->tap_swiped_ = true;
                            PhotoUi::GetInstance().HandleSwipe(dx < 0 ? LV_DIR_LEFT : LV_DIR_RIGHT);
                        }
                    }, LV_EVENT_RELEASED, this);
                    // 菜单面板在 overlay 之后创建，因此盖在它之上，按钮能正常吃到点击
                    CreateMusicUi(lv_layer_top());
                    // 歌曲列表：之前漏了创建，Show() 见 root_ 为空就静默返回，
                    // 表现就是"点列表没反应"。这里和别的界面一起建好。
                    MusicListUi::GetInstance().Create(
                        lv_layer_top(),
                        [this]() { ShowMusicUi(); },                       // 返回：回音乐播放页
                        [this](int idx) {                                   // 点某首歌
                            if (SdMusic::GetInstance().Play(idx)) {
                                ShowMusicUi();
                            }
                        });
                    PhotoUi::GetInstance().Create(lv_layer_top(), [this]() {
                        // 照片页左上角「首页」
                        PhotoUi::GetInstance().Hide();
                        HideMenu();
                    });
                    RadioUi::GetInstance().Create(lv_layer_top(), [this]() {
                        RadioUi::GetInstance().Hide();
                        HideMenu();
                    });
                    lv_timer_create([](lv_timer_t* t) {
                        if (RadioUi::GetInstance().IsVisible()) {
                            RadioUi::GetInstance().RefreshStatus();
                        }
                    }, 800, nullptr);
                    RadioUi::GetInstance().Create(lv_layer_top(), [this]() {
                        RadioUi::GetInstance().Hide();
                        HideMenu();
                    });
                    lv_timer_create([](lv_timer_t* t) {
                        if (RadioUi::GetInstance().IsVisible()) {
                            RadioUi::GetInstance().RefreshStatus();
                        }
                    }, 800, nullptr);
                    RadioUi::GetInstance().Create(lv_layer_top(), [this]() {
                        RadioUi::GetInstance().Hide();
                        HideMenu();
                    });
                    lv_timer_create([](lv_timer_t* t) {
                        if (RadioUi::GetInstance().IsVisible()) {
                            RadioUi::GetInstance().RefreshStatus();
                        }
                    }, 800, nullptr);
                    ReaderUi::GetInstance().Create(lv_layer_top(), [this]() {
                        // 阅读器左上角「首页」：收起阅读器，回到小瑞对话界面
                        ReaderUi::GetInstance().Hide();
                        HideMenu();
                    });
                    CreateTouchMenu(lv_layer_top());
                    lv_timer_create(MusicUiTimerCb, 500, this);
                    ESP_LOGI(TAG, "Touch v7: zone tap (left/right emoji, top-right menu, center chat)");
                }
                ESP_LOGI(TAG, "Touch enabled: tap=toggle chat, swipe=change emoji");
            }
            lvgl_port_unlock();
        }
    }

    void InitializeCamera() {
        // Open camera power
        pca9557_->SetOutputState(2, 0);

        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_2;
        config.ledc_timer = LEDC_TIMER_2;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(config);
    }

    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });

        // [语音] 语音打开 TF 卡四个功能，外加回首页。
        // 全部走 HandleMenuAction 这条已有路径，和手指点菜单完全一致，避免两套逻辑分叉。
        mcp_server.AddTool("self.app.open_music",
            "Open the music player on the device screen. Use this when the user asks to play music, open the music player, or listen to songs on the device.",
            PropertyList(), [this](const PropertyList& properties) {
                // 两步走，两个坑都在这里避开：
                // 1) 先用独立任务等语音反馈播完（TTS 和电台/音乐共用播放队列，混播会打架）
                // 2) 再用 lv_async_call 投递到 LVGL 自己的任务（那时持锁），
                //    直接在主循环画界面会 LoadProhibited 崩溃
                // 两件事在这里一次做完：
                // 1) 打开前打断语音反馈（TTS 和电台/音乐共用喇叭，混播会打架）
                //    注意：不能等 IsIdle —— TTS 是在工具执行完之后才生成的，
                //    等到的只是上一轮的空闲，纯属白等。
                // 2) 打开后结束本轮对话（StopListening），否则她继续聆听，
                //    人在收音机界面再喊一句也会被收走，指令就串了。
                lv_async_call([](void* a) {
                    auto* b2 = static_cast<LichuangDevBoard*>(a);
                    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
                        // 界面是懒创建的，语音路径可能没走过初始化
                        b2->CreateMusicUi(lv_layer_top());
                        b2->HandleMenuAction(kMenuMusic);
                    // 彻底回待机：StopListening 只是退出聆听，她可能仍停在对话态，
                    // 结果在 app 界面里喊话照样被收走，两个界面动作叠加就把机器搞崩。
                    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
                    // 清掉底部那句对话文字：语音虽然被我们打断了，文本还画在屏幕上飘着
                    if (auto disp = Board::GetInstance().GetDisplay()) {
                        disp->ClearChatMessages();
                    }
                }, this);
                return true;
            });


        mcp_server.AddTool("self.app.open_photos",
            "Open the photo viewer (electronic photo album) on the device screen. Use this when the user asks to look at photos or open the album.",
            PropertyList(), [this](const PropertyList& properties) {
                // 两步走，两个坑都在这里避开：
                // 1) 先用独立任务等语音反馈播完（TTS 和电台/音乐共用播放队列，混播会打架）
                // 2) 再用 lv_async_call 投递到 LVGL 自己的任务（那时持锁），
                //    直接在主循环画界面会 LoadProhibited 崩溃
                // 两件事在这里一次做完：
                // 1) 打开前打断语音反馈（TTS 和电台/音乐共用喇叭，混播会打架）
                //    注意：不能等 IsIdle —— TTS 是在工具执行完之后才生成的，
                //    等到的只是上一轮的空闲，纯属白等。
                // 2) 打开后结束本轮对话（StopListening），否则她继续聆听，
                //    人在收音机界面再喊一句也会被收走，指令就串了。
                lv_async_call([](void* a) {
                    auto* b2 = static_cast<LichuangDevBoard*>(a);
                    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
                        // 界面是懒创建的，语音路径可能没走过初始化
                        PhotoUi::GetInstance().Create(lv_layer_top(), [b2]() { b2->ShowMenu(); });
                        b2->HandleMenuAction(kMenuPhotos);
                    // 彻底回待机：StopListening 只是退出聆听，她可能仍停在对话态，
                    // 结果在 app 界面里喊话照样被收走，两个界面动作叠加就把机器搞崩。
                    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
                    // 清掉底部那句对话文字：语音虽然被我们打断了，文本还画在屏幕上飘着
                    if (auto disp = Board::GetInstance().GetDisplay()) {
                        disp->ClearChatMessages();
                    }
                }, this);
                return true;
            });


        mcp_server.AddTool("self.app.open_books",
            "Open the e-book reader on the device screen. Use this when the user asks to read a book, open the e-book reader, or read the notes.",
            PropertyList(), [this](const PropertyList& properties) {
                // 两步走，两个坑都在这里避开：
                // 1) 先用独立任务等语音反馈播完（TTS 和电台/音乐共用播放队列，混播会打架）
                // 2) 再用 lv_async_call 投递到 LVGL 自己的任务（那时持锁），
                //    直接在主循环画界面会 LoadProhibited 崩溃
                // 两件事在这里一次做完：
                // 1) 打开前打断语音反馈（TTS 和电台/音乐共用喇叭，混播会打架）
                //    注意：不能等 IsIdle —— TTS 是在工具执行完之后才生成的，
                //    等到的只是上一轮的空闲，纯属白等。
                // 2) 打开后结束本轮对话（StopListening），否则她继续聆听，
                //    人在收音机界面再喊一句也会被收走，指令就串了。
                lv_async_call([](void* a) {
                    auto* b2 = static_cast<LichuangDevBoard*>(a);
                    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
                        // 界面是懒创建的，语音路径可能没走过初始化
                        ReaderUi::GetInstance().Create(lv_layer_top(), [b2]() { b2->ShowMenu(); });
                        b2->HandleMenuAction(kMenuBook);
                    // 彻底回待机：StopListening 只是退出聆听，她可能仍停在对话态，
                    // 结果在 app 界面里喊话照样被收走，两个界面动作叠加就把机器搞崩。
                    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
                    // 清掉底部那句对话文字：语音虽然被我们打断了，文本还画在屏幕上飘着
                    if (auto disp = Board::GetInstance().GetDisplay()) {
                        disp->ClearChatMessages();
                    }
                }, this);
                return true;
            });


        mcp_server.AddTool("self.app.open_radio",
            "Open the network radio player on the device screen. Use this when the user asks to listen to the radio or open the radio.",
            PropertyList(), [this](const PropertyList& properties) {
                // 两步走，两个坑都在这里避开：
                // 1) 先用独立任务等语音反馈播完（TTS 和电台/音乐共用播放队列，混播会打架）
                // 2) 再用 lv_async_call 投递到 LVGL 自己的任务（那时持锁），
                //    直接在主循环画界面会 LoadProhibited 崩溃
                // 两件事在这里一次做完：
                // 1) 打开前打断语音反馈（TTS 和电台/音乐共用喇叭，混播会打架）
                //    注意：不能等 IsIdle —— TTS 是在工具执行完之后才生成的，
                //    等到的只是上一轮的空闲，纯属白等。
                // 2) 打开后结束本轮对话（StopListening），否则她继续聆听，
                //    人在收音机界面再喊一句也会被收走，指令就串了。
                lv_async_call([](void* a) {
                    auto* b2 = static_cast<LichuangDevBoard*>(a);
                    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
                        // 界面是懒创建的，语音路径可能没走过初始化
                        RadioUi::GetInstance().Create(lv_layer_top(), [b2]() { b2->ShowMenu(); });
                        b2->HandleMenuAction(kMenuRadio);
                    // 彻底回待机：StopListening 只是退出聆听，她可能仍停在对话态，
                    // 结果在 app 界面里喊话照样被收走，两个界面动作叠加就把机器搞崩。
                    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
                    // 清掉底部那句对话文字：语音虽然被我们打断了，文本还画在屏幕上飘着
                    if (auto disp = Board::GetInstance().GetDisplay()) {
                        disp->ClearChatMessages();
                    }
                }, this);
                return true;
            });


        mcp_server.AddTool("self.app.back_home",
            "Close any TF-card feature (music / photos / e-book / radio) and go back to the voice conversation screen. Use this when the user asks to go back or close it.",
            PropertyList(), [this](const PropertyList& properties) {
                // 两步走，两个坑都在这里避开：
                // 1) 先用独立任务等语音反馈播完（TTS 和电台/音乐共用播放队列，混播会打架）
                // 2) 再用 lv_async_call 投递到 LVGL 自己的任务（那时持锁），
                //    直接在主循环画界面会 LoadProhibited 崩溃
                // 两件事在这里一次做完：
                // 1) 打开前打断语音反馈（TTS 和电台/音乐共用喇叭，混播会打架）
                //    注意：不能等 IsIdle —— TTS 是在工具执行完之后才生成的，
                //    等到的只是上一轮的空闲，纯属白等。
                // 2) 打开后结束本轮对话（StopListening），否则她继续聆听，
                //    人在收音机界面再喊一句也会被收走，指令就串了。
                lv_async_call([](void* a) {
                    auto* b2 = static_cast<LichuangDevBoard*>(a);
                    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
                        b2->HandleMenuAction(kMenuHome);
                    // 彻底回待机：StopListening 只是退出聆听，她可能仍停在对话态，
                    // 结果在 app 界面里喊话照样被收走，两个界面动作叠加就把机器搞崩。
                    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
                    // 清掉底部那句对话文字：语音虽然被我们打断了，文本还画在屏幕上飘着
                    if (auto disp = Board::GetInstance().GetDisplay()) {
                        disp->ClearChatMessages();
                    }
                }, this);
                return true;
            });


        // Allow switching between press-to-talk (长按说话) and click-to-talk (单击唤醒)
        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();
    }

public:
    LichuangDevBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeTouch();
        InitializeButtons();
        InitializePowerSaveTimer();
        InitializeCamera();
        InitializeTools();
        StartChatClearWatch();   // 功能界面期间持续清底部对话文字

        GetBacklight()->RestoreBrightness();

        // [TF卡] 异步挂载；挂不上就按无卡模式运行，绝不影响启动与对话
        SdCardManager::GetInstance().InitAsync();
        SdLogInitEarly();
        SdRecorder::GetInstance().EnsureTapInstalled();   // [TF卡] 开机即挂，录音点下去立刻可用
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(
            i2c_bus_, 
            pca9557_);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(LichuangDevBoard);
