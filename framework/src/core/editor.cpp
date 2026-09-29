// The platform-independent GUI: the tree of shown views, conditions, flow, fit, animation and
// scrolling, painting, input, text entry, menus and tooltips, lists, runtime values, MIDI learn,
// actions, GUI state and live reload; plus Editor and renderView on top of it. The custom kinds
// draw and take input in kinds.cpp.
#include "core.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <random>

namespace hollow {

using Clock = std::chrono::steady_clock;
using ms = std::chrono::milliseconds;

static Vars toVars(const Json& j) {
    Vars v;
    for (auto& m : j.members) v.emplace_back(m.first, m.second.str());
    return v;
}

static void setVar(Vars& vars, const std::string& k, const std::string& v) {
    for (auto& kv : vars)
        if (kv.first == k) { kv.second = v; return; }
    vars.emplace_back(k, v);
}

static void writeVars(const Vars& vars, std::string& o) {
    o += '{';
    for (size_t i = 0; i < vars.size(); ++i) o += (i ? "," : "") + jsonQuote(vars[i].first) + ":" + jsonQuote(vars[i].second);
    o += '}';
}

static bool visible(const Inst& s) { return !s.hidden && s.shown; }

static void outline(Canvas& c, Rect r, uint32_t col, int k = 1) {   // a k px rectangle on r
    fillRect(c, {r.x, r.y, r.w, k}, col);
    fillRect(c, {r.x, r.y + r.h - k, r.w, k}, col);
    fillRect(c, {r.x, r.y + k, k, r.h - 2 * k}, col);
    fillRect(c, {r.x + r.w - k, r.y + k, k, r.h - 2 * k}, col);
}

// The size of text as drawText lays it out: lines break at a newline or the two characters "\n".
static void textSize(const Font& f, const std::string& s, int& w, int& h) {
    w = 0;
    int lines = 1;
    for (size_t i = 0, start = 0; i <= s.size(); ++i) {
        bool br = i < s.size() && (s[i] == '\n' || (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n'));
        if (i < s.size() && !br) continue;
        w = std::max(w, f.width(s.substr(start, i - start)));
        if (!br) break;
        ++lines;
        if (s[i] == '\\') ++i;
        start = i + 1;
    }
    h = lines * f.height;
}

Gui::Gui(const Skin* skin, State& state, Editor::Host* host, const std::string& root)
    : skin_(skin), state_(state), host_(host), rootName_(root) {
    vars_ = skin->vars;
    midiSeen_ = state.midiInCount();
    std::random_device rd;   // Tausworthe seeds must exceed 1, 7 and 15
    taus_[0] = rd() | 2;
    taus_[1] = rd() | 8;
    taus_[2] = rd() | 16;
    rebuild();
}

Gui::~Gui() {   // closed mid-gesture: notes stop, springs return, the host's gesture ends
    releasePress();
    end();
}

// ---- the tree of shown views ------------------------------------------------------------------

void Gui::rebuild() {
    releasePress();
    finishEdit(false);
    closeMenus();
    end();
    ++gen_;
    listFocus_ = {};
    learnTarget_ = {};
    const std::string& name = rootName_.empty() ? skin_->root : rootName_;
    const View* v = skin_->view(name);
    w_ = v ? std::max(v->w, 1) : 320;
    h_ = v ? std::max(v->h, 1) : 200;
    canvas_.assign((size_t)w_ * h_, 0);
    root_ = Node();
    build(root_, name, nullptr, "", 0);
    resolve(root_);
    conds(root_, false);
    relayout();
}

void Gui::build(Node& n, const std::string& view, Node* parent, const std::string& path, int depth) {
    n.view = skin_->view(view);
    n.viewName = view;
    n.parent = parent;
    n.path = path;
    n.w.clear();
    if (!n.view) return;
    n.h = n.view->h;
    n.w.resize(n.view->widgets.size());
    for (size_t i = 0; i < n.w.size(); ++i) {
        const Widget& w = n.view->widgets[i];
        Inst& s = n.w[i];
        s.hidden = w.hidden;
        s.local = s.local2 = w.value;
        s.r = w.rect;
        if (w.kind != Kind::Embed || depth >= 16) continue;
        bool loop = false;
        for (Node* p = &n; p; p = p->parent) loop |= p->viewName == w.view;
        if (loop) continue;
        s.child.reset(new Node);
        s.child->vars = w.vars;
        build(*s.child, w.view, &n, path.empty() ? w.name : path + "/" + w.name, depth + 1);
    }
}

// Shows view in the embed widget k of node p (a goto, or restoring saved pages).
void Gui::show(Node& p, int k, const std::string& view, const Vars& vars) {
    releasePress();
    finishEdit(false);
    end();
    ++gen_;
    listFocus_ = {};
    if (learnTarget_.node) setLearnTarget({});   // leaving a page clears the learn target
    int depth = 0;
    for (Node* q = &p; q->parent; q = q->parent) ++depth;
    const Widget& e = p.view->widgets[k];
    Inst& s = p.w[k];
    s.child.reset(new Node);
    s.child->vars = vars;
    s.scroll = 0;
    build(*s.child, view, &p, p.path.empty() ? e.name : p.path + "/" + e.name, depth + 1);
    resolve(*s.child);
    conds(root_, false);
    relayout();
}

const std::string* Gui::lookup(const Node& n, const std::string& key) const {
    for (const Node* p = &n; p; p = p->parent)
        for (auto& kv : p->vars)
            if (kv.first == key) return &kv.second;
    for (auto& kv : vars_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

// {name} is the var's value, {name:upper} the value in upper case; unknown names stay as they are.
std::string Gui::subst(const Node& n, const std::string& s) const {
    if (s.find('{') == std::string::npos) return s;
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        size_t e = s[i] == '{' ? s.find('}', i) : std::string::npos;
        std::string key = e != std::string::npos ? s.substr(i + 1, e - i - 1) : "";
        bool upper = key.size() > 6 && key.compare(key.size() - 6, 6, ":upper") == 0;
        const std::string* v = e != std::string::npos ? lookup(n, upper ? key.substr(0, key.size() - 6) : key) : nullptr;
        if (!v) { o += s[i]; continue; }
        for (char c : *v) o += upper && c >= 'a' && c <= 'z' ? char(c - 32) : c;
        i = e;
    }
    return o;
}

int Gui::paramOf(const Hit& h, const std::string& id) const { return id.empty() ? -1 : state_.indexOf(subst(*h.node, id)); }

void Gui::resolve(Node& n) {
    if (!n.view) return;
    for (size_t i = 0; i < n.w.size(); ++i) {
        const Widget& w = n.view->widgets[i];
        Inst& s = n.w[i];
        const std::string& p1 = w.params.size() > 0 ? w.params[0] : w.param;
        s.param = p1.empty() ? -1 : state_.indexOf(subst(n, p1));
        s.param2 = w.params.size() > 1 ? state_.indexOf(subst(n, w.params[1])) : -1;
        s.mirror.clear();
        for (auto& m : w.mirror)
            if (int q = state_.indexOf(subst(n, m)); q >= 0) s.mirror.push_back(q);
        s.cond[0] = w.showIf.param.empty() ? -1 : state_.indexOf(subst(n, w.showIf.param));
        s.cond[1] = w.enableIf.param.empty() ? -1 : state_.indexOf(subst(n, w.enableIf.param));
        s.text = subst(n, w.text.text);
        s.key = subst(n, w.key);
        if (s.child) resolve(*s.child);
    }
}

Rect Gui::rectOf(const Node& n, int i) const {
    const Rect& r = n.w[i].r;
    return {n.x + r.x, n.y + r.y, r.w, r.h};
}

Node* Gui::findEmbed(Node& n, const std::string& name, int* index) {
    if (!n.view) return nullptr;
    for (size_t i = 0; i < n.w.size(); ++i)
        if (n.view->widgets[i].kind == Kind::Embed && n.view->widgets[i].name == name) { *index = (int)i; return &n; }
    for (auto& s : n.w)
        if (s.child)
            if (Node* r = findEmbed(*s.child, name, index)) return r;
    return nullptr;
}

bool Gui::findWidget(Node& n, const std::string& name, Hit& out, bool deep) {
    if (!n.view) return false;
    for (size_t i = 0; i < n.w.size(); ++i)
        if (n.view->widgets[i].name == name) { out = {&n, (int)i, gen_}; return true; }
    if (deep)
        for (auto& s : n.w)
            if (s.child && findWidget(*s.child, name, out, true)) return true;
    return false;
}

Node* Gui::nodeAt(const std::string& path) {
    Node* n = &root_;
    for (size_t pos = 0; pos < path.size() && n;) {
        size_t e = std::min(path.find('/', pos), path.size());
        std::string seg = path.substr(pos, e - pos);
        pos = e + 1;
        Node* next = nullptr;
        for (size_t i = 0; n->view && i < n->w.size() && !next; ++i)
            if (n->w[i].child && n->view->widgets[i].name == seg) next = n->w[i].child.get();
        n = next;
    }
    return n;
}

// The node the embed named stack shows, if it shows view with those vars (vars it does not list are ignored).
Node* Gui::shownIn(const std::string& stack, const std::string& view, const Vars& vars) const {
    int k;
    Node* p = const_cast<Gui*>(this)->findEmbed(const_cast<Node&>(root_), stack, &k);
    Node* shown = p ? p->w[k].child.get() : nullptr;
    if (!shown || shown->viewName != view) return nullptr;
    for (auto& kv : vars)
        if (std::find(shown->vars.begin(), shown->vars.end(), kv) == shown->vars.end()) return nullptr;
    return shown;
}

// ---- conditions, flow, fit, animation and scrolling ---------------------------------------------

// Params compare plain values; a var compares as text for equals / notEquals and as a number for
// atLeast / atMost. An unknown param satisfies only notEquals, a var that is not a number neither
// atLeast nor atMost.
bool Gui::test(const Node& n, const Cond& c, int p) const {
    if (c.op == Cond::None) return true;
    if (c.op == Cond::Shows) return shownIn(c.stack, c.view, c.vars) != nullptr;
    if (c.op == Cond::All || c.op == Cond::Any) {   // nested parts resolve their params here
        for (auto& part : c.parts)
            if (test(n, part, part.param.empty() ? -1 : state_.indexOf(subst(n, part.param))) != (c.op == Cond::All)) return c.op == Cond::Any;
        return c.op == Cond::All;
    }
    double v = 0;
    if (!c.var.empty()) {
        const std::string* s = lookup(n, c.var);
        std::string t = s ? *s : "";
        if (c.op == Cond::Set) return !t.empty();
        if (c.op == Cond::Equals || c.op == Cond::NotEquals) return (t == c.text) == (c.op == Cond::Equals);
        const char* q = t.c_str();
        if (!parseNumber(q, q + t.size(), v)) return false;
    } else if (p < 0) {
        return c.op == Cond::NotEquals;
    } else {
        v = state_.get(p);
    }
    double ref = c.op == Cond::Set ? state_.def(p).min : c.value, eps = 1e-9 * std::max(1.0, std::fabs(ref));
    switch (c.op) {
    case Cond::Set: return std::fabs(v - ref) > eps;
    case Cond::Equals: return std::fabs(v - ref) <= eps;
    case Cond::NotEquals: return std::fabs(v - ref) > eps;
    case Cond::AtLeast: return v >= ref - eps;
    case Cond::AtMost: return v <= ref + eps;
    default: return true;
    }
}

// Also passes each embed's enabled state down: a disabled embed disables everything inside it. In
// a view with "animate" a widget that appears grows from nothing and one that hides shrinks first.
bool Gui::conds(Node& n, bool anim) {
    bool moved = false;
    for (size_t i = 0; n.view && i < n.w.size(); ++i) {
        const Widget& w = n.view->widgets[i];
        Inst& s = n.w[i];
        bool shown = test(n, w.showIf, s.cond[0]), on = test(n, w.enableIf, s.cond[1]);
        if (shown != (s.shown && !s.hiding)) {
            moved = true;
            if (anim && n.view->animate > 0) {
                if (shown && !s.shown) s.animH = 0;
                if (!shown && s.animH < 0) s.animH = s.r.h;
                s.shown = true;
                s.hiding = !shown;
                animating_ = true;
            } else {
                s.shown = shown;
                s.hiding = false;
                s.animH = -1;
            }
        }
        if (on != s.enabled) { s.enabled = on; invalidate(rectOf(n, (int)i) & n.clip); }
        if (!s.child) continue;
        bool inside = enabled(n, (int)i);
        if (inside != s.child->enabled) { s.child->enabled = inside; invalidate(rectOf(n, (int)i) & n.clip); }
        moved |= conds(*s.child, anim);
    }
    return moved;
}

void Gui::update() {
    if (conds(root_, true)) relayout();
}

// A root view with "fit" makes the window the bounding box of its visible widgets.
void Gui::relayout() {
    flow(root_);
    if (root_.view && root_.view->fit) {
        int w = 1, h = 1;
        for (auto& s : root_.w)
            if (visible(s)) { w = std::max(w, s.r.x + s.r.w); h = std::max(h, s.r.y + s.r.h); }
        if (w != w_ || h != h_) {
            w_ = w;
            h_ = h;
            canvas_.assign((size_t)w_ * h_, 0);
            dirty_ = {};
            resizeWindow();
        }
    }
    place(root_, 0, 0, {0, 0, w_, h_});
    invalidateAll();
}

int Gui::fullHeight(const Node& n, int i) const {
    const Widget& w = n.view->widgets[i];
    const Inst& s = n.w[i];
    return w.fit && s.child && s.child->view ? s.child->h : w.rect.h;
}

// Each widget's rect: its own, the size of the view it shows ("fit"), its animated height; then
// "flow": "column" stacks the visible ones in file order from the first widget's y.
void Gui::flow(Node& n) {
    if (!n.view) return;
    for (size_t i = 0; i < n.w.size(); ++i) {
        const Widget& w = n.view->widgets[i];
        Inst& s = n.w[i];
        s.r = w.rect;
        if (s.child) {
            flow(*s.child);
            if (w.fit && s.child->view) {
                s.r.w = s.child->view->w;
                s.r.h = s.child->h;
            }
        }
        if (s.animH >= 0) s.r.h = std::min(s.animH, s.r.h);
    }
    if (!n.view->flow) {
        n.h = n.view->h;
        return;
    }
    int y = n.w.empty() ? 0 : n.view->widgets[0].rect.y, count = 0;
    for (auto& s : n.w)
        if (visible(s)) {
            if (count++) y += n.view->gap;
            s.r.y = y;
            y += s.r.h;
        }
    n.h = y;
}

// One 30 Hz frame of animated flow: each animating widget grows or shrinks by its view's step
// (the rack: min(h + 25, full) opening, max(h, 25) - 25 closing); a scroll with "follow" keeps a
// growing widget inside the view.
void Gui::animate() {
    bool any = false;
    std::vector<std::pair<Node*, int>> growing;
    std::function<void(Node&)> step = [&](Node& n) {
        for (size_t i = 0; n.view && i < n.w.size(); ++i) {
            Inst& s = n.w[i];
            if (s.animH >= 0) {
                int full = fullHeight(n, (int)i), by = std::max(1, n.view->animate);
                if (s.hiding) {
                    s.animH = std::max(s.animH, by) - by;
                    if (s.animH == 0) { s.shown = false; s.hiding = false; s.animH = -1; }
                } else {
                    s.animH = std::min(s.animH + by, full);
                    if (s.animH >= full) s.animH = -1;
                    else growing.push_back({&n, (int)i});
                }
                any = any || s.animH >= 0;
            }
            if (s.child) step(*s.child);
        }
    };
    step(root_);
    animating_ = any;
    relayout();
    for (auto& g : growing) {
        Node* view = g.first;
        Node* p = view->parent;
        if (!p) continue;
        for (size_t k = 0; k < p->w.size(); ++k) {
            if (p->w[k].child.get() != view || !p->view->widgets[k].scroll.follow) continue;
            const Rect& r = view->w[g.second].r;
            int s = p->w[k].scroll, h = rectOf(*p, (int)k).h;
            if (r.y + r.h > s + h) s = r.y + r.h - h;
            if (r.y < s) s = r.y;
            setScroll(*p, (int)k, s);
        }
    }
}

void Gui::place(Node& n, int x, int y, Rect clip) {
    n.x = x;
    n.y = y;
    n.clip = clip;
    for (size_t i = 0; n.view && i < n.w.size(); ++i)
        if (n.w[i].child) placeChild(n, (int)i);
}

void Gui::placeChild(Node& n, int i) {
    Inst& s = n.w[i];
    Rect r = rectOf(n, i), content = r;
    int range = n.view->widgets[i].scroll.width > 0 ? std::max(0, s.child->h - r.h) : 0;
    s.scroll = std::clamp(s.scroll, 0, range);
    content.w -= bar(n, i).w;
    place(*s.child, r.x, r.y - s.scroll, n.clip & content);
}

int Gui::contentHeight(const Node& n, int i) const {
    const Widget& w = n.view->widgets[i];
    if (n.w[i].child) return n.w[i].child->h;
    if (w.kind == Kind::List) return listRows(n, i) * std::max(1, w.rowHeight + w.rowGap);
    return 0;
}

Rect Gui::bar(const Node& n, int i) const {
    const Widget& w = n.view->widgets[i];
    if (w.scroll.width <= 0 || (w.kind != Kind::List && !n.w[i].child)) return {};
    Rect r = rectOf(n, i);
    if (!w.scroll.always && contentHeight(n, i) <= r.h) return {};
    int bw = std::min(w.scroll.width, r.w);
    return {r.x + r.w - bw, r.y, bw, r.h};
}

// The thumb's length is in proportion to the part shown, and at least the bar's width.
Rect Gui::thumb(const Node& n, int i) const {
    Rect b = bar(n, i);
    if (b.empty()) return b;
    int total = std::max(contentHeight(n, i), b.h), range = total - b.h;
    int th = std::clamp((int)((long long)b.h * b.h / total), std::min(b.w, b.h), b.h);
    return {b.x, b.y + (range > 0 ? (int)((long long)(b.h - th) * n.w[i].scroll / range) : 0), b.w, th};
}

void Gui::setScroll(Node& n, int i, int pixels) {
    Inst& s = n.w[i];
    int old = s.scroll;
    s.scroll = std::clamp(pixels, 0, std::max(0, contentHeight(n, i) - rectOf(n, i).h));
    if (s.child) placeChild(n, i);
    if (s.scroll == old) return;
    invalidate(rectOf(n, i) & n.clip);
    if (s.child) saveUi();
}

// An embed with scroll "reveal" centres the first radio button inside it that is on (a chooser's
// current choice), after an action changes pages or vars.
void Gui::reveal(Node& n) {
    for (size_t i = 0; n.view && i < n.w.size(); ++i) {
        Inst& s = n.w[i];
        if (!s.child) continue;
        const Node& c = *s.child;
        for (size_t k = 0; n.view->widgets[i].scroll.reveal && c.view && k < c.w.size(); ++k) {
            const Action& a = c.view->widgets[k].action;
            if (a.type == Action::Value && c.w[k].param >= 0 && state_.get(c.w[k].param) == a.amount) {
                setScroll(n, (int)i, c.w[k].r.y + c.w[k].r.h / 2 - rectOf(n, (int)i).h / 2);
                break;
            }
        }
        reveal(*s.child);
    }
}

// The innermost embed or list under the point whose content is taller than it.
Hit Gui::scroller(Node& n, int x, int y) {
    if (!n.view || !n.clip.contains(x, y)) return {};
    for (auto it = n.view->order.rbegin(); it != n.view->order.rend(); ++it) {
        const Widget& w = n.view->widgets[*it];
        Inst& s = n.w[*it];
        if (!visible(s) || !rectOf(n, *it).contains(x, y)) continue;
        bool tall = contentHeight(n, *it) > rectOf(n, *it).h;
        if (w.kind == Kind::List) {
            if (tall) return {&n, *it, gen_};
            continue;
        }
        if (!s.child) continue;
        if (!enabled(n, *it)) return {};
        if (Hit h = scroller(*s.child, x, y)) return h;
        if (w.scroll.width > 0 && tall) return {&n, *it, gen_};
    }
    return {};
}

// ---- values -------------------------------------------------------------------------------------

double Gui::plain(const Node& n, int i, int which) const {
    const Inst& s = n.w[i];
    int p = which ? s.param2 : s.param;
    return p >= 0 ? state_.get(p) : which ? s.local2 : s.local;
}

double Gui::norm(const Node& n, int i, int which) const {
    const Inst& s = n.w[i];
    int p = which ? s.param2 : s.param;
    if (p >= 0) return state_.toNormal(p, state_.get(p));
    const Widget& w = n.view->widgets[i];
    double v = which ? s.local2 : s.local;
    if (w.kind == Kind::Custom) return std::clamp(v, 0.0, 1.0);   // pads keep 0..1 locally
    return w.rangeMax == w.rangeMin ? 0 : std::clamp((v - w.rangeMin) / (w.rangeMax - w.rangeMin), 0.0, 1.0);
}

int Gui::stepsOf(const Node& n, int i) const {
    int p = n.w[i].param;
    return p >= 0 ? state_.def(p).steps : n.view->widgets[i].steps;
}

int Gui::index(const Node& n, int i) const {
    const Inst& s = n.w[i];
    if (s.param < 0) return (int)std::lround(s.local);
    int steps = state_.def(s.param).steps;
    double t = state_.toNormal(s.param, state_.get(s.param));
    return steps > 0 ? (int)std::lround(t * steps) : t >= 0.5 ? 1 : 0;
}

void Gui::setPlain(const Hit& h, double v, int which) {
    Inst& s = inst(h);
    int p = which ? s.param2 : s.param;
    if (p >= 0) {
        double old = state_.get(p);
        state_.set(p, v);
        if (state_.get(p) != old && host_ && state_.def(p).host) host_->edit(p, state_.get(p));
        for (int m : which ? std::vector<int>() : s.mirror) {   // "mirror": the same normalized value
            double before = state_.get(m);
            state_.set(m, state_.fromNormal(m, state_.toNormal(p, state_.get(p))));
            if (state_.get(m) != before && host_ && state_.def(m).host) host_->edit(m, state_.get(m));
        }
        for (int q : editAllParams(h, which)) {   // "editAll": the same value at every listed var value
            double before = state_.get(q);
            state_.set(q, state_.get(p));
            if (state_.get(q) != before && host_ && state_.def(q).host) host_->edit(q, state_.get(q));
        }
    } else {
        const Widget& w = wid(h);
        if (w.kind == Kind::Dial || w.kind == Kind::Number) {
            v = std::clamp(v, std::min(w.rangeMin, w.rangeMax), std::max(w.rangeMin, w.rangeMax));
            if (w.steps > 0 && w.rangeMax != w.rangeMin)
                v = w.rangeMin + std::round((v - w.rangeMin) / (w.rangeMax - w.rangeMin) * w.steps) * (w.rangeMax - w.rangeMin) / w.steps;
        }
        (which ? s.local2 : s.local) = v;
    }
    invalidate(rectOf(*h.node, h.i));
    sendMidi(h);
    update();
}

void Gui::setNorm(const Hit& h, double t, int which) {
    t = std::clamp(t, 0.0, 1.0);
    int p = which ? inst(h).param2 : inst(h).param;
    const Widget& w = wid(h);
    setPlain(h, p >= 0 ? state_.fromNormal(p, t) : w.kind == Kind::Custom ? t : w.rangeMin + t * (w.rangeMax - w.rangeMin), which);
}

void Gui::setParam(int p, double v) {
    if (p < 0) return;
    bool host = host_ && state_.def(p).host;
    double old = state_.get(p);
    if (host) host_->beginEdit(p);
    state_.set(p, v);
    if (host && state_.get(p) != old) host_->edit(p, state_.get(p));
    if (host) host_->endEdit(p);
    update();
}

void Gui::setParams(const std::vector<std::pair<int, double>>& values) {
    std::vector<int> gesture;
    for (auto& pv : values)
        if (pv.first >= 0 && host_ && state_.def(pv.first).host && std::find(gesture.begin(), gesture.end(), pv.first) == gesture.end()) {
            host_->beginEdit(pv.first);
            gesture.push_back(pv.first);
        }
    for (auto& pv : values) {
        if (pv.first < 0) continue;
        double old = state_.get(pv.first);
        state_.set(pv.first, pv.second);
        if (host_ && state_.def(pv.first).host && state_.get(pv.first) != old) host_->edit(pv.first, state_.get(pv.first));
    }
    for (int q : gesture) host_->endEdit(q);
    update();
}

double Gui::defaultOf(const Hit& h, int which) const {
    int p = which ? inst(h).param2 : inst(h).param;
    if (p >= 0) return state_.def(p).def;
    const Widget& w = wid(h);
    return w.kind == Kind::Custom ? w.value : w.def;
}

// "editAll": while its param is on, a widget whose param template holds {var} writes the params
// of every listed value of the var (the current one included), in the same gesture.
std::vector<int> Gui::paramsFor(const Node& n, const std::string& tpl) const {
    std::vector<int> out;
    auto add = [&](const std::string& t) {
        int q = state_.indexOf(subst(n, t));
        if (q >= 0 && std::find(out.begin(), out.end(), q) == out.end()) out.push_back(q);
    };
    int on = skin_->editAllParam.empty() ? -1 : state_.indexOf(skin_->editAllParam);
    std::string var = "{" + skin_->editAllVar + "}";
    if (on < 0 || state_.get(on) <= state_.def(on).min || tpl.find(var) == std::string::npos) {
        add(tpl);
        return out;
    }
    for (auto& v : skin_->editAllValues) {
        std::string t = tpl;
        for (size_t pos; (pos = t.find(var)) != std::string::npos;) t.replace(pos, var.size(), v);
        add(t);
    }
    return out;
}

// The params besides the widget's own that an edit of it writes under "editAll".
std::vector<int> Gui::editAllParams(const Hit& h, int which) const {
    const Widget& w = wid(h);
    const std::string& tpl = which ? (w.params.size() > 1 ? w.params[1] : std::string()) : (w.params.empty() ? w.param : w.params[0]);
    std::vector<int> out = tpl.empty() ? std::vector<int>() : paramsFor(*h.node, tpl);
    int self = which ? inst(h).param2 : inst(h).param;
    out.erase(std::remove(out.begin(), out.end(), self), out.end());
    return out;
}

// Only host params reach the host; host: false ones live in State alone.
void Gui::begin(const Hit& h) {
    std::vector<int> ps = {inst(h).param, inst(h).param2};
    ps.insert(ps.end(), inst(h).mirror.begin(), inst(h).mirror.end());
    for (int which : {0, 1})
        for (int q : editAllParams(h, which)) ps.push_back(q);
    for (int p : ps)
        if (p >= 0 && host_ && state_.def(p).host && std::find(editing_.begin(), editing_.end(), p) == editing_.end()) {
            host_->beginEdit(p);
            editing_.push_back(p);
        }
}

void Gui::end() {
    for (int p : editing_) host_->endEdit(p);
    editing_.clear();
}

// A dial with "midi" plays its value as pitch bend (14 bits) or a controller (7 bits), on change.
void Gui::sendMidi(const Hit& h) {
    const Widget& w = wid(h);
    if (w.kind != Kind::Dial || w.midi < 0) return;
    double t = norm(*h.node, h.i);
    int v = (int)std::lround(t * (w.midi == 128 ? 16383 : 127));
    Inst& s = inst(h);
    if (v == s.sent) return;
    s.sent = v;
    uint8_t m[3] = {0xe0, (uint8_t)(v & 127), (uint8_t)(v >> 7)};
    if (w.midi < 128) { m[0] = 0xb0; m[1] = (uint8_t)w.midi; m[2] = (uint8_t)v; }
    state_.pushMidi(m, 3);
}

// Incoming MIDI of a dial's kind moves it like a drag would (without sending it back).
void Gui::midiToDials(Node& n) {
    for (size_t i = 0; n.view && i < n.w.size(); ++i) {
        const Widget& w = n.view->widgets[i];
        Inst& s = n.w[i];
        if (s.child) midiToDials(*s.child);
        if (w.kind != Kind::Dial || w.midi < 0 || !visible(s)) continue;
        Hit h{&n, (int)i, gen_};
        if (pressed(h)) continue;
        int v = w.midi == 128 ? state_.pitchBend() + 8192 : state_.cc(w.midi);
        if (v < 0 || v == s.sent) continue;
        s.sent = v;
        begin(h);
        setNorm(h, v / (w.midi == 128 ? 16383.0 : 127.0));
        end();
    }
}

// ---- painting -----------------------------------------------------------------------------------

void Gui::invalidate(Rect r) {
    r = r & Rect{0, 0, w_, h_};
    if (r.empty()) return;
    dirty_ = dirty_ | r;
    if (window) platformInvalidate(window, r);
}

const std::vector<uint32_t>& Gui::pixels() {
    if (!dirty_.empty()) {
        Canvas c{canvas_.data(), w_, h_, dirty_};
        dirty_ = {};
        for (int y = c.clip.y; y < c.clip.y + c.clip.h; ++y) std::fill_n(&canvas_[(size_t)y * w_ + c.clip.x], c.clip.w, 0u);
        paint(c, root_);
        if (learnOn_ && live(learnTarget_)) {   // the MIDI learn target's outline
            Rect saved = c.clip;
            c.clip = c.clip & learnTarget_.node->clip;
            outline(c, rectOf(*learnTarget_.node, learnTarget_.i), skin_->learnOutline, skin_->lw());
            c.clip = saved;
        }
        if (!tipShown_.empty()) paintTip(c);
        for (auto& m : menus_) paintMenu(c, m);
    }
    return canvas_;
}

// Fills and widget artwork go through the skin's surface (skin.json "surface"); text never does.
void Gui::drawFill(Canvas& c, const Fill& f, Rect r, int tile, bool stretch) {
    if (f.image >= 0) drawImage(c, skin_->images[f.image], f.tile >= 0 ? f.tile : tile, r, stretch, surf());
    else if (f.hasColour) fillRect(c, r, f.colour, surf());
}

void Gui::drawCaption(Canvas& c, const Text& t, const std::string& s, Rect r, const int pad[4], int dx, int dy, bool multiline) {
    if (s.empty() || t.font < 0) return;
    Rect tr{r.x + pad[0] + dx, r.y + pad[1] + dy, r.w - pad[0] - pad[2], r.h - pad[1] - pad[3]};
    drawText(c, skin_->fonts[t.font], s, tr, t.align, t.valign, multiline);
}

void Gui::paint(Canvas& c, Node& n) {
    if (!n.view) return;
    Rect saved = c.clip;
    c.clip = saved & n.clip;
    if (!c.clip.empty()) {
        drawFill(c, n.view->fill, {n.x, n.y, n.view->w, n.h}, 0);
        Rect inner = c.clip;
        for (int i : n.view->order) {
            Rect r = rectOf(n, i);
            c.clip = inner & r;
            if (visible(n.w[i]) && !c.clip.empty()) paintWidget(c, n, i, r);
        }
    }
    c.clip = saved;
}

bool Gui::activeAction(const Action& a) const {
    if (a.type == Action::Set) {
        for (auto& kv : a.vars) {
            const std::string* v = nullptr;
            for (auto& s : vars_)
                if (s.first == kv.first) v = &s.second;
            if (!v || *v != kv.second) return false;
        }
        return !a.vars.empty();
    }
    return a.type == Action::Goto && shownIn(a.stack, a.target, a.vars);
}

bool Gui::active(const Node& n, int i) const { return activeAction(n.view->widgets[i].action); }

bool Gui::enabled(const Node& n, int i) const { return n.enabled && !n.view->widgets[i].disabled && n.w[i].enabled; }

// Button paint: the strip holds [normal][pressed][hover] groups (each present only
// when flagged) plus a disabled tile; pressed wins over hover, the value clamps inside its group.
int Gui::buttonTile(const Node& n, int i) const {
    const Widget& w = n.view->widgets[i];
    if (w.fill.tile >= 0 || w.fill.image < 0) return std::max(w.fill.tile, 0);
    int tiles = skin_->images[w.fill.image].tiles;
    if (tiles < 2) return 0;
    if (!enabled(n, i) && w.disabledTile) return tiles - 1;
    int states = std::max(1, (tiles - (w.disabledTile ? 1 : 0)) / (1 + w.hoverTiles + w.pressedTiles));
    Hit me{const_cast<Node*>(&n), i};
    bool pressed = w.pressedTiles && live(press_) && press_ == me;
    bool hovered = w.hoverTiles && live(hover_) && hover_ == me;
    int group = pressed ? 1 : hovered ? 1 + w.pressedTiles : 0;
    int value = w.source == Widget::MidiIn || w.source == Widget::Modified ? sourceValue(w) == "1"
              : w.action.type == Action::Goto || w.action.type == Action::Set ? active(n, i)
              : w.action.type == Action::Value ? n.w[i].param >= 0 && state_.get(n.w[i].param) == w.action.amount
              : w.action.type == Action::Step ? 0 : index(n, i);
    return std::clamp(value, 0, states - 1) + states * group;
}

// What a "source" shows: CPU load in percent or the voice count as text, MIDI activity or the
// modified state as "0" / "1".
std::string Gui::sourceValue(const Widget& w) const {
    switch (w.source) {
    case Widget::Cpu: return formatInt(w.format.empty() ? "%d" : w.format, (int)std::lround(state_.load() * 100));
    case Widget::MidiIn: return Clock::now() < midiUntil_ ? "1" : "0";
    case Widget::Modified: return state_.modified() ? "1" : "0";
    case Widget::Voices: return state_.voices() < 0 ? "" : formatInt(w.format.empty() ? "%d" : w.format, state_.voices());
    case Widget::Scale: return std::to_string(scale_) + "x";
    default: return "";
    }
}

// Plates and textboxes: the status line when they show it, a runtime value, text data, their text.
std::string Gui::shownText(Node& n, int i) {
    const Widget& w = n.view->widgets[i];
    Inst& s = n.w[i];
    if (w.source == Widget::Cpu || w.source == Widget::Voices || w.source == Widget::Scale) return s.data = sourceValue(w);
    s.stored = !s.key.empty() && state_.hasData(s.key);
    s.data = s.stored ? state_.data(s.key) : std::string();
    if (w.status && !status_.empty()) return status_;
    return s.stored ? s.data : s.text;
}

// "valueText": the table's cell [row][col], each index fixed, a param's value, or else v (the widget's
// value) less "first". Null when there is none.
const Json& Gui::valueCell(const Node& n, int i, double v) const {
    const Widget::ValueText& vt = n.view->widgets[i].valueText;
    if (vt.table.empty()) return skin_->table("");
    Hit h{const_cast<Node*>(&n), i, gen_};
    auto index = [&](int fixed, const std::string& param) -> long {
        if (fixed >= 0) return fixed;
        int p = param.empty() ? -1 : paramOf(h, param);
        return std::lround(p >= 0 ? state_.get(p) : v - vt.first);
    };
    long r = index(vt.row, vt.rowParam), c = index(vt.col, vt.colParam);
    return r >= 0 && c >= 0 ? skin_->table(vt.table)[(size_t)r][(size_t)c] : skin_->table("");
}

std::string Gui::numberText(const Node& n, int i) const {
    const Widget& w = n.view->widgets[i];
    double v = plain(n, i);
    if (w.hasZeroText && v == 0) return w.zeroText;
    const Widget::ValueText& vt = w.valueText;
    if (vt.scale != 0 && vt.table.empty())   // "valueText" scaled: value * scale through its format
        return formatNumber(safeFormat(vt.format) ? vt.format.c_str() : "%g", v * vt.scale);
    const Json& cell = valueCell(n, i, v);
    if (cell.type == Json::String && !cell.s.empty()) return cell.s;   // "valueText" from a table, else the plain number
    ParamDef local;
    local.min = w.rangeMin;
    local.max = w.rangeMax;
    int p = n.w[i].param;
    return displayText(p >= 0 ? state_.def(p) : local, v, w.format);
}

void Gui::paintScrollbar(Canvas& c, const Node& n, int i) {   // the track stretched to the full height, the thumb over it
    Rect b = bar(n, i);
    if (b.empty()) return;
    const Widget& w = n.view->widgets[i];
    Rect t = thumb(n, i);
    if (w.scroll.track >= 0) drawImage(c, skin_->images[w.scroll.track], 0, b, true, surf());
    else fillRect(c, b, 0xff303030);
    if (w.scroll.thumb >= 0) drawImage(c, skin_->images[w.scroll.thumb], pressed(Hit{const_cast<Node*>(&n), i}) ? 1 : 0, t, true, surf());
    else fillRect(c, t, 0xff909090);
}

void Gui::paintWidget(Canvas& c, Node& n, int i, Rect r) {
    const Widget& w = n.view->widgets[i];
    struct Keep { bool& on; bool was; ~Keep() { on = was; } } keep{surfOn_, surfOn_};   // restored after the widget
    surfOn_ = surfOn_ && w.surface;
    Inst& s = n.w[i];
    static const int noPad[4] = {0, 0, 0, 0};
    if (s.param >= 0) s.drawn = state_.get(s.param);
    if (s.param2 >= 0) s.drawn2 = state_.get(s.param2);
    switch (w.kind) {
    case Kind::Plate:
        drawFill(c, w.fill, r, w.source == Widget::MidiIn || w.source == Widget::Modified ? sourceValue(w) == "1" : 0);
        drawCaption(c, w.text, withData(n, shownText(n, i)), r, w.pad, 0, 0, true);
        break;
    case Kind::Button:
    case Kind::Dropdown: {
        drawFill(c, w.fill, r, buttonTile(n, i));
        bool pressed = w.pressedTiles && live(press_) && press_ == Hit{&n, i};   // offset only with pressed tiles
        std::string caption = w.source == Widget::Cpu || w.source == Widget::Voices || w.source == Widget::Scale ? sourceValue(w) : s.text;
        if (w.captionFromItem) {
            std::function<bool(const std::vector<Item>&)> find = [&](const std::vector<Item>& items) {
                for (auto& it : items) {
                    if (!it.separator && it.items.empty() && it.action.type == Action::None && std::abs(it.value - plain(n, i)) < 1e-9) {
                        caption = it.shortLabel;
                        return true;
                    }
                    if (find(it.items)) return true;
                }
                return false;
            };
            find(w.items);
        }
        drawCaption(c, w.text, withData(n, caption), r, w.pad, pressed ? w.pressOffset[0] : 0, pressed ? w.pressOffset[1] : 0, true);
        break;
    }
    case Kind::Dial:
    case Kind::Meter:
        if (w.kind == Kind::Dial) drawFill(c, w.fill, r, 0);
        if (w.image >= 0 && w.slider && w.kind == Kind::Dial) {   // slider: tile 0 moved along the axis
            const Image& img = skin_->images[w.image];
            Rect t = img.tile(0);
            double v = norm(n, i);
            int x = w.vertical ? r.x : r.x + (int)(v * (r.w - t.w - 1));
            int y = w.vertical ? r.y + (r.h - t.h) - (int)(v * (r.h - t.h - 1)) : r.y;
            drawImage(c, img, 0, {x, y, t.w, t.h}, false, surf());
        } else if (w.image >= 0) {
            const Image& img = skin_->images[w.image];
            // "frames": "floor": frame floor(level * frames / range), the last frame at full scale
            int tile = w.floorFrames ? std::min((int)std::floor(norm(n, i) * img.tiles), img.tiles - 1)
                                     : (int)std::lround(norm(n, i) * (img.tiles - 1));
            drawImage(c, img, tile, r, false, surf());
        }
        break;
    case Kind::Number:
        drawFill(c, w.fill, r, 0);
        if (editing(n, i)) paintEdit(c);
        else drawCaption(c, w.text, numberText(n, i), r, w.pad, 0, 0, false);
        break;
    case Kind::Textbox:
        drawFill(c, w.fill, r, 0);
        if (editing(n, i)) paintEdit(c);
        else drawCaption(c, w.text, shownText(n, i), r, w.pad, 0, 0, w.multiline);
        break;
    case Kind::Plot: {   // the processor's scope values across the width, or zero data (a trace on the bottom row, no bars)
        drawFill(c, w.fill, r, 0);
        std::vector<int> col((size_t)std::max(r.w, 0), 0);
        float vals[256];
        int n = w.source == Widget::Scope ? std::min(state_.scope(vals, 256), 256) : 0;
        auto height = [&](float v) { return (int)std::lround(std::clamp(v, 0.0f, 1.0f) * (r.h - 1)); };
        if (n > 0 && w.drawMode == 0) {   // a trace: each column reads the value under it
            for (int x = 0; x < r.w; ++x) col[(size_t)x] = height(vals[(size_t)x * n / r.w]);
        } else if (n > 0) {               // bars: one per value, centred on its share of the width
            int bw = std::max(1, r.w / n / 3);
            for (int k = 0; k < n; ++k) {
                int x = (int)((k + 0.5) * r.w / n) - bw / 2;
                for (int b = 0; b < bw; ++b)
                    if (x + b >= 0 && x + b < r.w) col[(size_t)(x + b)] = height(vals[k]);
            }
        }
        drawScope(c, r, col, w.drawMode, w.line, w.line2, skin_->lw());
        break;
    }
    case Kind::List:
        drawFill(c, w.fill, r, 0);
        if (w.rowHeight > 0 && (!w.columns.empty() || w.source != Widget::NoSource || !w.rows.empty())) paintList(c, n, i, r);
        else drawCaption(c, w.header, w.header.text, {r.x, r.y, r.w, w.rowHeight > 0 ? w.rowHeight : r.h}, noPad, 0, 0, false);
        break;
    case Kind::Embed:
        if (s.child) paint(c, *s.child);
        paintScrollbar(c, n, i);
        break;
    case Kind::Custom:
        drawFill(c, w.fill, r, 0);
        if (w.ops && w.ops->draw) w.ops->draw(*this, c, Hit{&n, i, gen_}, r);
        break;
    case Kind::Veil:
    case Kind::Tree:
        drawFill(c, w.fill, r, 0);
        break;
    }
}

// ---- lists --------------------------------------------------------------------------------------

// Rows of a list: a midi_map list's assignments ("cc | param name"), then its fixed rows unless
// already listed (a param with its CC or -1, a label whose CC cell is an internal param), then its
// static rows, leaving out one that names a param already listed.
std::vector<Gui::ListRow> Gui::listModel(const Node& n, int i) const {
    const Widget& w = n.view->widgets[i];
    std::vector<ListRow> rows;
    std::vector<std::string> names;
    if (w.source == Widget::MidiMap) {
        auto map = state_.midiMap();
        for (auto& cp : map) {
            rows.push_back({{std::to_string(cp.first), state_.def(cp.second).name}, cp.first, cp.second, -1});
            names.push_back(state_.def(cp.second).name);
        }
        for (auto& f : w.fixed) {
            if (!f.ccParam.empty()) {
                int q = state_.indexOf(f.ccParam);
                if (q >= 0) rows.push_back({{std::to_string((int)std::lround(state_.get(q))), f.label}, -1, -1, q});
                continue;
            }
            int q = state_.indexOf(f.param);
            if (q < 0 || std::any_of(map.begin(), map.end(), [&](const std::pair<int, int>& cp) { return cp.second == q; })) continue;
            rows.push_back({{"-1", f.label.empty() ? state_.def(q).name : f.label}, -1, q, -1});
            names.push_back(rows.back().cells[1]);
        }
    }
    for (auto row : w.rows) {
        for (auto& cell : row) cell = withData(n, cell);
        if (row.empty() || std::find(names.begin(), names.end(), row.back()) == names.end()) rows.push_back({row, -1, -1, -1});
    }
    const Json& d = w.json["dataRows"];   // after them, a row for each line of a text data key
    if (d.has("key")) {
        std::string ref = subst(n, d["key"].str()), text = state_.hasData(ref) ? state_.data(ref) : std::string();
        for (size_t p = 0, k = 1; p < text.size(); ++k) {
            size_t e = std::min(text.find('\n', p), text.size());
            std::vector<std::string> fields;   // the line's tab-separated fields: {1}, {2}, ...; {n} is the row's number
            for (size_t f = p; f <= e;) {
                size_t t = std::min(text.find('\t', f), e);
                fields.push_back(text.substr(f, t - f));
                f = t + 1;
            }
            std::vector<std::string> cells;
            for (auto& tpl : d["row"].items) {
                std::string c = tpl.str(), out;
                for (size_t q = 0; q < c.size(); ++q) {
                    if (c[q] == '{' && q + 2 < c.size() && c[q + 2] == '}' && (c[q + 1] == 'n' || (c[q + 1] >= '1' && c[q + 1] <= '9'))) {
                        size_t f = (size_t)(c[q + 1] - '1');
                        out += c[q + 1] == 'n' ? std::to_string(k) : f < fields.size() ? fields[f] : std::string();
                        q += 2;
                    } else out += c[q];
                }
                cells.push_back(out);
            }
            rows.push_back({cells, -1, -1, -1});
            p = e + 1;
        }
    }
    return rows;
}

// "{data:key|default}" anywhere in a caption or cell: the text data under key (with {vars}, which
// the caption's own {vars} have already filled), or the default until there is some.
std::string Gui::withData(const Node& n, const std::string& s) const {
    std::string out;
    size_t p = 0;
    for (size_t at; (at = s.find("{data:", p)) != std::string::npos;) {
        size_t end = s.find('}', at + 6);
        if (end == std::string::npos) break;
        std::string ref = s.substr(at + 6, end - at - 6), def;
        size_t bar = ref.find('|');
        if (bar != std::string::npos) def = ref.substr(bar + 1), ref = ref.substr(0, bar);
        ref = subst(n, ref);
        out += s.substr(p, at - p) + (state_.hasData(ref) && !state_.data(ref).empty() ? state_.data(ref) : def);
        p = end + 1;
    }
    return out + s.substr(p);
}

// A chooser list's "set" value for a row: one number for every row, or an array of one per row.
static double listSetValue(const Json& j, int row) { return j.type == Json::Array ? j[(size_t)row].num() : j.num(); }

// What a bound list's row sets: a static row, the list's param to its "values" entry (or "first" + row)
// with its "set" values; a "dataRows" row, their "param" (else the list's) to their "first" + its index
// among them with their "set" values.
std::vector<std::pair<int, double>> Gui::listRowParams(const Hit& h, int row) const {
    const Widget& w = h.node->view->widgets[h.i];
    const Json& values = w.json["values"];
    std::vector<std::pair<int, double>> v;
    if (row < (int)w.rows.size()) {
        v.push_back({h.node->w[h.i].param, values.size() ? values[(size_t)row].num() : row + w.json["first"].num()});
        for (auto& m : w.json["set"].members) v.push_back({paramOf(h, m.first), listSetValue(m.second, row)});
        return v;
    }
    const Json& d = w.json["dataRows"];
    row -= (int)w.rows.size();
    v.push_back({d.has("param") ? paramOf(h, d["param"].str()) : h.node->w[h.i].param, row + d["first"].num()});
    for (auto& m : d["set"].members) v.push_back({paramOf(h, m.first), listSetValue(m.second, row)});
    return v;
}

// Rows from the top, each rowHeight tall and rowGap apart over gapFill, down to the bottom even
// past the data; cells from the left, colGap apart, in the row text style inset by the widget's pad.
void Gui::paintList(Canvas& c, Node& n, int i, Rect r) {
    const Widget& w = n.view->widgets[i];
    const Inst& s = n.w[i];
    Rect content = r;
    content.w -= bar(n, i).w;
    auto model = listModel(n, i);
    int pitch = std::max(1, w.rowHeight + w.rowGap), rows = (int)model.size(), sel = s.row;
    if (s.param >= 0) {   // a bound list shows the first row whose params all hold what it sets
        Hit h{&n, i, gen_};
        sel = -1;
        for (int k = 0; k < rows && sel < 0; ++k) {
            bool held = true;
            for (auto& pv : listRowParams(h, k)) held = held && pv.first >= 0 && state_.get(pv.first) == pv.second;
            if (held) sel = k;
        }
        if (sel != s.revealed) {   // a selection that moved (a load, the host, another list) scrolls into view, once
            Inst& m = n.w[i];
            m.revealed = sel;
            int top = sel * pitch;
            if (sel >= 0 && (top < m.scroll || top + w.rowHeight > m.scroll + content.h))
                m.scroll = std::clamp(top - (content.h - w.rowHeight) / 2, 0, std::max(0, contentHeight(n, i) - r.h));
        }
    }
    Rect saved = c.clip;
    c.clip = c.clip & content;
    if (w.hasGapFill) fillRect(c, content, w.gapFill);
    for (int k = s.scroll / pitch;; ++k) {
        int y = content.y + k * pitch - s.scroll;
        if (y >= content.y + content.h) break;
        Rect row{content.x, y, content.w, w.rowHeight};
        if (k == sel && k < rows && w.hasSelectFill) fillRect(c, row, w.selectFill);
        else if (w.hasRowFill) fillRect(c, row, w.rowFill);
        if (k >= rows || w.row.font < 0) continue;
        const auto& cells = model[(size_t)k].cells;
        int x = content.x;
        for (size_t col = 0; col < cells.size(); ++col) {
            int cw = col < w.columns.size() ? w.columns[col].width : content.x + content.w - x;
            if (col < w.columns.size() && w.columns[col].image >= 0) {   // an icon: tile 1 on the selected row when there is one
                const Image& img = skin_->images[w.columns[col].image];
                Rect t = img.tile(0);
                if (!cells[col].empty())
                    drawImage(c, img, k == sel && img.tiles > 1 ? 1 : 0, {x + (cw - t.w) / 2, y + (w.rowHeight - t.h) / 2, t.w, t.h});
                x += cw + w.colGap;
                continue;
            }
            Text style = k == sel && w.selectRow.font >= 0 ? w.selectRow : w.row;
            if (col < w.columns.size()) style.align = w.columns[col].align;
            drawCaption(c, style, cells[col], {x, y, cw, w.rowHeight}, w.pad, 0, 0, false);
            x += cw + w.colGap;
        }
    }
    c.clip = saved;
    if (editing(n, i)) paintEdit(c);
    paintScrollbar(c, n, i);
}

// A press selects the row under it (and gives the list the keyboard for Delete); a double-click on
// an "edit": "int" cell of a source row edits it.
void Gui::listDown(const Hit& h, int x, int y, bool dbl) {
    const Widget& w = wid(h);
    Inst& s = inst(h);
    Rect r = rectOf(*h.node, h.i);
    int pitch = std::max(1, w.rowHeight + w.rowGap), row = (y - r.y + s.scroll) / pitch;
    if (row < 0 || row >= listRows(*h.node, h.i) || (y - r.y + s.scroll) % pitch >= w.rowHeight) return;
    s.row = row;
    if (s.param >= 0) setParams(listRowParams(h, row));   // a bound list: the row's params
    listFocus_ = h;
    if (window) platformFocus(window, true);
    invalidate(r);
    if (!dbl) return;
    int cx = r.x;
    for (size_t col = 0; col < w.columns.size(); ++col) {
        if (x >= cx && x < cx + w.columns[col].width) {
            if (w.columns[col].editInt) editCell(h, row, (int)col);
            return;
        }
        cx += w.columns[col].width + w.colGap;
    }
}

// { "midi_map": "remove" } drops the selected row's assignment (a fixed CC cell goes back to its
// default); "reset" drops them all and restores the skin's default assignments.
void Gui::listAction(const Hit& h, const Action& a) {
    Inst& s = inst(h);
    auto model = listModel(*h.node, h.i);
    if (a.target == "reset") {
        for (auto& cp : state_.midiMap()) state_.assign(cp.first, -1);
        applyLearnDefaults(state_, *skin_);
    } else if (a.target == "remove" && s.row >= 0 && s.row < (int)model.size()) {
        const ListRow& r = model[(size_t)s.row];
        if (r.cc >= 0) state_.assign(r.cc, -1);
        else if (r.ccParam >= 0) setParam(r.ccParam, state_.def(r.ccParam).def);
    }
    s.row = std::min(s.row, listRows(*h.node, h.i) - 1);
    invalidate(rectOf(*h.node, h.i));
}

// ---- input ----------------------------------------------------------------------------------------

Hit Gui::hit(Node& n, int x, int y) {
    if (!n.view || !n.clip.contains(x, y)) return {};
    for (auto it = n.view->order.rbegin(); it != n.view->order.rend(); ++it) {
        int i = *it;
        const Widget& w = n.view->widgets[i];
        Inst& s = n.w[i];
        if (!visible(s) || !rectOf(n, i).contains(x, y)) continue;
        if (s.child) {
            if (!enabled(n, i)) continue;
            if (bar(n, i).contains(x, y)) return {&n, i, gen_};
            if (Hit h = hit(*s.child, x, y)) return h;
            continue;
        }
        if (!enabled(n, i)) continue;
        bool input = w.kind == Kind::Button || w.kind == Kind::Dropdown || w.kind == Kind::Dial || w.kind == Kind::Number ||
                     w.kind == Kind::Veil || w.kind == Kind::List || !w.context.empty() ||
                     (w.kind == Kind::Textbox && w.editable && !w.key.empty()) ||
                     (w.kind == Kind::Custom && w.ops && (w.ops->down || w.ops->dbl || w.ops->right));
        if (input) return {&n, i, gen_};
    }
    return {};
}

// The topmost visible widget under the point that has a tip.
Hit Gui::tipAt(Node& n, int x, int y) {
    if (!n.view || !n.clip.contains(x, y)) return {};
    for (auto it = n.view->order.rbegin(); it != n.view->order.rend(); ++it) {
        Inst& s = n.w[*it];
        if (!visible(s) || !rectOf(n, *it).contains(x, y)) continue;
        if (s.child) {
            if (Hit h = tipAt(*s.child, x, y)) return h;
        } else if (!n.view->widgets[*it].tip.empty()) {
            return {&n, *it, gen_};
        }
    }
    return {};
}

void Gui::flip(const Hit& h) {
    Inst& s = inst(h);
    if (s.param < 0) {
        setPlain(h, s.local != 0 ? 0 : 1);
        return;
    }
    const ParamDef& d = state_.def(s.param);
    int k = index(*h.node, h.i);
    double v = d.steps >= 2 ? state_.fromNormal(s.param, double((k + 1) % (d.steps + 1)) / d.steps) : k ? d.min : d.max;
    begin(h);
    setPlain(h, v);
    end();
}

void Gui::act(const Hit& h) {
    if (wid(h).toggle) flip(h);
    runAction(*h.node, h.i, wid(h).action);
}

// May rebuild part of the tree: callers must not use their Hit afterwards.
void Gui::runAction(Node& n, int i, const Action& action) {
    const Action a = action;   // a copy: the tree (and a menu's items) may change under it
    switch (a.type) {
    case Action::None:
        return;
    case Action::Url:
        platformOpenUrl(a.target);
        return;
    case Action::Standalone:   // "settings": the app's audio and MIDI settings
        if (window && standalone()) platformAudioSettings(window);
        return;
    case Action::Scale:   // the window scale menu, under the widget
        scaleMenu(rectOf(n, i));
        return;
    case Action::File: {   // a native dialog; the chosen path goes to text data, for the product to act on
        std::string path;
        if (!window || !platformFileDialog(window, a.target == "save", a.title, a.vars, withData(n, a.nameKey), path)) return;
        state_.setData(subst(n, a.key), path);
        invalidateAll();
        return;
    }
    case Action::MidiMap:
        listAction({&n, i, gen_}, a);
        return;
    case Action::Presets:
        presetMenu(*this, {&n, i, gen_}, a);
        return;
    case Action::Sequence:
        sequence(n, a);
        return;
    case Action::Data:   // text data keys set to the given texts (a curve reset, a name)
        for (auto& kv : a.vars) state_.setData(subst(n, kv.first), kv.second);
        return;
    case Action::Step:
        if (n.w[i].param >= 0) {
            Hit h{&n, i, gen_};
            begin(h);
            setPlain(h, state_.get(n.w[i].param) + a.amount);
            end();
        }
        return;
    case Action::Set:
        for (auto& kv : a.vars) setVar(vars_, kv.first, kv.second);
        resolve(root_);
        break;
    case Action::Toggle: {
        Hit t;
        if (!findWidget(n, a.target, t, false) && !findWidget(root_, a.target, t, true)) return;
        inst(t).hidden = !inst(t).hidden;
        inst(t).toggled = true;
        break;
    }
    case Action::Value:   // a radio button: its param to this value, then its goto if it has one
        if (n.w[i].param >= 0) {
            Hit h{&n, i, gen_};
            begin(h);
            setPlain(h, a.amount);
            end();
        }
        if (a.stack.empty()) return;
        [[fallthrough]];
    case Action::Goto: {
        int k;
        Node* p = findEmbed(root_, a.stack, &k);
        if (!p || !skin_->view(a.target)) return;
        show(*p, k, a.target, a.vars);
        break;
    }
    }
    conds(root_, false);
    relayout();
    reveal(root_);
    saveUi();
}

// ---- step sequences ---------------------------------------------------------------------------------

// A three-component Tausworthe generator, seeded per editor.
uint32_t Gui::random() {
    uint32_t& s1 = taus_[0];
    uint32_t& s2 = taus_[1];
    uint32_t& s3 = taus_[2];
    s1 = ((s1 & 0xfffffffeu) << 12) ^ (((s1 << 13) ^ s1) >> 19);
    s2 = ((s2 & 0xfffffff8u) << 4) ^ (((s2 << 2) ^ s2) >> 25);
    s3 = ((s3 & 0xfffffff0u) << 17) ^ (((s3 << 3) ^ s3) >> 11);
    return s1 ^ s2 ^ s3;
}

// Params <prefix><n>.<leaf>, n = 1..count, as a step sequence. insert / delete shift every leaf at
// step index; reset sets one leaf row to its defaults; random fills it with random valid values,
// by these rules for the rows (on, tie, accent: a coin; octave -1..1; note_order: steps
// that are on 0..7, steps that are off the previous on-step's value; transpose: a major or minor
// scale degree, the scale picked once per row), uniform over the param's values otherwise.
void Gui::sequence(Node& n, const Action& a) {
    if (a.count < 1) return;
    auto id = [&](int step, const std::string& leaf) { return state_.indexOf(a.prefix + std::to_string(step) + "." + leaf); };
    if (a.target == "reset" || a.target == "random") {
        const std::string& leaf = a.leaf;
        if (a.target == "reset") {
            for (int step = 1; step <= a.count; ++step)
                if (int q = id(step, leaf); q >= 0) setParam(q, state_.def(q).def);
            return;
        }
        static const int major[7] = {0, 2, 4, 5, 7, 9, 11}, minor[7] = {0, 2, 3, 5, 7, 8, 10};
        const int* scale = major;
        double lastOn = 0;
        for (int step = 1; step <= a.count; ++step) {
            int q = id(step, leaf);
            if (q < 0) continue;
            uint32_t r = random();
            const ParamDef& d = state_.def(q);
            double v;
            if (leaf == "on" || leaf == "tie" || leaf == "accent") {
                v = r & 1;
            } else if (leaf == "octave") {
                v = (int)(r % 3) - 1;
            } else if (leaf == "note_order") {
                int on = id(step, "on");
                if (on < 0 || state_.get(on) != 0) lastOn = v = r & 7;
                else v = std::clamp(lastOn, -2.0, 9.0);
            } else if (leaf == "transpose") {
                if (step == 1) scale = (random() & 1) ? major : minor;   // step 1 takes a second value for the scale
                v = scale[r % 7];
            } else {
                double t = d.steps > 0 ? double(r % (uint32_t)(d.steps + 1)) / d.steps : (r >> 8) / double(1 << 24);
                v = state_.fromNormal(q, t);
            }
            setParam(q, v);
        }
        return;
    }
    std::string idx = subst(n, a.index);
    const char* p = idx.c_str();
    double at;
    if (!parseNumber(p, p + idx.size(), at)) return;
    int k = std::clamp((int)std::lround(at), 1, a.count);
    std::vector<std::string> leaves;
    std::string first = a.prefix + "1.";
    for (size_t q = 0; q < state_.size(); ++q)
        if (state_.def(q).id.compare(0, first.size(), first) == 0) leaves.push_back(state_.def(q).id.substr(first.size()));
    for (auto& leaf : leaves) {
        if (a.target == "insert") {   // index..count-1 one up (the last dropped), index to its defaults
            for (int step = a.count; step > k; --step)
                if (id(step, leaf) >= 0 && id(step - 1, leaf) >= 0) setParam(id(step, leaf), state_.get(id(step - 1, leaf)));
            if (id(k, leaf) >= 0) setParam(id(k, leaf), state_.def(id(k, leaf)).def);
        } else if (a.target == "delete") {   // index+1..count one down, count to its defaults
            for (int step = k; step < a.count; ++step)
                if (id(step, leaf) >= 0 && id(step + 1, leaf) >= 0) setParam(id(step, leaf), state_.get(id(step + 1, leaf)));
            if (id(a.count, leaf) >= 0) setParam(id(a.count, leaf), state_.def(id(a.count, leaf)).def);
        }
    }
}

// A press cut short (the tree rebuilt, the editor closed) ends as a release far outside, so held
// notes stop and springs return.
void Gui::releasePress() {
    if (!live(press_)) return;
    Hit h = press_;
    press_ = {};
    const Widget& w = wid(h);
    if (w.kind == Kind::Custom && w.ops->up) w.ops->up(*this, h, rectOf(*h.node, h.i), INT_MIN / 2, INT_MIN / 2);
    if (w.kind == Kind::Dial && w.spring >= 0) setNorm(h, w.spring);
}

void Gui::setLearnTarget(const Hit& h) {
    if (live(learnTarget_)) invalidate(rectOf(*learnTarget_.node, learnTarget_.i));
    int p = h ? inst(h).param : -1;
    learnTarget_ = p >= 0 && state_.def(p).host ? h : Hit{};
    state_.learn(learnTarget_ ? p : -1);
    if (live(learnTarget_)) invalidate(rectOf(*h.node, h.i));
}

void Gui::drag(const Hit& h, int x, int y, bool shift, bool start) {
    const Widget& w = wid(h);
    Rect r = rectOf(*h.node, h.i);
    if (w.kind == Kind::Dial && w.drag == DragAbsolute) {
        int len = std::max((w.vertical ? r.h : r.w) - 2 * w.margin - 1, 1);
        setNorm(h, w.vertical ? 1.0 - double(y - r.y - w.margin) / len : double(x - r.x - w.margin) / len);
        return;
    }
    if (w.kind == Kind::Dial && w.drag == DragRotary) {
        // Shortcut: a 270 degree sweep (7:30 to 4:30 o'clock) around the rect centre is assumed.
        double a = std::atan2(x - (r.x + r.w / 2.0), (r.y + r.h / 2.0) - y) * 57.29577951308232;
        setNorm(h, (a + 135) / 270);
        return;
    }
    // Relative drag: re-anchor while in range, so overshoot past an end is kept.
    bool vertical = w.kind == Kind::Number || w.vertical;
    int pos = vertical ? y : x;
    if (start) {
        anchorNorm_ = norm(*h.node, h.i);
        anchorPos_ = pos;
        zone_ = 0;   // "sensitivityZones": the column the drag starts in picks the rate
        if (w.kind == Kind::Number && !w.zones.empty() && r.w > 0)
            zone_ = w.zones[(size_t)std::clamp((int)((long long)(x - r.x) * (long long)w.zones.size() / r.w), 0, (int)w.zones.size() - 1)];
        return;
    }
    // Rates are per design pixel, so a skin at a higher density drags at the same speed on screen.
    double px = skin_->density;
    double perPixel = w.sensitivity / (200.0 * px);   // dials: a full range per 200 px at 1.0
    if (w.kind == Kind::Number) {                    // numbers: sensitivity is value units per pixel
        int p = inst(h).param;
        double span = p >= 0 ? state_.def(p).max - state_.def(p).min : w.rangeMax - w.rangeMin, units = zone_ > 0 ? zone_ : w.sensitivity;
        perPixel = units <= 0 ? 1 / (200.0 * px) : span != 0 ? units / std::fabs(span) / px : 0;   // a log taper gets the linear rate
    }
    double t = anchorNorm_ + (vertical ? anchorPos_ - pos : pos - anchorPos_) * perPixel * (shift ? w.fine : 1.0);
    if (w.kind == Kind::Number && w.hasRange && inst(h).param >= 0) {   // "range" bounds the drag within the param
        double a = state_.toNormal(inst(h).param, w.rangeMin), b = state_.toNormal(inst(h).param, w.rangeMax);
        t = std::clamp(t, std::min(a, b), std::max(a, b));
    }
    if (t >= 0 && t <= 1) {
        anchorNorm_ = t;
        anchorPos_ = pos;
    }
    setNorm(h, t);
}

void Gui::mouseDown(int x, int y, bool right, bool dbl, bool shift) {
    hideTip(true);
    if (!menus_.empty()) {   // a press inside a menu waits for its release; outside it only closes them
        size_t level;
        int row;
        if (menuAt(x, y, level, row)) menuPressed_ = true;
        else closeMenus();
        return;
    }
    if (editing_on_) {
        if (!right && edit_.r.contains(x, y)) {   // in the field: place the caret, or select all on a double-click
            edit_.caret = dbl ? edit_.text.size() : editIndexAt(x, y);
            if (dbl) edit_.anchor = 0;
            else if (!shift) edit_.anchor = edit_.caret;
            editDrag_ = !dbl;
            restartBlink();
            invalidate(edit_.r);
            return;
        }
        finishEdit(true);   // clicking elsewhere commits, and the click goes on
    }
    if (live(press_)) return;
    Hit h = hit(x, y);
    if (live(listFocus_) && !(h == listFocus_)) {
        listFocus_ = {};
        if (window && !wantsKeys()) platformFocus(window, false);
    }
    if (right) {   // context menus open on the release
        if (!h) scaleMenu({x, y, 0, 0});
        else if (wid(h).kind == Kind::Custom && wid(h).ops && wid(h).ops->right) wid(h).ops->right(*this, h, rectOf(*h.node, h.i), x, y, shift, false);
        return;
    }
    if (!h) return;
    if (learnOn_) setLearnTarget(h);   // MIDI learn: the clicked control learns
    const Widget& w = wid(h);
    Rect r = rectOf(*h.node, h.i);
    switch (w.kind) {
    case Kind::Button:
        press_ = h;
        invalidate(r);
        if (w.press == PressDown) act(h);
        else if (w.press != PressUp) {
            if (w.toggle) flip(h);
            else if (w.action.type != Action::Step) { begin(h); setPlain(h, 1); }
            nextRepeat_ = Clock::now() + ms(400);
            runAction(*h.node, h.i, w.action);
        }
        break;
    case Kind::Dropdown:
        dropdown(h);
        break;
    case Kind::Embed: {   // its scrollbar: drag the thumb, or page toward the press
        Rect t = thumb(*h.node, h.i);
        if (t.contains(x, y)) {
            press_ = h;
            anchorPos_ = y;
            anchorScroll_ = inst(h).scroll;
            invalidate(t);
        } else {
            setScroll(*h.node, h.i, inst(h).scroll + (y < t.y ? -r.h : r.h));
        }
        break;
    }
    case Kind::List: {
        Rect t = thumb(*h.node, h.i);
        if (!t.empty() && bar(*h.node, h.i).contains(x, y)) {
            if (t.contains(x, y)) { press_ = h; anchorPos_ = y; anchorScroll_ = inst(h).scroll; }
            else setScroll(*h.node, h.i, inst(h).scroll + (y < t.y ? -r.h : r.h));
        } else {
            listDown(h, x, y, dbl);
        }
        break;
    }
    case Kind::Custom:
        if (!w.ops) break;
        if (dbl && w.ops->dbl) w.ops->dbl(*this, h, r, x, y);
        else if (w.ops->down && w.ops->down(*this, h, r, x, y, shift)) press_ = h;
        break;
    case Kind::Textbox:
        if (!w.editable || w.key.empty()) break;
        if (w.editOnClick) {
            startEdit(h, true, x, y);
            editDrag_ = true;
        } else if (dbl) {
            startEdit(h, false, x, y);
        }
        break;
    case Kind::Number:
        if (dbl && w.editable) {
            startEdit(h, false, x, y);
            break;
        }
        if (!w.dragOn && !dbl) break;   // "drag": false
        [[fallthrough]];
    case Kind::Dial:
        begin(h);
        if (dbl) {   // "reset": the default, 0 when the range spans 0, or nothing
            int p = inst(h).param;
            double lo = p >= 0 ? state_.def(p).min : w.rangeMin, hi = p >= 0 ? state_.def(p).max : w.rangeMax;
            if (w.reset == Widget::ResetDefault) setPlain(h, defaultOf(h));
            else if (w.reset == Widget::ResetZero && std::min(lo, hi) < 0 && std::max(lo, hi) > 0) setPlain(h, 0);
            end();
            break;
        }
        press_ = h;
        drag(h, x, y, shift, true);
        break;
    default:   // a veil swallows the click
        break;
    }
    updateStatus();
}

void Gui::mouseMove(int x, int y, bool shift) {
    mouseX_ = x;
    mouseY_ = y;
    if (!menus_.empty()) {   // the highlight follows the pointer; leaving the menus keeps it
        size_t level;
        int row;
        if (menuAt(x, y, level, row)) hoverMenu(level, row);
        return;
    }
    if (editDrag_ && editing_on_) {
        edit_.caret = editIndexAt(x, y);
        invalidate(edit_.r);
        return;
    }
    if (live(press_)) {
        const Widget& w = wid(press_);
        if (w.kind == Kind::Dial || w.kind == Kind::Number) {
            drag(press_, x, y, shift, false);
        } else if (w.kind == Kind::Custom && w.ops->drag) {
            w.ops->drag(*this, press_, rectOf(*press_.node, press_.i), x, y, shift);
        } else if (w.kind == Kind::Embed || w.kind == Kind::List) {
            Node& n = *press_.node;
            Rect b = bar(n, press_.i), t = thumb(n, press_.i);
            int travel = b.h - t.h, range = contentHeight(n, press_.i) - b.h;
            if (travel > 0) setScroll(n, press_.i, anchorScroll_ + (int)((long long)(y - anchorPos_) * range / travel));
        }
        updateStatus();
        return;
    }
    Hit tip = tipAt(root_, x, y);
    if (skin_->tooltip.on) {   // shown after the pointer rests on one widget; moving to another hides it
        if (!(tip == tipHit_ && tip.gen == tipHit_.gen)) {
            hideTip(false);
            tipBlocked_ = false;
            tipHit_ = tip;
        }
        if (tipShown_.empty()) tipStill_ = Clock::now();
    } else {
        std::string text = tip ? wid(tip).tip : std::string();
        if (text != tip_) {
            tip_ = text;
            if (window) platformTip(window, tip_);
        }
    }
    Hit h = hit(x, y);
    if (!(h == hover_ && live(hover_))) {
        if (live(hover_)) {
            Rect r = rectOf(*hover_.node, hover_.i);
            invalidate(r);
            const Widget& o = wid(hover_);
            if (o.ops && o.ops->hover) o.ops->hover(*this, hover_, r, INT_MIN / 2, INT_MIN / 2);
        }
        hover_ = h;
        wheelAcc_ = 0;
        if (h) invalidate(rectOf(*h.node, h.i));
    }
    if (h && wid(h).ops && wid(h).ops->hover) wid(h).ops->hover(*this, h, rectOf(*h.node, h.i), x, y);
    updateStatus();
}

void Gui::mouseUp(int x, int y, bool shift) {
    if (!menus_.empty()) {
        // A release soon after opening is ignored (unless the menu was pressed), so a click opens
        // it and leaves it open. Later, a release over an item picks it. With a release guard (the
        // press-drag-release style) a release outside closes the menus; without one it is ignored,
        // so a click on a button that opens a menu below it leaves the menu open.
        const MenuStyle& st = *menus_[0].style;
        if (!menuPressed_ && Clock::now() - menuOpened_ < ms(st.releaseGuard)) return;
        size_t level;
        int row;
        if (menuAt(x, y, level, row)) {
            if (row >= 0 && menus_[level].items[row].items.empty() && !menus_[level].items[row].separator) pickMenu(menus_[level].items[row].id);
        } else if (st.releaseGuard > 0) {
            closeMenus();
        }
        return;
    }
    editDrag_ = false;
    if (!live(press_)) {
        press_ = {};
        return;
    }
    Hit h = press_;
    press_ = {};
    const Widget& w = wid(h);
    Rect r = rectOf(*h.node, h.i);
    invalidate(r);
    if (w.kind == Kind::Button && (w.press == PressMomentary || w.press == PressRepeat)) {
        if (w.toggle) flip(h);
        else if (w.action.type != Action::Step) { setPlain(h, 0); end(); }
        if (w.action.type != Action::Step) runAction(*h.node, h.i, w.action);   // a nudge acts only while held
    } else if (w.kind == Kind::Button && w.press == PressUp) {
        if (r.contains(x, y)) act(h);
    } else {
        if (w.kind == Kind::Custom && w.ops->up) w.ops->up(*this, h, r, x, y);
        if (w.kind == Kind::Dial && w.spring >= 0) setNorm(h, w.spring);
        end();
    }
    mouseMove(x, y, shift);
}

// Right releases: a custom kind's right hook, or the widget's context menu at the pointer.
void Gui::rightUp(int x, int y, bool shift) {
    if (!menus_.empty() || editing_on_) return;
    Hit h = hit(x, y);
    if (!h) return;
    const Widget& w = wid(h);
    if (w.kind == Kind::Custom && w.ops && w.ops->right) w.ops->right(*this, h, rectOf(*h.node, h.i), x, y, shift, true);
    else if (!w.context.empty()) contextMenu(h, x, y);
}

void Gui::mouseLeave() {
    hideTip(false);
    tipHit_ = {};
    if (live(press_) || !live(hover_)) return;
    Rect r = rectOf(*hover_.node, hover_.i);
    invalidate(r);
    const Widget& o = wid(hover_);
    if (o.ops && o.ops->hover) o.ops->hover(*this, hover_, r, INT_MIN / 2, INT_MIN / 2);
    hover_ = {};
    updateStatus();
}

// Dials and numbers with a "wheel" fraction move by it per notch (Shift multiplies it by fine); a
// stepped param keeps the unsnapped remainder between notches and lands on the nearest step.
// Elsewhere the wheel scrolls the embed or list under the pointer.
void Gui::wheel(int x, int y, double notches, bool shift) {
    if (live(press_) || !menus_.empty()) return;
    hideTip(true);
    Hit h = hit(x, y);
    if (h && (wid(h).kind == Kind::Dial || wid(h).kind == Kind::Number) && wid(h).wheel > 0) {
        const Widget& w = wid(h);
        int steps = stepsOf(*h.node, h.i);
        double target = norm(*h.node, h.i) + wheelAcc_ + notches * w.wheel * (shift ? w.fine : 1.0);
        target = std::clamp(target, 0.0, 1.0);
        double snapped = steps > 0 ? std::round(target * steps) / steps : target;
        wheelAcc_ = target - snapped;
        begin(h);
        setNorm(h, snapped);
        end();
        return;
    }
    // Shortcut: 30 px per notch in embeds (a fixed step, not measured), a row in lists.
    if (Hit e = scroller(root_, x, y)) {
        const Widget& w = wid(e);
        int step = w.kind == Kind::List ? std::max(1, w.rowHeight + w.rowGap) : skin_->dp(30);
        setScroll(*e.node, e.i, inst(e).scroll - (int)std::lround(notches * step));
    }
}

// ---- keyboard: text entry, menus, lists -------------------------------------------------------------

bool Gui::wantsKeys() const { return !menus_.empty() || editing_on_ || live(listFocus_); }

void Gui::focusLost() {
    closeMenus();
    finishEdit(true);
    listFocus_ = {};
}

bool Gui::keyDown(Key k, bool shift, bool ctrl) {
    if (!menus_.empty()) {   // arrows move over the top menu's items (skipping separators, wrapping), Enter picks
        Menu& m = menus_.back();
        size_t level = menus_.size() - 1;
        if (k == KeyEscape) {
            closeMenus();
            return true;
        }
        if (m.style->escapeOnly) return true;
        int n = (int)m.items.size();
        if (k == KeyLeft && level > 0) {
            closeMenus(level);
        } else if ((k == KeyEnter || k == KeyRight) && m.sel >= 0 && !m.items[m.sel].items.empty()) {
            hoverMenu(level, m.sel);   // opens the submenu
            if (menus_.size() > level + 1) keyDown(KeyHome, false, false);
        } else if (k == KeyEnter) {
            if (m.sel >= 0) pickMenu(m.items[m.sel].id);
        } else if (n && (k == KeyUp || k == KeyDown || k == KeyHome || k == KeyEnd)) {
            int dir = k == KeyUp || k == KeyEnd ? -1 : 1;
            int s = k == KeyHome ? -1 : k == KeyEnd ? n : m.sel < 0 && dir < 0 ? n : m.sel;
            for (int t = 0; t < n; ++t) {
                s = ((s + dir) % n + n) % n;
                if (!m.items[s].separator) break;
            }
            if (!m.items[s].separator) {
                m.sel = s;
                closeMenus(level + 1);
                invalidate(m.r);
            }
        }
        return true;
    }
    if (!editing_on_) {
        if (!live(listFocus_)) return false;
        if (k == KeyEscape) listFocus_ = {};
        else if (k == KeyDelete && wid(listFocus_).source == Widget::MidiMap) listAction(listFocus_, Action{Action::MidiMap, "remove"});
        return true;
    }
    Edit& e = edit_;
    size_t lo = std::min(e.anchor, e.caret), hi = std::max(e.anchor, e.caret), size = e.text.size();
    auto prev = [&](size_t i) { while (i > 0 && (e.text[--i] & 0xc0) == 0x80) {} return i; };
    auto next = [&](size_t i) { while (i < size && (e.text[++i] & 0xc0) == 0x80) {} return i; };
    bool move = true;   // a caret move; without Shift it drops the selection
    switch (k) {
    case KeyEscape:
        finishEdit(false);
        return true;
    case KeyTab:
        finishEdit(true);
        return true;
    case KeyEnter:
        if (!e.multiline || ctrl) {
            finishEdit(true);
            return true;
        }
        editInsert("\n");
        return true;
    case KeySelectAll:
        e.anchor = 0;
        e.caret = size;
        move = false;
        break;
    case KeyLeft:
        e.caret = lo != hi && !shift ? lo : prev(e.caret);
        break;
    case KeyRight:
        e.caret = lo != hi && !shift ? hi : next(e.caret);
        break;
    case KeyHome:
        while (e.caret > 0 && e.text[e.caret - 1] != '\n') --e.caret;
        break;
    case KeyEnd:
        while (e.caret < size && e.text[e.caret] != '\n') ++e.caret;
        break;
    case KeyUp:
    case KeyDown: {   // multi-line: the same x on the line above or below
        if (!e.multiline) break;
        const Font& f = skin_->fonts[e.style.font];
        Rect tr{e.r.x + e.pad[0], e.r.y + e.pad[1], e.r.w - e.pad[0] - e.pad[2], e.r.h - e.pad[1] - e.pad[3]};
        int lines = 1, ln = 0;
        for (size_t i = 0; i < size; ++i)
            if (e.text[i] == '\n') { ++lines; ln += i < e.caret; }
        if (k == KeyUp ? ln == 0 : ln + 1 == lines) break;
        size_t a = e.caret;
        while (a > 0 && e.text[a - 1] != '\n') --a;
        int top = tr.y + (e.style.valign == 1 ? (tr.h - f.height * lines) / 2 : e.style.valign == 2 ? tr.h - f.height * lines : 0);
        size_t lineEnd = std::min(e.text.find('\n', a), size);
        int tw = f.width(e.text.substr(a, lineEnd - a));
        int x = tr.x + (e.style.align == 1 ? (tr.w - tw) / 2 : e.style.align == 2 ? tr.w - tw : 0) + f.width(e.text.substr(a, e.caret - a));
        e.caret = editIndexAt(x, top + (ln + (k == KeyUp ? -1 : 1)) * f.height);
        break;
    }
    case KeyBackspace:
    case KeyDelete:
        if (lo == hi) {
            if (k == KeyBackspace) lo = prev(lo);
            else hi = next(hi);
        }
        e.text.erase(lo, hi - lo);
        e.caret = e.anchor = lo;
        move = false;
        break;
    case KeyNone:
        return true;
    }
    if (move && !shift) e.anchor = e.caret;
    restartBlink();
    invalidate(e.r);
    return true;
}

void Gui::keyChar(unsigned cp) {
    if (!editing_on_ || cp < 32 || cp == 127 || cp > 0x10ffff) return;
    std::string u;
    if (cp < 0x80) u += (char)cp;
    else if (cp < 0x800) { u += (char)(0xc0 | cp >> 6); u += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { u += (char)(0xe0 | cp >> 12); u += (char)(0x80 | (cp >> 6 & 63)); u += (char)(0x80 | (cp & 63)); }
    else { u += (char)(0xf0 | cp >> 18); u += (char)(0x80 | (cp >> 12 & 63)); u += (char)(0x80 | (cp >> 6 & 63)); u += (char)(0x80 | (cp & 63)); }
    editInsert(u);
}

void Gui::editInsert(const std::string& s) {
    size_t lo = std::min(edit_.anchor, edit_.caret), hi = std::max(edit_.anchor, edit_.caret);
    edit_.text.replace(lo, hi - lo, s);
    edit_.caret = edit_.anchor = lo + s.size();
    restartBlink();
    invalidate(edit_.r);
}

void Gui::restartBlink() {
    blinkStart_ = Clock::now();
    caretOn_ = true;
}

// Numbers (double-click when editable): the whole text selected, the caret at its start; they
// commit through the param's text parsing (labels, then the number; units are ignored) and keep
// their value if it does not parse. Textboxes (double-click, or a click with "editOn": "click",
// which places the caret): their text, stored under their key.
void Gui::startEdit(const Hit& h, bool atClick, int x, int y) {
    const Widget& w = wid(h);
    if (w.text.font < 0) return;   // nothing to type with
    closeMenus();
    Edit& e = edit_;
    e = Edit();
    e.h = h;
    e.r = rectOf(*h.node, h.i);
    e.style = w.text;
    std::copy(w.pad, w.pad + 4, e.pad);
    if (w.kind == Kind::Number) {
        e.text = numberText(*h.node, h.i);
        e.anchor = e.text.size();
        e.commit = [this, h](const std::string& t) {
            double v;
            int p = inst(h).param;
            const char* s = t.c_str();
            while (*s == ' ') ++s;
            if (!(p >= 0 ? state_.parse(p, t, v) : parseNumber(s, t.c_str() + t.size(), v))) return;
            begin(h);
            setPlain(h, v);
            end();
        };
    } else {
        const std::string& key = inst(h).key;
        e.multiline = w.multiline;
        e.text = state_.hasData(key) ? state_.data(key) : inst(h).text;
        if (w.multiline)
            for (size_t p; (p = e.text.find("\\n")) != std::string::npos;) e.text.replace(p, 2, "\n");
        e.caret = e.text.size();
        e.commit = [this, key](const std::string& t) { state_.setData(key, t); };
    }
    editing_on_ = true;
    if (atClick) e.caret = e.anchor = editIndexAt(x, y);
    restartBlink();
    hideTip(true);
    if (window) platformFocus(window, true);
    invalidate(e.r);
    updateStatus();
}

// A list's "edit": "int" cell (0..127): an assignment or a fixed param row moves to the typed CC,
// a fixed label row sets its internal param.
void Gui::editCell(const Hit& h, int row, int col) {
    const Widget& w = wid(h);
    auto model = listModel(*h.node, h.i);
    if (w.row.font < 0 || row >= (int)model.size()) return;
    const ListRow r = model[(size_t)row];
    if (r.param < 0 && r.ccParam < 0) return;
    Rect wr = rectOf(*h.node, h.i);
    int pitch = std::max(1, w.rowHeight + w.rowGap), x = wr.x;
    for (int c = 0; c < col; ++c) x += w.columns[c].width + w.colGap;
    Edit& e = edit_;
    e = Edit();
    e.h = h;
    e.r = {x, wr.y + row * pitch - inst(h).scroll, w.columns[col].width, w.rowHeight};
    e.style = w.row;
    e.style.align = w.columns[col].align;
    std::copy(w.pad, w.pad + 4, e.pad);
    e.text = r.cells[(size_t)col];
    e.caret = 0;
    e.anchor = e.text.size();
    e.commit = [this, r](const std::string& t) {
        double v;
        const char* s = t.c_str();
        while (*s == ' ') ++s;
        if (!parseNumber(s, t.c_str() + t.size(), v)) return;
        int to = std::clamp((int)std::lround(v), 0, 127);
        if (r.ccParam >= 0) {
            setParam(r.ccParam, to);
            return;
        }
        if (to == r.cc) return;
        if (r.cc >= 0) state_.assign(r.cc, -1);
        state_.assign(to, r.param);
    };
    editing_on_ = true;
    restartBlink();
    if (window) platformFocus(window, true);
    invalidate(e.r);
}

void Gui::finishEdit(bool commit) {
    if (!editing_on_) return;
    editing_on_ = false;
    editDrag_ = false;
    Edit e = std::move(edit_);
    edit_ = Edit();
    if (live(e.h)) {
        invalidate(e.r);
        if (commit && e.commit) e.commit(e.text);
    }
    if (window && !wantsKeys()) platformFocus(window, false);
    updateStatus();
}

namespace {
struct EditLine {
    size_t a, b;   // bytes of the text
    int x, y;      // where the line starts
};
}

// The edited text laid out as drawText lays out a caption: lines at newlines, in the padded field.
static std::vector<EditLine> editLines(const Font& f, const Text& style, const int pad[4], Rect r, const std::string& s) {
    Rect tr{r.x + pad[0], r.y + pad[1], r.w - pad[0] - pad[2], r.h - pad[1] - pad[3]};
    std::vector<EditLine> lines;
    for (size_t a = 0;;) {
        size_t b = std::min(s.find('\n', a), s.size());
        lines.push_back({a, b, 0, 0});
        if (b == s.size()) break;
        a = b + 1;
    }
    int total = f.height * (int)lines.size();
    int y = tr.y + (style.valign == 1 ? (tr.h - total) / 2 : style.valign == 2 ? tr.h - total : 0);
    for (auto& l : lines) {
        int tw = f.width(s.substr(l.a, l.b - l.a));
        l.x = tr.x + (style.align == 1 ? (tr.w - tw) / 2 : style.align == 2 ? tr.w - tw : 0);
        l.y = y;
        y += f.height;
    }
    return lines;
}

size_t Gui::editIndexAt(int x, int y) const {
    const Font& f = skin_->fonts[edit_.style.font];
    auto lines = editLines(f, edit_.style, edit_.pad, edit_.r, edit_.text);
    size_t k = 0;
    while (k + 1 < lines.size() && y >= lines[k + 1].y) ++k;
    const EditLine& l = lines[k];
    int px = l.x;
    for (size_t i = l.a; i < l.b;) {
        size_t j = i + 1;
        while (j < l.b && (edit_.text[j] & 0xc0) == 0x80) ++j;
        int gw = f.width(edit_.text.substr(i, j - i));
        if (x < px + gw / 2) return i;
        px += gw;
        i = j;
    }
    return l.b;
}

// The selection band and the caret in the font's colours ("fonts" in skin.json), the text in its own.
// Shortcut: text wider than the field is clipped, not scrolled to keep the caret in view.
void Gui::paintEdit(Canvas& c) {
    const Edit& e = edit_;
    const Font& f = skin_->fonts[e.style.font];
    Rect saved = c.clip;
    c.clip = c.clip & e.r;
    size_t lo = std::min(e.anchor, e.caret), hi = std::max(e.anchor, e.caret);
    for (auto& l : editLines(f, e.style, e.pad, e.r, e.text)) {
        size_t a = std::clamp(lo, l.a, l.b), b = std::clamp(hi, l.a, l.b);
        if (a < b) fillRect(c, {l.x + f.width(e.text.substr(l.a, a - l.a)), l.y, f.width(e.text.substr(a, b - a)), f.height}, f.selection);
        drawText(c, f, e.text.substr(l.a, l.b - l.a), {l.x, l.y, e.r.x + e.r.w - l.x, f.height}, 0, 0, false);
        if (caretOn_ && e.caret >= l.a && e.caret <= l.b) {
            int cx = std::min(l.x + f.width(e.text.substr(l.a, e.caret - l.a)), e.r.x + e.r.w - 1);
            fillRect(c, {cx, l.y, 1, f.height}, f.caret ? f.caret : f.colour);
        }
    }
    c.clip = saved;
}

// ---- menus --------------------------------------------------------------------------------------

// A menu of items (dropdowns, context menus): an item with an action runs it, any other sets the
// widget's value and runs the widget's own action. The pick keeps a copy of the items, since a
// skin reload may come before it.
void Gui::itemMenu(const Hit& h, const std::vector<Item>& items, Rect at, const std::string& style, bool current) {
    auto copy = std::make_shared<std::vector<Item>>(items);
    auto flat = std::make_shared<std::vector<const Item*>>();
    double v = plain(*h.node, h.i);
    std::function<std::vector<MenuEntry>(const std::vector<Item>&)> build = [&](const std::vector<Item>& its) {
        std::vector<MenuEntry> out;
        for (auto& it : its) {
            if (it.action.type == Action::Standalone && !standalone()) continue;   // only the app has its settings
            MenuEntry e;
            e.label = it.label;
            e.separator = it.separator;
            e.columnBreak = it.columnBreak;
            e.check = it.check;
            bool on = it.action.type != Action::None ? activeAction(it.action) : std::abs(it.value - v) < 1e-9;
            e.current = current && !it.separator && it.items.empty() && it.action.type == Action::None && on;
            e.checked = it.check && on;
            if (!it.separator && it.items.empty()) {
                e.id = (int)flat->size();
                flat->push_back(&it);
            }
            e.items = build(it.items);
            out.push_back(std::move(e));
        }
        return out;
    };
    openMenu(build(*copy), at, style, [this, h, copy, flat](int id) {
        if (id < 0 || id >= (int)flat->size() || !live(h)) return;
        const Item& it = *(*flat)[id];
        if (it.action.type != Action::None) {
            runAction(*h.node, h.i, it.action);
            return;
        }
        begin(h);
        setPlain(h, it.value);
        end();
        runAction(*h.node, h.i, wid(h).action);
    });
}

void Gui::dropdown(const Hit& h) { itemMenu(h, wid(h).items, menuAnchor(h), wid(h).menuStyle, true); }

// A menu opens below its widget (or over it, by style), or with "menuAt" at a point of the widget's
// view, such as the top-left of a neighbouring value field.
Rect Gui::menuAnchor(const Hit& h) const {
    Rect r = rectOf(*h.node, h.i);
    const Widget& w = wid(h);
    if (!w.hasMenuAt) return r;
    const Rect& v = h.node->w[(size_t)h.i].r;
    return {r.x - v.x + w.menuAt[0], r.y - v.y + w.menuAt[1], 0, 0};
}

// At the pointer, in the widget's menu style, else the style named "plain" if the skin has one.
void Gui::contextMenu(const Hit& h, int x, int y) {
    const Widget& w = wid(h);
    std::string style = !w.menuStyle.empty() ? w.menuStyle : skin_->menuStyles.count("plain") ? "plain" : "";
    itemMenu(h, w.context, {x, y, 0, 0}, style, false);
}

void Gui::scaleMenu(Rect at) {
    std::vector<MenuEntry> menu;
    for (int s = 1; s <= 4; ++s) {
        MenuEntry e;
        e.label = std::to_string(s) + "x";
        e.id = s;
        e.current = s == scale_;
        menu.push_back(e);
    }
    openMenu(std::move(menu), at, "", [this](int id) {
        if (id > 0) setScale(id);
    });
}

// Skinned in the named style (or the default "menu"), or native: for "native", or without a menu
// style. Native menus mark the current item with their own check mark.
void Gui::openMenu(std::vector<MenuEntry> items, Rect at, const std::string& style, std::function<void(int)> pick) {
    if (!window || items.empty()) return;
    const MenuStyle* st = &skin_->menu;
    auto named = skin_->menuStyles.find(style);
    if (named != skin_->menuStyles.end()) st = &named->second;
    if ((style == "native" && kNativeMenus) || !st->on) {   // native menus check only toggle items, never the current value
        pick(platformMenu(window, items, at.x, at.y + at.h));
        return;
    }
    closeMenus();
    pick_ = std::move(pick);
    menuOpened_ = Clock::now();
    menuPressed_ = false;
    openSkinned(0, std::move(items), at, !st->over, st);
    platformFocus(window, true);
}

static bool hasShadow(const MenuStyle& s) { return (s.shadow >> 24) != 0; }
static int shadowOf(const Skin& sk, const MenuStyle& s) { return hasShadow(s) ? sk.lw() : 0; }   // its width

// Geometry: rows (rowHeight, separators separatorHeight) inside pad, labels after an indent when
// any item is a check item, 10 px more for a submenu arrow, a 1 px shadow right and below. Placed
// below `at` (or over it, top-left on its top-left), then slid to stay inside the window.
void Gui::openSkinned(size_t level, std::vector<MenuEntry> items, Rect at, bool below, const MenuStyle* st) {
    const Font& f = skin_->fonts[st->font];
    Menu m;
    m.style = st;
    const Skin& sk = *skin_;
    int checkW = st->check >= 0 ? sk.images[st->check].tile(0).w : sk.dp(5), sh = shadowOf(sk, *st);
    for (auto& e : items)
        if (e.check) m.indent = std::max(sk.dp(8), checkW + sk.dp(3));
    int w = 0, h = 0, colH = 0, cols = 1;
    for (size_t k = 0; k < items.size(); ++k) {
        const MenuEntry& e = items[k];
        if (e.columnBreak && k > 0) { ++cols; colH = 0; }   // columns side by side, as the native menus have them
        m.col.push_back(cols - 1);
        if (!e.separator) w = std::max(w, f.width(e.label) + (e.items.empty() ? 0 : sk.dp(10)));
        colH += e.separator ? st->separatorHeight : st->rowHeight;
        h = std::max(h, colH);
        if (e.current) m.sel = (int)k;
    }
    m.colW = w + m.indent + st->pad[0] + st->pad[2];   // each column padded, so columns never touch
    h += st->pad[1] + st->pad[3] + sh;
    w = m.colW * cols + sh;
    if (below && level == 0) w = std::max(w, at.w);
    int x = at.x, y = below ? at.y + at.h : at.y;
    m.r = {std::max(0, std::min(x, w_ - w)), std::max(0, std::min(y, h_ - h)), w, h};
    m.items = std::move(items);
    closeMenus(level);
    menus_.push_back(std::move(m));
    invalidate(menus_.back().r);
}

void Gui::closeMenus(size_t from) {
    if (menus_.size() <= from) return;
    for (size_t k = from; k < menus_.size(); ++k) invalidate(menus_[k].r);
    menus_.resize(from);
    if (from > 0) return;
    pick_ = nullptr;
    if (window && !wantsKeys()) platformFocus(window, false);
}

void Gui::pickMenu(int id) {
    std::function<void(int)> pick = std::move(pick_);
    closeMenus();
    if (pick) pick(id);
}

Rect Gui::menuRow(size_t level, int row) const {
    const Menu& m = menus_[level];
    const MenuStyle& st = *m.style;
    int y = m.r.y + st.pad[1];
    for (int k = 0; k < row; ++k)
        if (m.col[(size_t)k] == m.col[(size_t)row]) y += m.items[k].separator ? st.separatorHeight : st.rowHeight;
    int left = m.r.x + st.pad[0] + m.col[(size_t)row] * m.colW, right = m.r.x + m.r.w - st.pad[2] - shadowOf(*skin_, st);
    if (m.col.back() > 0) right = left + m.colW - st.pad[0] - st.pad[2];   // one of several columns
    return {left, y, right - left, m.items[row].separator ? st.separatorHeight : st.rowHeight};
}

// The topmost menu under the point, and the row under it (-1 outside the rows).
bool Gui::menuAt(int x, int y, size_t& level, int& row) const {
    for (size_t k = menus_.size(); k-- > 0;) {
        if (!menus_[k].r.contains(x, y)) continue;
        level = k;
        row = -1;
        for (int j = 0; j < (int)menus_[k].items.size(); ++j) {
            Rect r = menuRow(k, j);
            if (y >= r.y && y < r.y + r.h && x >= r.x && x < r.x + r.w) row = j;
        }
        return true;
    }
    return false;
}

// Highlights a row (a separator clears the highlight) and opens its submenu at the menu's right
// edge + 1, its top on the row's top; any deeper menu closes.
void Gui::hoverMenu(size_t level, int row) {
    Menu& m = menus_[level];
    if (row >= 0 && m.items[row].separator) row = -1;
    if (row < 0 || row == m.sel) {
        if (row < 0 && m.sel >= 0) { m.sel = -1; invalidate(m.r); }
        if (row < 0) closeMenus(level + 1);
        return;
    }
    m.sel = row;
    invalidate(m.r);
    closeMenus(level + 1);
    if (!m.items[row].items.empty()) {
        Rect r = menuRow(level, row);
        openSkinned(level + 1, m.items[row].items, {m.r.x + m.r.w + 1, r.y - m.style->pad[1], 0, 0}, false, m.style);
    }
}

// The body (cut corners when there is no border), the shadow, then the rows: the hovered one in
// hoverFill, a hoverBand one text line tall behind its label and hoverFont; check marks and submenu
// arrows from the style's images, or drawn in the separator colour.
void Gui::paintMenu(Canvas& c, const Menu& m) {
    const MenuStyle& st = *m.style;
    const Rect& r = m.r;
    const int k1 = skin_->lw(), sh = shadowOf(*skin_, st), R = r.x + r.w, B = r.y + r.h;   // k1: a line, sh: the shadow
    Rect body{r.x, r.y, r.w - sh, r.h - sh};
    if (st.border >> 24) {
        fillRect(c, body, st.border);
        fillRect(c, {body.x + k1, body.y + k1, body.w - 2 * k1, body.h - 2 * k1}, st.fill);
    } else {   // cut corners
        fillRect(c, {body.x + k1, body.y, body.w - 2 * k1, k1}, st.fill);
        fillRect(c, {body.x, body.y + k1, body.w, body.h - 2 * k1}, st.fill);
        fillRect(c, {body.x + k1, body.y + body.h - k1, body.w - 2 * k1, k1}, st.fill);
    }
    if (sh) {
        fillRect(c, {R - sh, r.y + 2 * sh, sh, (B - 2 * sh) - (r.y + 2 * sh)}, st.shadow);
        fillRect(c, {R - 2 * sh, B - 2 * sh, 2 * sh, sh}, st.shadow);
        fillRect(c, {r.x + 2 * sh, B - sh, (R - sh) - (r.x + 2 * sh), sh}, st.shadow);
    }
    for (int k = 0; k < (int)m.items.size(); ++k) {
        const MenuEntry& e = m.items[k];
        Rect row = menuRow((size_t)(&m - menus_.data()), k);
        if (e.separator) {
            fillRect(c, {row.x, row.y + skin_->dp(1), row.w, k1}, st.separator);
            continue;
        }
        bool hot = k == m.sel;
        const Font& f = skin_->fonts[hot && st.hoverFont >= 0 ? st.hoverFont : st.font];
        int textX = row.x + m.indent;
        if (hot) {
            fillRect(c, row, st.hoverFill);
            if (st.hoverBand >> 24) fillRect(c, {textX, row.y, row.w, f.height}, st.hoverBand);
        }
        if (e.checked) {
            if (st.check >= 0) {
                const Image& img = skin_->images[st.check];
                Rect t = img.tile(0);
                drawImage(c, img, 0, {row.x, row.y + (row.h - t.h + 1) / 2, t.w, t.h});
            } else {   // a 5 x 5 diamond (odd sizes at other densities)
                int n = skin_->dp(5) | 1;
                for (int i = 0; i < n; ++i) {
                    int hgt = n - 2 * std::abs(i - n / 2);
                    fillRect(c, {row.x + i, row.y + row.h / 2 - hgt / 2, 1, hgt}, st.separator);
                }
            }
        }
        drawText(c, f, e.label, {textX, row.y, row.x + row.w - textX, f.height}, 0, 0, false);
        if (!e.items.empty()) {
            if (st.arrow >= 0) {
                const Image& img = skin_->images[st.arrow];
                Rect t = img.tile(0);
                drawImage(c, img, 0, {row.x + row.w - skin_->dp(7), row.y + (row.h - t.h) / 2, t.w, t.h});
            } else {   // a 4 x 7 right-pointing triangle
                int tw = skin_->dp(4), th = 2 * tw - 1;
                for (int i = 0; i < tw; ++i) fillRect(c, {row.x + row.w - skin_->dp(7) + i, row.y + (row.h - th) / 2 + i, 1, th - 2 * i}, st.separator);
            }
        }
    }
}

// ---- skinned tooltips and the status display ------------------------------------------------------

void Gui::showTip() {
    const TipStyle& t = skin_->tooltip;
    const std::string& text = wid(tipHit_).tip;
    int tw, th;
    textSize(skin_->fonts[t.font], text, tw, th);
    int b = skin_->lw(), w = tw + t.pad[0] + t.pad[2] + 2 * b, h = th + t.pad[1] + t.pad[3] + 2 * b;
    int y = mouseY_ + std::max(1, skin_->dp(20) / scale_);   // below the pointer, or above it at the bottom edge
    if (y + h > h_) y = std::max(0, mouseY_ - h - skin_->dp(2));
    tipShown_ = text;
    tipRect_ = {std::max(0, std::min(mouseX_, w_ - w)), y, w, h};
    invalidate(tipRect_);
}

void Gui::hideTip(bool block) {
    tipBlocked_ = tipBlocked_ || block;
    if (tipShown_.empty()) return;
    tipShown_.clear();
    invalidate(tipRect_);
}

void Gui::paintTip(Canvas& c) {
    const TipStyle& t = skin_->tooltip;
    Rect r = tipRect_;
    int b = skin_->lw();
    fillRect(c, r, t.border);
    fillRect(c, {r.x + b, r.y + b, r.w - 2 * b, r.h - 2 * b}, t.fill);
    drawText(c, skin_->fonts[t.font], tipShown_,
             {r.x + b + t.pad[0], r.y + b + t.pad[1], r.w - 2 * b - t.pad[0] - t.pad[2], r.h - 2 * b - t.pad[1] - t.pad[3]}, 0, 0, true);
}

// The param being dragged or edited, else the one under the pointer, else the last one for 1.5 s.
void Gui::updateStatus() {
    auto now = Clock::now();
    int p = -1;
    for (const Hit* h : {&press_, &edit_.h, &hover_})
        if (p < 0 && live(*h) && (h != &edit_.h || editing_on_)) p = inst(*h).param;
    if (p >= 0) {
        statusParam_ = p;
        statusUntil_ = now + ms(1500);
    } else if (now >= statusUntil_) {
        statusParam_ = -1;
    }
    std::string t = statusParam_ >= 0 ? state_.def(statusParam_).name + ": " + state_.text(statusParam_, state_.get(statusParam_)) : std::string();
    if (t == status_) return;
    status_ = t;
    invalidateStatus(root_);
}

void Gui::invalidateStatus(Node& n) {
    for (size_t i = 0; n.view && i < n.w.size(); ++i) {
        if (n.view->widgets[i].status && visible(n.w[i])) invalidate(rectOf(n, (int)i) & n.clip);
        if (n.w[i].child) invalidateStatus(*n.w[i].child);
    }
}

// ---- ticks: automation, sources, kinds, MIDI, repeats, tooltips, caret, animation, live reload ------

void Gui::checkValues(Node& n) {
    if (!n.view) return;
    for (size_t i = 0; i < n.w.size(); ++i) {
        const Widget& w = n.view->widgets[i];
        Inst& s = n.w[i];
        if (!visible(s)) continue;
        if (s.child) {
            checkValues(*s.child);
            continue;
        }
        Rect r = rectOf(n, (int)i);
        if ((r & n.clip).empty()) continue;
        bool changed = (s.param >= 0 && state_.get(s.param) != s.drawn) || (s.param2 >= 0 && state_.get(s.param2) != s.drawn2) ||
                       (!s.key.empty() && (state_.hasData(s.key) != s.stored || state_.data(s.key) != s.data));
        if (changed && !s.key.empty() && w.kind == Kind::Custom) {   // kinds keep no copy of their data at paint
            s.stored = state_.hasData(s.key);
            s.data = state_.data(s.key);
        }
        if (w.kind == Kind::List && w.json["dataRows"].has("key")) {   // a list follows the text data its rows come from
            std::string d = state_.data(subst(n, w.json["dataRows"]["key"].str()));
            if (d != s.data) { s.data = d; changed = true; }
        }
        if (w.source == Widget::MidiMap) {   // the list follows the assignments and its CC params
            std::string sig;
            for (auto& row : listModel(n, (int)i))
                for (auto& c : row.cells) sig += c + "";
            if (sig != s.data) { s.data = sig; changed = true; }
        } else if (w.source == Widget::LevelL || w.source == Widget::LevelR) {   // a -60..0 dB meter
            float pk = state_.takePeak(w.source == Widget::LevelR);
            double t = pk > 1e-6f ? std::clamp((20 * std::log10((double)pk) + 60) / 60, 0.0, 1.0) : 0.0;
            double span = w.rangeMax - w.rangeMin, shown = span ? (s.local - w.rangeMin) / span : 0;
            t = std::max(t, shown - 0.03);   // falls about 54 dB a second
            double v = w.rangeMin + std::max(t, 0.0) * span;
            if (v != s.local) { s.local = v; changed = true; }
        } else if (w.source == Widget::Scope) {
            std::string v = std::to_string(state_.scopeCount());
            if (v != s.data) { s.data = v; changed = true; }
        } else if (w.source != Widget::NoSource) {
            std::string v = sourceValue(w);
            if (v != s.data) { s.data = v; changed = true; }
        }
        if (changed) invalidate(r);
        if (w.kind == Kind::Custom && w.ops && w.ops->tick) w.ops->tick(*this, Hit{&n, (int)i, gen_}, r);
    }
}

void Gui::tick() {
    ++ticks_;
    auto now = Clock::now();
    if (live(press_) && wid(press_).kind == Kind::Button && wid(press_).press == PressRepeat) {
        // Shortcut: repeats run on this ~30 Hz tick, so the 100 ms period has up to one tick of jitter.
        if (now >= nextRepeat_) {
            nextRepeat_ = now + ms(100);
            runAction(*press_.node, press_.i, wid(press_).action);
        }
    }
    if (state_.ui() != savedUi_) {   // the host loaded a state
        loaded = true;
        applyUi(state_.ui());
        kindsLoaded(*this);
    }
    unsigned midi = state_.midiInCount();
    if (midi != midiSeen_) {
        midiSeen_ = midi;
        midiUntil_ = now + ms(200);
        midiToDials(root_);
    }
    int lp = skin_->learnParam.empty() ? -1 : state_.indexOf(skin_->learnParam);
    bool learn = lp >= 0 && state_.get(lp) > state_.def(lp).min;
    if (learn != learnOn_) {
        learnOn_ = learn;
        setLearnTarget({});
    }
    checkValues(root_);
    loaded = false;
    update();
    updateStatus();
    if (animating_) animate();
    if (skin_->tooltip.on && window && live(tipHit_) && tipShown_.empty() && !tipBlocked_ && menus_.empty() && !live(press_) &&
        now - tipStill_ >= ms(skin_->tooltip.delay))
        showTip();
    if (editing_on_) {   // the caret blinks every 600 ms
        bool on = (now - blinkStart_) / ms(600) % 2 == 0;
        if (on != caretOn_) {
            caretOn_ = on;
            invalidate(edit_.r);
        }
    }
    if (owned_ && !owned_->dir.empty() && ticks_ % 30 == 0) {
        uint64_t stamp = skinDirStamp(owned_->dir);
        if (stamp != stamp_) {
            stamp_ = stamp;
            std::string err;
            auto s = loadSkinDir(owned_->dir, &err);   // a broken edit keeps the current skin
            if (!err.empty()) std::fprintf(stderr, "hollow: %s", err.c_str());
            if (s) {
                setSkin(s.get());   // reads the old tree, so the old skin must still be alive
                owned_ = s;
            }
        }
    }
}

void Gui::watch(std::shared_ptr<Skin> skin) {
    owned_ = std::move(skin);
    if (!owned_->dir.empty()) stamp_ = skinDirStamp(owned_->dir);
}

// ---- GUI state ----------------------------------------------------------------------------------

// Asks the host for the new size and sizes our window; when the host declines (or cannot resize),
// the windows around ours are resized too.
void Gui::resizeWindow() {
    if (!window) return;
    bool asked = host_ && host_->resize(w_ * scale_, h_ * scale_);
    if (!asked) platformResizeParents(window, w_ * scale_, h_ * scale_);   // measures the frames before we move
    platformSize(window, w_ * scale_, h_ * scale_);
}

void Gui::setScale(int s) {
    s = std::clamp(s, 1, 4);
    if (s == scale_) return;
    scale_ = s;
    resizeWindow();
    saveUi();
}

void Gui::writeEmbeds(const Node& n, std::string& o) const {
    for (size_t i = 0; n.view && i < n.w.size(); ++i) {
        const Node* c = n.w[i].child.get();
        if (!c) continue;
        const Widget& w = n.view->widgets[i];
        if (c->viewName != w.view || c->vars != w.vars) {
            o += (o.back() == '{' ? "" : ",") + jsonQuote(c->path) + ":{\"view\":" + jsonQuote(c->viewName) + ",\"vars\":";
            writeVars(c->vars, o);
            o += '}';
        }
        writeEmbeds(*c, o);
    }
}

void Gui::writeHidden(const Node& n, std::string& o) const {
    for (size_t i = 0; n.view && i < n.w.size(); ++i) {
        const Inst& s = n.w[i];
        if (s.toggled) {
            std::string path = (n.path.empty() ? "" : n.path + "/") + n.view->widgets[i].name;
            o += (o.back() == '{' ? "" : ",") + jsonQuote(path) + ":" + (s.hidden ? "true" : "false");
        }
        if (s.child) writeHidden(*s.child, o);
    }
}

void Gui::writeScroll(const Node& n, std::string& o) const {
    for (size_t i = 0; n.view && i < n.w.size(); ++i) {
        const Inst& s = n.w[i];
        if (!s.child) continue;
        if (s.scroll) o += (o.back() == '{' ? "" : ",") + jsonQuote(s.child->path) + ":" + std::to_string(s.scroll);
        writeScroll(*s.child, o);
    }
}

std::string Gui::uiJson() const {
    std::string o = "{\"scale\":" + std::to_string(scale_) + ",\"vars\":";
    writeVars(vars_, o);
    o += ",\"embeds\":{";
    writeEmbeds(root_, o);
    o += "},\"hidden\":{";
    writeHidden(root_, o);
    o += "},\"scroll\":{";
    writeScroll(root_, o);
    return o + "}}";
}

void Gui::saveUi() {
    savedUi_ = uiJson();
    state_.setUi(savedUi_);
}

void Gui::applyUi(const std::string& json) {
    Json j;
    int scale = scale_;
    vars_ = skin_->vars;
    if (parseJson(json, j) && j.type == Json::Object) {
        scale = std::clamp(j["scale"].integer(scale), 1, 4);
        for (auto& m : j["vars"].members) setVar(vars_, m.first, m.second.str());
    }
    rebuild();
    // Saved pages go parent first, so each path finds the embed its parent entry just showed.
    for (auto& m : j["embeds"].members) {
        size_t slash = m.first.rfind('/');
        Node* p = nodeAt(slash == std::string::npos ? "" : m.first.substr(0, slash));
        std::string name = m.first.substr(slash == std::string::npos ? 0 : slash + 1);
        std::string view = m.second["view"].str();
        for (size_t i = 0; p && p->view && skin_->view(view) && i < p->w.size(); ++i)
            if (p->w[i].child && p->view->widgets[i].name == name) { show(*p, (int)i, view, toVars(m.second["vars"])); break; }
    }
    for (auto& m : j["hidden"].members) {
        size_t slash = m.first.rfind('/');
        Node* p = nodeAt(slash == std::string::npos ? "" : m.first.substr(0, slash));
        Hit t;
        if (p && findWidget(*p, m.first.substr(slash == std::string::npos ? 0 : slash + 1), t, false)) {
            inst(t).hidden = m.second.flag();
            inst(t).toggled = true;
        }
    }
    for (auto& m : j["scroll"].members) {   // clamped by the layout below
        Node* c = nodeAt(m.first);
        Node* p = c ? c->parent : nullptr;
        for (size_t i = 0; p && i < p->w.size(); ++i)
            if (p->w[i].child.get() == c) p->w[i].scroll = m.second.integer();
    }
    conds(root_, false);
    relayout();
    setScale(scale);
    saveUi();
}

void Gui::setSkin(const Skin* skin) {
    std::string ui = uiJson();
    int w = w_, h = h_;
    skin_ = skin;
    applyUi(ui);
    if (w != w_ || h != h_) resizeWindow();
}

// ---- Editor and offscreen rendering --------------------------------------------------------------

struct Editor::Impl {
    Gui gui;
    Impl(std::shared_ptr<Skin> skin, State& state, Host& host) : gui(skin.get(), state, &host) {
        gui.watch(std::move(skin));
        gui.applyUi(state.ui());
    }
};

Editor::Editor(std::shared_ptr<Skin> skin, State& state, Host& host)
    : impl_(new Impl(skin ? std::move(skin) : std::make_shared<Skin>(), state, host)) {}

Editor::~Editor() { detach(); }

bool Editor::attach(void* parent) {
    if (!impl_->gui.window) impl_->gui.window = platformOpen(parent, &impl_->gui);
    return impl_->gui.window != nullptr;
}

void Editor::detach() {
    if (!impl_->gui.window) return;
    platformHold(impl_->gui.window, true);
    impl_->gui.focusLost();   // commits text entry and closes menus while the window still exists
    platformHold(impl_->gui.window, false);
    platformClose(impl_->gui.window);
    impl_->gui.window = nullptr;
}

int Editor::width() const { return impl_->gui.width() * impl_->gui.scale(); }
int Editor::height() const { return impl_->gui.height() * impl_->gui.scale(); }
int Editor::scale() const { return impl_->gui.scale(); }
void Editor::setScale(int s) {
    if (impl_->gui.window) platformHold(impl_->gui.window, true);
    impl_->gui.setScale(s);
    if (impl_->gui.window) platformHold(impl_->gui.window, false);
}

bool renderView(const Skin& skin, const std::string& view, std::vector<uint8_t>& rgba, int& width, int& height, const std::string& stateBlob) {
    if (!skin.view(view)) return false;
    State state(skin.params);
    if (!stateBlob.empty()) state.load(stateBlob);
    Gui gui(&skin, state, nullptr, view);
    if (!stateBlob.empty()) gui.applyUi(state.ui());   // its vars and pages too
    const std::vector<uint32_t>& px = gui.pixels();
    width = gui.width();
    height = gui.height();
    rgba.resize(px.size() * 4);
    for (size_t i = 0; i < px.size(); ++i) {
        rgba[i * 4] = (uint8_t)(px[i] >> 16);
        rgba[i * 4 + 1] = (uint8_t)(px[i] >> 8);
        rgba[i * 4 + 2] = (uint8_t)px[i];
        rgba[i * 4 + 3] = (uint8_t)(px[i] >> 24);
    }
    return true;
}

} // namespace hollow
