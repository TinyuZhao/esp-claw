/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "display_arbiter.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lvgl.h"

#define LUA_MODULE_LVGL_NAME "lvgl"
#define LUA_MODULE_LVGL_OBJ_METATABLE "lvgl.obj"
#define LUA_MODULE_LVGL_DEFAULT_BUFFER_LINES 40
#define LUA_MODULE_LVGL_DEFAULT_TICK_MS 5
#define LUA_MODULE_LVGL_DEFAULT_TASK_PERIOD_MS 10
#define LUA_MODULE_LVGL_TASK_STACK 4096
#define LUA_MODULE_LVGL_TASK_PRIO 5
#define LUA_MODULE_LVGL_PANEL_IF_IO 0
#define LUA_MODULE_LVGL_PANEL_IF_RGB 1
#define LUA_MODULE_LVGL_PANEL_IF_MIPI_DSI 2

typedef enum {
    LUA_LVGL_OBJ_GENERIC = 0,
    LUA_LVGL_OBJ_SCREEN,
    LUA_LVGL_OBJ_LABEL,
    LUA_LVGL_OBJ_BUTTON,
    LUA_LVGL_OBJ_BAR,
    LUA_LVGL_OBJ_SLIDER,
} lua_lvgl_obj_type_t;

typedef struct lua_lvgl_obj_record {
    lv_obj_t *obj;
    uint32_t generation;
    lua_lvgl_obj_type_t type;
    lua_State *owner;
    bool owned;
    bool valid;
    struct lua_lvgl_obj_record *next;
} lua_lvgl_obj_record_t;

typedef struct {
    lua_lvgl_obj_record_t *record;
} lua_lvgl_obj_ud_t;

typedef struct {
    SemaphoreHandle_t mutex;
    bool lvgl_initialized;
    bool runtime_initialized;
    bool display_owner_acquired;
    volatile bool task_stop;
    TaskHandle_t task_handle;
    TaskHandle_t task_waiter;
    esp_timer_handle_t tick_timer;
    lv_display_t *display;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    int width;
    int height;
    int panel_if;
    uint32_t generation;
    uint32_t tick_ms;
    uint32_t task_period_ms;
    void *draw_buf;
    size_t draw_buf_size;
    lua_State *runtime_owner;
    lua_lvgl_obj_record_t *records;
} lua_lvgl_state_t;

typedef struct {
    bool has_opts;
    const char *text;
    const char *align_value;
    int x;
    int y;
    int w;
    int h;
    int min_value;
    int max_value;
    int value;
} lua_lvgl_opts_t;

static const char *TAG = "lua_lvgl";
static lua_lvgl_state_t s_lvgl;

