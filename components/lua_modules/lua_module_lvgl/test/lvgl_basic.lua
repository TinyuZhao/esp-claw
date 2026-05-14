local board_manager = require("board_manager")
local delay = require("delay")
local lvgl = require("lvgl")

local panel_handle, io_handle, width, height, panel_if =
    board_manager.get_display_lcd_params("display_lcd")

lvgl.init(panel_handle, io_handle, width, height, panel_if, {
    buffer_lines = 40,
    tick_ms = 5,
    task_period_ms = 10,
})

local ok, err = pcall(function()
    local scr = lvgl.create_screen()

    lvgl.label(scr, {
        text = "LVGL Lua",
        align = "top_mid",
        x = 0,
        y = 20,
    })

    lvgl.button(scr, {
        text = "OK",
        align = "center",
        w = 120,
        h = 44,
    })

    lvgl.bar(scr, {
        align = "bottom_mid",
        x = 0,
        y = -48,
        w = width - 40,
        h = 18,
        min = 0,
        max = 100,
        value = 65,
    })

    lvgl.slider(scr, {
        align = "bottom_mid",
        x = 0,
        y = -20,
        w = width - 40,
        h = 18,
        min = 0,
        max = 100,
        value = 35,
    })

    lvgl.load(scr)
    delay.delay_ms(5000)
end)

if not ok then
    error(err)
end
