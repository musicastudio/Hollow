// Internal to the core and the platform layers: the loaded skin, the software renderer and the
// platform-independent GUI (widget tree, input, actions). Nothing here is public API.
#pragma once
#include "hollow/hollow.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hollow {

// ---- JSON: only what a skin and the GUI state need ------------------------------------------

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;                             // Array
    std::vector<std::pair<std::string, Json>> members;   // Object, in file order

    const Json& operator[](const std::string& key) const; // Null when missing
    const Json& operator[](size_t i) const;
    size_t size() const { return type == Array ? items.size() : members.size(); }
    bool has(const std::string& key) const { return (*this)[key].type != Null; }
    double num(double def = 0) const { return type == Number ? n : type == Bool ? (b ? 1 : 0) : def; }
    int integer(int def = 0) const { return type == Number || type == Bool ? (int)num() : def; }
    bool flag(bool def = false) const { return type == Bool ? b : type == Number ? n != 0 : def; }
    std::string str(const std::string& def = "") const { return type == String ? s : def; }
};
bool parseJson(const std::string& text, Json& out, std::string* error = nullptr);
std::string jsonQuote(const std::string& s);
std::string writeJson(const Json& j);                              // compact, members in order
bool parseNumber(const char*& p, const char* end, double& out);   // locale-independent
std::string formatNumber(const char* fmt, double v);               // printf, always '.' decimal
bool safeFormat(const std::string& fmt);                           // one conversion, for a double
std::string formatInt(const std::string& fmt, int v);              // one %d/%i conversion, else plain digits
uint32_t parseColour(const std::string& s, uint32_t def);          // "#rrggbb" or "#rrggbbaa" to 0xAARRGGBB
// A value as the skin shows it: the label, else format (or the param's), else %.0f / %.2f. No unit.
std::string displayText(const ParamDef& d, double plain, const std::string& format);

// ---- skin -----------------------------------------------------------------------------------

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect operator&(const Rect& o) const;
    Rect operator|(const Rect& o) const;   // bounding box; an empty side is ignored
};

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // 0xAARRGGBB, straight alpha, row-major
    int tiles = 1, passes = 1;   // passes: times the image is composited (skin.json "passes")
    bool axisX = false;
    bool sliced = false;
    int slice[4] = {0, 0, 0, 0}; // top, right, bottom, left
    bool surface = true;         // skin.json "surface": false keeps the skin's surface off this image
    std::vector<int8_t> slope;   // with a reflecting surface: the slope of its shading, x and y per pixel (-127..127)
    Rect tile(int i) const;
};

// skin.json "surface": a texture laid over the window through the light, grey pixels of the artwork
// and colour fills (not text). Such a pixel becomes the texture's colour at its window point times
// the pixel's own lightness, blended in by weight[lightness] * chroma[max - min] (0..256 each). A
// reflecting surface (relief) looks the texture up where the pixel's slope points instead: the
// artwork's own slope times relief, plus the sheet's (normals, R and G around 128) times dents.
struct Surface {
    const Image* tex = nullptr;      // tiled from the window's top-left
    const Image* normals = nullptr;  // the sheet's slope, in window pixels
    int relief = 0, dents = 0;       // pixels a full slope moves the lookup
    int weight[256] = {}, chroma[256] = {};
};

struct Font {
    Image img;
    int top = 0, height = 0;     // glyph rows in img
    int x[256] = {}, w[256] = {};
    uint32_t colour = 0xffffffff; // average colour of its opaque pixels
    uint32_t caret = 0, selection = 0x803c78d8;   // text entry; caret 0 = colour (skin.json "fonts")
    int width(const std::string& utf8) const;
};

using Vars = std::vector<std::pair<std::string, std::string>>;

enum class Kind { Plate, Button, Dropdown, Dial, Number, Textbox, Meter, Plot, Veil, List, Tree, Embed, Custom };
enum Press { PressDown, PressMomentary, PressRepeat, PressUp };
enum Drag { DragLinear, DragAbsolute, DragRotary };