static esp_err_t lua_lvgl_lock(void)
{
    if (!s_lvgl.mutex) {
        s_lvgl.mutex = xSemaphoreCreateMutex();
    }
    if (!s_lvgl.mutex) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_lvgl.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void lua_lvgl_unlock(void)
{
    if (s_lvgl.mutex) {
        xSemaphoreGive(s_lvgl.mutex);
    }
}

static int lua_lvgl_error_esp(lua_State *L, const char *what, esp_err_t err)
{
    return luaL_error(L, "lvgl %s failed: %s", what, esp_err_to_name(err));
}

static const char *lua_lvgl_display_owner_name(display_arbiter_owner_t owner)
{
    switch (owner) {
    case DISPLAY_ARBITER_OWNER_NONE:
        return "none";
    case DISPLAY_ARBITER_OWNER_LUA:
        return "lua";
    case DISPLAY_ARBITER_OWNER_EMOTE:
        return "emote";
    default:
        return "unknown";
    }
}

static lua_lvgl_obj_ud_t *lua_lvgl_check_ud(lua_State *L, int index)
{
    return (lua_lvgl_obj_ud_t *)luaL_checkudata(L, index, LUA_MODULE_LVGL_OBJ_METATABLE);
}

static void lua_lvgl_obj_delete_event_cb(lv_event_t *event)
{
    lua_lvgl_obj_record_t *record = (lua_lvgl_obj_record_t *)lv_event_get_user_data(event);

    if (!record || lv_event_get_code(event) != LV_EVENT_DELETE) {
        return;
    }
    record->obj = NULL;
    record->valid = false;
}

static lv_obj_t *lua_lvgl_validate_ud_locked(const lua_lvgl_obj_ud_t *ud,
                                             lua_lvgl_obj_type_t *out_type,
                                             const char **out_error)
{
    lua_lvgl_obj_record_t *record = ud ? ud->record : NULL;

    if (!s_lvgl.runtime_initialized) {
        *out_error = "lvgl runtime is not initialized";
        return NULL;
    }
    if (!record || record->generation != s_lvgl.generation) {
        *out_error = "lvgl object belongs to an old runtime";
        return NULL;
    }
    if (!record->valid || !record->obj) {
        *out_error = "lvgl object has been deleted";
        return NULL;
    }
    if (!lv_obj_is_valid(record->obj)) {
        record->obj = NULL;
        record->valid = false;
        *out_error = "lvgl object is no longer valid";
        return NULL;
    }
    if (out_type) {
        *out_type = record->type;
    }
    return record->obj;
}

static lua_lvgl_obj_ud_t *lua_lvgl_push_obj(lua_State *L, lv_obj_t *obj, lua_lvgl_obj_type_t type, bool owned)
{
    lua_lvgl_obj_record_t *record = (lua_lvgl_obj_record_t *)calloc(1, sizeof(*record));
    lua_lvgl_obj_ud_t *ud;

    if (!record) {
        return NULL;
    }
    ud = (lua_lvgl_obj_ud_t *)lua_newuserdata(L, sizeof(*ud));
    record->obj = obj;
    record->generation = s_lvgl.generation;
    record->type = type;
    record->owner = L;
    record->owned = owned;
    record->valid = true;
    record->next = s_lvgl.records;
    s_lvgl.records = record;
    ud->record = record;
    lv_obj_add_event_cb(obj, lua_lvgl_obj_delete_event_cb, LV_EVENT_DELETE, record);
    luaL_getmetatable(L, LUA_MODULE_LVGL_OBJ_METATABLE);
    lua_setmetatable(L, -2);
    return ud;
}

static int lua_lvgl_get_opt_int_field(lua_State *L, int index, const char *field, int default_value)
{
    int value = default_value;

    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        if (!lua_isinteger(L, -1)) {
            luaL_error(L, "lvgl option '%s' must be an integer", field);
        }
        value = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static const char *lua_lvgl_get_opt_string_field(lua_State *L, int index, const char *field)
{
    const char *value = NULL;

    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        value = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static bool lua_lvgl_opt_table(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) {
        return false;
    }
    luaL_checktype(L, index, LUA_TTABLE);
    return true;
}

static esp_err_t lua_lvgl_parse_align(lua_State *L, const char *value, lv_align_t *out_align)
{
    if (!value || strcmp(value, "top_left") == 0 || strcmp(value, "default") == 0) {
        *out_align = LV_ALIGN_TOP_LEFT;
        return ESP_OK;
    }
    if (strcmp(value, "top_mid") == 0 || strcmp(value, "top") == 0) {
        *out_align = LV_ALIGN_TOP_MID;
        return ESP_OK;
    }
    if (strcmp(value, "top_right") == 0) {
        *out_align = LV_ALIGN_TOP_RIGHT;
        return ESP_OK;
    }
    if (strcmp(value, "bottom_left") == 0) {
        *out_align = LV_ALIGN_BOTTOM_LEFT;
        return ESP_OK;
    }
    if (strcmp(value, "bottom_mid") == 0 || strcmp(value, "bottom") == 0) {
        *out_align = LV_ALIGN_BOTTOM_MID;
        return ESP_OK;
    }
    if (strcmp(value, "bottom_right") == 0) {
        *out_align = LV_ALIGN_BOTTOM_RIGHT;
        return ESP_OK;
    }
    if (strcmp(value, "left_mid") == 0 || strcmp(value, "left") == 0) {
        *out_align = LV_ALIGN_LEFT_MID;
        return ESP_OK;
    }
    if (strcmp(value, "right_mid") == 0 || strcmp(value, "right") == 0) {
        *out_align = LV_ALIGN_RIGHT_MID;
        return ESP_OK;
    }
    if (strcmp(value, "center") == 0 || strcmp(value, "centre") == 0) {
        *out_align = LV_ALIGN_CENTER;
        return ESP_OK;
    }
    (void)L;
    return ESP_ERR_INVALID_ARG;
}

static void lua_lvgl_parse_opts(lua_State *L, int index, lua_lvgl_opts_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->w = -1;
    opts->h = -1;
    opts->max_value = 100;

    if (!lua_lvgl_opt_table(L, index)) {
        return;
    }

    opts->has_opts = true;
    opts->x = lua_lvgl_get_opt_int_field(L, index, "x", 0);
    opts->y = lua_lvgl_get_opt_int_field(L, index, "y", 0);
    opts->w = lua_lvgl_get_opt_int_field(L, index, "w", -1);
    opts->h = lua_lvgl_get_opt_int_field(L, index, "h", -1);
    opts->align_value = lua_lvgl_get_opt_string_field(L, index, "align");
    opts->text = lua_lvgl_get_opt_string_field(L, index, "text");
    opts->min_value = lua_lvgl_get_opt_int_field(L, index, "min", 0);
    opts->max_value = lua_lvgl_get_opt_int_field(L, index, "max", 100);
    opts->value = lua_lvgl_get_opt_int_field(L, index, "value", opts->min_value);
}

static void lua_lvgl_apply_common_opts_locked(lv_obj_t *obj, const lua_lvgl_opts_t *opts)
{
    lv_align_t align;

    if (!opts->has_opts) {
        return;
    }

    if (opts->w > 0 && opts->h > 0) {
        lv_obj_set_size(obj, opts->w, opts->h);
    }
    if (opts->align_value && lua_lvgl_parse_align(NULL, opts->align_value, &align) == ESP_OK) {
        lv_obj_align(obj, align, opts->x, opts->y);
    } else {
        lv_obj_set_pos(obj, opts->x, opts->y);
    }
}

static void lua_lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    lua_lvgl_state_t *state = (lua_lvgl_state_t *)lv_display_get_user_data(display);

    if (state && state->panel) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(state->panel,
                                                  area->x1,
                                                  area->y1,
                                                  area->x2 + 1,
                                                  area->y2 + 1,
                                                  px_map);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "flush failed: %s", esp_err_to_name(err));
        }
    }
    lv_display_flush_ready(display);
}

