/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

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
    LUA_LVGL_OBJ_CONTAINER,
    LUA_LVGL_OBJ_LABEL,
    LUA_LVGL_OBJ_BUTTON,
    LUA_LVGL_OBJ_BAR,
    LUA_LVGL_OBJ_SLIDER,
    LUA_LVGL_OBJ_IMAGE,
    LUA_LVGL_OBJ_LINE,
    LUA_LVGL_OBJ_ARC,
    LUA_LVGL_OBJ_SPINNER,
    LUA_LVGL_OBJ_SCALE,
    LUA_LVGL_OBJ_CHECKBOX,
    LUA_LVGL_OBJ_SWITCH,
    LUA_LVGL_OBJ_DROPDOWN,
    LUA_LVGL_OBJ_ROLLER,
    LUA_LVGL_OBJ_KEYBOARD,
    LUA_LVGL_OBJ_LIST,
    LUA_LVGL_OBJ_LIST_TEXT,
    LUA_LVGL_OBJ_LIST_BUTTON,
    LUA_LVGL_OBJ_TEXTAREA,
    LUA_LVGL_OBJ_TABLE,
} lua_lvgl_obj_type_t;

typedef struct lua_lvgl_obj_record {
    lv_obj_t *obj;
    lv_obj_t *aux_obj;
    lv_point_precise_t *line_points;
    uint32_t line_point_count;
    int32_t *grid_cols;
    int32_t *grid_rows;
    int value_cache;
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

extern lua_lvgl_state_t s_lvgl;

esp_err_t lua_lvgl_lock(void);
void lua_lvgl_unlock(void);
int lua_lvgl_error_esp(lua_State *L, const char *what, esp_err_t err);

lua_lvgl_obj_ud_t *lua_lvgl_check_ud(lua_State *L, int index);
void lua_lvgl_record_release_resources(lua_lvgl_obj_record_t *record);
lua_lvgl_obj_ud_t *lua_lvgl_push_obj(lua_State *L, lv_obj_t *obj, lua_lvgl_obj_type_t type, bool owned);
lv_obj_t *lua_lvgl_validate_ud_locked(const lua_lvgl_obj_ud_t *ud,
                                      lua_lvgl_obj_type_t *out_type,
                                      const char **out_error);
void lua_lvgl_cleanup_state_objects_locked(lua_State *L);
void lua_lvgl_invalidate_records_locked(void);
int lua_lvgl_obj_gc(lua_State *L);

int lua_lvgl_get_opt_int_field(lua_State *L, int index, const char *field, int default_value);
const char *lua_lvgl_get_opt_string_field(lua_State *L, int index, const char *field);
bool lua_lvgl_get_opt_bool_field(lua_State *L, int index, const char *field, bool default_value);
bool lua_lvgl_has_field(lua_State *L, int index, const char *field);
bool lua_lvgl_opt_table(lua_State *L, int index);
esp_err_t lua_lvgl_parse_color(lua_State *L, int index, lv_color_t *out_color);
esp_err_t lua_lvgl_parse_align(lua_State *L, const char *value, lv_align_t *out_align);
esp_err_t lua_lvgl_parse_flex_flow(const char *value, lv_flex_flow_t *out_flow);
esp_err_t lua_lvgl_parse_flex_align(const char *value, lv_flex_align_t *out_align);
esp_err_t lua_lvgl_parse_grid_align(const char *value, lv_grid_align_t *out_align);
esp_err_t lua_lvgl_parse_dir(const char *value, lv_dir_t *out_dir);
esp_err_t lua_lvgl_parse_scrollbar(const char *value, lv_scrollbar_mode_t *out_mode);
esp_err_t lua_lvgl_parse_scroll_snap(const char *value, lv_scroll_snap_t *out_snap);
esp_err_t lua_lvgl_parse_arc_mode(const char *value, lv_arc_mode_t *out_mode);
esp_err_t lua_lvgl_parse_scale_mode(const char *value, lv_scale_mode_t *out_mode);
esp_err_t lua_lvgl_parse_keyboard_mode(const char *value, lv_keyboard_mode_t *out_mode);
esp_err_t lua_lvgl_parse_roller_mode(const char *value, lv_roller_mode_t *out_mode);
void lua_lvgl_parse_opts(lua_State *L, int index, lua_lvgl_opts_t *opts);
void lua_lvgl_apply_common_opts_locked(lv_obj_t *obj, const lua_lvgl_opts_t *opts);
char *lua_lvgl_build_options_string(lua_State *L, int index, const char *field);
int32_t *lua_lvgl_build_grid_tracks(lua_State *L, int opts_index, const char *field);
lv_point_precise_t *lua_lvgl_build_line_points(lua_State *L, int opts_index, uint32_t *out_count);

void lua_lvgl_apply_style_opts_locked(lua_State *L, int index, lv_obj_t *obj);

esp_err_t lua_lvgl_deinit_runtime(void);
void lua_lvgl_state_cleanup(lua_State *L);
int lua_lvgl_create_widget(lua_State *L, lua_lvgl_obj_type_t type);

extern const luaL_Reg lua_lvgl_runtime_funcs[];
extern const luaL_Reg lua_lvgl_core_widget_funcs[];
extern const luaL_Reg lua_lvgl_extra_widget_funcs[];
extern const luaL_Reg lua_lvgl_value_funcs[];
extern const luaL_Reg lua_lvgl_style_funcs[];
extern const luaL_Reg lua_lvgl_layout_funcs[];
extern const luaL_Reg lua_lvgl_object_funcs[];
