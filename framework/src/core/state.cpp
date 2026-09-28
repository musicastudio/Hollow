// State: parameter values (lock-free), their text form, text data, the GUI's MIDI queue and the
// saved instance blob.
#include "core.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace hollow {

struct State::Impl {
    std::vector<ParamDef> defs;
    std::unique_ptr<std::atomic<double>[]> values;
    std::unordered_map<std::string, int> byId;
    std::unordered_map<uint32_t, int> byHost;
    std::vector<size_t> hostParams;   // State index of each host param, in params.json order
    std::vector<int> hostSlots;       // host position of each State index, -1 for host: false
    mutable std::mutex uiLock;        // ui and data
    std::string ui;
    std::map<std::string, std::string> data;
    // MIDI from the GUI: single producer (GUI thread), single consumer (audio thread). A full queue
    // drops messages, 256 is far more than a mouse can play between two audio blocks.
    struct Msg { uint8_t b[3]; uint8_t n; };
    Msg midi[256];
    std::atomic<uint32_t> midiHead{0}, midiTail{0};   // head: next write, tail: next read
    // What the audio side reports (written on the audio thread, read by the GUI).
    std::atomic<bool> held[128] = {}, sustained[128] = {}, pedal{false};
    std::atomic<int> bend{0}, voices{-1}, learnParam{-1};
    std::atomic<int> ccs[128], ccMap[128];   // last value (-1 = never), assigned param (-1 = none)
    std::atomic<unsigned> midiCount{0};
    std::atomic<double> cpu{0};
    std::atomic<bool> modified{true};   // a fresh instance has never been saved or loaded, so it starts modified
    std::atomic<unsigned> loads{0};
    std::atomic<float> peaks[2] = {};
    mutable std::mutex scopeLock;       // the processor's worker writes, the GUI reads; never the audio thread
    std::vector<float> scope;
    std::atomic<unsigned> scopeCount{0};
};

// A format string is used only if it has exactly one conversion and it takes a double.
bool safeFormat(const std::string& f) {
    int convs = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') continue;
        if (++i < f.size() && f[i] == '%') continue;
        while (i < f.size() && std::strchr("-+ #0123456789.", f[i])) ++i;
        if (i >= f.size() || !std::strchr("eEfFgGaA", f[i])) return false;
        ++convs;
    }
    return convs == 1;
}

// A "log" taper maps normalized n to min * (max / min) ^ n; it needs min and max > 0.
static bool isLog(const ParamDef& d) { return d.log && d.min > 0 && d.max > 0 && d.min != d.max; }

static double normOf(const ParamDef& d, double v) {
    if (d.max == d.min) return 0;
    double t = isLog(d) ? std::log(v / d.min) / std::log(d.max / d.min) : (v - d.min) / (d.max - d.min);
    return std::isnan(t) ? 0 : std::clamp(t, 0.0, 1.0);
}

static double plainOf(const ParamDef& d, double t) {
    return isLog(d) ? d.min * std::pow(d.max / d.min, t) : d.min + t * (d.max - d.min);
}

static double snap(const ParamDef& d, double v) {
    if (std::isnan(v)) v = d.def;
    v = std::clamp(v, std::min(d.min, d.max), std::max(d.min, d.max));
    // k * span / steps rather than (k / steps) * span: an integer range comes out exact, so a stepped value
    // compares equal to the number a list row, a radio button or a condition names (13 of 0..383 was
    // 12.999999999999998 the other way).
    if (d.steps > 0 && d.max != d.min) {
        const double k = std::round(normOf(d, v) * d.steps);
        v = isLog(d) ? plainOf(d, k / d.steps) : d.min + k * (d.max - d.min) / d.steps;
    }
    return v;
}

