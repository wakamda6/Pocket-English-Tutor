#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tutor_session.h"
#include "tutor_setup.h"

LV_FONT_DECLARE(tutor_ui_font_16);
LV_FONT_DECLARE(tutor_ui_font_20);

#define WIFI_CONNECTED BIT0
#define WIFI_FAILED BIT1
#define WIFI_SETUP BIT2
#define WIFI_VERIFYING BIT3
#define WIFI_ATTEMPTS 5
#define WIFI_JOIN_TIMEOUT_MS 25000
#define CLOCK_TIMEOUT_MS 20000
#define SETUP_TIMEOUT_MS 300000
#define SETUP_RESTART_MS 3000
#define STORE_NAMESPACE "english_tutor"
#define STORE_KEY "config_v1"
#define SPEED_KEY "speed_pct"
#define EARLIEST_TLS_TIME ((time_t)1700000000)

static const char *TAG = "english_tutor";

typedef struct {
    bsp_btn_t button;
    bsp_btn_ev_t action;
} button_event_t;

typedef struct {
    const char *heading;
    const char *action;
    uint32_t color;
} view_t;

static QueueHandle_t buttons;
static EventGroupHandle_t network;
static unsigned wifi_attempts;
static bool clock_started;
static bool clock_sync_attempted;
static TickType_t clock_attempt_at;
static bool setup_mode;
static bool setup_online;
static bool setup_restarting;
static TickType_t setup_started_at;
static TickType_t setup_result_at;
static TickType_t verification_started_at;
static tutor_setup_request_t pending_setup;
static uint8_t volume_percent = TUTOR_DEFAULT_VOLUME;
static tutor_mode_t mode = TUTOR_MODE_ENGLISH_CHAT;
static int8_t speech_speed;
static tutor_config_t credentials;
static lv_obj_t *heading_label;
static lv_obj_t *action_label;
static lv_obj_t *battery_label;
static lv_obj_t *volume_label;
static lv_obj_t *orb;
static lv_obj_t *setup_qr;

static void on_button(bsp_btn_t button, bsp_btn_ev_t action, void *arg)
{
    (void)arg;
    if (action == BSP_BTN_CLICK || action == BSP_BTN_LONG) {
        button_event_t event = { .button = button, .action = action };
        xQueueSend(buttons, &event, 0);
    }
}

static void wifi_connect(void)
{
    if (wifi_attempts >= WIFI_ATTEMPTS || esp_wifi_connect() != ESP_OK) {
        xEventGroupSetBits(network, WIFI_FAILED);
    } else {
        wifi_attempts++;
    }
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)data;
    EventBits_t bits = xEventGroupGetBits(network);
    if (bits & WIFI_SETUP) {
        if (bits & WIFI_VERIFYING) {
            if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
                xEventGroupSetBits(network, WIFI_CONNECTED);
            } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
                xEventGroupClearBits(network, WIFI_CONNECTED);
                xEventGroupSetBits(network, WIFI_FAILED);
            }
        }
        return;
    }
    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_attempts = 0;
        xEventGroupClearBits(network, WIFI_FAILED);
        xEventGroupSetBits(network, WIFI_CONNECTED);
    } else if (event_id == WIFI_EVENT_STA_START && base == WIFI_EVENT) {
        wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED && base == WIFI_EVENT) {
        xEventGroupClearBits(network, WIFI_CONNECTED);
        if (!(xEventGroupGetBits(network) & WIFI_FAILED)) {
            wifi_connect();
        }
    }
}

// An explicit retry runs on the same event loop as disconnect callbacks.
static void on_retry(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    (void)data;
    if ((xEventGroupGetBits(network) & (WIFI_FAILED | WIFI_SETUP)) == WIFI_FAILED) {
        wifi_attempts = 0;
        if (esp_wifi_stop() == ESP_OK && esp_wifi_start() == ESP_OK) {
            xEventGroupClearBits(network, WIFI_FAILED);
        }
    }
}

ESP_EVENT_DEFINE_BASE(TUTOR_WIFI_RETRY);

static esp_err_t init_wifi(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL),
                        TAG, "Wi-Fi event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL),
                        TAG, "IP event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(TUTOR_WIFI_RETRY, 0, on_retry, NULL),
                        TAG, "retry event");
    return ESP_OK;
}

static esp_err_t set_station(const tutor_config_t *settings)
{
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, settings->ssid, strlen(settings->ssid));
    memcpy(config.sta.password, settings->password, strlen(settings->password));
    config.sta.threshold.authmode = settings->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
    mbedtls_platform_zeroize(&config, sizeof(config));
    return result;
}

