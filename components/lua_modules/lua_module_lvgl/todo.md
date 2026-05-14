# lua_module_lvgl TODO

This file tracks what is still missing from the current `lua_module_lvgl`
wrapper. The current implementation is a minimal v1 runtime: it can initialize
LVGL on an existing LCD panel, create a few basic widgets, update basic layout
properties, and deinitialize the runtime.

## P0: correctness and lifecycle gaps

Status: implemented for v1. The module now has state-aware Lua cleanup,
runtime ownership by the script that calls `lvgl.init()`, best-effort per-state
object cleanup, LVGL delete-event invalidation for stale userdata, task-stop
notification, and clearer display ownership errors.

- Follow-up: add dedicated hardware/lifecycle regression scripts.
  - Owner script exits without explicit `lvgl.deinit()`.
  - Non-owner script creates objects and exits while owner script remains alive.
  - `delete(parent)` and `clean(parent)` followed by child handle access.
  - Non-owner `lvgl.deinit()` error path.

- Follow-up: decide whether object records should be aggressively reclaimed.
  - Records are kept independent from Lua userdata so LVGL delete callbacks do
    not reference freed Lua memory.
  - Current cleanup favors avoiding use-after-free over reclaiming every record
    immediately.

## P1: v1 polish APIs and widget coverage

Status: implemented for v1.

- Implemented basic getters and validity checks.
  - `get_pos(obj) -> x, y`
  - `get_size(obj) -> w, h`
  - `get_value(obj) -> value`
  - `is_valid(obj) -> boolean`

- Implemented value and range setters.
  - `set_value(obj, value[, anim])`
  - `set_range(obj, min, max)`
  - Supports bar, slider, arc, scale, dropdown, roller, checkbox, and switch
    where applicable.

- Expanded text support.
  - `set_text()` supports label, button, checkbox, dropdown, textarea, and list
    text/button handles.
  - Button records keep the internal label pointer so button text can be updated
    later.

- Added generic object/container constructors and constrained style helpers.
  - `object(parent, opts)` and `container(parent, opts)`
  - `set_style(obj, opts)` plus style fields in creation opts.
  - Style remains selector `0` only; advanced LVGL parts/states are still out of
    scope.

- Added P1 widget constructors.
  - `image`, `line`, `arc`, `spinner`, `scale`, `checkbox`, `switch`,
    `dropdown`, `roller`, `keyboard`, `list`, `textarea`, and `table`.
  - `line` stores C-owned point buffers.
  - `image` only forwards string `src`; FS/decoder setup remains P2.

- Added layout primitives.
  - `set_flex`
  - `set_grid`
  - `set_grid_cell`
  - `set_scroll`
  - Grid descriptor buffers are C-owned and replaced safely by subsequent
    `set_grid()` calls.

- Follow-up: add parent/child ordering helpers such as move foreground/background.

## P2: events and input devices

- Design Lua event callbacks.
  - v1 intentionally does not expose Lua closure callbacks.
  - Need a safe model for callback lifetime, Lua runtime ownership, async job
    shutdown, and dispatch from the LVGL task context.

- Add basic event API after callback ownership is solved.
  - Click/change/value_changed/focused/pressed/released.
  - Event userdata should avoid exposing raw LVGL internals.

- Bind input devices.
  - Touch panel pointer input.
  - Encoder/keypad input.
  - Focus group support for non-touch devices.

## P2: rendering and display support

- Validate panel interface variants.
  - Constants exist for IO/RGB/MIPI DSI, but the flush path currently always
    calls `esp_lcd_panel_draw_bitmap()`.
  - Confirm this is correct for every board target, or split per-interface
    behavior.

- Add rotation and display configuration.
  - Rotation/mirroring/swap XY.
  - Invalidate/full refresh helpers.
  - Optional double buffering if memory allows.

- Revisit draw buffer allocation.
  - Current code allocates one stripe buffer with SPIRAM fallback to internal
    RAM.
  - Add capability selection for DMA-capable memory if required by specific
    panels.
  - Add better diagnostics for requested buffer size and available heap.

## P2: assets, fonts, and filesystem

- Add font configuration strategy.
  - Current module relies on whatever LVGL fonts are compiled into firmware.
  - Chinese and other non-ASCII text are not guaranteed to render.
  - Decide whether to expose a small set of compiled fonts or allow app-specific
    font registration.

- Add image support.
  - Decide source model: C array assets, FATFS paths, HTTP/downloaded files, or
    in-memory buffers.
  - Decide whether to enable LVGL FS and PNG/JPEG decoders.

- Add theme support.
  - Basic light/dark theme selection.
  - Default style policy for created widgets.

## P3: API ergonomics

- Add object methods or syntactic sugar.
  - Current API is function-based: `lvgl.set_pos(obj, x, y)`.
  - Method-style syntax would require setting `__index` on userdata, e.g.
    `obj:set_pos(x, y)`.

- Add constants/tables for alignments and flags.
  - Current API uses strings for alignment.
  - Constants reduce spelling errors but make the Lua API more verbose.

- Add batch update helpers.
  - `lvgl.update(obj, opts)` for position/size/text/value/style in one call.
  - `lvgl.with_lock(function() ... end)` is probably not safe until callback and
    Lua runtime ownership are designed.

## P3: tests and validation

- Add negative Lua tests.
  - Missing panel handle.
  - Invalid parent.
  - Invalid align string.
  - Use after `delete`.
  - Use after `deinit`.
  - Double `init`.

- Add lifecycle tests on hardware.
  - Run `test/lvgl_basic.lua` repeatedly.
  - Trigger a Lua error after `lvgl.init()` and verify cleanup releases display
    ownership.
  - Run conflict tests with the existing `display` module.

- Add memory pressure tests.
  - Large `buffer_lines`.
  - Internal RAM fallback.
  - Multiple init/deinit cycles.

- Add documentation examples.
  - Minimal label-only screen.
  - Dynamic slider/bar update loop.
  - Display ownership conflict and cleanup example.

## Explicitly out of scope for the current wrapper

- Full mechanical binding of `lvgl.h`.
- Arbitrary LVGL style graph exposure.
- Lua callbacks before runtime ownership is solved.
- Complex animation/timeline support.
- Full image codec and filesystem integration.
- Complete international text rendering without an agreed font strategy.