struct Fill {
    int image = -1, tile = -1;   // image index in Skin::images; tile -1 = chosen by the widget
    bool hasColour = false;
    uint32_t colour = 0;
};
struct Text {
    std::string text;
    int font = -1;               // index in Skin::fonts
    int align = 0, valign = 0;   // 0 left/top, 1 center/middle, 2 right/bottom
};
struct Action {
    enum Type { None, Goto, Set, Toggle, Url, Step, MidiMap, Presets, Sequence, Data, File, Value, Standalone, Scale } type = None;
    std::string target, stack;   // Goto: view and stack; Toggle: widget name; Url: the URL; MidiMap: remove / reset;
                                 // Presets: the data table
    Vars vars;                   // Goto and Set; Data: text key (may hold {vars}) to text
    double amount = 0;           // Step; Value: the value it sets
    std::string key, nameKey;    // Presets: the envelope data key, the text key of the name field
    bool save = false;           // Presets: store into the chosen slot instead of loading it
    int perColumn = 0;           // Presets: items per menu column
    std::string prefix, index;   // Sequence: param prefix, step number (may hold {vars}); target insert / delete / reset / random
    std::string leaf;            // Sequence reset / random: the row
    int count = 0;               // Sequence: steps
    std::string title;           // File: the dialog's title (target: open / save, key: the text data key, nameKey:
                                 // a save's file name, vars: file types, name to extensions)
};
struct Item {                    // dropdown and context menu entries
    std::string label, shortLabel;
    double value = 0;
    bool separator = false, check = false, columnBreak = false;
    Action action;               // run when chosen, instead of setting the value
    std::vector<Item> items;     // a submenu
};
struct Cond {                    // showIf / enableIf
    enum Op { None, Set, Equals, NotEquals, AtLeast, AtMost, Shows, All, Any } op = None;   // Set: the param is above its minimum
    std::string param, var;      // one of them; param may hold {vars}
    std::string text;            // Equals / NotEquals on a var
    double value = 0;            // the value compared: a param's plain value, or a var read as a number
    std::string stack, view;     // Shows: the embed `stack` shows `view` (and vars)
    Vars vars;
    std::vector<Cond> parts;     // All, Any
};
struct Scroll {
    int track = -1, thumb = -1;  // images
    int width = 0;               // 0 = the embed does not scroll
    bool always = false, follow = false, reveal = false;
};
struct FixedRow {                // midi_map list rows: a param (its CC or -1), or a label whose CC cell is ccParam
    std::string param, label, ccParam;
};
struct Column {                  // list columns
    int width = 0, align = 0;
    bool editInt = false;
    int image = -1;              // an icon column: this image, centred, for each non-empty cell
};

class Gui;
struct Canvas;
struct Hit;
// A custom widget kind (kinds.cpp): its drawing (after the fill) and input. Rects are canvas
// pixels. down returns true to capture the pointer, so drag and up follow; dbl, when set, takes
// the second press of a double-click instead of down. hover gets a point outside r when the
// pointer leaves. right gets right presses and releases over the widget. tick runs about 30
// times a second while the widget is shown (redraws when what it shows changed). Hooks left null
// do nothing.
struct KindOps {
    void (*draw)(Gui&, Canvas&, const Hit&, Rect r);
    bool (*down)(Gui&, const Hit&, Rect r, int x, int y, bool shift);
    void (*drag)(Gui&, const Hit&, Rect r, int x, int y, bool shift);
    void (*up)(Gui&, const Hit&, Rect r, int x, int y);
    void (*dbl)(Gui&, const Hit&, Rect r, int x, int y);
    void (*hover)(Gui&, const Hit&, Rect r, int x, int y);
    void (*right)(Gui&, const Hit&, Rect r, int x, int y, bool shift, bool up);
    void (*tick)(Gui&, const Hit&, Rect r);
};
const KindOps* findKind(const std::string& kind);   // null: an unknown kind, drawn as its fill
struct KindState {                                   // what one custom widget keeps between events
    virtual ~KindState() = default;
};
void kindsLoaded(Gui& g);                            // the host loaded a state (kinds that refit)
void presetMenu(Gui& g, const Hit& h, const Action& a); // the "presets" action (kinds.cpp)
// Scope buffers (plot and the scope kinds): one height per column from the bottom, -1 = a gap.
// mode 0 trace, 1 bars, 2 bars then trace, 3 bars from the centre then trace.
void drawScope(Canvas& c, Rect r, const std::vector<int>& v, int mode, uint32_t c1, uint32_t c2, int thick = 1);