static esp_err_t start_station(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode");
    ESP_RETURN_ON_ERROR(set_station(&credentials), TAG, "Station configuration");
    return esp_wifi_start();
}

static bool load_config(void)
{
    nvs_handle_t handle;
    if (nvs_open(STORE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t size = sizeof(credentials);
    esp_err_t result = nvs_get_blob(handle, STORE_KEY, &credentials, &size);
    int8_t saved_speed;
    if (nvs_get_i8(handle, SPEED_KEY, &saved_speed) == ESP_OK && tutor_speed_valid(saved_speed)) {
        speech_speed = saved_speed;
    }
    nvs_close(handle);
    if (result == ESP_OK && size == sizeof(credentials) && tutor_config_valid(&credentials)) {
        return true;
    }
    mbedtls_platform_zeroize(&credentials, sizeof(credentials));
    return false;
}

static void change_speech_speed(int step)
{
    int speed = speech_speed + step;
    if (!tutor_speed_valid(speed)) return;
    speech_speed = speed;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_i8(handle, SPEED_KEY, speech_speed);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result != ESP_OK) ESP_LOGW(TAG, "Unable to save speech speed: %s", esp_err_to_name(result));
}

static esp_err_t write_config(const tutor_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = config ? nvs_set_blob(handle, STORE_KEY, config, sizeof(*config))
                    : nvs_erase_key(handle, STORE_KEY);
    if (result == ESP_OK || (!config && result == ESP_ERR_NVS_NOT_FOUND)) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static void open_setup(void)
{
    if (setup_online) {
        return;
    }
    setup_mode = true;
    xEventGroupSetBits(network, WIFI_SETUP);
    xEventGroupClearBits(network, WIFI_CONNECTED | WIFI_FAILED | WIFI_VERIFYING);
    esp_wifi_stop();
    esp_err_t result = tutor_setup_start();
    setup_online = result == ESP_OK;
    setup_started_at = xTaskGetTickCount();
    if (!setup_online) {
        ESP_LOGW(TAG, "Setup hotspot unavailable: %s", esp_err_to_name(result));
    }
}

static void close_setup(void)
{
    xEventGroupClearBits(network, WIFI_VERIFYING | WIFI_CONNECTED | WIFI_FAILED);
    tutor_setup_stop();
    esp_wifi_stop();
    mbedtls_platform_zeroize(&pending_setup, sizeof(pending_setup));
    setup_mode = false;
    setup_online = false;
    setup_restarting = false;
}

static void finish_setup(bool success, bool clearing)
{
    esp_err_t result = success ? write_config(clearing ? NULL : &pending_setup.config) : ESP_FAIL;
    mbedtls_platform_zeroize(&pending_setup, sizeof(pending_setup));
    xEventGroupClearBits(network, WIFI_VERIFYING | WIFI_CONNECTED | WIFI_FAILED);
    if (result == ESP_OK) {
        tutor_setup_report(clearing ? TUTOR_SETUP_CLEARED : TUTOR_SETUP_SAVED);
        setup_restarting = true;
        setup_result_at = xTaskGetTickCount();
        return;
    }
    // Keep the old NVS record and return to the portal with scanning available.
    esp_wifi_disconnect();
    tutor_setup_report(success ? TUTOR_SETUP_SAVE_FAILED : TUTOR_SETUP_WIFI_FAILED);
}

static void poll_setup(TickType_t now, EventBits_t wifi, bool configured)
{
    if (setup_restarting) {
        if (now - setup_result_at >= pdMS_TO_TICKS(SETUP_RESTART_MS)) {
            close_setup();
            esp_restart();
        }
        return;
    }
    if (wifi & WIFI_VERIFYING) {
        if (wifi & WIFI_CONNECTED) {
            finish_setup(true, false);
        } else if ((wifi & WIFI_FAILED) ||
                   now - verification_started_at >= pdMS_TO_TICKS(WIFI_JOIN_TIMEOUT_MS)) {
            finish_setup(false, false);
        }
        return;
    }
    if (tutor_setup_take_request(&pending_setup)) {
        setup_started_at = now;
        if (pending_setup.clear) {
            finish_setup(true, true);
        } else {
            xEventGroupClearBits(network, WIFI_CONNECTED | WIFI_FAILED);
            if (set_station(&pending_setup.config) != ESP_OK) {
                finish_setup(false, false);
                return;
            }
            verification_started_at = now;
            xEventGroupSetBits(network, WIFI_VERIFYING);
            if (esp_wifi_connect() != ESP_OK) {
                finish_setup(false, false);
            }
        }
    } else if (now - setup_started_at >= pdMS_TO_TICKS(SETUP_TIMEOUT_MS)) {
        close_setup();
        if (configured) esp_restart();
    }
}

static lv_obj_t *new_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static bool create_screen(void)
{
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init() || !bsp_lvgl_lock(1000)) {
        return false;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x12222C), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = new_label(screen, &tutor_ui_font_20, 0xF4EBDC);
    lv_label_set_text(title, "英语外教");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 23);
    battery_label = new_label(screen, &lv_font_montserrat_14, 0xBFCCC8);
    lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -20, 26);
    lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);

    orb = lv_obj_create(screen);
    lv_obj_remove_flag(orb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(orb, 112, 112);
    lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(orb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(orb, 6, 0);
    lv_obj_set_style_pad_all(orb, 0, 0);
    lv_obj_align(orb, LV_ALIGN_TOP_MID, 0, 71);
    for (int i = 0; i < 5; i++) {
        static const uint8_t heights[] = { 17, 32, 49, 32, 17 };
        lv_obj_t *bar = lv_obj_create(orb);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 5, heights[i]);
        lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xE9E5D8), 0);
        lv_obj_align(bar, LV_ALIGN_CENTER, (i - 2) * 13, 0);
    }
    heading_label = new_label(screen, &tutor_ui_font_20, 0xF4EBDC);
    lv_obj_align(heading_label, LV_ALIGN_TOP_MID, 0, 195);
    action_label = new_label(screen, &tutor_ui_font_16, 0xBFCCC8);
    lv_obj_set_width(action_label, 208);
    lv_obj_set_style_text_align(action_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(action_label, LV_ALIGN_TOP_MID, 0, 225);
    volume_label = new_label(screen, &tutor_ui_font_16, 0xBFCCC8);
    lv_obj_align(volume_label, LV_ALIGN_BOTTOM_MID, 0, -9);
    bsp_lvgl_unlock();
    bsp_display_backlight(85);
    return true;
}

