# Names

Hollow uses its own names for everything in the framework, the skin and the editor. This file is the naming rules a skin follows, and FSVR's params as the worked example.

## Naming rules

- Everything is lowercase `snake_case`. Parameter ids are dotted paths of such segments.
- **Views**: `page_*` for navigator pages, `dialog_*` for modal boxes, `fx_*` for effect strips, otherwise a short noun for the region (`topbar`, `sidebar`, `keys`).
- **Images**: `<group>/<name>`, grouped by what the image *is*, never one folder per view. Groups: `backgrounds` (whole-view backdrops), `panels` (frames, boxes, 9-slice plates), `knobs` (rotary strips), `faders` (linear slider strips and their tracks), `buttons` (push/toggle strips), `switches` (multi-state selectors, radio groups), `leds`, `meters`, `displays` (scope, envelope and graph backdrops), `icons`, `keyboard`, `branding` (logos, wordmarks), `scrollbars`, `library` (browser-only art), `dialogs` (setup, licence and about boxes), `misc` (last resort, keep it small). Names say what it looks like or does, with size or variant suffixes when a group has several: `knobs/rotary_large`, `knobs/rotary_small`, `buttons/nav_page`, `leds/on_off_green`.
- **Fonts**: `<colour-or-role>_<size>`, e.g. `caption_grey_12`, `digits_grey_14`, `caps_white_bold_12`.
- **Widgets**: unique in their view. Interactive widgets are named after what they control (`speed`, `fine`, `nav_library`); their caption is `<name>_label`, a readout next to a dial is `<name>_value`, a decorative frame is `<section>_frame`, a section heading `<section>_title`. Pure decoration that nobody will look for can stay `deco_<n>`.
- **Params**: `<section>.<name>` or `<section>.<instance>.<name>`: `perf.volume`, `fseq.speed`, `ctrl.1.depth`, `sys.tune`. Display names are short Title Case in our own words.
- **Instance segments.** Families that repeat per instance use the instance as its own segment, with identical leaf names across instances, so one view can serve all instances through `{var}` templates (`op.{op}.v.coarse.{part}`).

## FSVR's params

FSVR (the FS1R editor, its skin in the FSVR repository's `plugin/skin`) names its params this way, the part last as `p1`..`p4` where a parameter belongs to a part (its `{part}` var):
- `sys.<leaf>`, `perf.<leaf>`, `fseq.<leaf>`: the system, the performance and its Fseq
- `ctrl.<1..8>.<leaf>`: a controller set, its sources one switch each (`ctrl.3.src.kn1`)
- `fx.<rev|var|ins|eq>.<leaf>`: the effects, their type-dependent words `p1`..`p16`
- `part.<leaf>.<part>`, `voice.<leaf>.<part>`: a part's own settings and its voice's common ones
- `op.<1..8>.<v|u>.<leaf>.<part>`: a voiced (`v`) or unvoiced (`u`) operator; leaves the two share have the same name, so one page serves both through `{layer}`
- `perf.program` picks a factory performance; `browse.*` are the browser's own (not host) params
- `perf.bank` with `perf.user`, `part.user.<part>` and `fseq.user` number the user's own performances, voices and Fseqs (1 to 16,384) where the unit had a fixed internal bank; `knob.1`..`knob.4` are the knobs as KN1 to KN4; `part.morph_<x|y|jitter_x|jitter_y|seed|edit>.<part>` are a part's morph (FSVR's `docs/editor.md`)
