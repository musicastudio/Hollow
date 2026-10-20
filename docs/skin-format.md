# Hollow skin format (version 1)

A skin is a folder. The runtime (`framework/src/core`) and the web editor (`editor/`) both read and write exactly this format, so this file is the contract between them. Anything not described here does not exist.

```
skin/
  skin.json            name, root view, image metadata
  params.json          host parameters (array)
  views/<view>.json    one file per view
  images/<group>/<name>.png
  fonts/<font>.png     glyph strips, or the TrueType files skin.json "fonts" names
  scales/<s>/<group>/<name>.png   the art rendered for another of the skin's "scales", optional
  scales/<s>/fonts/<font>.png     a glyph strip drawn for another scale, optional
  data/<name>.json     tables custom kinds read (waveforms, presets), optional
```

All images are 8-bit RGBA PNG with straight (not premultiplied) alpha. Names are lowercase `snake_case`; image names are paths relative to `images/` without the extension (`knobs/rotary_large`), font names are file names in `fonts/` without the extension (`caption_grey`). Coordinates are integer pixels at scale 1, origin top-left, y down. Colours are CSS-style hex strings `#rrggbb` or `#rrggbbaa`.

## skin.json

```json
{
  "format": 1,
  "name": "My Synth",
  "root": "main",
  "images": {
    "knobs/rotary_large": { "tiles": 128 },
    "switches/led_toggle": { "tiles": 4, "axis": "x" },
    "panels/group_box":    { "slice": [17, 4, 4, 69] }
  }
}
```