static void request_clock_sync(void)
{
    clock_attempt_at = xTaskGetTickCount();
    clock_sync_attempted = true;
    esp_err_t result;
    if (clock_started) {
        result = esp_netif_sntp_start();
    } else {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
        config.wait_for_sync = false;
        result = esp_netif_sntp_init(&config);
        clock_started = result == ESP_OK;
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Clock sync unavailable: %s", esp_err_to_name(result));
    }
}

static view_t current_view(bool configured, bool wifi_ready, EventBits_t wifi)
{
    if (setup_mode) {
        if (!setup_online) {
            return (view_t){ "无法配网", "短按确认重试\n长按确认返回", 0xE58375 };
        }
        if (setup_restarting) {
            return (view_t){ "设置已保存", "设备正在重启", 0x6ED6B5 };
        }
        const char *heading = (wifi & WIFI_VERIFYING) ? "正在验证网络" : tutor_setup_ssid();
        const char *action = (wifi & WIFI_VERIFYING) ?
                             "请保持连接设备热点" : "扫码连接设备热点\nhttp://" TUTOR_SETUP_IP;
        return (view_t){ heading, action, 0xE5AB70 };
    }
    if (!configured) {
        return (view_t){ "等待配网", "按确认键开启热点", 0xE5AB70 };
    }
    if (!wifi_ready || ((wifi & WIFI_FAILED) && !(wifi & WIFI_CONNECTED))) {
        return (view_t){ "无线网络不可用", wifi_ready ? "短按确认重试\n长按确认配网" :
                         "长按确认配网", 0xE58375 };
    }
    if (!(wifi & WIFI_CONNECTED)) {
        return (view_t){ "正在连接网络", "长按确认配网", 0xE5AB70 };
    }
    if (time(NULL) < EARLIEST_TLS_TIME) {
        if (clock_sync_attempted &&
            xTaskGetTickCount() - clock_attempt_at >= pdMS_TO_TICKS(CLOCK_TIMEOUT_MS)) {
            return (view_t){ "无法同步时间", "短按确认重试\n长按确认配网", 0xE58375 };
        }
        return (view_t){ "正在同步时间", "等待网络时间", 0xE5AB70 };
    }
    switch (tutor_session_status()) {
    case TUTOR_CONNECTING:
        return (view_t){ "正在连接云端", "长按确认结束对话", 0xE5AB70 };
    case TUTOR_READY:
        return (view_t){ "轮到你了", "短按确认录音\n长按确认结束对话", 0x6ED6B5 };
    case TUTOR_ARMING:
        return (view_t){ "正在准备录音", "长按确认结束对话", 0xE5AB70 };
    case TUTOR_LISTENING:
        return (view_t){ "正在听", "按确认键发送", 0x6ED6B5 };
    case TUTOR_SENDING:
        return (view_t){ "正在发送", "长按确认结束对话", 0x9BAFEB };
    case TUTOR_THINKING:
        return (view_t){ "正在思考", "长按确认结束对话", 0x9BAFEB };
    case TUTOR_SPEAKING:
        return (view_t){ "正在回复", "长按确认结束对话", 0x9BAFEB };
    case TUTOR_CLOSING:
        return (view_t){ "正在结束对话", "", 0xBFCCC8 };
    case TUTOR_CLOUD_FAILED:
        return (view_t){ "云端不可用", "短按确认重试\n长按确认配网", 0xE58375 };
    case TUTOR_AUDIO_FAILED:
        return (view_t){ "音频不可用", "按确认键重试", 0xE58375 };
    default:
        return (view_t){ mode == TUTOR_MODE_TEACHING ? "教学模式" : "畅聊模式",
                         "短按确认开始\n长按确认配网", 0x6ED6B5 };
    }
}

