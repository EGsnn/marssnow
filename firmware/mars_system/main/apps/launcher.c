#include "launcher.h"

#include <stdbool.h>
#include <string.h>

#include "apps.h"
#include "box3_board.h"
#include "box3_gfx.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mars_anim.h"
#include "mars_ui_adapt.h"
#include "mars_gesture.h"
#include "mars_board.h"
#include "round_ui.h"
#include "esp_log.h"
#include "mars_app.h"
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
#include "mars_runtime.h"
#include "mars_dynamic_bridge.h"
#include "app_drawer.h"
static bool s_drawer_return_to_ui;
static bool s_drawer_wait_touch_up;
#endif
#include "ui_runtime.h"
#ifdef CONFIG_BOARD_TYPE_MARS_ATK_BOX
#include "esp_ota_ops.h"
#include "esp_log.h"

static void ota_confirm_cb(void *arg)
{
    (void)arg;
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI("mars_launcher", "firmware validated after 5s healthy uptime");
}
#endif

#define RGB565(r, g, b) (uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3))

enum {
    COLOR_BG = RGB565(13, 18, 31),
    COLOR_PANEL = RGB565(28, 36, 55),
    COLOR_TEXT = RGB565(235, 240, 250),
    COLOR_MUTED = RGB565(145, 157, 180),
    COLOR_GREEN = RGB565(57, 192, 131),
};

/* Frame deadline includes drawing and synchronous DMA, rather than adding a
 * fixed sleep after it. FreeRTOS tick granularity still limits achieved FPS. */
static int64_t s_frame_deadline_us;
static void animation_frame_wait(int64_t started_us)
{
    const int64_t ended_us = esp_timer_get_time();
    mars_ui_motion_sample((uint32_t)(ended_us - started_us));
    const int64_t period = 1000000 / mars_ui_motion().fps;
    if (!s_frame_deadline_us || started_us > s_frame_deadline_us + period)
        s_frame_deadline_us = started_us;
    s_frame_deadline_us += period;
    const int64_t remaining_ms = (s_frame_deadline_us - esp_timer_get_time()) / 1000;
    TickType_t delay = remaining_ms > 0 ? pdMS_TO_TICKS(remaining_ms) : 0;
    vTaskDelay(delay ? delay : 1);
}

static size_t s_selected_app;
static bool s_app_open;
static bool s_return_to_ui;
static bool s_native_return;
#if MARS_BOARD_DISPLAY_ROUND
static mars_gesture_t s_app_gesture;
#endif

/* 桌面保持静止，App 从右侧滑入或向右侧滑出。 */

static mars_anim_t s_transition;
static bool s_animating;
static bool s_transition_leaving;
static bool s_dynamic_first_frame_held;

static void transition_step(float value, void *ctx)
{
    (void)ctx;
    int off = (int)value;
    if (off < 0) off = 0;
    if (off > BOX3_LCD_WIDTH) off = BOX3_LCD_WIDTH;
    gfx_present_slide(off);
}

static void transition_done(void *ctx)
{
    (void)ctx;
    s_animating = false;
    if (s_transition_leaving) {
        s_app_open = false;
        if (s_return_to_ui) {
            s_return_to_ui = false;
            ui_runtime_resume();
        } else {
            draw_home_render();
            gfx_present();
        }
    } else {
        if(!round_ui_app_native())gfx_present();
        if (s_dynamic_first_frame_held) {
            gfx_release_first_frame(true);
            s_dynamic_first_frame_held = false;
        }
    }
}

static void transition_start(bool leaving)
{
    s_transition_leaving = leaving;
    s_animating = true;
    s_frame_deadline_us = 0;
    const mars_ui_motion_t motion = mars_ui_motion();
    mars_anim_start(&s_transition, leaving ? 0.0f : (float)BOX3_LCD_WIDTH,
                    leaving ? (float)BOX3_LCD_WIDTH : 0.0f, motion.transition_ms,
                    motion.ease, transition_step, transition_done, NULL);
}