static void lua_lvgl_tick_timer_cb(void *arg)
{
    lua_lvgl_state_t *state = (lua_lvgl_state_t *)arg;

    lv_tick_inc(state && state->tick_ms ? state->tick_ms : LUA_MODULE_LVGL_DEFAULT_TICK_MS);
}

static void lua_lvgl_task(void *arg)
{
    lua_lvgl_state_t *state = (lua_lvgl_state_t *)arg;

    while (!state->task_stop) {
        if (lua_lvgl_lock() == ESP_OK) {
            if (state->runtime_initialized) {
                (void)lv_timer_handler();
            }
            lua_lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(state->task_period_ms ? state->task_period_ms : LUA_MODULE_LVGL_DEFAULT_TASK_PERIOD_MS));
    }
    if (state->task_waiter) {
        xTaskNotifyGive(state->task_waiter);
        state->task_waiter = NULL;
    }
    state->task_handle = NULL;
    vTaskDelete(NULL);
}

static void lua_lvgl_stop_task(void)
{
    TaskHandle_t task = s_lvgl.task_handle;

    if (!task) {
        return;
    }
    s_lvgl.task_waiter = xTaskGetCurrentTaskHandle();
    s_lvgl.task_stop = true;
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
        ESP_LOGW(TAG, "lvgl task stop timeout");
    }
    s_lvgl.task_waiter = NULL;
    s_lvgl.task_handle = NULL;
}

static void lua_lvgl_invalidate_records_locked(void)
{
    lua_lvgl_obj_record_t *record;

    for (record = s_lvgl.records; record != NULL; record = record->next) {
        if (record->generation == s_lvgl.generation) {
            record->obj = NULL;
            record->valid = false;
        }
    }
}

static void lua_lvgl_cleanup_state_objects_locked(lua_State *L)
{
    lua_lvgl_obj_record_t *record;
    lv_obj_t *fallback_screen = NULL;

    if (!L || !s_lvgl.runtime_initialized) {
        return;
    }

    for (record = s_lvgl.records; record != NULL; record = record->next) {
        if (record->generation != s_lvgl.generation ||
                record->owner != L ||
                !record->owned ||
                !record->valid ||
                !record->obj) {
            continue;
        }
        if (!lv_obj_is_valid(record->obj)) {
            record->obj = NULL;
            record->valid = false;
            continue;
        }
        if (record->obj == lv_screen_active()) {
            if (!fallback_screen) {
                fallback_screen = lv_obj_create(NULL);
            }
            if (fallback_screen) {
                lv_screen_load(fallback_screen);
            }
        }
        lv_obj_delete(record->obj);
        record->obj = NULL;
        record->valid = false;
    }
}

