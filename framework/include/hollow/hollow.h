// Hollow: a small plug-in framework whose GUI is a data-driven skin (docs/skin-format.md).
// This header is the whole public API. A product implements pluginInfo() and, if it makes
// sound, createProcessor(); the format layers (CLAP, VST2, DXi, and VST3/AU/standalone through
// clap-wrapper) and the editor do the rest.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hollow {

// ---- product description ------------------------------------------------------------------

struct Info {
    const char* id;            // reverse-DNS, e.g. "com.example.my-synth"
    const char* name;          // "My Synth"
    const char* vendor;
    const char* url;
    const char* version;       // "1.0.0"
    const char* description;
    bool instrument = true;    // false = audio effect
    int inputs = 0;            // audio input channels (0 or 2)
    int outputs = 2;           // audio output channels
    bool midiIn = true;
    bool midiOut = false;
};

// ---- parameters (the skin's params.json) ---------------------------------------------------

struct ParamDef {
    std::string id, name, unit, format;
    double min = 0, max = 1, def = 0;
    int steps = 0;                        // 0 = continuous, n = n + 1 values
    bool log = false;                     // params.json "taper": "log"
    bool host = true;                     // params.json "host": false keeps it off the host's list (still saved)
    std::vector<std::string> labels;
    uint32_t hostId = 0;                  // fnv1a32(id) & 0x7fffffff
};

class Skin;                               // a loaded skin; opaque outside the core

// The product's skin: the copy embedded at build time, or HOLLOW_SKIN_DIR when set.
std::shared_ptr<Skin> loadSkin();
const std::vector<ParamDef>& skinParams(const Skin&);
class State;
// A fresh instance's default MIDI assignments (the skin's learn.defaults); the formats call it once
// after creating the State.
void applyLearnDefaults(State&, const Skin&);

// Parameter values and GUI state of one plug-in instance, shared by the audio thread, the
// host wrapper and the editor. Values are plain (min..max). get/set are lock-free.
class State {
public:
    explicit State(std::vector<ParamDef> defs);
    ~State();
    size_t size() const;
    const ParamDef& def(size_t i) const;
    int indexOfHostId(uint32_t hostId) const;   // -1 if unknown
    int indexOf(const std::string& id) const;   // -1 if unknown
    double get(size_t i) const;
    void set(size_t i, double plain);           // clamps and snaps to steps
    double toNormal(size_t i, double plain) const;
    double fromNormal(size_t i, double normal) const;
    std::string text(size_t i, double plain) const;
    bool parse(size_t i, const std::string& text, double& plain) const;

    // Whole-instance state for the host (param values by id plus the GUI state), and back.
    std::string save() const;
    bool load(const std::string& blob);
    // Counts loads, odd while one is running: a processor that acts on param changes compares it
    // to tell a restored session from an edit.
    unsigned loads() const;

    // GUI state (open pages, toggled widgets, scale) kept here so it outlives the editor.
    std::string ui() const;
    void setUi(const std::string& s);

    // The params hosts see, in params.json order (VST2 index k = hostParam(k)).
    size_t hostCount() const;
    size_t hostParam(size_t k) const;           // State index of the k-th host param
    int hostSlot(size_t i) const;               // host position of State index i, -1 for host: false

    // Text data saved with the instance (editable text, envelope shapes), keyed by the skin. GUI thread.
    std::string data(const std::string& key) const;
    bool hasData(const std::string& key) const;   // true once set, even to ""
    void setData(const std::string& key, const std::string& value);

    // MIDI the GUI plays (on-screen keys, wheels): the editor pushes on the GUI thread, the format
    // layer pops on the audio thread and hands each message to Processor::midi at offset 0.
    void pushMidi(const uint8_t* bytes, int size);   // up to 3 bytes
    bool popMidi(uint8_t bytes[3], int& size);