/* ── App 打开/返回（带过渡动画） ── */

static void open_app_animated(size_t index)
{
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    mars_input_publish_touch(0, 0, 0);
    mars_input_set_back_visible(1);
#endif
    s_native_return = round_ui_active();
    if (s_native_return) {
        round_ui_suspend();
#if MARS_BOARD_DISPLAY_ROUND
        mars_gesture_init(&s_app_gesture,(mars_gesture_config_t){412,412,44,28,20,600,true});
        mars_gesture_barrier(&s_app_gesture);
#endif
        gfx_defer_owner_present(true);
        gfx_clear(COLOR_BG);
    }
    gfx_capture_background();
    s_selected_app = index;
    s_app_open = true;
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    const mars_app_t *app = mars_app_get(index);
    const bool dynamic = app != NULL && strncmp(app->name, "APP", 3) == 0;
    if (dynamic) {
        s_dynamic_first_frame_held = gfx_hold_first_frame();
        if (!s_dynamic_first_frame_held && s_native_return) {
            round_ui_app_fallback();
            gfx_defer_owner_present(false);
        }
    }
#endif
    mars_app_open(index); /* 渲染新页到 framebuffer */
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    if (dynamic && (!s_dynamic_first_frame_held || !gfx_wait_first_frame(1500))) {
        /* 失败或旧 App 不提交首帧：不让动画读正在写的画布，后续由 App 直接刷新。 */
        if (s_native_return && s_dynamic_first_frame_held) {
            round_ui_app_fallback();
            gfx_defer_owner_present(false);
        }
        gfx_release_first_frame(false);
        s_dynamic_first_frame_held = false;
        return;
    }
#endif
    if (s_native_return) {
        round_ui_app_enter();
        gfx_defer_owner_present(false);
        s_transition_leaving = false;
        transition_done(NULL);
    } else transition_start(false);
}

static void back_animated(void)
{
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    mars_input_publish_touch(0, 0, 0);
#endif
    mars_app_close(s_selected_app);
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    mars_input_set_back_visible(1);
#endif
    if (s_native_return) {
        s_native_return = false;
        s_app_open = false;
        s_return_to_ui = false;
        round_ui_resume();
    } else transition_start(true);
}

static bool open_named_app(const char *name)
{
    for (size_t i = 0; i < mars_app_count(); ++i) {
        const mars_app_t *app = mars_app_get(i);
        if (app != NULL && strcmp(app->name, name) == 0) {
            s_return_to_ui = true;
            ui_runtime_suspend();
            open_app_animated(i);
            return true;
        }
    }
    return false;
}

