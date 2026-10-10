<p align="right"><a href="route-display.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# CTRA route display

Titles show six UTF-8 characters, followed by `...` only for longer names. The 32-byte FAPR name ends on a complete UTF-8 character. The title font is documented in [assets](../../../assets/README.md); unsupported glyphs retain placeholders. BLE and GPS share one fixed line; battery percentage has its own label.

## Controls

- UP/DOWN click: browse; hold: zoom.
- OK click: switch between bottom roadbook/status and distance/elevation profile.
- OK hold: recenter and follow; double click: replay the last voice cue.
- The miniapp recenter command keeps its existing behavior.

Upload progress overrides the selected panel. No route shows the connection prompt. A profile can be viewed before GPS starts, but its position cursor needs a valid fix. Missing elevation shows `NO ELEVATION DATA`.

## Geometry and memory

Every original segment is clipped independently. Static I2 buffers use about 10.4 KB for the map and 2 KB for the profile, independent of route length. Keep `LV_BIN_DECODER_RAM_LOAD` disabled for line-based decoding in the 24 KB LVGL pool. Past segments are muted; upcoming segments take priority at overlaps. Exactly coincident geometry occupies the same pixels.

Both compilers retain original vertices as well as uniform samples to preserve short returns and bends. Files exceeding 20,000 retained points are rejected; simplify the source GPX first.

## Optional elevation (FAPR v1)

The 96-byte header and existing records remain unchanged. An optional block occupies the permitted gap after points and before `markers_offset`: four bytes `ELEV`, a little-endian uint32 equal to `point_count`, then one little-endian int32 per point in decimeters. `INT32_MIN` means missing; supported elevations are -12,000 to 10,000 meters. CRC, section offsets, total size and audio offsets include the block.

New readers accept old files. Old readers ignore the permitted gap. Re-import and sync the original GPX with `<ele>` to obtain a profile; old files cannot recover discarded elevations. No elevation means no extension.

## Checks

The gate covers geometry, format and compiler logic. `tests/test_route_ui_render.c` additionally exercises the pinned LVGL with a 24 KB pool on a desktop; an optional output directory exports PPM frames. Physical button timing, screen quality and target refresh performance require board testing.