    // What the audio side knows and the GUI shows. The format layer calls midiIn for every incoming
    // MIDI message and setLoad once per block (audio thread); a Processor may call setVoices.
    void midiIn(const uint8_t* bytes, int size);   // key and wheel display, MIDI learn, assigned CCs
    void setLoad(double fraction);                 // time spent in process() / block duration
    void setVoices(int voices);
    bool noteHeld(int note) const;
    int pitchBend() const;                          // -8192..8191
    int cc(int number) const;                       // last value 0..127, -1 if never received
    unsigned midiInCount() const;                   // grows with every incoming message
    double load() const;
    int voices() const;                             // -1 if never reported
    bool modified() const;                          // a value changed since the last save or load

    // The output monitor. The format layer reports each block's peak per channel; the GUI takes the
    // highest since it last looked (a meter's "source": "level_l" / "level_r"). A processor may publish
    // up to 256 values 0..1 for a plot's "source": "scope" (a spectrum, a waveform); any thread.
    void peak(float left, float right);
    float takePeak(int channel);
    void setScope(const float* values, int count);
    int scope(float* out, int max) const;           // the count; copies up to max values
    unsigned scopeCount() const;                    // grows with every setScope

    // MIDI learn: the next incoming CC after learn(param) is assigned to that param (saved with the
    // instance); an assigned CC then sets its param's normalized value.
    void learn(int param);                          // -1 stops learning
    int learning() const;
    std::vector<std::pair<int, int>> midiMap() const;   // (cc, param) sorted by cc
    void assign(int cc, int param);                  // param -1 removes the assignment
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- sound (optional) -----------------------------------------------------------------------

class Processor {
public:
    virtual ~Processor() = default;
    virtual void prepare(double sampleRate, int maxFrames) { (void)sampleRate; (void)maxFrames; }
    virtual void midi(int frameOffset, const uint8_t* bytes, int size) { (void)frameOffset; (void)bytes; (void)size; }
    // in/out: channel pointers; outputs arrive zeroed, so a hollow product can leave them alone.
    virtual void process(const float* const* in, float* const* out, int frames) { (void)in; (void)out; (void)frames; }

    // The host is about to save the instance (main thread): put what State does not hold into its
    // text data now.
    virtual void saving() {}

    // Set by the format layer; call from process() to send MIDI out (needs Info::midiOut).
    std::function<void(int frameOffset, const uint8_t* bytes, int size)> sendMidi;
    // Set by the format layer; any thread. The processor changed params itself (a patch load), so the
    // host re-reads every value; it records none of them as automation.
    std::function<void()> paramsChanged;
};

// ---- implemented once per product (plugins/<name>/plugin.cpp) ---------------------------------

const Info& pluginInfo();
std::unique_ptr<Processor> createProcessor(State& state);

// ---- the editor window ------------------------------------------------------------------------

class Editor {
public:
    // Implemented by the format layer; called on the GUI thread, for host params only.
    struct Host {
        virtual ~Host() = default;
        virtual void beginEdit(size_t param) = 0;
        virtual void edit(size_t param, double plain) = 0;   // value is already in State
        virtual void endEdit(size_t param) = 0;
        virtual bool resize(int width, int height) { (void)width; (void)height; return false; }
        // True in the standalone app, whose audio and MIDI settings a skin's "standalone" action opens.
        virtual bool standalone() const { return false; }
    };

    Editor(std::shared_ptr<Skin> skin, State& state, Host& host);
    ~Editor();
    bool attach(void* parent);            // HWND on Windows, NSView* on macOS, an X11 Window id on Linux
    void detach();
    int width() const;                    // window size in pixels: root view size * scale
    int height() const;
    int scale() const;                    // 1..4, remembered in State::ui()
    void setScale(int s);                 // resizes the window and calls Host::resize
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Offscreen render of a view (tools and tests): RGBA8 straight alpha, row-major.
bool renderView(const Skin& skin, const std::string& view, std::vector<uint8_t>& rgba, int& width, int& height,
                const std::string& stateBlob = {});   // stateBlob: a saved state ("hollow-state 1") to render with

} // namespace hollow
