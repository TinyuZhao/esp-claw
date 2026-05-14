# Lua LVGL

This module exposes a small LVGL runtime to Lua scripts.

`lvgl` can:
- Initialize LVGL on an existing `esp_lcd` panel handle
- Create screens and basic widgets
- Update widget position, size, text, and alignment
- Deinitialize the LVGL runtime and release display ownership

## How to call

- Import it with `local lvgl = require("lvgl")`
- Get LCD parameters from `board_manager.get_display_lcd_params(...)` or `lcd.new(...)`
- Call `lvgl.init(panel_handle, io_handle, width, height, panel_if, options)`
- Create a screen with `lvgl.create_screen()` or use `lvgl.screen()`
- Create widgets with `lvgl.label(...)`, `lvgl.button(...)`, `lvgl.bar(...)`, or `lvgl.slider(...)`
- Call `lvgl.deinit()` when finished

## Important rules

- `lvgl` uses the same display ownership path as the `display` module.
- Do not use `display.init(...)` and `lvgl.init(...)` at the same time.
- The module owns a single LVGL display runtime at a time.
- The Lua script that successfully calls `lvgl.init(...)` owns the LVGL runtime.
- When that owner script exits, the module automatically deinitializes LVGL,
  stops the LVGL task and tick timer, frees the draw buffer, and releases
  display ownership. Calling `lvgl.deinit()` explicitly is still recommended
  when the script is done with the UI.
- `lvgl.init(...)` raises an error if LVGL is already initialized, including
  when another Lua runtime owns it.
- A non-owner Lua script cannot call `lvgl.deinit()` for a runtime created by
  another script.
- Lua object handles become invalid after `lvgl.deinit()` or after their parent object is deleted.
- Objects created by a non-owner script are cleaned up when that script exits.
  `lvgl.screen()` returns a borrowed handle and is not deleted by script cleanup.
- v1 does not support Lua event callbacks, image files, LVGL FS, custom fonts, or input devices.
- Chinese or other non-ASCII text may not render unless the firmware LVGL font configuration includes a matching font.

## Typical example

```lua
local board_manager = require("board_manager")
local lvgl = require("lvgl")
local delay = require("delay")

local panel_handle, io_handle, width, height, panel_if =
    board_manager.get_display_lcd_params("display_lcd")

lvgl.init(panel_handle, io_handle, width, height, panel_if, {
    buffer_lines = 40,
    tick_ms = 5,
    task_period_ms = 10,
})

local scr = lvgl.create_screen()
lvgl.label(scr, {
    text = "LVGL from Lua",
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
    y = -34,
    w = width - 40,
    h = 18,
    min = 0,
    max = 100,
    value = 65,
})

lvgl.load(scr)
delay.delay_ms(5000)
lvgl.deinit()
```

## API

### `lvgl.init(panel_handle, io_handle, width, height[, panel_if, options])`

Initializes LVGL for an existing LCD panel.

- `panel_handle`: lightuserdata for an `esp_lcd_panel_handle_t`
- `io_handle`: lightuserdata for an `esp_lcd_panel_io_handle_t`; required for IO panels
- `width`: display width
- `height`: display height
- `panel_if`: optional panel interface constant, default `lvgl.PANEL_IF_IO`
- `options`: optional table

Options:
- `buffer_lines`: number of lines in the partial draw buffer, default `40`
- `tick_ms`: LVGL tick interval, default `5`
- `task_period_ms`: LVGL handler task period, default `10`

Returns `true` on success. Raises a Lua error on failure.

### `lvgl.deinit()`

Stops the LVGL task and tick timer, deletes the LVGL display, frees the draw buffer, and releases display ownership.
Only the Lua script that initialized the runtime can deinitialize it explicitly.
The same cleanup is also performed automatically when that owner script exits.

Returns `true`.

### `lvgl.screen()`

Returns the current active screen object.

### `lvgl.create_screen()`

Creates and returns a new screen object.

### `lvgl.load(screen)`

Loads a screen object.

Returns `true`.

### `lvgl.label(parent, opts)`

Creates a label under `parent`.

Supported options:
- `text`
- `x`, `y`
- `w`, `h`
- `align`

### `lvgl.button(parent, opts)`

Creates a button under `parent`. If `opts.text` is set, the module creates a centered child label inside the button.

Supported options:
- `text`
- `x`, `y`
- `w`, `h`
- `align`

### `lvgl.bar(parent, opts)`

Creates a bar under `parent`.

Supported options:
- `x`, `y`
- `w`, `h`
- `align`
- `min`, `max`, `value`

### `lvgl.slider(parent, opts)`

Creates a slider under `parent`.

Supported options:
- `x`, `y`
- `w`, `h`
- `align`
- `min`, `max`, `value`

### `lvgl.set_text(obj, text)`

Sets text on a label object.

Returns `true`.

### `lvgl.set_pos(obj, x, y)`

Sets object position.

Returns `true`.

### `lvgl.set_size(obj, w, h)`

Sets object size.

Returns `true`.

### `lvgl.align(obj, align[, x, y])`

Aligns an object.

Supported align strings:
- `"top_left"`
- `"top_mid"` or `"top"`
- `"top_right"`
- `"bottom_left"`
- `"bottom_mid"` or `"bottom"`
- `"bottom_right"`
- `"left_mid"` or `"left"`
- `"right_mid"` or `"right"`
- `"center"` or `"centre"`

Returns `true`.

### `lvgl.delete(obj)`

Deletes an LVGL object and invalidates the Lua handle.

Returns `true`.

### `lvgl.clean(obj)`

Deletes all children of an LVGL object.

Returns `true`.