static void lua_lvgl_release_runtime_locked(void)
{
    if (s_lvgl.display) {
        lv_display_set_user_data(s_lvgl.display, NULL);
        lv_display_delete(s_lvgl.display);
        s_lvgl.display = NULL;
    }
    lua_lvgl_invalidate_records_locked();
    heap_caps_free(s_lvgl.draw_buf);
    s_lvgl.draw_buf = NULL;
    s_lvgl.draw_buf_size = 0;
    s_lvgl.panel = NULL;
    s_lvgl.io = NULL;
    s_lvgl.width = 0;
    s_lvgl.height = 0;
    s_lvgl.panel_if = LUA_MODULE_LVGL_PANEL_IF_IO;
    s_lvgl.runtime_initialized = false;
    s_lvgl.runtime_owner = NULL;
    s_lvgl.generation++;
}

static esp_err_t lua_lvgl_deinit_runtime(void)
{
    esp_timer_handle_t timer = s_lvgl.tick_timer;
    esp_err_t err;

    if (timer) {
        (void)esp_timer_stop(timer);
        (void)esp_timer_delete(timer);
        s_lvgl.tick_timer = NULL;
    }

    if (s_lvgl.task_handle) {
        lua_lvgl_stop_task();
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return err;
    }
    lua_lvgl_release_runtime_locked();
    lua_lvgl_unlock();

    if (s_lvgl.display_owner_acquired) {
        err = display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "display owner release failed: %s", esp_err_to_name(err));
        }
        s_lvgl.display_owner_acquired = false;
    }
    return ESP_OK;
}