State::State(std::vector<ParamDef> defs) : impl_(new Impl) {
    for (int i = 0; i < 128; ++i) { impl_->ccs[i].store(-1); impl_->ccMap[i].store(-1); }
    impl_->defs = std::move(defs);
    impl_->values.reset(new std::atomic<double>[impl_->defs.size()]);
    for (size_t i = 0; i < impl_->defs.size(); ++i) {
        ParamDef& d = impl_->defs[i];
        if (!d.hostId) {
            uint32_t h = 2166136261u;
            for (unsigned char c : d.id) h = (h ^ c) * 16777619u;
            d.hostId = h & 0x7fffffff;
        }
        impl_->values[i].store(snap(d, d.def));
        impl_->byId.emplace(d.id, (int)i);
        impl_->hostSlots.push_back(d.host ? (int)impl_->hostParams.size() : -1);
        if (!d.host) continue;
        impl_->byHost.emplace(d.hostId, (int)i);
        impl_->hostParams.push_back(i);
    }
}

State::~State() = default;
size_t State::size() const { return impl_->defs.size(); }
const ParamDef& State::def(size_t i) const { return impl_->defs[i]; }

int State::indexOfHostId(uint32_t hostId) const {
    auto it = impl_->byHost.find(hostId);
    return it == impl_->byHost.end() ? -1 : it->second;
}

int State::indexOf(const std::string& id) const {
    auto it = impl_->byId.find(id);
    return it == impl_->byId.end() ? -1 : it->second;
}

double State::get(size_t i) const { return i < size() ? impl_->values[i].load(std::memory_order_relaxed) : 0; }

// A change to a host param marks the instance modified; GUI-side (host: false) params do not.
void State::set(size_t i, double plain) {
    if (i >= size()) return;
    double v = snap(impl_->defs[i], plain);
    if (impl_->values[i].exchange(v, std::memory_order_relaxed) != v && impl_->defs[i].host) impl_->modified.store(true);
}

double State::toNormal(size_t i, double plain) const { return normOf(def(i), plain); }

double State::fromNormal(size_t i, double normal) const {
    const ParamDef& d = def(i);
    return snap(d, plainOf(d, std::clamp(normal, 0.0, 1.0)));
}

std::string displayText(const ParamDef& d, double plain, const std::string& format) {
    plain = snap(d, plain);
    if (format.empty() && d.steps > 0 && !d.labels.empty()) {
        size_t k = (size_t)std::lround(normOf(d, plain) * d.steps);
        if (k < d.labels.size()) return d.labels[k];
    }
    const std::string& f = format.empty() ? d.format : format;
    return safeFormat(f) ? formatNumber(f.c_str(), plain) : formatNumber(d.steps > 0 ? "%.0f" : "%.2f", plain);
}

std::string State::text(size_t i, double plain) const {
    const ParamDef& d = def(i);
    std::string s = displayText(d, plain, "");
    return d.unit.empty() ? s : s + " " + d.unit;
}

bool State::parse(size_t i, const std::string& text, double& plain) const {
    if (i >= size()) return false;
    const ParamDef& d = def(i);
    for (size_t k = 0; k < d.labels.size() && d.steps > 0; ++k) {
        const std::string& l = d.labels[k];
        if (l.size() == text.size() && std::equal(l.begin(), l.end(), text.begin(), [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); })) {
            plain = snap(d, plainOf(d, double(k) / d.steps));
            return true;
        }
    }
    const char* p = text.c_str();
    while (*p == ' ') ++p;
    double v;
    if (!parseNumber(p, text.c_str() + text.size(), v)) return false;
    plain = snap(d, v);
    return true;
}

size_t State::hostCount() const { return impl_->hostParams.size(); }
size_t State::hostParam(size_t k) const { return k < impl_->hostParams.size() ? impl_->hostParams[k] : 0; }
int State::hostSlot(size_t i) const { return i < impl_->hostSlots.size() ? impl_->hostSlots[i] : -1; }

std::string State::data(const std::string& key) const {
    std::lock_guard<std::mutex> g(impl_->uiLock);
    auto it = impl_->data.find(key);
    return it == impl_->data.end() ? std::string() : it->second;
}

bool State::hasData(const std::string& key) const {
    std::lock_guard<std::mutex> g(impl_->uiLock);
    return impl_->data.count(key) != 0;
}

void State::setData(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> g(impl_->uiLock);
    auto it = impl_->data.find(key);
    if (it != impl_->data.end() && it->second == value) return;
    impl_->data[key] = value;
    impl_->modified.store(true);
}

// ---- what the audio side knows ----------------------------------------------------------------