static bool handle_ui_action(const char *action_id)
{
    ESP_LOGI("launcher_action", "native=%d busy=%d app=%d transition=%d",
             round_ui_active(), round_ui_busy(), s_app_open, s_animating);
    if (round_ui_active()) {
        if (round_ui_busy()) return false;
        if (round_ui_action(action_id)) return true;
    }
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    if (app_drawer_is_open()) {
        if (strcmp(action_id, "app.back") == 0 || strcmp(action_id, "app.apps.close") == 0) {
            app_drawer_close();
            return true;
        }
        return false;
    }
#endif
    if (strcmp(action_id, "app.back") == 0) {
        if (!s_app_open || s_animating) return false;
        back_animated();
        return true;
    }
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    /* Mirrors a physical content-area tap for repeatable voice acceptance. */
    if (strcmp(action_id, "app.xiaozhi.talk") == 0) {
        if (!s_app_open || s_animating) return false;
        const char *name = mars_dynamic_bridge_registry_name("xiaozhi");
        const mars_app_t *app = mars_app_get(s_selected_app);
        if (name == NULL || app == NULL || strcmp(app->name, name) != 0 || app->touch == NULL) return false;
        app->touch(160, 120);
        return true;
    }
#endif
    if (s_app_open || s_animating) return false;
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    const bool fluid_clock_action=strcmp(action_id,"app.fluid_clock.open")==0;
    const char *fluid_clock_name=fluid_clock_action?mars_dynamic_bridge_registry_name("fluid_clock"):NULL;
    if (strcmp(action_id, "app.apps.open") == 0 || (fluid_clock_action&&fluid_clock_name==NULL)) {
        if (round_ui_active()) return round_ui_action("app.apps.open");
        s_drawer_return_to_ui = ui_runtime_is_active();
        ui_runtime_suspend();
        app_drawer_open();
        return true;
    }
    if(fluid_clock_action)return open_named_app(fluid_clock_name);
    if (strcmp(action_id, "app.xiaozhi.open") == 0) {
        const char *dynamic_xiaozhi = mars_dynamic_bridge_registry_name("xiaozhi");
        if (dynamic_xiaozhi != NULL) return open_named_app(dynamic_xiaozhi);
    }
#endif
    static const struct {
        const char *action_id;
        const char *app_name;
    } builtin_actions[] = {
        {"app.xiaozhi.open", "XIAOZHI"},
        {"app.clock.open", "CLOCK"},
        {"app.timer.open", "TIMER"},
        {"app.counter.open", "COUNTER"},
        {"app.system.open", "SETTINGS"},
        {"app.about.open", "ABOUT"},
    };
    for (size_t i = 0; i < sizeof(builtin_actions) / sizeof(builtin_actions[0]); ++i) {
        if (strcmp(action_id, builtin_actions[i].action_id) == 0) {
            return open_named_app(builtin_actions[i].app_name);
        }
    }
    /* 动态应用：app.dyn.<slot>.open → 打开对应 "APP<slot>" 注册项 */
    if (strncmp(action_id, "app.dyn.", 8) == 0 &&
        strcmp(action_id + strlen(action_id) - 5, ".open") == 0) {
        char dyn_name[16];
        int slot = 0;
        for (const char *p = action_id + 8; *p && *p != '.'; ++p) {
            if (*p < '0' || *p > '9') return false;
            slot = slot * 10 + (*p - '0');
        }
        snprintf(dyn_name, sizeof(dyn_name), "APP%d", slot);
        return open_named_app(dyn_name);
    }
    return false;
}

static void draw_app_card(size_t index, int x, int y)
{
    const mars_app_t *app = mars_app_get(index);
    if (!app) return;
    const bool selected = index == s_selected_app;
    gfx_rect(x, y, 92, 68, selected ? app->accent : COLOR_PANEL);
    gfx_rect(x + 31, y + 10, 30, 22, selected ? COLOR_TEXT : app->accent);
    const int text_x = x + (92 - gfx_text_width(app->name, 1)) / 2;
    gfx_text(text_x, y + 45, app->name, 1, COLOR_TEXT, selected ? app->accent : COLOR_PANEL);
    if (selected) gfx_frame(x - 3, y - 3, 98, 74, 3, COLOR_TEXT);
}

/* 只渲染桌面到 framebuffer，不刷屏（动画路径用） */
void draw_home_render(void)
{
    gfx_clear(COLOR_BG);
    gfx_text(14, 11, "ATK DESKTOP", 3, COLOR_TEXT, COLOR_BG);
    gfx_text(262, 16, "READY", 1, COLOR_GREEN, COLOR_BG);
    for (size_t i = 0; i < mars_app_count(); ++i) {
        const int column = (int)(i % 3);
        const int row = (int)(i / 3);
        draw_app_card(i, 10 + column * 104, 53 + row * 82);
    }
    gfx_text(94, 226, "TOUCH AN APP TO OPEN", 1, COLOR_MUTED, COLOR_BG);
}

static void draw_home(void)
{
    draw_home_render();
    gfx_present();
}