static int lua_lvgl_init(lua_State *L)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lua_touserdata(L, 1);
    esp_lcd_panel_io_handle_t io = (esp_lcd_panel_io_handle_t)lua_touserdata(L, 2);
    int width = (int)luaL_checkinteger(L, 3);
    int height = (int)luaL_checkinteger(L, 4);
    int panel_if = lua_isnoneornil(L, 5) ? LUA_MODULE_LVGL_PANEL_IF_IO : (int)luaL_checkinteger(L, 5);
    int buffer_lines = LUA_MODULE_LVGL_DEFAULT_BUFFER_LINES;
    int tick_ms = LUA_MODULE_LVGL_DEFAULT_TICK_MS;
    int task_period_ms = LUA_MODULE_LVGL_DEFAULT_TASK_PERIOD_MS;
    size_t draw_buf_size;
    void *draw_buf = NULL;
    lv_display_t *display = NULL;
    esp_timer_handle_t tick_timer = NULL;
    display_arbiter_owner_t owner;
    esp_timer_create_args_t timer_args = {
        .callback = lua_lvgl_tick_timer_cb,
        .arg = &s_lvgl,
        .name = "lua_lvgl_tick",
    };
    esp_err_t err;

    luaL_argcheck(L, panel != NULL, 1, "panel_handle lightuserdata expected");
    if (panel_if == LUA_MODULE_LVGL_PANEL_IF_IO) {
        luaL_argcheck(L, io != NULL, 2, "io_handle lightuserdata expected for IO panels");
    }
    luaL_argcheck(L, width > 0 && height > 0, 3, "width and height must be positive");
    luaL_argcheck(L,
                  panel_if >= LUA_MODULE_LVGL_PANEL_IF_IO && panel_if <= LUA_MODULE_LVGL_PANEL_IF_MIPI_DSI,
                  5,
                  "panel_if must be 0, 1, or 2");

    if (lua_lvgl_opt_table(L, 6)) {
        buffer_lines = lua_lvgl_get_opt_int_field(L, 6, "buffer_lines", buffer_lines);
        tick_ms = lua_lvgl_get_opt_int_field(L, 6, "tick_ms", tick_ms);
        task_period_ms = lua_lvgl_get_opt_int_field(L, 6, "task_period_ms", task_period_ms);
    }
    luaL_argcheck(L, buffer_lines > 0 && buffer_lines <= height, 6, "buffer_lines must be in range 1..height");
    luaL_argcheck(L, tick_ms > 0, 6, "tick_ms must be positive");
    luaL_argcheck(L, task_period_ms > 0, 6, "task_period_ms must be positive");

    if (!s_lvgl.mutex) {
        s_lvgl.mutex = xSemaphoreCreateMutex();
        if (!s_lvgl.mutex) {
            return lua_lvgl_error_esp(L, "create mutex", ESP_ERR_NO_MEM);
        }
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (s_lvgl.runtime_initialized) {
        bool same_owner = s_lvgl.runtime_owner == L;

        lua_lvgl_unlock();
        return luaL_error(L,
                          same_owner ? "lvgl runtime is already initialized"
                                     : "lvgl runtime is already initialized by another Lua runtime");
    }
    lua_lvgl_unlock();

    owner = display_arbiter_get_owner();
    if (owner == DISPLAY_ARBITER_OWNER_LUA) {
        return luaL_error(L,
                          "display is already owned by Lua; deinit display/lvgl before lvgl.init");
    }
    if (owner != DISPLAY_ARBITER_OWNER_NONE && owner != DISPLAY_ARBITER_OWNER_EMOTE) {
        return luaL_error(L,
                          "display is already owned by %s",
                          lua_lvgl_display_owner_name(owner));
    }

    err = display_arbiter_acquire(DISPLAY_ARBITER_OWNER_LUA);
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "acquire display", err);
    }
    s_lvgl.display_owner_acquired = true;

    draw_buf_size = (size_t)width * (size_t)buffer_lines * sizeof(lv_color_t);
    draw_buf = heap_caps_malloc(draw_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!draw_buf) {
        draw_buf = heap_caps_malloc(draw_buf_size, MALLOC_CAP_8BIT);
    }
    if (!draw_buf) {
        (void)display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
        s_lvgl.display_owner_acquired = false;
        return luaL_error(L, "lvgl draw buffer allocation failed; reduce buffer_lines");
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        heap_caps_free(draw_buf);
        (void)display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
        s_lvgl.display_owner_acquired = false;
        return lua_lvgl_error_esp(L, "lock", err);
    }

    if (!s_lvgl.lvgl_initialized) {
        lv_init();
        s_lvgl.lvgl_initialized = true;
    }

    display = lv_display_create(width, height);
    if (!display) {
        lua_lvgl_unlock();
        heap_caps_free(draw_buf);
        (void)display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
        s_lvgl.display_owner_acquired = false;
        return luaL_error(L, "lvgl display create failed");
    }

    s_lvgl.panel = panel;
    s_lvgl.io = io;
    s_lvgl.width = width;
    s_lvgl.height = height;
    s_lvgl.panel_if = panel_if;
    s_lvgl.tick_ms = (uint32_t)tick_ms;
    s_lvgl.task_period_ms = (uint32_t)task_period_ms;
    s_lvgl.draw_buf = draw_buf;
    s_lvgl.draw_buf_size = draw_buf_size;
    s_lvgl.display = display;
    s_lvgl.runtime_initialized = true;
    s_lvgl.runtime_owner = L;
    s_lvgl.task_stop = false;

    lv_display_set_user_data(display, &s_lvgl);
    lv_display_set_buffers(display, draw_buf, NULL, (uint32_t)draw_buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, lua_lvgl_flush_cb);
    lua_lvgl_unlock();

    err = esp_timer_create(&timer_args, &tick_timer);
    if (err != ESP_OK) {
        (void)lua_lvgl_deinit_runtime();
        return lua_lvgl_error_esp(L, "create tick timer", err);
    }
    s_lvgl.tick_timer = tick_timer;
    err = esp_timer_start_periodic(tick_timer, (uint64_t)tick_ms * 1000ULL);
    if (err != ESP_OK) {
        (void)lua_lvgl_deinit_runtime();
        return lua_lvgl_error_esp(L, "start tick timer", err);
    }

    if (xTaskCreate(lua_lvgl_task,
                    "lua_lvgl",
                    LUA_MODULE_LVGL_TASK_STACK,
                    &s_lvgl,
                    LUA_MODULE_LVGL_TASK_PRIO,
                    &s_lvgl.task_handle) != pdPASS) {
        (void)lua_lvgl_deinit_runtime();
        return lua_lvgl_error_esp(L, "create task", ESP_ERR_NO_MEM);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_deinit(lua_State *L)
{
    esp_err_t err;

    if (s_lvgl.runtime_initialized && s_lvgl.runtime_owner && s_lvgl.runtime_owner != L) {
        return luaL_error(L, "lvgl runtime is owned by another Lua runtime");
    }

    err = lua_lvgl_deinit_runtime();

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "deinit", err);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_screen(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *screen;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (!s_lvgl.runtime_initialized) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl runtime is not initialized");
    }
    screen = lv_screen_active();
    if (!lua_lvgl_push_obj(L, screen, LUA_LVGL_OBJ_SCREEN, false)) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl object record allocation failed");
    }
    lua_lvgl_unlock();
    return 1;
}