struct Widget {
    Kind kind = Kind::Plate;
    std::string name, param, customKind, view, format, tip;
    Rect rect;
    int layer = 3;
    bool hidden = false, disabled = false;
    Cond showIf, enableIf;
    double value = 0;
    Fill fill;
    Text text, header, row, selectRow;   // selectRow: the selected row of a list, when it has a font
    int pad[4] = {0, 0, 0, 0};   // left, top, right, bottom
    std::string key;             // textbox, custom: text data key (may hold {vars})
    bool editable = false, status = false, editOnClick = false;
    enum Source { NoSource, Cpu, MidiIn, Modified, Voices, MidiMap, Scope, LevelL, LevelR, Scale } source = NoSource;
    // button, dropdown, context menus
    bool toggle = false, hoverTiles = false, pressedTiles = false, disabledTile = false, captionFromItem = false;
    Press press = PressDown;
    int pressOffset[2] = {0, 0};
    Action action;
    std::vector<Item> items, context;
    std::string menuStyle;       // a skin.json menu style name, "native", or "" for the default
    bool hasMenuAt = false;      // "menuAt": the menu opens with its top-left at this point of the view
    int menuAt[2] = {0, 0};
    // dial, number, meter
    int image = -1;
    Drag drag = DragLinear;
    bool vertical = true, slider = false, floorFrames = false;
    double sensitivity = 1, fine = 0.1, rangeMin = 0, rangeMax = 1, def = 0, wheel = 0;
    bool hasRange = false;       // a number bound to a param: "range" also bounds what a drag sets
    struct ValueText {           // number "valueText": the text from a data table's cell, or value * scale
        std::string table, rowParam, colParam, format;
        int row = -1, col = -1;  // a fixed row or column; -1: the param named, else the widget's value less first
        double first = 0, scale = 0;
    } valueText;
    enum Reset { ResetDefault, ResetZero, ResetNone } reset = ResetDefault;
    int margin = 0, steps = 0;
    int midi = -1;               // dial: -1 none, 0..127 that controller, 128 pitch bend
    double spring = -1;          // dial: normalized value it returns to on release, -1 = stays
    bool hasZeroText = false;    // number: zeroText replaces the value while it is exactly 0
    std::string zeroText;
    bool dragOn = true;          // number: "drag": false
    std::vector<double> zones;   // number: sensitivityZones, value units per pixel per column
    std::vector<std::string> mirror;   // params written with every edit of the bound one
    // textbox, plot, list
    bool multiline = false;
    uint32_t line = 0xffffffff, line2 = 0xffffffff;
    int drawMode = 0;            // plot: 0 trace, 1 bars, 2 bars and trace, 3 centre bars and trace
    int rowHeight = 0, rowGap = 0, colGap = 0;
    std::vector<Column> columns;
    std::vector<std::vector<std::string>> rows;
    std::vector<FixedRow> fixed;
    bool hasGapFill = false, hasRowFill = false, hasSelectFill = false;
    uint32_t gapFill = 0, rowFill = 0, selectFill = 0;
    // embed, custom
    Vars vars;
    Scroll scroll;               // embeds and lists
    bool fit = false;            // embed: takes the size of the view it shows
    std::vector<std::string> params;
    const KindOps* ops = nullptr;
    int first = 36, count = 61;  // piano: MIDI note of the first key, number of keys
    int velocity = 0;            // piano: a fixed velocity, 0 = from the height on the key
    bool glide = true;           // piano: dragging across keys plays them
    int keyImages[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};   // piano: c d e f g a b black top
    bool surface = true;         // false keeps the skin's surface off all it draws (an embed: its whole view)
    Json json;                   // the whole widget object, for fields only a custom kind reads
};

