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
    lvgl.set_style(scr, {
        bg_color = "#101820",
    })

    lvgl.label(scr, {
        text = "LVGL Lua",
        align = "top_mid",
        x = 0,
        y = 20,
        text_color = "#f5f7fa",
    })

    local panel = lvgl.container(scr, {
        align = "center",
        w = width - 40,
        h = 150,
        bg_color = "#ffffff",
        bg_opa = 32,
        border_color = "#4f8cff",
        border_width = 1,
        radius = 8,
        pad = 10,
        pad_row = 8,
    })
    lvgl.set_flex(panel, {
        flow = "column",
        main = "center",
        cross = "center",
        track = "center",
    })

    local button = lvgl.button(panel, {
        text = "OK",
        w = 120,
        h = 44,
        radius = 6,
        bg_color = "#2f80ed",
        text_color = "#ffffff",
    })
    lvgl.set_text(button, "Updated")

    local checkbox = lvgl.checkbox(panel, {
        text = "checked",
        checked = true,
        text_color = "#f5f7fa",
    })

    local bar = lvgl.bar(scr, {
        align = "bottom_mid",
        x = 0,
        y = -54,
        w = width - 40,
        h = 18,
        min = 0,
        max = 100,
        value = 65,
        bg_color = "#263241",
    })
    lvgl.set_value(bar, 72)

    local slider = lvgl.slider(scr, {
        align = "bottom_mid",
        x = 0,
        y = -24,
        w = width - 40,
        h = 18,
        min = 0,
        max = 100,
        value = 35,
    })
    lvgl.set_range(slider, 0, 120)
    lvgl.set_value(slider, 48)

    local value = lvgl.get_value(slider)
    lvgl.set_text(checkbox, "slider " .. value)

    lvgl.load(scr)
    delay.delay_ms(5000)
end)

if not ok then
    error(err)
end