void State::midiIn(const uint8_t* b, int count) {
    Impl& m = *impl_;
    m.midiCount.fetch_add(1, std::memory_order_relaxed);
    if (count < 2) return;
    int status = b[0] & 0xf0, d1 = b[1] & 127, d2 = count > 2 ? b[2] & 127 : 0;
    if (status == 0x90 && d2 > 0) {
        m.held[d1].store(true);
        m.sustained[d1].store(false);
    } else if (status == 0x80 || status == 0x90) {
        if (m.pedal.load()) m.sustained[d1].store(true);   // the pedal keeps it sounding
        else m.held[d1].store(false);
    } else if (status == 0xe0) {
        m.bend.store((d1 | d2 << 7) - 8192);
    } else if (status == 0xb0) {
        m.ccs[d1].store(d2);
        if (d1 == 64) {
            m.pedal.store(d2 >= 64);
            for (int n = 0; n < 128 && d2 < 64; ++n)
                if (m.sustained[n].exchange(false)) m.held[n].store(false);
        }
        if (d1 == 120 || d1 == 123)
            for (int n = 0; n < 128; ++n) { m.held[n].store(false); m.sustained[n].store(false); }
        int learning = m.learnParam.exchange(-1);
        if (learning >= 0) {   // one parameter, one controller: learning moves an older assignment
            for (int c = 0; c < 128; ++c)
                if (m.ccMap[c].load() == learning) m.ccMap[c].store(-1);
            m.ccMap[d1].store(learning);
        }
        int p = m.ccMap[d1].load();
        if (p >= 0 && (size_t)p < size()) set((size_t)p, fromNormal((size_t)p, d2 / 127.0));
    }
}

void State::setLoad(double fraction) { impl_->cpu.store(fraction, std::memory_order_relaxed); }
void State::setVoices(int voices) { impl_->voices.store(voices, std::memory_order_relaxed); }
bool State::noteHeld(int note) const { return note >= 0 && note < 128 && impl_->held[note].load(std::memory_order_relaxed); }
int State::pitchBend() const { return impl_->bend.load(std::memory_order_relaxed); }
int State::cc(int number) const { return number >= 0 && number < 128 ? impl_->ccs[number].load(std::memory_order_relaxed) : -1; }
unsigned State::midiInCount() const { return impl_->midiCount.load(std::memory_order_relaxed); }
double State::load() const { return impl_->cpu.load(std::memory_order_relaxed); }
int State::voices() const { return impl_->voices.load(std::memory_order_relaxed); }
bool State::modified() const { return impl_->modified.load(); }
unsigned State::loads() const { return impl_->loads.load(); }

// A peak only rises between looks, so a short transient still reaches a 30 Hz meter.
void State::peak(float left, float right) {
    const float v[2] = {left, right};
    for (int c = 0; c < 2; ++c) {
        float old = impl_->peaks[c].load(std::memory_order_relaxed);
        while (v[c] > old && !impl_->peaks[c].compare_exchange_weak(old, v[c], std::memory_order_relaxed)) {}
    }
}

float State::takePeak(int channel) { return channel == 0 || channel == 1 ? impl_->peaks[channel].exchange(0) : 0; }

void State::setScope(const float* values, int count) {
    std::lock_guard<std::mutex> g(impl_->scopeLock);
    impl_->scope.assign(values, values + std::clamp(count, 0, 256));
    impl_->scopeCount.fetch_add(1);
}

int State::scope(float* out, int max) const {
    std::lock_guard<std::mutex> g(impl_->scopeLock);
    const int n = (int)impl_->scope.size();
    std::copy(impl_->scope.begin(), impl_->scope.begin() + std::min(n, std::max(max, 0)), out);
    return n;
}

unsigned State::scopeCount() const { return impl_->scopeCount.load(); }
void State::learn(int param) { impl_->learnParam.store(param < 0 || (size_t)param >= size() ? -1 : param); }
int State::learning() const { return impl_->learnParam.load(); }

std::vector<std::pair<int, int>> State::midiMap() const {
    std::vector<std::pair<int, int>> out;
    for (int c = 0; c < 128; ++c)
        if (int p = impl_->ccMap[c].load(); p >= 0) out.emplace_back(c, p);
    return out;
}