static int lua_lvgl_create_screen(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *screen;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (!s_lvgl.runtime_initialized) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl runtime is not initialized");
    }
    screen = lv_obj_create(NULL);
    if (!lua_lvgl_push_obj(L, screen, LUA_LVGL_OBJ_SCREEN, true)) {
        lv_obj_delete(screen);
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl object record allocation failed");
    }
    lua_lvgl_unlock();
    return 1;
}

static int lua_lvgl_load(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *screen;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    screen = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!screen) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_screen_load(screen);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_create_widget(lua_State *L, lua_lvgl_obj_type_t type)
{
    lua_lvgl_obj_ud_t *parent_ud = lua_lvgl_check_ud(L, 1);
    lua_lvgl_opts_t opts;
    lv_align_t align;
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *parent;
    lv_obj_t *obj = NULL;
    const char *obj_error = NULL;

    lua_lvgl_parse_opts(L, 2, &opts);
    if (opts.align_value && lua_lvgl_parse_align(L, opts.align_value, &align) != ESP_OK) {
        return luaL_error(L, "lvgl align must be top_left, top_mid, top_right, bottom_left, bottom_mid, bottom_right, left_mid, right_mid, or center");
    }
    if ((type == LUA_LVGL_OBJ_BAR || type == LUA_LVGL_OBJ_SLIDER) && opts.max_value <= opts.min_value) {
        return luaL_error(L, "lvgl option 'max' must be greater than 'min'");
    }

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    parent = lua_lvgl_validate_ud_locked(parent_ud, NULL, &obj_error);
    if (!parent) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }

    switch (type) {
    case LUA_LVGL_OBJ_LABEL:
        obj = lv_label_create(parent);
        break;
    case LUA_LVGL_OBJ_BUTTON:
        obj = lv_button_create(parent);
        break;
    case LUA_LVGL_OBJ_BAR:
        obj = lv_bar_create(parent);
        break;
    case LUA_LVGL_OBJ_SLIDER:
        obj = lv_slider_create(parent);
        break;
    default:
        obj = lv_obj_create(parent);
        break;
    }
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl object create failed");
    }

    if (opts.has_opts) {
        lua_lvgl_apply_common_opts_locked(obj, &opts);
        if (type == LUA_LVGL_OBJ_LABEL && opts.text) {
            lv_label_set_text(obj, opts.text);
        } else if (type == LUA_LVGL_OBJ_BUTTON && opts.text) {
            lv_obj_t *label = lv_label_create(obj);
            lv_label_set_text(label, opts.text);
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        }
        if (type == LUA_LVGL_OBJ_BAR || type == LUA_LVGL_OBJ_SLIDER) {
            if (type == LUA_LVGL_OBJ_BAR) {
                lv_bar_set_range(obj, opts.min_value, opts.max_value);
                lv_bar_set_value(obj, opts.value, LV_ANIM_OFF);
            } else {
                lv_slider_set_range(obj, opts.min_value, opts.max_value);
                lv_slider_set_value(obj, opts.value, LV_ANIM_OFF);
            }
        }
    }

    if (!lua_lvgl_push_obj(L, obj, type, true)) {
        lv_obj_delete(obj);
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl object record allocation failed");
    }
    lua_lvgl_unlock();
    return 1;
}

static int lua_lvgl_label(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_LABEL);
}

static int lua_lvgl_button(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_BUTTON);
}

static int lua_lvgl_bar(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_BAR);
}

static int lua_lvgl_slider(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_SLIDER);
}