static void update_screen(view_t view, int battery)
{
    static char previous_heading[25];
    static char previous_action[96];
    static unsigned previous_volume;
    static int previous_speed = -100;
    static int previous_battery = -1;
    static bool previous_setup;
    static bool previous_active;
    bool active = tutor_session_active();
    bool heading_changed = strcmp(view.heading, previous_heading) != 0;
    bool action_changed = strcmp(view.action, previous_action) != 0;
    bool show_qr = setup_online && !setup_restarting;
    if ((!heading_changed && !action_changed && setup_mode == previous_setup &&
         active == previous_active &&
         speech_speed == previous_speed &&
         show_qr == (setup_qr != NULL) &&
         volume_percent == previous_volume && battery == previous_battery) ||
        !bsp_lvgl_lock(25)) {
        return;
    }
    if (show_qr && !setup_qr) {
        char payload[48];
        snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", tutor_setup_ssid());
        lv_obj_t *qr = lv_qrcode_create(lv_screen_active());
        lv_qrcode_set_size(qr, 122);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_set_quiet_zone(qr, true);
        lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 67);
        if (lv_qrcode_update(qr, payload, strlen(payload)) == LV_RESULT_OK) {
            setup_qr = qr;
            lv_obj_add_flag(orb, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_delete(qr);
            ESP_LOGW(TAG, "Could not render setup QR");
        }
    } else if (!show_qr && setup_qr) {
        lv_obj_delete(setup_qr);
        setup_qr = NULL;
        lv_obj_remove_flag(orb, LV_OBJ_FLAG_HIDDEN);
    }
    if (heading_changed) {
        lv_label_set_text(heading_label, view.heading);
        lv_obj_set_style_border_color(orb, lv_color_hex(view.color), 0);
        snprintf(previous_heading, sizeof(previous_heading), "%s", view.heading);
    }
    if (action_changed) {
        lv_label_set_text(action_label, view.action);
        snprintf(previous_action, sizeof(previous_action), "%s", view.action);
    }
    if (setup_mode != previous_setup || active != previous_active || speech_speed != previous_speed ||
        volume_percent != previous_volume) {
        if (setup_mode) {
            lv_label_set_text_static(volume_label, "按确认键返回");
        } else if (active) {
            lv_label_set_text_fmt(volume_label, "上下键调音量 %u%%", volume_percent);
        } else {
            int percent = 100 + speech_speed;
            lv_obj_set_style_text_align(volume_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text_fmt(volume_label, "上键畅聊 / 下键教学\n长按上下键调语速 %d.%d倍",
                                  percent / 100, percent % 100 / 10);
        }
        previous_speed = speech_speed;
        previous_volume = volume_percent;
        previous_setup = setup_mode;
        previous_active = active;
    }
    if (battery != previous_battery) {
        if (battery >= 0) {
            lv_label_set_text_fmt(battery_label, "%d%%", battery);
            lv_obj_remove_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
        }
        previous_battery = battery;
    }
    bsp_lvgl_unlock();
}

static void handle_button(button_event_t event, bool configured, bool wifi_ready, EventBits_t wifi)
{
    if (event.button != BSP_BTN_OK) {
        if (setup_mode) return;
        if (!tutor_session_active()) {
            if (event.action == BSP_BTN_LONG) {
                change_speech_speed(event.button == BSP_BTN_UP ? TUTOR_SPEED_STEP : -TUTOR_SPEED_STEP);
            } else {
                mode = event.button == BSP_BTN_UP ? TUTOR_MODE_ENGLISH_CHAT : TUTOR_MODE_TEACHING;
            }
            return;
        }
        if (event.action != BSP_BTN_CLICK) return;
        int value = volume_percent + (event.button == BSP_BTN_UP ? 5 : -5);
        if (value >= 0 && value <= 100) {
            volume_percent = value;
            tutor_session_set_volume(volume_percent);
        }
        return;
    }
    if (event.action == BSP_BTN_LONG) {
        if (tutor_session_active()) {
            tutor_session_stop();
        } else if (setup_mode && !setup_online) {
            close_setup();
            if (configured) esp_restart();
        } else {
            open_setup();
        }
    } else if (event.action == BSP_BTN_CLICK) {
        if (setup_restarting) {
            return;
        } else if (setup_online) {
            close_setup();
            if (configured) esp_restart();
        } else if (setup_mode || !configured) {
            open_setup();
        } else if (tutor_session_active()) {
            tutor_session_toggle_mic();
        } else if (wifi & WIFI_CONNECTED) {
            if (time(NULL) >= EARLIEST_TLS_TIME) {
                tutor_session_start(credentials.key, mode, speech_speed);
            } else {
                request_clock_sync();
            }
        } else if (wifi_ready && (wifi & WIFI_FAILED)) {
            esp_event_post(TUTOR_WIFI_RETRY, 0, NULL, 0, 0);
        }
    }
}

void app_main(void)
{
    if (!create_screen()) {
        ESP_LOGE(TAG, "Display unavailable");
        return;
    }
    buttons = xQueueCreate(8, sizeof(button_event_t));
    network = xEventGroupCreate();
    if (!buttons || !network || bsp_button_init(on_button, NULL) != ESP_OK) {
        update_screen((view_t){ "启动失败", "请重启设备", 0xE58375 }, -1);
        return;
    }
    esp_err_t storage = nvs_flash_init();
    if (storage != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable: %s", esp_err_to_name(storage));
        update_screen((view_t){ "存储不可用", "请重启设备", 0xE58375 }, -1);
        return;
    }
    bool configured = load_config();
    bool wifi_initialized = init_wifi() == ESP_OK;
    if (!wifi_initialized) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed");
        update_screen((view_t){ "无线网络不可用", "请重启设备", 0xE58375 }, -1);
        return;
    }
    bool wifi_ready = configured && start_station() == ESP_OK;
    if (!configured) open_setup();
    bool battery_ready = bsp_battery_init() == ESP_OK;
    TickType_t battery_read_at = xTaskGetTickCount() - pdMS_TO_TICKS(5000);
    TickType_t wifi_wait_since = xTaskGetTickCount();
    int battery = -1;
    while (true) {
        EventBits_t wifi = xEventGroupGetBits(network);
        TickType_t now = xTaskGetTickCount();
        if (setup_online) {
            poll_setup(now, wifi, configured);
        } else if (setup_mode) {
            // Retain the setup error screen while waiting for an explicit retry.
        } else if (wifi & WIFI_CONNECTED) {
            wifi_wait_since = now;
            if (!clock_sync_attempted) {
                request_clock_sync();
            }
        } else {
            if (tutor_session_active()) {
                tutor_session_stop();
            }
            if (wifi & WIFI_FAILED) {
                wifi_wait_since = now;
            } else if (wifi_ready && now - wifi_wait_since >= pdMS_TO_TICKS(WIFI_JOIN_TIMEOUT_MS)) {
                xEventGroupSetBits(network, WIFI_FAILED);
            }
        }
        if (battery_ready && now - battery_read_at >= pdMS_TO_TICKS(5000)) {
            battery = bsp_battery_soc();
            battery_read_at = now;
        }
        update_screen(current_view(configured, wifi_ready, wifi), battery);
        button_event_t event;
        if (xQueueReceive(buttons, &event, pdMS_TO_TICKS(40)) == pdTRUE) {
            handle_button(event, configured, wifi_ready, wifi);
        }
    }
}