static void handle_touch_press(uint16_t x, uint16_t y)
{
    if (s_app_open) {
        bool back_visible = true;
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
        back_visible = mars_input_back_visible() != 0;
#endif
        if (back_visible && y < 46 && x >= 230) {
            back_animated();
            return;
        }
        const mars_app_t *app = mars_app_get(s_selected_app);
        if (app && app->touch) app->touch(x, y);
        return;
    }

    for (size_t i = 0; i < mars_app_count(); ++i) {
        const int column = (int)(i % 3);
        const int row = (int)(i / 3);
        const int left = 7 + column * 104;
        const int top = 50 + row * 82;
        if (x >= left && x < left + 98 && y >= top && y < top + 74) {
            open_app_animated(i);
            return;
        }
    }
}

static void handle_key_press(int key)
{
    if (s_app_open) {
        if (key == 3) {
            back_animated();
        } else {
            const mars_app_t *app = mars_app_get(s_selected_app);
            if (app && app->key) app->key(key);
        }
        return;
    }

    const size_t count = mars_app_count();
    if (key == 1) {
        s_selected_app = (s_selected_app + count - 1) % count;
        draw_home();
    } else if (key == 2) {
        s_selected_app = (s_selected_app + 1) % count;
        draw_home();
    } else if (key == 3) {
        open_app_animated(s_selected_app);
    }
}

void launcher_run(void)
{
    /* 固定先注册内置应用，动态槽始终追加在后面。动作分发仍按名称查找，
     * 避免未来安装/删除动态应用时改变内置应用的含义。 */
    box_apps_register_all();
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    mars_dynamic_bridge_init();
#endif
    const bool native_ui = round_ui_init();
    if (native_ui) {
        ui_runtime_init_suspended();
        round_ui_resume();
    } else {
#ifdef CONFIG_BOARD_TYPE_MARS_ATK_BOX
        gfx_clear(0x0000);
        gfx_present();
#else
        draw_home();
#endif
        ui_runtime_init();
    }
    ui_runtime_set_action_handler(handle_ui_action);
#ifdef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    /* Delay OTA confirmation by 5s: if voice/audio/resources crash during
     * that window, we never call mark_app_valid and the bootloader rolls
     * back to the previous slot automatically. */
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t ota_state;
        if (running != NULL &&
            esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
            ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_timer_handle_t t;
            const esp_timer_create_args_t args = {
                .callback = ota_confirm_cb,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "ota_confirm",
            };
            if (esp_timer_create(&args, &t) == ESP_OK) {
                esp_timer_start_once(t, 5 * 1000 * 1000);
            }
        }
    }