struct View {
    std::string name;
    int w = 0, h = 0;
    bool flow = false;           // "flow": "column"
    bool fit = false;            // root view: the window shrinks to its visible widgets
    int gap = 0, animate = 0;    // flow: gap, and px per frame that appearing widgets grow by
    Fill fill;
    std::vector<Widget> widgets;
    std::vector<int> order;      // draw order: by layer, then file order
};

struct MenuStyle {               // skin.json "menu", or one of its "styles"; off = native menus
    bool on = false;
    int font = -1, hoverFont = -1, disabledFont = -1, check = -1, arrow = -1;
    uint32_t fill = 0xffffffff, border = 0, hoverFill = 0xff3060a0, separator = 0xff808080, shadow = 0, hoverBand = 0;
    int rowHeight = 16, separatorHeight = 3, pad[4] = {16, 2, 8, 2};
    bool over = false;           // place "over" the widget, else below it
    bool escapeOnly = false;     // keys "escape": no arrows or Enter
    int releaseGuard = 0;        // ms after opening in which a mouse-up is ignored
};
struct TipStyle {                // skin.json "tooltip"; off = native tooltips
    bool on = false;
    int font = -1, delay = 700;
    uint32_t fill = 0xffffffe1, border = 0xff000000;
    int pad[4] = {3, 2, 3, 2};
};

class Skin {
public:
    std::string name, root, dir;  // dir: folder it came from (live reload), empty when embedded
    // skin.json "density": pixels per unit of the runtime's own geometry (the custom kinds' drawings,
    // menu and tooltip chrome, defaults of pixel fields, drag rates). 1.5 for a skin drawn at 1.5x.
    double density = 1;
    int dp(double v) const { return (int)std::lround(v * density); }   // built-in geometry at this density
    int lw() const { return std::max(1, dp(1)); }                     // line width of built-in drawings
    Vars vars;
    std::vector<ParamDef> params;
    std::vector<Image> images;
    std::vector<std::string> imageNames, fontNames;   // parallel to images and fonts
    std::vector<Font> fonts;
    std::map<std::string, View> views;
    MenuStyle menu;
    std::map<std::string, MenuStyle> menuStyles;   // "styles": the default with overrides
    Surface surface;                               // skin.json "surface"; off while surface.tex is null
    TipStyle tooltip;
    std::string editAllParam, editAllVar;          // skin.json "editAll": while the param is on, edits of params
    std::vector<std::string> editAllValues;        // whose template holds {var} write every value's param
    std::string learnParam;       // skin.json "learn": the internal param that arms MIDI learn
    uint32_t learnOutline = 0xff000099;
    std::vector<std::pair<int, std::string>> learnDefaults;   // CC and param id of a fresh instance
    std::map<std::string, Json> tables;            // data/<name>.json
    const View* view(const std::string& name) const;
    const Json& table(const std::string& name) const;   // Null when missing
};

// Loads a skin folder; null (and *error) on failure. loadSkin() uses it for HOLLOW_SKIN_DIR.
std::shared_ptr<Skin> loadSkinDir(const std::string& dir, std::string* error = nullptr);
// Signature of every file's name and modification time under dir, for live reload polling.
uint64_t skinDirStamp(const std::string& dir);

// ---- software renderer -------------------------------------------------------------------------

struct Canvas {
    uint32_t* px;
    int w, h;
    Rect clip;
};
// With a surface (skin artwork and fills; never text or drawings), light grey pixels take its texture.
void fillRect(Canvas& c, Rect r, uint32_t argb, const Surface* surface = nullptr);
// Unsliced images draw 1:1 at dst's top-left, or scaled to dst with stretch.
void drawImage(Canvas& c, const Image& img, int tile, Rect dst, bool stretch = false, const Surface* surface = nullptr);
// The surface burnt into an image drawn with its top-left at window point (x, y) >= 0, every tile at
// that place: the pixels the runtime shows there, now in the artwork (tools/bake.cpp).
void bakeSurface(Image& img, int x, int y, const Surface& sf);
// Text layout: lines stacked from valign, each aligned in r and clipped to r's right/bottom.
void drawText(Canvas& c, const Font& f, const std::string& s, Rect r, int align, int valign, bool multiline);