void State::assign(int cc, int param) {
    if (cc >= 0 && cc < 128) impl_->ccMap[cc].store(param < 0 || (size_t)param >= size() ? -1 : param);
}

void State::pushMidi(const uint8_t* bytes, int size) {
    uint32_t head = impl_->midiHead.load(std::memory_order_relaxed);
    if (size < 1 || size > 3 || head - impl_->midiTail.load(std::memory_order_acquire) >= 256) return;
    Impl::Msg& m = impl_->midi[head % 256];
    std::memcpy(m.b, bytes, (size_t)size);
    m.n = (uint8_t)size;
    impl_->midiHead.store(head + 1, std::memory_order_release);
}

bool State::popMidi(uint8_t bytes[3], int& size) {
    uint32_t tail = impl_->midiTail.load(std::memory_order_relaxed);
    if (tail == impl_->midiHead.load(std::memory_order_acquire)) return false;
    const Impl::Msg& m = impl_->midi[tail % 256];
    std::memcpy(bytes, m.b, 3);
    size = m.n;
    impl_->midiTail.store(tail + 1, std::memory_order_release);
    return true;
}

// Text blob: a header line, "param <id> <value>" per parameter (host: false ones too),
// "data [<key>, <value>]" per text data entry (JSON strings, so one line each), "cc <n> <id>" per
// MIDI assignment, then "ui <json>".
std::string State::save() const {
    std::string s = "hollow-state 1\n";
    for (size_t i = 0; i < size(); ++i) s += "param " + def(i).id + " " + formatNumber("%.17g", get(i)) + "\n";
    {
        std::lock_guard<std::mutex> g(impl_->uiLock);
        for (auto& kv : impl_->data) s += "data [" + jsonQuote(kv.first) + "," + jsonQuote(kv.second) + "]\n";
    }
    for (auto& cp : midiMap()) s += "cc " + std::to_string(cp.first) + " " + def(cp.second).id + "\n";
    impl_->modified.store(false);
    return s + "ui " + ui() + "\n";
}

bool State::load(const std::string& blob) {
    if (blob.compare(0, 15, "hollow-state 1\n") != 0) return false;
    struct Count {   // odd from here to the return
        std::atomic<unsigned>& n;
        explicit Count(std::atomic<unsigned>& c) : n(c) { ++n; }
        ~Count() { ++n; }
    } count(impl_->loads);
    {
        std::lock_guard<std::mutex> g(impl_->uiLock);
        impl_->data.clear();   // the blob holds the instance's whole text data and MIDI map
    }
    for (int c = 0; c < 128; ++c) impl_->ccMap[c].store(-1);
    size_t pos = 15;
    while (pos < blob.size()) {
        size_t eol = blob.find('\n', pos);
        if (eol == std::string::npos) eol = blob.size();
        std::string line = blob.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.compare(0, 3, "ui ") == 0) { setUi(line.substr(3)); continue; }
        Json kv;
        if (line.compare(0, 5, "data ") == 0 && parseJson(line.substr(5), kv) && kv.size() == 2) {
            setData(kv[0].str(), kv[1].str());
            continue;
        }
        if (line.compare(0, 3, "cc ") == 0) {
            size_t sp = line.find(' ', 3);
            if (sp != std::string::npos) assign(std::atoi(line.c_str() + 3), indexOf(line.substr(sp + 1)));
            continue;
        }
        if (line.compare(0, 6, "param ") != 0) continue;
        size_t sp = line.find(' ', 6);
        if (sp == std::string::npos) continue;
        int i = indexOf(line.substr(6, sp - 6));
        const char* p = line.c_str() + sp + 1;
        double v;
        if (i >= 0 && parseNumber(p, line.c_str() + line.size(), v)) set(i, v);
    }
    impl_->modified.store(false);
    return true;
}

std::string State::ui() const {
    std::lock_guard<std::mutex> g(impl_->uiLock);
    return impl_->ui;
}

void State::setUi(const std::string& s) {
    std::string clean = s;
    std::replace(clean.begin(), clean.end(), '\n', ' ');
    std::lock_guard<std::mutex> g(impl_->uiLock);
    impl_->ui = clean;
}

} // namespace hollow