- `root`: the view the editor window shows; its size is the window size at scale 1.
- `vars` (optional): variables visible to every view (see Variables), e.g. `{ "part": "p1" }`. Buttons change them with a `set` action.
- `density` (default 1): the skin's pixels per unit of the runtime's own geometry. Everything the skin states (rects, pads, slices, fonts, artwork) is in the skin's own pixels; `density` scales what the runtime supplies itself: the drawings of the custom kinds (the FM matrix wires, envelope and key scaling editors, pad handles, the piano's key-art offsets, scope line widths), menu and tooltip chrome (borders, shadows, check marks, arrows, the tooltip's offset), the defaults of pixel fields (menu `rowHeight`, `separatorHeight` and `pad`, tooltip `pad`, scroll `width`, morph `handle`, envelope `handleOffset`, key scaling `scaleRect`), and drag rates, which stay per design pixel so a denser skin drags at the same speed on screen. Built-in sizes round half away from zero and lines are `round(density)` px wide.
- `scales` (optional): the window scales the skin is drawn at, each at its own resolution (see Scale), e.g. `[0.5, 0.75, 1, 1.5, 2]`.
- `fonts` (optional): the TrueType fonts by name (see Fonts), and any font's text entry colours.
- `colours` (optional): a palette, `{ "op1": "#406bb3ff", "op1_line": "#6e9eeeff", ... }`. Any string `"@op1"` in a view is that colour, and `"@op1/40"` the same with alpha `40` (two hex digits) in place of its own, so one palette serves a fill, a curve and its translucent area. A reference holding a `{var}` is resolved where it is drawn, once the var has a value: `"@op{op}"` in a custom kind's colour field follows the operator a view shows. The `matrix_wires` box and the `fseq` track colours also take `{n}`, the box's or the track's number (1..8): `"@op{n}"`.
- `images`: metadata only for images that need it. An image without an entry is one tile, no slicing.
  - `tiles` (default 1): the image is a strip of equal tiles (animation or state frames).
  - `axis` (default `"y"`): `"y"` = tiles stacked top to bottom, `"x"` = side by side. Tile `i` of `n` in a `w x h` image is `(0, i*h/n, w, h/n)` for `"y"` and `(i*w/n, 0, w/n, h)` for `"x"`. A requested tile index is clamped to `0..n-1`.
  - `slice` `[top, right, bottom, left]`: nine-slice bands in pixels. When an image with `slice` is drawn into a rect of a different size than its tile, the four corners are copied 1:1, and the edges and the centre are filled by repeating their band from its top-left (tiled, not scaled). A band of 0 means that axis scales as a whole. Without `slice` an image is always drawn 1:1 at the rect's top-left and clipped to the rect.
  - `passes` (default 1): how many times the image is composited, for art meant to be blended more than once.
  - `surface` (default true): false keeps the skin's `surface` off this image.
- `steppers` (optional): `{ "width": 9, "plates": { "<plate image>": ["<down image>", "<up image>"], ... } }`. Every `number` drawn on one of these plates (its `fill` image) and taking input gets a down button at its left end and an up button at its right, each `width` px wide, the images centred in them: two tiles, at rest and held. A press steps the value once, and again every 100 ms after 400 ms held: one step of a stepped param, a hundredth of a continuous one, within the widget's `range` when it has one. The text keeps clear of them. A number with `"stepper": false` has none, nor does one too narrow for both.
- `surface` (optional): a texture laid over the light, grey pixels of the skin's artwork and colour fills: every widget and view fill, dial, meter and slider image, and scrollbar part. Text, menus, tooltips and the custom kinds' own drawings are never touched. `{ "image": "surfaces/ice_metal", "lightness": [0.70, 0.86], "chroma": [0.06, 0.14], "strength": 1 }`:
  - The texture is tiled from the window's top-left in window pixels, so a texture as large as the root view lies across the whole window without a seam, whatever image a pixel comes from.
  - A pixel of lightness `l` (0..255, `(77 r + 150 g + 29 b) >> 8`) becomes the texture's colour at its window point times `l / 255`, so white turns into the texture and a bevel's shading stays.
  - How much of that it takes is `weight[l] * chroma[max - min] >> 8`. `lightness` is where the weight starts and where it is full, and `chroma` is where it starts to fade for colourful pixels and where it is gone, both smoothstepped over 0..1. `strength` scales the whole.
  - A reflecting surface, chrome, adds `"normals": "<image>"` and `"relief": [art, sheet]`. The texture is then an environment the surface mirrors, and a pixel looks it up where its slope points instead of where it is:
    - its image's own slope times `art` pixels, where a full slope is 127. The runtime works that slope out once per image and tile, as the Sobel gradient of lightness times opacity, lighter below reading as tilted up; colour fills have none.
    - plus the sheet's slope from `normals` (R and G around 128, in window pixels) times `sheet` pixels.
    - So flat panels mirror the environment continuously, bevels bend the reflection sharply, and dents in the sheet ripple it.
  - A see-through display (a grid drawn over the page) would still show the surface behind it, so give each display screen a plain plate with `"surface": false` behind it and turn the surface off on the display itself.

Compositing uses an integer blend: images and glyphs `(s*a + d*(255-a)) >> 8`, colour fills `(s*a + d*(256-a)) >> 8`, per channel, writing the source alpha.

## params.json

```json
[
  { "id": "arp.enabled", "name": "Arp On", "min": 0, "max": 1, "default": 0, "steps": 1, "labels": ["Off", "On"] },
  { "id": "arp.tempo", "name": "Arp Tempo", "min": 20, "max": 300, "default": 120, "unit": "bpm", "format": "%3.0f" }
]
```

- `id` (required, unique): dotted lowercase path. Widgets bind to it. The host sees `hostId = fnv1a32(id) & 0x7fffffff`, so reordering the file never breaks saved sessions. Formats that only know indices (VST2) use array order, so append new params at the end.
- `name` (required): shown in the host.
- `min`, `max`, `default`: plain values, defaults 0, 1, `min`.
- `steps`: 0 = continuous; `n` = `n + 1` discrete values from `min` to `max`.
- `labels`: optional text per discrete value.
- `taper`: `"linear"` (default) or `"log"`. With `"log"` (both `min` and `max` > 0) the normalized value `n` maps to `min * (max / min) ^ n`, for frequency and time sweeps. Widgets and hosts move in normalized steps, so a log param sweeps evenly per octave.
- `unit`, `format` (printf, one `%` conversion for a double): optional, for display. `format` covers the number only; hosts get the unit after it (`"200 ms"`), or as a separate label where the format has one (VST2). A widget's own `format` may add text of its own (`"%.0f ms"`) for the GUI. Default format `%.2f`, or the label, or an integer for stepped params.

## views/&lt;view&gt;.json

```json
{
  "size": [823, 357],
  "fill": { "image": "backgrounds/arp" },
  "widgets": [ ... ]
}
```

The view's name is its file name. `fill` paints the background (see Fill). Widgets draw in ascending `layer` (0..7, default 3), and in file order within a layer; hit testing runs the other way (topmost first).

### Common widget fields

| Field | Meaning |
|---|---|
| `type` | one of the types below (required) |
| `name` | unique within the view, `snake_case` (required) |
| `rect` | `[x, y, w, h]` in the view (required) |
| `layer` | 0..7, default 3 |
| `hidden` | start invisible (default false) |
| `surface` | false keeps the skin's `surface` off everything this widget draws, embedded views included (default true) |
| `disabled` | draws the disabled tile, ignores input (default false) |
| `onIf` | buttons: a condition (see Conditions) that draws the button lit while it holds, in place of what its action would say; a tab that stands for several pages. Plates: draws the fill's tile 1 and the text's `onFont` while it holds (an icon over a button that lights with it) |
| `param` | parameter id this widget shows and edits; may contain `{var}` placeholders (see Variables) |
| `tip` | tooltip text (optional) |

A widget with no `param` keeps a local value (starting at `value`, default 0), so a skin works before any parameter exists.

### Fill

`{ "colour": "#b0b0b0ff" }` fills the rect. `{ "image": "panels/group_box" }` draws the image, with the tile chosen by the widget (buttons: state tile, others: tile 0, or `"tile": n` to force one). A missing `fill` draws nothing.

`"tint": "#406bb3ff"` beside an image colours it: each channel is multiplied by the tint's over the mean lightness of the image's tile 0, so that tile takes the tint's colour where it is as light as on average, its shading and other tiles lighter and darker in proportion (a grey button becomes a coloured one whose lit and hover tiles still read as such). Alpha is the art's. A palette tint may name a `{var}` (`"@op{op}"`), and a `number`'s may name `{value}`, the text the box shows, so a box that picks an operator takes that operator's colour (`"@op{value}"`); both resolve where the widget is drawn.

### Text

```json
"text": { "text": "Rate", "font": "caption_grey", "align": "center", "valign": "middle" }
```

`align` is `left` (default), `center` or `right`; `valign` is `top` (default), `middle` or `bottom`. The literal two characters `\n` in `text` break the line. Text is laid out inside the widget rect shrunk by `pad` `[left, top, right, bottom]` (default 0s).

`"onFont": "<font>"` is the font a button or dropdown draws its text in while it draws lit (any value tile above the first: a radio button's choice, a toggle that is on, a navigation button on its page), so a lit tile darker than the rest can carry white text. A plate uses it while its `onIf` holds.

### Types

| Type | Draws | Input | Fields |
|---|---|---|---|
| `plate` | `fill`, then `text` | none | `fill`, `text`, `pad` |
| `button` | `fill.image` tile for the current state (below), then `text` | click | `fill`, `text`, `pad`, `toggle`, `hoverTiles`, `pressedTiles`, `disabledTile`, `press`, `pressOffset`, `action` |
| `dropdown` | as `button` | click opens a native menu; choosing an item sets the value | button fields plus `items`, `captionFromItem` |
| `dial` | `fill`, then tile `round(norm * (tiles-1))` of `image` | drag, double-click resets, wheel | `image`, `fill`, `drag`, `vertical`, `sensitivity`, `fine`, `margin`, `range`, `steps`, `default` |
| `number` | `fill`, then the formatted value as `text` | vertical drag, double-click resets | `fill`, `text` (font and alignment; its `text` is ignored), `pad`, `format`, `range`, `sensitivity` |
| `textbox` | `fill`, then `text` (multi-line if `multiline`) | none in version 1 | `fill`, `text`, `pad`, `multiline` |
| `meter` | tile `round(norm * (tiles-1))` of `image` | none | `image`, `range` |
| `plot` | `fill`, then a trace in `line` colour (a flat line when idle) | none | `fill`, `line`, `line2`, `source` |
| `veil` | `fill` | swallows clicks | `fill` |
| `list` | `fill`, `header` text, empty rows | none in version 1 | `fill`, `header`, `rowHeight`, `row` (text style) |
| `tree` | `fill` | none in version 1 | `fill`, `row` |
| `embed` | another view at `rect` (clipped) | forwarded | `view`, `vars` |
| `custom` | `fill` plus a kind-specific overlay | kind-specific | `kind`, `fill`, `params` |

Button state tile: the strip is laid out as groups `[normal][pressed, if pressedTiles][hover, if hoverTiles]` followed by one disabled tile if `disabledTile`. Each group holds `states = (tiles - disabledTile) / (1 + hoverTiles + pressedTiles)` tiles, one per value (so a toggle's groups hold 2). The tile drawn is `min(value, states - 1) + states * group`, where `group` is 1 while pressed (if `pressedTiles`), else `1 + pressedTiles` while hovered (if `hoverTiles`), else 0; or the last tile when disabled and `disabledTile`. The pressed tile shows while the button is held, even if the pointer leaves it. `pressOffset` applies only while pressed with `pressedTiles`.

`toggle`: each act flips the value between 0 and 1, or steps to the next value (wrapping) for a param with more than two values. A button without `toggle` just acts (runs its `action`, and a bound param briefly reads 1 for `momentary`/`repeat`).

`press` says when a button acts: `"down"` (default: acts and toggles on mouse down), `"momentary"` (acts on down and again on release, value returns to 0 on release), `"repeat"` (like momentary and repeats every 100 ms while held after a 400 ms delay), `"up"` (standard push button: acts once on release inside). `pressOffset` `[dx, dy]` shifts the text while pressed.

`action` (buttons only, optional; runs when the button acts):
- `{ "goto": "page_arp", "stack": "pages", "vars": { "op": "b" } }`: set the embed named `stack` (searched from the root view down) to show view `goto` with those `vars`. A button with `goto` draws as on (value 1) while that embed shows that view and every var the button lists has that value (vars it does not list are ignored), and off otherwise, so navigation buttons light up by themselves. `"set": { "op": "3" }` beside it sets skin-wide vars first, as a `set` action would, and the button is on only while they hold those values too: one button that picks something every view shares and opens its page.
- `{ "set": { "timbre": "br" } }`: set skin-wide `vars`. The button draws as on while every listed var has that value.
- `{ "toggle": "about_box" }`: flip `hidden` on the widget with that name (searched in the same view first, then from the root).
- `{ "url": "https://example.com" }`: open in the browser.
- `{ "standalone": "settings" }`: open the standalone app's own audio and MIDI settings window, or with skin.json `"standalone": { "settings": "<view>" }` that view as a modal, filled from the app's setup (see Modals); where that view already shows in a page or as the modal, it opens the app's window, which has the driver's own control panel where the platform has one (an ASIO driver's, on Windows). A menu item with it is listed only in the standalone, so a File menu can carry it in every format. `{ "standalone": "close" }` closes the standalone without asking.
- `{ "modal": "<view>", "data": { "<key>": "<text>" } }`: store the text data first (keys may use `{var}`), then show that view as the modal; `"modal": ""` closes it. See Modals.
- `{ "scale": "menu" }`: open the window scale menu under the widget: the skin's `"scales"`, else 1x to 4x. A right-click on empty space opens nothing, so a skin that wants the scales offers them with this. `{ "scale": 1.5 }` sets that scale (the nearest the skin has) and draws the button as on while it is the window's scale, a radio button per scale.
- `{ "sequence": "insert", "prefix": "arp.step.", "index": "{step}", "count": 32 }` (or `"delete"`): treats params named `<prefix><n>.<leaf>` for n = 1..count as a step sequence. Insert moves steps index..count-1 one place up (the last is dropped) and resets step index to its defaults; delete moves steps index+1..count one place down and resets step count. `index` may use `{var}`.
- `{ "sequence": "reset" | "random", "prefix": "arp.step.", "leaf": "on", "count": 32 }`: sets `<prefix><n>.<leaf>` for n = 1..count to its default, or to random valid values (for a step sequencer's rows).
- `{ "presets": "<table>", "key": "env.{op}", "name": "<text key>", "save": true, "columns": 16 }`: a native menu of the preset slots of `data/<table>.json` (kept, once changed, in text data `presets.<table>`), `columns` items per column. Choosing a slot copies its content into text data `key` and its name into text data `name`; with `save` the menu also offers storing the current content and name into a slot.
- `{ "data": { "keyscale.z": "0 -60 0.5|127 67 0.5" } }`: store these strings as text data (keys may use `{var}`), for buttons that restore a shape.
- `{ "midi_map": "remove" }` (or `"reset"`): for the context menu of a `midi_map` list, drop the selected row's assignment (a fixed row's CC goes back to its default), or drop them all and restore `learn.defaults`.
- `{ "file": "open" | "save", "key": "<text key>", "types": { "SysEx": "syx", "Audio": "wav;mp3" }, "title": "...", "name": "<a save's file name>" }`: a native open or save dialog (`name` may hold `{data:<key>|<default>}`, so a processor can start a save in a folder of its own: `"{data:library.dir|}Performance.syx"`); the chosen file's path (UTF-8) is stored as text data under `key` (with `{var}`s), for the product's processor to act on and clear. Cancelling changes nothing.
- `{ "value": 2 }`: set the bound param to this plain value, a radio button: it draws as on while the param holds that value. With a `goto`, `stack` and optional `vars` as well, it then shows that view there, so a chooser closes itself on a pick.
- `{ "step": 0.01 }`: add this amount to the bound param's plain value (clamped to its range). With `"press": "repeat"` it nudges while held. A `step` button's own tile does not follow the param.

`items` (dropdown): `[ { "label": "Sine", "value": 0 }, { "separator": true }, ... ]`. Choosing an item sets the value to its `value`. With `captionFromItem` the button text shows the chosen label.

`dial` with `"slider": true` draws tile 0 of `image` as a handle moved along the axis instead of picking a tile: vertical `y = top + (h - th) - trunc(norm * (h - th - 1))`, horizontal `x = left + trunc(norm * (w - tw - 1))` (`th`/`tw` = tile size).

`dial` fields: `drag` is `"linear"` (default, relative drag along the axis), `"absolute"` (handle follows the mouse along the axis, minus `margin` pixels at both ends) or `"rotary"` (angle around the centre). `vertical` (default true) picks the axis for linear/absolute. `sensitivity` (default 1.0) scales relative drags: a full range per 200 px at 1.0. `fine` (default 0.1) multiplies it while Shift is held. `range` `[min, max]`, `steps` and `default` apply only when there is no `param` (a bound widget uses the param's range).

`meter` and `number`: `range` `[min, max]` as for dial; a `number` bound to a param keeps what a drag sets inside its `range` (for a readout of part of a wide param). A `number`'s `"valueText"` replaces the formatted value with text: `{ "table": "<data table>", "row": 3, "col": "<param id>", "first": 0 }` shows the table's cell `[row][col]`, where each of `row` and `col` is a fixed index, a param whose value is the index (`{var}`s allowed), or, left out, the widget's own value less `first`; an empty or missing cell shows the plain number. `{ "scale": 0.1, "format": "%.1f" }` shows the value times `scale` instead. `number` `format` defaults to the param's format. A `number`'s `sensitivity` is in value units per pixel of vertical drag (default: a full range per 200 px), and Shift applies `fine`.

A `plot`'s `line`, `line2` and `glow` are read where it is drawn, so they may name the palette with a `{var}` (`"@op{op}_line"`: the operator the view shows). `plot` field `"draw"`: `"trace"` (default), `"bars"`, `"bars_trace"` or `"centre_bars"`. A trace of a `key`'s values is drawn through them antialiased, `width` (default 1) times the skin's `density` wide, over a line four times as wide in `glow` if that colour is set. With `"key": "<text key>"` (`{var}`s allowed) the values are that text data's numbers, 0..1, up to 256 of them, separated by spaces, and the plot redraws when they change. With no data a trace lies along the bottom row of the rect and bars draw nothing. With `"source": "scope"` the data is what the processor last published with `State::setScope` (up to 256 values, 0..1 the full height): a trace reads the value under each column, bars put one bar per value, centred on its share of the width.

`custom` kinds in version 1: `"pad"` (a handle dragged in 2D; `params: [x, y]`), `"morph_pad"` (the same with a quartered grid), `"envelope"` (its colours are fields, for a skin with a dark display: `grid` the tempo grid lines, default `#c3c3c3ff`; `area` the fill under the curve, `#fbffffa5`; `curve` `#888888ff`; `handle` an unselected point's inside, `#fdfdfdff`; `dots` the loop's dotted segment, `#0c0c0cff`; `"keyscale"` takes the same), `"fx_chain"` and `"spectrum"` (draw `fill` only for now), `"piano"` (a keyboard of `count` keys starting at MIDI note `first`, white and black keys, the pressed key highlighted; without a `fill` it draws only the pressed-key highlight over the key art beneath it). Unknown kinds draw `fill` and ignore input, so a newer skin still opens in an older runtime.

Kinds for synths whose settings are plain params (FSVR's):
- `"stage_env"`: an envelope of stages, its levels and times params. `start` is the level at key on and `points` `[[time, level], ...]` the stages, each a param id (with `{vars}`) or a number; a point's time is how long the stage into it takes. `sustain` is the index of the point held until key off (a dashed marker and a plateau `gap` time units wide, default 25, follow it). `range` `[lo, hi]` spans the levels (default the first level param's range; a range across 0 draws the zero line and fills from it), and `minTime` (default 150) the least time shown across the width. Colours as the envelope's (`grid`, `area`, `curve`, `handle`, `dots`), and the curve styles below. Dragging a point moves its time sideways and its level up and down, Shift finer. With `model` naming a model the product registered (`registerStageModel`, `hollow.h`) the curve is the product's own envelope instead, on a time axis in seconds from key on: the model gets the widget's `inputs` (`{name: param id or number}`) and returns the levels from key on to the sustain point and from key off to the end, and where each of `points` is reached, which is where its handle goes. The key off falls where the longest curve has settled; `minSpan` (seconds, default 0.1) is the least time across the width, `text.font` labels the time grid in a band above the plot, `dbParam` names a param that, while off, shows a dB model as linear amplitude, and `overlay` (`{"var", "values", "param"}`) draws the same model faintly for each other value of the var while its param is on, in `overlayCurve` (default the curve's colour at alpha 60), each with the var at its own value: with `"@op{op}_line/80"` every other operator's envelope is in its own colour. Dragging with a model puts the held point where the pointer is: the level and the time become the values between the params' steps that land it there, found by bisection through the model, which draws the curve at those values, and the params get them rounded; on release the curve is drawn from the params again; a drag is one host gesture per param. The model returns vertices (`StageCurve`, `hollow.h`), so it can be cheap enough to ask many times a move. The plot is fitted to its curves, and again when its inputs resolve to other params. The wheel zooms about the pointer, no further out than the fit, Shift+wheel and a drag on empty plot pan, a double-click fits again, and the fitted width stays put while the curves still suit it. `syncParam` names a param that, while on, has the view's `stage_env` widgets naming it share one time axis: fitted to the longest of their curves, and zoomed and panned together. With a model the area fades from `area` at its far edge to `area2` at its base (default `area` at 15 % of its alpha). Without a registered model the widget draws as above.
- `"fseq"`: a formant sequence from `data/<table>.json` (`table`, default `fseqs`), the entry the param named by `fseq` picks: its eight tracks' formant frequencies over time (`layer` `"v"` voiced or `"u"` unvoiced), each segment as opaque as the frame is loud, in `line` (`{n}` in it: the track's number, so each can take its own colour); the pitch in `line2`; the params named by `loop` as dotted markers in `dots`. The height spans the frequencies the sequence's audible frames use. With `"key": "<text key>"` the entry is the text data under that key instead, one entry of the same shape as JSON, for a sequence the processor holds (FSVR's user Fseqs). `"position"` names a param holding the frame playback is at, drawn as a solid line in `cursor` (default `#e8b84bff`); the processor keeps it current.
- `"level_scale"`: a level key scaling curve across the keyboard, from the params named by `breakPoint` (semitones above `breakBase`, default 21), `leftDepth`, `rightDepth` (0..99), `leftCurve` and `rightCurve` (0 -lin, 1 -exp, 2 +exp, 3 +lin), in `line`, with the centre line in `grid` and the break point in `dots`. The shape is drawn, not a chip's table.
- `"matrix_wires"` with `algorithm` (a param id) draws a fixed algorithm's wires instead of amounts: the entry of `data/<table>.json` (`table`, default `algorithms`) at the param's value minus `first` (default 1), `{"mod": [[src, dst], ...], "fb": [src, dst], "out": [carriers]}` with operators 1..8. `number` in place of `algorithm` draws that algorithm always. The wires are unbroken, from each operator box to its destination's edge with an arrow and down into the output bus, so a skin's amount tags go over them in a higher layer. A thumbnail of one: `scale` (default 1) shrinks the whole drawing, `box` (a colour) draws the eight operator boxes itself, numbered in the widget's `text.font` when it has one, for where no operator art sits over the wires, and `boxSize` `[w, h]` sizes the boxes the wires meet (`{n}` in `box`: the box's number, `"@op{n}"` a colour each from the palette; `"wire"`, with `{n}` the same way, colours each operator's lines while it is on, which are otherwise light), in the drawing's units before `scale` (default `[16.67, 13.33]`, the visible box of FSVR's operator art at density 1.5).
- `"key_range"`: a note range on a small keyboard. `low` and `high` (params, MIDI notes), the keys `first` and `count` (default 0 and 128), colours `white`, `black`, `range` (white keys in the range), `rangeBlack`, `gap`, `arrow` (the shift's triangle and label) and `rail` (default `arrow` at 40%). Dragging a key moves the nearer end of the range to it, and on past the other end if the drag goes there: with `low` above `high` the range wraps, every key lit but those between the two (the FS1R's note limit). With `shift` (a param, semitones) a strip over the keys is its slider: a rail under the keys the shift's range reaches from `centre` (default 60, middle C), a tick under it at no shift, and a triangle over the key the centre moves to, labelled `+1`, `-12` and so on in the widget's `text.font` on the side it moved to, but never starting left of `labelLeft` (skin px from the widget's left, default 0: clear of a caption the skin puts there; past it, the label takes the other side). A press on the rail puts the triangle there, a drag slides it, and a double-click there clears the shift; presses on the strip beside the rail fall through to nothing.
- `"velocity"`: a velocity range and sense curve, velocity in across and out up. `low`, `high`, `depth` and `offset` (params); out is `depth * in / 64 + (offset - 64) * 2` clamped to 1..127, the FS1R's (and XG's) part velocity sense. Colours `grid` (the out = in diagonal), `area`, `curve`, `handle` and `off` (over the velocities outside the range). The curve's ends are handles: the low end's drags the range's low end across and the offset up and down, the high end's the high end and the depth. Either may pass the other: with `low` above `high` the range wraps, every velocity lit but those between the two, and the curve is drawn either side of that gap.
- These kinds redraw whenever a param their fields name changes. Their curves are antialiased, as wide as the skin's `density`.
- `stage_env` and `velocity` share a curve style: `width` (default 1, times the skin's `density`), `glow` (a colour for a line four times as wide under the curve, default none), `area2` (the fill's colour at the plot's bottom row, `area` being its colour at the top, so the fill fades; default `area`, flat) and `ring` (a ring around each handle, the screen's colour so the curve parts around it; default none). A handle is a dot in `handle`, haloed in it while dragged.

### Embeds, stacks and variables

An `embed` shows the view named `view` inside its rect, clipped. Any embed can be switched at runtime by a button `goto` whose `stack` names it, which is how pages work: the main view has an embed called `pages`, and each navigation button is a `goto` on it. Adding a page means adding a view file and a button.

`vars` is an object of strings. Inside the embedded view (and further embeds, which inherit and may override them) every `{name}` in a widget's `param` and `text.text` is replaced by the value. `{name:upper}` gives the value in upper case (for captions such as an operator letter). Lookup order: the nearest embed's `vars`, then outer embeds, then the skin-wide `vars` from `skin.json` (as changed by `set` actions). When a `set` action or a `goto` changes a var, every bound widget re-resolves its param and redraws. One operator view can then serve eight operators: `"param": "op.{op}.ratio"` with `vars: { "op": "1" }` .. `{ "op": "8" }`.

The runtime saves the current view and vars of every embed, the visibility of every widget toggled by an action, and the GUI scale in the plugin state, so a session reopens on the same page.

## Data, conditions, scrolling, menus and text

These fields extend the tables above. A runtime that predates them ignores them.

### Internal params and text data

- A param with `"host": false` is saved with the instance and bindable like any other, but hosts never see it (not listed, not automatable; VST2 indices count host params only). Use it for GUI-side state that must persist, such as sequencer steps, wheel positions or the selected envelope point.
- Text data in captions: a `plate`, `button` or `dropdown` caption (and a list cell) may hold `{data:<key>|<default>}`, filled with the text data under that key (`{var}`s allowed) or the default until there is some, redrawn as the data changes (a caption within the runtime's next tick).
- Text data: a `textbox` with `"key": "<key>"` (may contain `{var}`) shows and, when `editable`, edits the string stored under that key in the instance; `"text.text"` is the value shown until one is stored. `custom` kinds that keep shapes (envelopes) store them under their `key` the same way.

### Conditions

- `"showIf"` and `"enableIf"` on any widget: `"fx.reverb.on"` (true while that param is not at its minimum), or `{ "param": "<id>", "equals": 2 }`, `{ "param": "<id>", "notEquals": 0 }`, `{ "param": "<id>", "atLeast": 4 }`, `{ "param": "<id>", "atMost": 8 }` (plain values), `{ "var": "op", "equals": "pitch" }`. An `enableIf` on an `embed` disables everything inside it. `{ "stack": "pages", "view": "page_fseq" }` is true while that embed shows that view (with optional `"vars"` that must match too), for markers that follow the current page. A widget whose `showIf` is false is not drawn and not hit; one whose `enableIf` is false draws disabled and ignores input. Both re-evaluate whenever values or vars change. `{var}` placeholders work in the param id.

### Fitting

- An `embed` with `"fit": true` takes the size of the view it shows. A root view with `"fit": true` shrinks to the bounding box of its visible widgets, and the editor window resizes with it (through `Editor::Host::resize`), so hiding a keyboard strip makes the window shorter.

### Edit all

- `skin.json` `"editAll": { "param": "<internal param>", "var": "part", "values": ["p1", "p2", "p3", "p4"] }`: while that param is on, an edit to a widget whose param template contains `{part}` writes every one of those values' params, not just the current one, in the same gesture.

### Compound conditions and mirrored params

- Conditions combine: `{ "all": [ <condition>, ... ] }` and `{ "any": [ <condition>, ... ] }`, nestable, anywhere a condition is allowed.
- `"mirror": ["<param id>", ...]` on any bound widget: every edit also writes the same normalized value to those params (with `{var}` placeholders resolved), inside the same gesture.

### Number drag details

- `number` field `"sensitivityZones": [1, 0.1, 0.01, 0.001, 0.0001]`: the widget is split into that many equal columns left to right, and a drag started in column i moves `zones[i]` value units per pixel (Shift still applies `fine`). Without it `sensitivity` applies everywhere.
- `number` field `"drag": false`: the box does not drag (it may still be editable).

### Number zero text

- `number` field `"zeroText"`: the text shown instead of the formatted value while the value is exactly 0 (for example `""`, so only the artwork behind shows).

### Piano key art

- `piano` field `"keyImages": { "c": "<image>", "d": ..., "e": ..., "f": ..., "g": ..., "a": ..., "b": ..., "black": ..., "top": ..., "low": ... }`: each a two-tile image (normal, pressed). With it, a pressed key is drawn as tile 1 of its image at the key's place (the top C of the range uses `"top"`, and the first key, when it is white, `"low"`: an 88-key board's A0, with no black key beside it), instead of a highlight. With `keyImages` the piano draws every key from its image (tile 0, or tile 1 while pressed) at fixed positions, one octave per 130 px.

### Flow and scrolling

- A view with `"flow": "column"` places its widgets top to bottom in file order: each visible widget keeps its `x` and its height, starts where the previous visible one ended (plus `"gap"`, default 0), and hidden widgets take no space. The first visible widget keeps the `y` of the first widget in the file, so a column can start below a heading. The view's height becomes the total.
- An `embed` with `"scroll": { "track": "<image>", "thumb": "<image>", "width": 12 }` scrolls vertically when its view is taller than the rect: the wheel scrolls it, and a scrollbar of that width is drawn at the rect's right edge (the track image stretched to the full height, the thumb sized in proportion and dragged). The scroll position is saved with the GUI state. With `"reveal": true` the first radio button inside it that is on (a `value` action whose param holds its value) is scrolled to the middle after any button's `goto`, `set` or `toggle`, so a chooser opens on the current choice.

### Text entry

- `number` with `"editable": true`: double-click opens an inline editor over the widget (same font, a caret in the text colour, the whole text selected). Enter or clicking elsewhere commits through the param's text parsing (labels, then the number; units are ignored), Escape cancels. Without `editable` a double-click resets to the default as before.
- `textbox` with `"editable": true` and a `key`: double-click edits the text in place, Enter commits (with `multiline`, Ctrl+Enter), Escape cancels.

### Status display

- A `textbox` or `plate` with `"status": true` shows the name and value (`"Name: value"`) of the param under the pointer or being edited, and falls back to its own text 1.5 s after the pointer leaves or the edit ends.

### Menus and tooltips

- `skin.json` may define `"menu": { "font": "<font>", "fill": "#...", "border": "#...", "hoverFill": "#...", "hoverFont": "<font>", "disabledFont": "<font>", "separator": "#...", "rowHeight": 16, "pad": [l, t, r, b], "check": "<image>", "arrow": "<image>" }`. With it, dropdown menus and the scale menu are drawn by the runtime in that style inside the editor window (placed below the widget, or above when it would not fit, and clipped to the window), keyboard navigable (arrows, Enter, Escape); the chosen item is checked. Without it the runtime uses native menus.
- `skin.json` may define `"tooltip": { "font": "<font>", "fill": "#...", "border": "#...", "pad": [l, t, r, b], "delay": 700 }`. A widget's `tip` then shows near the pointer after hovering still for `delay` ms, until the pointer moves off the widget or a button is pressed.

Further `menu` fields:
- `shadow` (colour): a 1 px drop shadow right and below. `hoverBand` (colour or null): after `hoverFill`, a band one text line tall across the inner width behind the hovered text. `separatorHeight` (default 3): a separator row's height, its 1 px line one pixel below its top.
- `place`: `"below"` (default) or `"over"` (the menu's top-left sits on the widget's top-left). Either way the menu slides to stay inside the window.
- `releaseGuard` (ms, default 0): a mouse-up this soon after opening is ignored; a later one over an item chooses it.
- `keys`: `"all"` (default: arrows, Enter, Escape) or `"escape"` (only Escape closes).
- `styles`: named partial overrides of these fields, e.g. `{ "plain": { "fill": "#fff0ffff", "hoverBand": null } }`. A widget picks one with `"menuStyle": "<name>"`, or `"menuStyle": "native"` for a native menu.
- On opening, the item for the current value is highlighted.
- Menu items may also carry: `"items": [...]` (a submenu, opening on hover to the right), `"disabled": true` (shown in the style's `disabledFont`, never highlighted or chosen: a heading), `"check": true` (a toggle item showing a check mark), `"short": "<text>"` (the caption shown by `captionFromItem` instead of the label), `"columnBreak": true` (starts a new column, native menus), and `"action": { ... }` (any button action, run when chosen instead of setting the value).
- `"menuAt": [x, y]` on a dropdown or a `presets` button: the menu opens with its top-left at that point of the view (for example on a neighbouring name field), instead of relative to the widget.
- Native menus check only `check` items (never the current value), and their column breaks draw no divider line.
- `"context": [items]` on any widget: right-click (on release) opens a menu of those items at the pointer, in the widget's `menuStyle` (default the named style `"plain"` if it exists).

### Keyboard shortcuts

- `skin.json` `"keys"`: an array of bindings, each a `"chord"` and any action a button can carry. `[{ "chord": "alt+f", "goto": "page_filter", "stack": "pages" }, { "chord": "shift+1", "set": { "part": "p1" } }, { "chord": "alt+u", "cycle": { "layer": ["v", "u"] } }]`.
- A chord is the modifiers `shift`, `ctrl` and `alt` in any order, then one key: a letter, a digit, or one of `left`, `right`, `up`, `down`, `home`, `end`, `backspace`, `delete`, `enter`, `escape`, `tab`. `cmd` is a synonym for `ctrl`, `opt` and `option` for `alt`. A chord naming no key is dropped and the rest of the skin loads.
- Letters and digits match the **physical** key, not the character it types, so `shift+1` works where that types `!` and `alt+f` works on macOS where Option+F types `ƒ`. On macOS `ctrl` is Command, the modifier the runtime already reports as ctrl.
- **A skin with bindings holds the keyboard while its window is up**, so its chords work without clicking the editor first. It consumes only what it has a use for and hands every other key on to the host, so a DAW keeps its own shortcuts. Text entry, an open menu and a modal come first and take **every** key, space included, until they are done, so a chord never fires while a name is being typed or a dialog is up. A list with a selected row takes only the named keys, Escape, Delete, the arrows and the like; a key that types still reaches the chords and the host. A skin with no `"keys"` behaves as the runtime always did, taking the keyboard only while something in the editor wants it.
- The `cycle` action sets one var to its next listed value, wrapping past the last; a value outside the list goes to the first. It is the action for a shortcut that flips between states no single button sets.
- **Tooltips name the shortcut themselves.** A widget whose action a binding also runs shows the chord after its `tip`, as "The filter (Alt+F)", and shows the chord alone when it has no `tip`, so no binding hides. `goto` matches on view, stack and vars, `set` on the vars it writes, and a `cycle` matches any `set` of one of its values, so every button that reaches a state also names the chord that reaches it. Chords read as ASCII on every platform (`Alt+F`, not `⌥F`), since a skin's fonts hold Latin-1 only.

### Number and text details

- `number` field `"reset"`: `"default"` (double-click resets to the default when not editable), `"zero"` (resets to 0 when the range spans 0, else nothing) or `false` (nothing). `dial` takes the same field (default `"default"`).
- `dial` and `number` field `"wheel"`: the fraction of the range one wheel notch moves (0 disables the wheel; default 0.05 for dials, 0 for numbers). A stepped param accumulates the fraction and snaps to the nearest step. Shift multiplies it by `fine`.
- `textbox` field `"editOn": "click"` opens editing on a single click (default `"double"`).
- Caret and selection colours per font: `skin.json` `"fonts": { "<font>": { "caret": "#...", "selection": "#..." } }`; the caret blinks every 600 ms.

### Runtime values

- `"source"` on a `textbox`, `plate` or `button` shows a value the runtime knows: `"cpu"` (the audio thread's load in percent, as a number through the widget's `format`, e.g. `"%3d%%"`), `"midi_in"` (tile 1 for 200 ms after any incoming MIDI), `"modified"` (tile 1 while the state differs from the last load or save), `"voices"` (the processor's voice count, if it reports one), `"scale"` (the window scale as text, `1x` to `4x`).
- `"source"` on a `meter`: `"level_l"` or `"level_r"`, the output's peak level on that channel as the format layer measures every block, shown -60 to 0 dB over the meter's range and falling about 54 dB a second.
- A `list` with `dataRows` redraws whenever the text data its rows come from changes, so a processor can fill a list after it is shown.
- Text data under a key that starts `live.` is a processor's display data (a plot's points): it is never saved with the instance, and setting it never marks the instance modified.
- A `matrix_op` box with a `text.font` draws its `letter` over its art, centred on its ink, so the art needs no number in it. With `"tint"` (a colour, as a fill's) the box is its art in that colour, and the selected one keeps it rather than taking the selected tile: a ring in `"ring"` (default `#e8f3efff`), a line width outside the art's opaque bounds, marks it.
- `list` field `"source": "midi_map"`: rows are the MIDI learn assignments (below).

### Modals

- The root view gets two widgets at load: a `veil` over the whole window and an `embed` above it, both last in layer 7 and hidden. The text data `hollow.modal` names the view the embed shows, centred, with the veil behind it; `""` or an unknown view hides both. A `modal` action sets it, and so can a processor, from any thread. A modal the window cannot hold with a margin of 16 around it (a fitted root shrunk to a bar) opens in a window of its own where the platform has one (`platformPopup`: Windows, a frameless window with a drop shadow, owned by the editor's top-level window and centred over the editor, never above its top), while the editor keeps its size under the veil; the modal's view draws its own title and close box, and closing that window (Alt+F4) closes the modal as Escape does. Its GUI is a second `Gui` of the modal's view with the editor's skin, scale and vars, which never writes the session's GUI state. Where the platform has no such window (macOS and Linux for now) the editor grows to hold the modal and shrinks back when it closes. A modal takes the keyboard, and Escape closes it. A new editor window starts with none, and the embed is not saved with the GUI state.
- skin.json `"modal": { "veil": "#rrggbbaa" }`: the veil's colour (default `#00000080`).
- skin.json `"close": { "modal": "<view>", "if": <condition> }`: in the standalone (Windows), closing its window while the condition holds opens that modal instead. `{ "standalone": "close" }` then closes it without asking, as does a processor's text data `hollow.close` set to `"1"`.
- skin.json `"standalone": { "settings": "<view>" }`: the modal `{ "standalone": "settings" }` opens, or a view a page embeds: whenever that view comes into sight, as the modal or in an embed, the runtime reads the app's audio and MIDI setup (clap-wrapper's standalone extension, through `Editor::Host::devices`) into text data and params, and keeps them in step while it shows, reading again every second: for each of `api`, `output`, `input`, `rate` and `buffer`, the lines of `standalone.<list>.items` and the chosen line's index in the param `standalone.<list>` (a dropdown with `itemsData`), the input's first line `None`; `standalone.midi.items` lines are `name` then a tab and `1` for an open input or `0`. Setting a param changes that choice in the app, setting `standalone.midi` to 1 + a row flips that input, and everything is read again. The skin declares the params (`host: false`).
- The skin var `standalone` is `"1"` in the standalone and `"0"` elsewhere, for widgets only the app has. The processor learns the same from text data `live.standalone`, `"1"` in the standalone, which the format layer sets before it creates the processor.

### Lists

- `list` field `"contextRow": "<text key>"`: a right-click on a row stores the row's index and its cells, tab-separated, under that key before the list's `context` menu opens; a right-click off the rows opens nothing. The row lights, in place of the list's selection, until its menu and any modal it opened close, and `{cell:n}` in a menu item's label is the row's n-th cell (from 1), so the menu names the row. A column's `"tileCell": true` draws its image's tile numbered by the cell (`"0"`, `"1"`), a lamp per row.
- `dropdown` field `"itemsData": "<text key>"`: its items are that text data's lines, valued 0, 1, 2 ... in order, for choices only the product knows.
- A `list` with a `param` is a chooser: clicking row `i` sets the param to `values[i]` (or `first + i`, `first` default 0) and every param its `"set"` names (`{ "<param id>": <plain value> }`, `{var}` allowed, or an array of one value per row) to its value, in one gesture; the selected row is the one whose value and `set` values the params hold, and none while they hold others. A browser column of voices from several banks is a list whose `set` gives each row's bank.
- `list` fields: `"columns": [{ "width": 33, "align": "right", "edit": "int" }, { "width": 196 }, { "width": 20, "image": "<image>" }]` (an `image` column draws that image centred in each row whose cell is not empty, its tile 1 on the selected row when it has two, instead of the text), `"colGap"`, `"rowHeight"`, `"rowGap"`, `"gapFill"` (colour behind the rows, showing in the gaps), `"rowFill"`, `"selectFill"`, `"selectRow"` (the selected row's text style, for a text that reads on `selectFill`), `"valign"`, `"scroll"` (as for embeds, plus `"always": true` to draw the scrollbar even when not needed), `"rows"` (static rows, arrays of cell strings; a cell `"{data:<key>|<default>}"` shows the text data under that key, `{var}`s allowed, or the default until there is some), `"dataRows": { "key": "<text key>", "row": ["U{n}", "{1}", "{2}"], "param": "<id>", "first": 0, "set": { "<id>": 0 } }` (after the static rows, a row for each line of the text data under `key`, as many as there are: `{1}`, `{2}` ... are the line's tab-separated fields and `{n}` its number from 1; in a bound list a press sets `param`, else the list's, to `first` plus the row's index among them, with `set`), and the row text style in `"row"`. Rows fill down to the bottom even when empty. A click selects a row; double-click on an `"edit": "int"` cell edits it; Delete removes a selected row of a `source` list.

### MIDI in and MIDI learn

- The piano marks keys held by incoming MIDI as pressed, each from its note on to its note off. A sustain pedal is not followed: a keyboard showing every key played since the pedal went down says nothing about what is being played. Its fields: `"velocity"` (a fixed velocity; without it the height on the key decides), `"glide": false` (dragging across keys does not retrigger).
- A dial with `"midi"` also shows incoming MIDI of that kind (pitch bend, CC n): dragging and incoming messages move the same value.
- MIDI learn: while the internal param named by `skin.json` `"learn": { "param": "gui.midi_learn", "outline": "#ff000099" }` is on, clicking a widget with a host `param` makes it the learn target (drawn with a 1 px outline in that colour on its rect); the next incoming CC is assigned to that param. Assignments are saved with the instance; an assigned CC moves its param. `learn.defaults` (`{ "7": "master.volume", ... }`, CC number to param id) are the assignments a fresh instance starts with. A `"source": "midi_map"` list shows them (`cc | param name`), then its `"fixed"` rows unless already listed: `{ "param": "<id>" }` (that param's assigned CC, or -1) and `{ "label": "<text>", "ccParam": "<internal param>" }` (a row whose CC cell shows and edits that param), its CC cells editable, and its `context` items may use the actions `{ "midi_map": "remove" }` and `{ "midi_map": "reset" }`.

### Animated flow

- A flow view with `"animate": <px per frame>` grows a widget that becomes visible from 0 to its height (and shrinks one before it hides) by that many pixels per 30 Hz frame, moving what follows with it. An embed's `scroll` with `"follow": true` keeps the growing widget in view.

### MIDI from the GUI

- `custom` kind `"piano"` sends note on (velocity from the click's height on the key, higher on the key = softer) and note off to the plug-in's processor, and highlights the held key.
- A `dial` with `"midi": "bend"` sends 14-bit pitch bend (normalized 0.5 = centre), `"midi": "cc<n>"` sends controller `n` (0..127 over the dial's range); `"spring": <normalized>` returns the dial there on release (a pitch wheel springs to 0.5). Such a dial usually has no param, or a `host: false` one.

## Fonts

A font is a TrueType file or a PNG glyph strip. `skin.json` `"fonts"` names the TrueType ones: `{ "<font>": { "file": "<a .ttf in fonts/>", "size": <em in px at 1x>, "colour": "#rrggbbaa", "tracking": <px added to every advance>, "shadow": { "colour": "#rrggbbaa", "offset": [dx, dy] } } }`, `caret` and `selection` as for any font (Text entry). The runtime rasterizes its Latin-1 glyphs at the skin's scale when the skin loads, antialiased, the shadow under them, each advance kept to 1/64 px; kerning is not applied. Its box runs from the tallest ASCII ink to the deepest, so a Latin-1 accent above the capitals clips. A widget names it like any font, so a skin moves from strips to TrueType without touching its views.

A glyph strip at another of the skin's scales is `scales/<s>/fonts/<font>.png` when the skin has one, drawn at that size; without one the 1x strip is used as it is.

A glyph strip is a PNG holding the 256 characters of Latin-1 (codes 0..255) left to right. If row 0 of the image has exactly 256 pixels with non-zero alpha, it is a marker row: each marked column starts the next glyph, a glyph runs to the next marker (the last one to the right edge), and glyph pixels are rows 1..h-1. Otherwise the font is fixed width: each glyph is `w / 256` wide and `h` tall. Glyphs are drawn as they are coloured in the image (alpha blended). Text is UTF-8; code points above 255 draw as `?`.

## Scale

With `skin.json` `"scales": [0.5, 0.75, 1, 1.5, 2]` the skin is drawn at each scale it lists: the runtime loads it again with every rect, pad, offset, row height, column width, view size, slice band and font size multiplied (rects by their edges, so rects that meet at 1x still meet), its `density` multiplied with them, and each image from `scales/<scale>/<group>/<name>.png` (`scales/0.75/knobs/rotary_large.png`), or, where the skin has no such file, the 1x image resampled tile by tile. Sizes round half away from zero. The window is that load's root view, shown one pixel to one, so every scale is drawn at its own resolution; the session keeps the scale. Picture fonts stay at 1x unless the skin has a strip drawn for the scale (`scales/<s>/fonts/`), so a skin with scales wants TrueType fonts or a strip per scale.

Without `"scales"` the runtime draws at scale 1 into a buffer and presents it at an integer scale 1..4 with nearest-neighbour pixel replication, so the artwork stays sharp. A `scale` action offers the scales either way.

## Live editing

When the environment variable `HOLLOW_SKIN_DIR` points at a skin folder, the runtime loads the skin from there instead of the copy embedded in the plug-in, and reloads it about once a second when any file's modification time changes. Point it at the skin folder (`hollow.py run` points FSVR's standalone at `../FSVR/plugin/skin`), open the standalone, and edits saved from the web editor appear in the running plug-in.