// ---- platform window (platform/win32.cpp, platform/mac.mm) ---------------------------------------

class Gui;
struct PlatformWindow;
struct MenuEntry {
    std::string label;
    int id = -1;                  // what the pick reports
    bool separator = false;
    bool checked = false;         // draws the check mark (check items, and the current item of native menus)
    bool check = false;           // a check item: the menu indents its labels
    bool current = false;         // highlighted when the menu opens
    bool columnBreak = false;     // native menus: starts a new column
    std::vector<MenuEntry> items; // a submenu
};
PlatformWindow* platformOpen(void* parent, Gui* gui);
void platformClose(PlatformWindow* w);
// Editor holds the GUI while it calls into it from the host's thread; only Linux runs the GUI on a
// thread of its own, so elsewhere this does nothing.
void platformHold(PlatformWindow* w, bool hold);
void platformInvalidate(PlatformWindow* w, Rect r);                 // canvas pixels
void platformSize(PlatformWindow* w, int width, int height);        // window pixels
// The host did not resize its frame: resize the windows around ours that closely fit it.
void platformResizeParents(PlatformWindow* w, int width, int height);
int platformMenu(PlatformWindow* w, const std::vector<MenuEntry>& items, int x, int y);  // canvas pixels; the id or -1
extern const bool kNativeMenus;   // false: the platform has none, and a "native" menu is drawn in the skin's style
void platformTip(PlatformWindow* w, const std::string& text);        // tooltip of the whole window, "" = none
void platformFocus(PlatformWindow* w, bool on);   // take the keyboard (text entry, menus, lists), or hand it back
void platformOpenUrl(const std::string& url);
// The standalone app's own audio and MIDI settings window (clap-wrapper's); kAudioSettings is false
// where the platform's standalone has none.
extern const bool kAudioSettings;
void platformAudioSettings(PlatformWindow* w);
// A native open or save dialog; types: (name, "ext;ext"). False when cancelled; path in UTF-8.
bool platformFileDialog(PlatformWindow* w, bool save, const std::string& title, const Vars& types, const std::string& name, std::string& path);

enum Key { KeyNone, KeyLeft, KeyRight, KeyUp, KeyDown, KeyHome, KeyEnd, KeyBackspace, KeyDelete, KeyEnter, KeyEscape, KeyTab, KeySelectAll };

// ---- the GUI: one shown view tree, its input handling and painting --------------------------------

struct Node;
struct Inst {                    // one widget of one shown view
    bool hidden = false, toggled = false;   // hidden: its "hidden" field, flipped by toggle actions
    bool shown = true, enabled = true;      // showIf, enableIf
    bool hiding = false;          // animated flow: shrinking before it hides
    double local = 0, local2 = 0;
    int param = -1, param2 = -1;  // resolved State indices
    std::vector<int> mirror;      // resolved mirror params
    int cond[2] = {-1, -1};       // resolved showIf / enableIf params
    Rect r;                       // its rect in the view, after flow, fit and animation
    int animH = -1;               // animated flow: current height, -1 = not animating
    int scroll = 0;               // embeds and lists: pixels scrolled
    int sent = -1;                // dials with midi: the last value sent or received
    int row = -1;                 // lists: the selected row
    int revealed = -2;            // bound lists: the selected row when it was last scrolled into view
    std::string text;             // text.text with {vars} resolved
    std::string key, data;        // resolved text data key, and its text (or source value) at the last paint
    bool stored = false;          // whether the key held data at the last paint
    double drawn = -1e300, drawn2 = -1e300;   // bound values at the last paint
    std::unique_ptr<Node> child;  // embeds
    std::unique_ptr<KindState> ks;// custom kinds
};
struct Node {
    const View* view = nullptr;
    std::string viewName, path;   // path: embed names from the root, '/' separated
    Vars vars;
    Node* parent = nullptr;
    int x = 0, y = 0;             // origin in the canvas
    int h = 0;                    // content height: the view's, or the flow total
    bool enabled = true;          // false inside a disabled embed
    Rect clip;
    std::vector<Inst> w;
};
struct Hit {
    Node* node = nullptr;
    int i = -1;
    unsigned gen = 0;
    explicit operator bool() const { return node != nullptr; }
    bool operator==(const Hit& o) const { return node == o.node && i == o.i; }
};

