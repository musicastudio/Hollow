# Hollow skin editor

A browser editor for Hollow skins (the folder of JSON and PNG described in `docs/skin-format.md`). It works for any Hollow product: point it at a skin folder and it edits views, widgets, params, images and fonts in place. No build step and no dependencies: Python 3 for the local server, any current Chromium or Firefox for the page.

## Run it

Double-click `editor.cmd` in the repository root (or choose it in the `hollow.cmd` menu), or:

```
python editor/serve.py [skin_dir] [--port 8765] [--open]
```

`--open` opens the page in your browser; otherwise open the printed URL (`http://127.0.0.1:8765/`, or a free port when 8765 is taken). Without `skin_dir` it opens FSVR's skin, `plugin/skin` in the FSVR repository beside this one. The server binds to 127.0.0.1 only, serves the editor and one skin at a time, and keeps tile and glyph exports in a work folder next to the skin (`<skin_dir>/../tiles`).

The **skin button** at the left of the toolbar (it shows the open skin's name) lists FSVR's skin and every other folder opened since the server started, with **Open** for each, plus a field for any other folder with **Browse...** for a native folder picker. A folder without `skin.json` opens as a new, empty skin; Save creates its files. The dialog opens by itself when the skin folder is empty. Switching reloads the page, after asking about unsaved changes; a second tab still showing the old skin gets an error instead of writing to the new one.

## Live editing with the runtime

`run-hollow-fsvr.cmd`, or `python hollow.py run`, starts the FSVR standalone already pointed at FSVR's skin folder: edit that skin here, press Save, and the app redraws. For a host, or by hand:

1. Point the runtime at the skin folder, in the shell you start the plug-in from:
   - PowerShell: `$env:HOLLOW_SKIN_DIR = "E:\path\to\FSVR\plugin\skin"`
   - cmd: `set HOLLOW_SKIN_DIR=E:\path\to\FSVR\plugin\skin`
   - bash: `export HOLLOW_SKIN_DIR=/path/to/FSVR/plugin/skin`
2. Start the standalone build of the plug-in from that shell (or a host started from it).
3. Run `serve.py` on the same folder and edit in the browser.
4. Press Save (Ctrl+S). The runtime notices the new modification times within about a second and reloads the skin, keeping the current page.

Save writes images first, then JSON, then deletions, and the server replaces each file atomically, so the runtime never reads a half-written file or a reference to an image that is not there yet.

## Screen

- **Left**: tabs for Views (every view; `root` and `page` tags mark the root view and the views shown on the `pages` stack, other stack names appear as tags too), Images (tree by group with thumbnails, fonts at the end), Params, and Check (validation).
- **Centre**: the canvas with rulers, zoom 1 to 6, and a bar of widget types to add. Rendering follows `docs/skin-format.md` at scale 1 and is then enlarged with nearest-neighbour pixels. Embedded views draw inline with their `vars`; hidden widgets, and widgets whose `showIf` is false with the default values, draw faintly (toggle with H); widgets whose `enableIf` is false draw disabled; B outlines every widget. While the skin loads, and while tiles, glyphs or a save are written, a readout over the canvas shows the progress.
- **Right**: the inspector for the selected widget, several widgets (align, distribute, same size), the view and skin (nothing selected), an image, a font, or a param.
- The **?** button lists the keyboard shortcuts.

## Finding and changing things

You rarely need to know which view holds something:

- **Find** (the box at the top right, or Ctrl+F) searches the whole skin as you type: widget texts, menu items, widget names, tips, bound params, and view, param, image and font names. Each hit shows where it lives, as the chain of views from the root (`main › sidebar`). Arrows move through the hits and Enter opens one: a text opens its view with the widget selected and the text ready to type over, a menu item opens the menu editor at that item, a param opens it in the Params tab.
- **Double-click on the canvas** goes to what is under the pointer. Inside an embed (another view shown in this one) it opens that view with the widget selected; hovering shows that widget with a blue dashed outline and names it in the status bar. On a text it edits the text in place (Enter keeps it, Escape drops it). On a dropdown it opens the menu editor. Enter opens the view of the selected embed or navigation button.
- A **click** picks the widget that shows. Hidden widgets (drawn faintly while "hidden" is on) come after it, so an invisible overlay no longer swallows clicks; Alt+click cycles through everything under the pointer, hidden ones included.
- The **breadcrumb** in the toolbar shows where the open view is shown (`main › sidebar`); click a parent to open it. Back (Alt+Left) returns to the previous view.

So changing a label takes four steps: double-click it (inside an embed, twice), type, Enter, Ctrl+S. With Find: Ctrl+F, type part of it, Enter, type, Enter, Ctrl+S.

## Editing model

Every change stays in the editor until Save, with one exception noted below. Undo and redo (Ctrl+Z, Ctrl+Y) cover all of it: widgets, views, params, image imports, renames and deletions. Save writes only the files that changed. JSON is written with a 2-space indent, widget keys in the order `type`, `name`, `rect`, `layer` and then the rest as they were, and short values on one line. Fields the editor does not know are kept. Leaving the page with unsaved changes asks first.

Renaming a view updates every embed, `goto` and the root. Renaming an image updates `skin.json` and every view; renaming a font updates every text style; renaming a param id updates every exact binding. Deleting a view, image or font is refused while something still uses it, with the list of users.

**Add page** creates a view sized like the `pages` embed (or another stack you name) and adds a navigation button with a `goto` to it in the view you choose. When that view already has navigation buttons, the new one copies the style of the last one and sits below the lowest button of the same size.

## Play mode

**Play** (or P) turns the canvas into a live preview of the open view that behaves like the runtime (`framework/src/core/editor.cpp`); the rules live in `logic.mjs`, so the tests check them. Editing is off while playing: the palette, undo and the edit shortcuts do nothing and the inspector shows the Play panel instead, which lists what the played skin has changed (vars, pages, param values, text data, scroll positions, toggled widgets) and has **Reset**. P again returns to editing; switching views keeps the played values.

- Buttons: `press` modes `down`, `momentary`, `repeat` (400 ms, then every 100 ms) and `up`, `toggle` (stepping through a param's values), hover and pressed tiles, `pressOffset`, and the `goto`, `set`, `toggle`, `step`, `sequence` (insert or delete a step, or reset or randomize one leaf of every step; random picks uniformly among a param's values), `data` (stores text data), `presets` (a plain menu of the slots in `data/<table>.json`, loading a slot's content and name into text data, with a Save submenu when `save` is set) and `url` actions. Navigation and `set` buttons light up by themselves.
- Dials: `linear` (relative, Shift for `fine`), `absolute` (with `margin`) and `rotary` drags, sliders, double-click reset, the wheel (one step per notch for stepped values), `spring` on release and `midi` (the bend or controller value that would be sent shows in the footer). Numbers drag vertically and reset on double-click; an `editable` one opens an inline editor instead (Enter commits through the param's labels, then the number; units are ignored; Escape cancels). Editable textboxes with a `key` edit their text data the same way (Ctrl+Enter with `multiline`).
- Dials and numbers follow `reset` ("default", "zero" or off) and `wheel` (a fraction of the range per notch; a stepped value accumulates it and snaps to the nearest step); a number drags at the rate of the `sensitivityZones` column the drag starts in, not at all with `"drag": false`, and shows its `zeroText` at 0; every edit of a widget with `mirror` writes the same normalized value to those params, and while the skin's `editAll` param is on an edit through a template with its var writes every listed value's param; a textbox with `editOn: "click"` edits on one click.
- Menus: dropdowns and right-click `context` menus open in the skin's `menu` style or the widget's named `menuStyle` (a context menu uses "plain" when it exists), or a plain list standing in for a native menu. `place` below or over, sliding into the window; `releaseGuard`; `keys` all or escape; shadow, hover band, separator height, check and arrow images; submenus open on hover or with the Right key; check items show when their action is on; items with an `action` run it; `short` is the caption for `captionFromItem`; `menuAt` puts a dropdown's or presets button's menu at a point of the view; the plain list (a native menu) checks only `check` items and starts a column at `columnBreak`, without a divider.
- Lists draw `columns`, `rows`, fills and a scrollbar (`always` too); a click selects a row, a double-click edits an `int` cell, the wheel scrolls a row per notch. A `midi_map` list shows the MIDI learn assignments (a fresh play state starts from `learn.defaults`), then its `fixed` rows (a param's CC or -1; a label whose CC cell edits an internal param); Delete and "remove" drop an assignment, "reset" returns to the defaults.
- MIDI learn: while the skin's `learn` param is on, a click picks a widget with a host param (outlined); the Play panel's **MIDI in** row sends a simulated CC (assigning it to the target, then moving its param and dials with that `midi`) or note (held keys show on the piano). `source` values: `midi_in` lights for 200 ms after simulated input, `modified` while anything differs from the defaults, `voices` counts held notes, `cpu` reads 0.
- `showIf` and `enableIf` follow values, vars and stacks (`{ "stack", "view" }`), combined with `all` and `any`; an `enableIf` on an embed disables what it holds. `flow` views close the gaps of hidden widgets, starting at the first widget's y, and with `animate` grow and shrink at 30 Hz (a scroll with `follow` keeps the growing widget in view). Embeds and a root view with `fit` take their content's size, so the canvas follows. Scrolling embeds scroll with the wheel and the thumb. The piano highlights held keys, or draws every key from its `keyImages` (tile 0, tile 1 when pressed) where the key art puts them (a 130 px octave), with a fixed `velocity` and `glide`. `status` widgets show "Name: value" of the param under the pointer, and tips show in the skin's `tooltip` style after its delay (or as the browser's own tooltip when the skin has no style).

## Menu editor

A dropdown's **Items** and every widget's **context** menu show a summary and **Edit menu...**, which opens the menu editor on a copy of that menu:

- **The tree** on the left lists every item, separator and submenu, indented by level, with tags for the value, check, column break, action and short caption. Drag an item to reorder it: drop on the top or bottom half of a row to put it before or after, on a submenu row to make it that submenu's first item, or below the last row to move it to the end. The toolbar adds an item, a separator or a submenu after the selection, duplicates, deletes, moves up and down, and moves out of or into a submenu (into the item above, which becomes a submenu).
- **The item panel** on the right edits the selected item: its kind (item, separator, submenu, switching between them), label, value, `short`, `check`, `columnBreak`, and its action with the same fields as a button's (goto, set, toggle, url, step, sequence, presets, data, midi_map). **This item as JSON** takes any field, including ones the panel does not know.
- **The preview** below draws the menu at 2x in the widget's menu style (its `menuStyle`, "plain" for a context menu when the skin has it, else the native stand-in), with the submenus leading to the selection open, check marks, and the item for the current value marked. Click an item there to select it.
- **Add from lines...** adds items typed one per line (`label = value`, or `-` for a separator) after the selection, numbering lines without a value on from the largest value in the menu. **Edit as JSON...** edits the whole menu as one array.
- The editor has its own undo and redo (Ctrl+Z, Ctrl+Y). Keys in the tree: arrows select, Alt+Up and Alt+Down move, Alt+Left and Alt+Right move out of and into a submenu, Insert adds an item, Delete deletes, Ctrl+D duplicates. **Apply** (Ctrl+Enter) writes the menu back as one step of the page's undo; **Cancel** or Escape asks before dropping changes. An empty menu removes the field.

## New fields in the inspector

- Params: the **host** flag (off for `host: false`); the Params tab tags internal params and filters host or internal ones.
- Every bound widget: `mirror` params. Every widget: **Conditions** for `showIf` and `enableIf` (param on, param = / ≠ / ≥ / ≤ a value or label, var = / ≠ a value, a stack showing a view, or all / any compounds edited as JSON), with pickers for params, vars, stacks and views and a note when the condition is false with the default values; **Menus** for `menuStyle` and the `context` menu (see [Menu editor](#menu-editor)).
- Views: `flow`, `gap`, `animate` and `fit`. In a flow view the canvas shows the flowed positions; dragging moves widgets across, and Ctrl+[ and Ctrl+] change the order. While H shows hidden widgets they keep their place, so they can be selected.
- Textboxes: `key`, `editable`, `editOn`, `status`, `source`; plates: `status`, `source`; buttons: `source`; numbers: `editable`, `zeroText` (type `""` for an empty text), `reset`, `wheel`, `sensitivityZones`, `drag`; dials: `midi`, `spring`, `reset`, `wheel`; embeds: `fit` and `scroll` (track, thumb, width, always, follow); lists: columns (one per line: width, alignment, `int`), rows (cells separated by `|`), gaps, fills, `valign`, `source` and `scroll`; pianos: `velocity`, `glide`, `keyImages`.
- Skin (nothing selected): the `menu` style with every field, its named `styles` (add, remove, each field, and "no hoverBand"), the `tooltip` style, `editAll` (param, var, values), `learn` (param and outline), and a preview of a menu in each style (one item checked, one hovered, a submenu arrow) and a tooltip at 2x. A font's inspector sets its caret and selection colours, which the inline editors use.

## Images, tiles and fonts

Select an image to see it with its tiles outlined and its nine-slice bands dashed; editing `tiles`, `axis` and `slice` updates the view at once, and a sliced image shows a stretched preview at a size you choose. **Explode** lays the tiles out in a numbered grid. **Use for selected** assigns the image to the selected widgets.

- **Import** (button or drop PNG files on the page) asks for the group and name and stores the file as `images/<group>/<name>.png`, or as a font.
- **Export tiles** writes each tile of a strip to `tiles/<image name>/000.png`, `001.png` and so on, eight files at a time. Edit them in any paint program, then **Import tiles** reads them back in name order, checks they share one size, rebuilds the strip on the image's axis, updates `tiles` in `skin.json` and saves. **Import files...** does the same from files you pick.
- **Export glyphs** writes each non-empty glyph of a font to `tiles/fonts/<font>/<code>.png`, where the code is the decimal Latin-1 code (`65.png` is A). **Import glyphs** rebuilds the strip with a fresh marker row; glyphs without a file keep their current pixels and width, so spaces survive. Tile and glyph imports save straight away.
- The font view shows the glyph cells over the strip and a sample text box rendered in the font.

PNG files are read and written by the editor's own codec (8-bit RGBA, straight alpha), so pixels round-trip exactly.

## Tests

```
node editor/test.mjs          # tile maths, button tiles, nine-slice, fonts, PNG, references, JSON layout, printf,
                              # conditions, flow, scrollbars, lists, menus, and Play mode buttons, dials, menus,
                              # context menus, lists, MIDI learn, sources, fit, animated flow, text entry
python editor/test_serve.py   # the server on a throwaway skin: read, write, move, delete, path escapes
```

## Files

- `serve.py`: the local server (standard library only).
- `index.html`, `editor.css`, `editor.js`: the page; `editor.js` is the DOM and canvas side.
- `logic.mjs`: the pure rules shared by the page and the tests.
- `test.mjs`, `test_serve.py`: the tests.

## Density

A skin with `density` (FSVR's has 1.5) plays with the runtime's own geometry scaled the same way: the piano's key-art offsets, pad handles, the key scaling preview's insets, menu and tooltip defaults, scroll widths, and drag rates per design pixel.

## Surface

A skin with a `surface` shows it in the view: widget artwork and colour fills pass through the texture at their window place, with the runtime's arithmetic (`L.applySurface`), while text, menus and the custom kinds' drawings stay as they are. Each surfaced drawing is cached until the skin changes, so a busy view renders its first frame a little slower.

## Limits

- Scaled nine-slice parts use the browser's nearest-neighbour sampling, which picks the source pixel under each destination pixel centre. The runtime should sample the same way for identical results.
- While editing, buttons show their initial state (goto and set buttons light up from the files); Play mode shows hover, pressed and live values. `envelope`, `fx_chain` and `spectrum` draw their fill only, as in the runtime; a `keyscale` custom draws its stored curve as straight segments (a preview: the runtime bends them by each point's slope).
- Play mode plays the open view on its own: a `goto` whose stack is not inside it does nothing (open the root view to play the pages). Nothing is saved from Play mode, and no MIDI or sound comes out; the footer shows what would be sent.
- Inline editors in Play mode are plain inputs over the widget, not drawn in the skin's font. Menus without a skin style, and tips without one, use the browser's font.
- Compositing is the browser's: the runtime's integer blend (images and glyphs `(s*a + d*(255-a)) >> 8`, fills `(s*a + d*(256-a)) >> 8`, writing the source alpha) is not reproduced, so colours can differ by a level or so per channel (fully opaque images come out one level darker in the runtime). Doing it exactly would mean a software renderer over the whole canvas. Images with `passes` are drawn that many times; plots draw their idle trace for `trace` and `bars_trace`, nothing for bars.
- Where the format leaves a size open, the editor picks one: separators in skin menus are half a row, a menu without a `check` image marks the chosen item with a small square, the scroll thumb is at least 8 px, and the wheel scrolls 20 px per notch.
- Menu `disabledFont` is kept and edited but has nothing to draw: menu items have no disabled state in the format. Actions the format does not define do nothing in Play mode.
- The `data/` folder is read (preset tables) but never written, and the proposed `envelope`, `waveform` and matrix kinds draw their fill only, as unknown kinds do, until the format describes them.
- Undo across a Save restores the JSON only: an image or font already written stays on disk as saved (undoing an import after saving leaves the file in place, unused).
- Changes made to the skin by other tools while the editor is open are not picked up; reload the page.
- The clipboard lives in the page, so copy and paste work between views but not between browser tabs.
