/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

static void lua_lvgl_register_funcs(lua_State *L, const luaL_Reg *funcs)
{
    for (; funcs && funcs->name; funcs++) {
        lua_pushcfunction(L, funcs->func);
        lua_setfield(L, -2, funcs->name);
    }
}

int luaopen_lvgl(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_LVGL_OBJ_METATABLE)) {
        lua_pushcfunction(L, lua_lvgl_obj_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);

    lua_newtable(L);

    lua_lvgl_register_funcs(L, lua_lvgl_runtime_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_core_widget_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_extra_widget_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_value_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_style_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_layout_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_object_funcs);

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