class Gui {
public:
    Gui(const Skin* skin, State& state, Editor::Host* host, const std::string& root = "");
    ~Gui();
    void setSkin(const Skin* skin);              // live reload: rebuild, keeping the GUI state
    int width() const { return w_; }             // canvas size (root view, scale 1)
    int height() const { return h_; }
    int scale() const { return scale_; }
    void setScale(int s);
    std::string uiJson() const;
    void applyUi(const std::string& json);
    const std::vector<uint32_t>& pixels();       // repaints what is dirty, then returns the canvas
    void invalidate(Rect r);
    void invalidateAll() { invalidate({0, 0, w_, h_}); }

    void mouseDown(int x, int y, bool right, bool dbl, bool shift);
    void mouseMove(int x, int y, bool shift);
    void mouseUp(int x, int y, bool shift);
    void rightUp(int x, int y, bool shift);
    void mouseLeave();
    void wheel(int x, int y, double notches, bool shift);
    bool keyDown(Key k, bool shift, bool ctrl);  // true when used (text entry, a menu or a list has the keys)
    void keyChar(unsigned codepoint);
    bool wantsKeys() const;
    void focusLost();                            // commits text entry, closes menus, drops list focus
    void tick();                                 // about 30 Hz: automation, repeats, tooltips, caret, animation

    void watch(std::shared_ptr<Skin> skin);     // Editor: keep this skin, reload it when Skin::dir changes
    PlatformWindow* window = nullptr;