static int lua_lvgl_set_text(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *text = luaL_checkstring(L, 2);
    lua_lvgl_obj_type_t type;
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, &type, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    if (type != LUA_LVGL_OBJ_LABEL) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl set_text currently supports label objects only");
    }
    lv_label_set_text(obj, text);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_set_pos(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_obj_set_pos(obj, x, y);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_set_size(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    luaL_argcheck(L, w > 0 && h > 0, 2, "width and height must be positive");
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_obj_set_size(obj, w, h);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_align(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *align_value = luaL_checkstring(L, 2);
    int x = (int)luaL_optinteger(L, 3, 0);
    int y = (int)luaL_optinteger(L, 4, 0);
    lv_align_t align;
    esp_err_t err;
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (lua_lvgl_parse_align(L, align_value, &align) != ESP_OK) {
        return luaL_error(L, "lvgl align must be top_left, top_mid, top_right, bottom_left, bottom_mid, bottom_right, left_mid, right_mid, or center");
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_obj_align(obj, align, x, y);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_delete(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lua_lvgl_obj_record_t *record = ud->record;
    esp_err_t err = lua_lvgl_lock();

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (s_lvgl.runtime_initialized &&
            record &&
            record->generation == s_lvgl.generation &&
            record->valid &&
            record->obj &&
            lv_obj_is_valid(record->obj)) {
        lv_obj_delete(record->obj);
    }
    if (record) {
        record->obj = NULL;
        record->valid = false;
    }
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_clean(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_obj_clean(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_obj_gc(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = (lua_lvgl_obj_ud_t *)luaL_checkudata(L, 1, LUA_MODULE_LVGL_OBJ_METATABLE);

    ud->record = NULL;
    return 0;
}

static void lua_lvgl_state_cleanup(lua_State *L)
{
    esp_err_t err;

    if (!L) {
        return;
    }
    if (s_lvgl.runtime_initialized && s_lvgl.runtime_owner == L) {
        ESP_LOGI(TAG, "Lua runtime cleanup: deinitializing lvgl owned by exiting state");
        (void)lua_lvgl_deinit_runtime();
        return;
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Lua runtime cleanup: lock failed: %s", esp_err_to_name(err));
        return;
    }
    lua_lvgl_cleanup_state_objects_locked(L);
    lua_lvgl_unlock();
}

int luaopen_lvgl(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_LVGL_OBJ_METATABLE)) {
        lua_pushcfunction(L, lua_lvgl_obj_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);

    lua_newtable(L);

    lua_pushcfunction(L, lua_lvgl_init);
    lua_setfield(L, -2, "init");
    lua_pushcfunction(L, lua_lvgl_deinit);
    lua_setfield(L, -2, "deinit");
    lua_pushcfunction(L, lua_lvgl_screen);
    lua_setfield(L, -2, "screen");
    lua_pushcfunction(L, lua_lvgl_create_screen);
    lua_setfield(L, -2, "create_screen");
    lua_pushcfunction(L, lua_lvgl_load);
    lua_setfield(L, -2, "load");

    lua_pushcfunction(L, lua_lvgl_label);
    lua_setfield(L, -2, "label");
    lua_pushcfunction(L, lua_lvgl_button);
    lua_setfield(L, -2, "button");
    lua_pushcfunction(L, lua_lvgl_bar);
    lua_setfield(L, -2, "bar");
    lua_pushcfunction(L, lua_lvgl_slider);
    lua_setfield(L, -2, "slider");

    lua_pushcfunction(L, lua_lvgl_set_text);
    lua_setfield(L, -2, "set_text");
    lua_pushcfunction(L, lua_lvgl_set_pos);
    lua_setfield(L, -2, "set_pos");
    lua_pushcfunction(L, lua_lvgl_set_size);
    lua_setfield(L, -2, "set_size");
    lua_pushcfunction(L, lua_lvgl_align);
    lua_setfield(L, -2, "align");
    lua_pushcfunction(L, lua_lvgl_delete);
    lua_setfield(L, -2, "delete");
    lua_pushcfunction(L, lua_lvgl_clean);
    lua_setfield(L, -2, "clean");

    lua_pushinteger(L, LUA_MODULE_LVGL_PANEL_IF_IO);
    lua_setfield(L, -2, "PANEL_IF_IO");
    lua_pushinteger(L, LUA_MODULE_LVGL_PANEL_IF_RGB);
    lua_setfield(L, -2, "PANEL_IF_RGB");
    lua_pushinteger(L, LUA_MODULE_LVGL_PANEL_IF_MIPI_DSI);
    lua_setfield(L, -2, "PANEL_IF_MIPI_DSI");

    return 1;
}

esp_err_t lua_module_lvgl_register(void)
{
    esp_err_t err = cap_lua_register_module(LUA_MODULE_LVGL_NAME, luaopen_lvgl);

    if (err != ESP_OK) {
        return err;
    }
    return cap_lua_register_state_cleanup(lua_lvgl_state_cleanup);
}