#endif

    int candidate_key = 0;
    int stable_key = 0;
    int candidate_samples = 0;
    bool touch_was_down = false;
    while (true) {
        /* 动画进行中：只驱动过渡，屏蔽输入 */
        if (s_animating) {
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
            mars_input_publish_touch(0, 0, 0);
#endif
            const int64_t started_us = esp_timer_get_time();
            mars_anim_tick(&s_transition, started_us / 1000LL);
            animation_frame_wait(started_us);
            continue;
        }

        ui_runtime_poll();
        /* A serial action can start a transition during poll. Keep page tick
         * callbacks from presenting the foreground before the first slide. */
        if (s_animating) continue;

        if (round_ui_active()) {
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
            mars_input_publish_touch(0, 0, 0);
#endif
            const int64_t started_us = esp_timer_get_time();
            round_ui_poll(started_us / 1000LL);
            const char *action = round_ui_take_action();
            if (action) handle_ui_action(action);
            touch_was_down = false;
            if (round_ui_busy()) animation_frame_wait(started_us);
            else vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint16_t touch_x = 0;
        uint16_t touch_y = 0;
        bool touch_down;
#if MARS_BOARD_DISPLAY_ROUND
        if(s_native_return&&s_app_open){
            mars_touch_sample_t point=mars_board_touch_sample();
            mars_gesture_event_t gesture=mars_gesture_update(&s_app_gesture,point,esp_timer_get_time()/1000);
            if(gesture.kind==MARS_GESTURE_EDGE_BACK){
                mars_input_publish_touch(0,0,0);back_animated();touch_was_down=false;continue;
            }
            bool in_view;
            if(round_ui_app_native())in_view=round_ui_app_map_touch(point.x,point.y,&touch_x,&touch_y);
            else{
                in_view=point.x>=MARS_BOARD_UI_X&&point.y>=MARS_BOARD_UI_Y&&
                    point.x<MARS_BOARD_UI_X+BOX3_LCD_WIDTH&&point.y<MARS_BOARD_UI_Y+BOX3_LCD_HEIGHT;
                if(in_view){touch_x=point.x-MARS_BOARD_UI_X;touch_y=point.y-MARS_BOARD_UI_Y;}
            }
            bool reserved=s_app_gesture.blocked||s_app_gesture.edge_start;
            touch_down=point.down&&in_view&&!reserved;
            /* Edge-origin taps are delivered only after they cannot become
             * navigation. No press/record/timer operation leaks on swipe. */
            if(in_view&&reserved&&gesture.kind==MARS_GESTURE_TAP)handle_touch_press(touch_x,touch_y);
        }else touch_down=box3_touch_read(&touch_x,&touch_y);
#else
        touch_down=box3_touch_read(&touch_x,&touch_y);
#endif
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
        if(round_ui_app_native())mars_input_publish_native_touch(touch_down,touch_x,touch_y);
        else mars_input_publish_touch(touch_down && s_app_open && !ui_runtime_is_active() &&
                                 !app_drawer_is_open() && !s_drawer_wait_touch_up,
                                 touch_x, touch_y);
        if (app_drawer_is_open()) {
            const int64_t started_us = esp_timer_get_time();
            app_drawer_touch(touch_down, touch_x, touch_y);
            app_drawer_tick(esp_timer_get_time() / 1000LL);
            touch_was_down = touch_down;
            if (!app_drawer_is_open()) {
                /* A new finger held during fade-out must not reach the home
                 * page (its gesture tracker is independent of touch_was_down). */
                s_drawer_wait_touch_up = touch_down;
                const int slot = app_drawer_take_launch();
                if (s_drawer_return_to_ui) ui_runtime_resume();
                else draw_home();
                if (slot >= 0) {
                    char name[16];
                    snprintf(name, sizeof(name), "APP%d", slot);
                    open_named_app(name);
                }
            }
            animation_frame_wait(started_us);
            continue;
        }
        if (s_drawer_wait_touch_up) {
            touch_was_down = touch_down;
            if (!touch_down) s_drawer_wait_touch_up = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
#endif
        if (ui_runtime_is_active()) {
            ui_runtime_handle_touch(touch_down, touch_x, touch_y);
            touch_was_down = touch_down;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const int raw_key = box3_key_read();
        if (raw_key == candidate_key) {
            if (candidate_samples < 3) ++candidate_samples;
        } else {
            candidate_key = raw_key;
            candidate_samples = 1;
        }
        if (candidate_samples >= 3 && stable_key != candidate_key) {
            stable_key = candidate_key;
            if (stable_key != 0) handle_key_press(stable_key);
        }
        if (touch_down && !touch_was_down) handle_touch_press(touch_x, touch_y);
        touch_was_down = touch_down;
        if (s_animating) continue;

        if (s_app_open) {
            const mars_app_t *app = mars_app_get(s_selected_app);
            if (app && app->tick) app->tick(esp_timer_get_time() / 1000LL);
        }
#if MARS_BOARD_DISPLAY_ROUND
        vTaskDelay(pdMS_TO_TICKS(10));
#else
        vTaskDelay(pdMS_TO_TICKS(20));
#endif
    }
}