    // For the custom kinds (kinds.cpp).
    const Skin& skin() const { return *skin_; }
    State& state() { return state_; }
    const Widget& wid(const Hit& h) const { return h.node->view->widgets[h.i]; }
    Inst& inst(const Hit& h) const { return h.node->w[h.i]; }
    Rect rectOf(const Node& n, int i) const;
    bool pressed(const Hit& h) const { return live(press_) && press_ == h; }
    bool live(const Hit& h) const { return h.node && h.gen == gen_; }
    bool standalone() const { return kAudioSettings && host_ && host_->standalone(); }   // the app's settings are ours to open
    std::string subst(const Node& n, const std::string& s) const;   // {var} and {var:upper}
    int paramOf(const Hit& h, const std::string& id) const;         // a param id with {vars}, -1 if unknown
    double plain(const Node& n, int i, int which = 0) const;        // bound widgets read State, others their local value
    double norm(const Node& n, int i, int which = 0) const;
    void setPlain(const Hit& h, double v, int which = 0);
    void setNorm(const Hit& h, double t, int which = 0);
    void setParam(int p, double plain);          // a whole gesture on one param (kinds, menus)
    void setParams(const std::vector<std::pair<int, double>>& values);   // one gesture over several
    // A param template's params: the one for the current vars, or with "editAll" on and its var in
    // the template, one per listed value.
    std::vector<int> paramsFor(const Node& n, const std::string& tpl) const;
    double defaultOf(const Hit& h, int which = 0) const;
    void begin(const Hit& h);                    // host gestures for the widget's host params
    void end();                                  // ends every open gesture
    // Runs an action for the widget at n, i; may rebuild part of the tree, so callers must not use
    // their Hit afterwards.
    void runAction(Node& n, int i, const Action& a);
    void openMenu(std::vector<MenuEntry> items, Rect at, const std::string& style, std::function<void(int)> pick);
    Rect menuAnchor(const Hit& h) const;         // where the widget's menu opens (its rect, or "menuAt")
    int held = -1;                               // what the current press holds (the piano's key)
    std::map<std::string, double> kindData;      // values kinds share across widgets and pages (a selection by data key)
    bool loaded = false;                         // during the tick in which the host loaded a state

private:
    struct Menu {                                // one open skinned menu (a submenu follows its parent)
        std::vector<MenuEntry> items;
        const MenuStyle* style = nullptr;
        Rect r;
        int sel = -1, indent = 0;
        int colW = 0;                            // columns (an item's columnBreak starts one): their width,
        std::vector<int> col;                    // and each item's column
    };
    struct Edit {                                // text entry: a number, a textbox or a list cell
        Hit h;
        Rect r;                                  // the field in canvas pixels
        Text style;
        int pad[4] = {0, 0, 0, 0};
        bool multiline = false;
        std::string text;
        size_t caret = 0, anchor = 0;            // byte offsets; the selection runs between them
        std::function<void(const std::string&)> commit;
    };
    const Skin* skin_;
    State& state_;
    Editor::Host* host_;
    std::shared_ptr<Skin> owned_;
    std::string rootName_, savedUi_, tip_;
    Node root_;
    Vars vars_;
    int w_ = 0, h_ = 0, scale_ = 1, ticks_ = 0;
    uint64_t stamp_ = 0;
    std::vector<uint32_t> canvas_;
    Rect dirty_;
    unsigned gen_ = 1;                           // bumped whenever nodes may die; stale Hits are dead
    Hit hover_, press_;
    std::vector<int> editing_;                   // params inside a begin/endEdit gesture
    int anchorPos_ = 0, anchorScroll_ = 0, mouseX_ = 0, mouseY_ = 0;
    double anchorNorm_ = 0, wheelAcc_ = 0, zone_ = 0;   // zone_: a number drag's units per pixel from its zone, 0 = none
    using Time = std::chrono::steady_clock::time_point;
    Time nextRepeat_, tipStill_, statusUntil_, menuOpened_, blinkStart_, midiUntil_;
    std::vector<Menu> menus_;                    // open skinned menus, the root first
    std::function<void(int)> pick_;
    bool menuPressed_ = false;                   // a press on the open menu cancels the release guard
    Hit tipHit_;                                 // skinned tooltips: the widget under the pointer
    bool tipBlocked_ = false;                    // a press hid it until the pointer leaves the widget
    std::string tipShown_, status_;
    Rect tipRect_;
    int statusParam_ = -1;
    Edit edit_;
    bool editing_on_ = false, caretOn_ = true, editDrag_ = false;
    Hit listFocus_;                              // a list with a selected row takes Delete
    Hit learnTarget_;                            // MIDI learn: the widget whose param learns
    bool learnOn_ = false;
    unsigned midiSeen_ = 0;
    bool animating_ = false;
    uint32_t taus_[3] = {0, 0, 0};               // the random rows' generator (three-component Tausworthe)

    void rebuild();
    void build(Node& n, const std::string& view, Node* parent, const std::string& path, int depth);
    void show(Node& p, int k, const std::string& view, const Vars& vars);
    void resolve(Node& n);
    bool test(const Node& n, const Cond& c, int param) const;
    bool conds(Node& n, bool animate);           // re-evaluates showIf/enableIf; true if a visibility changed
    void update();                               // after values or vars change
    void relayout();                             // flow, fit, then every node's origin and clip
    void resizeWindow();                         // the window follows the canvas and scale
    void flow(Node& n);
    void place(Node& n, int x, int y, Rect clip);
    void placeChild(Node& n, int i);
    void animate();
    int fullHeight(const Node& n, int i) const;
    Rect bar(const Node& n, int i) const;        // an embed's or list's scrollbar, empty when not shown
    Rect thumb(const Node& n, int i) const;
    int contentHeight(const Node& n, int i) const;
    void setScroll(Node& n, int i, int pixels);
    void reveal(Node& n);                        // embeds with scroll "reveal": their lit radio button in view
    Hit scroller(Node& n, int x, int y);
    const std::string* lookup(const Node& n, const std::string& key) const;
    void paint(Canvas& c, Node& n);
    void paintWidget(Canvas& c, Node& n, int i, Rect r);
    void paintScrollbar(Canvas& c, const Node& n, int i);
    void paintList(Canvas& c, Node& n, int i, Rect r);
    void drawFill(Canvas& c, const Fill& f, Rect r, int tile, bool stretch = false);
    bool surfOn_ = true;         // the widget being painted lets the skin's surface on
    const Surface* surf() const { return surfOn_ && skin_->surface.tex ? &skin_->surface : nullptr; }
    void drawCaption(Canvas& c, const Text& t, const std::string& s, Rect r, const int pad[4], int dx, int dy, bool multiline);
    std::string shownText(Node& n, int i);       // plates and textboxes: status, source, data or text
    std::string sourceValue(const Widget& w) const;
    std::string numberText(const Node& n, int i) const;
    const Json& valueCell(const Node& n, int i, double v) const;     // "valueText": the table's cell for value v
    Hit hit(Node& n, int x, int y);
    Hit tipAt(Node& n, int x, int y);
    Hit hit(int x, int y) { return hit(root_, x, y); }
    bool enabled(const Node& n, int i) const;
    Node* shownIn(const std::string& stack, const std::string& view, const Vars& vars) const;

