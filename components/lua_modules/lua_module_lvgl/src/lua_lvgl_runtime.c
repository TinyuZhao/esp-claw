/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

static const char *TAG = "lua_lvgl";
lua_lvgl_state_t s_lvgl;

esp_err_t lua_lvgl_lock(void)
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

void lua_lvgl_unlock(void)
{
    if (s_lvgl.mutex) {
        xSemaphoreGive(s_lvgl.mutex);
    }
}

int lua_lvgl_error_esp(lua_State *L, const char *what, esp_err_t err)
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

esp_err_t lua_lvgl_deinit_runtime(void)
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
void lua_lvgl_state_cleanup(lua_State *L)
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

const luaL_Reg lua_lvgl_runtime_funcs[] = {
    {"init", lua_lvgl_init},
    {"deinit", lua_lvgl_deinit},
    {NULL, NULL},
};