    int stepsOf(const Node& n, int i) const;
    int index(const Node& n, int i) const;       // button/dropdown value as a step index
    void sendMidi(const Hit& h);
    std::vector<int> editAllParams(const Hit& h, int which) const;
    uint32_t random();
    void sequence(Node& n, const Action& a);
    void midiToDials(Node& n);

    bool active(const Node& n, int i) const;     // goto/set buttons light up by themselves
    bool activeAction(const Action& a) const;
    int buttonTile(const Node& n, int i) const;
    void act(const Hit& h);
    void flip(const Hit& h);
    Node* findEmbed(Node& n, const std::string& name, int* index);
    bool findWidget(Node& n, const std::string& name, Hit& out, bool deep);
    Node* nodeAt(const std::string& path);
    void releasePress();
    void dropdown(const Hit& h);
    void contextMenu(const Hit& h, int x, int y);
    void itemMenu(const Hit& h, const std::vector<Item>& items, Rect at, const std::string& style, bool current);
    void scaleMenu(Rect at);
    void openSkinned(size_t level, std::vector<MenuEntry> items, Rect at, bool below, const MenuStyle* st);
    void closeMenus(size_t from = 0);
    void pickMenu(int id);
    bool menuAt(int x, int y, size_t& level, int& row) const;
    Rect menuRow(size_t level, int row) const;
    void hoverMenu(size_t level, int row);
    void paintMenu(Canvas& c, const Menu& m);
    void showTip();
    void hideTip(bool block);
    void paintTip(Canvas& c);
    void updateStatus();
    void invalidateStatus(Node& n);
    bool editing(const Node& n, int i) const { return editing_on_ && live(edit_.h) && edit_.h.node == &n && edit_.h.i == i; }
    void startEdit(const Hit& h, bool atClick, int x, int y);
    void editCell(const Hit& h, int row, int col);
    void finishEdit(bool commit);
    void editInsert(const std::string& s);
    size_t editIndexAt(int x, int y) const;
    void paintEdit(Canvas& c);
    void restartBlink();
    void listDown(const Hit& h, int x, int y, bool dbl);
    struct ListRow {                             // one data row of a list
        std::vector<std::string> cells;
        int cc = -1, param = -1, ccParam = -1;   // midi_map rows: its CC, the param it moves, the param its CC cell shows
    };
    std::vector<ListRow> listModel(const Node& n, int i) const;
    std::string withData(const Node& n, const std::string& s) const;   // "{data:key|default}" filled in
    std::vector<std::pair<int, double>> listRowParams(const Hit& h, int row) const;
    int listRows(const Node& n, int i) const { return (int)listModel(n, i).size(); }
    void listAction(const Hit& h, const Action& a);
    void setLearnTarget(const Hit& h);
    void drag(const Hit& h, int x, int y, bool shift, bool start);
    void checkValues(Node& n);
    void writeEmbeds(const Node& n, std::string& out) const;
    void writeHidden(const Node& n, std::string& out) const;
    void writeScroll(const Node& n, std::string& out) const;
    void saveUi();
};

} // namespace hollow
