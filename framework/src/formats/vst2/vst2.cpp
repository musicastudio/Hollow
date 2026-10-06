// The VST2 plug-in: an AEffect on hollow::State, Editor and Processor, written against our own ABI
// header (third_party/vst2). VST2 knows parameters only by index: index k is State::hostParam(k), the
// k-th params.json entry the host may see (host: false params are left out, so indices stay contiguous).
// On 32-bit Windows the same DLL is also the DXi (formats/dxi), which hosts this AEffect.
#include <vst2.h>
#include <hollow/hollow.h>
#include "../vst_keys.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace hollow {
namespace {

uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

int32_t versionCode(const char* v) {   // "1.2.3" -> 0x010203
    int a = 0, b = 0, c = 0;
    std::sscanf(v, "%d.%d.%d", &a, &b, &c);
    return a << 16 | b << 8 | c;
}

void copy(void* dst, const std::string& s, size_t size) {   // size includes the terminator
    if (dst) std::snprintf((char*)dst, size, "%s", s.c_str());
}

int midiSize(uint8_t status) {
    if (status >= 0xf8 || status == 0xf6) return 1;
    if ((status & 0xe0) == 0xc0 || status == 0xf1 || status == 0xf3) return 2;   // program, pressure
    return 3;
}

struct Effect final : Editor::Host {
    AEffect eff{};
    AudioMasterCallback master;
    std::shared_ptr<Skin> skin;
    std::unique_ptr<State> state;
    std::unique_ptr<Processor> proc;
    std::unique_ptr<Editor> editor;
    ERect rect{};
    std::string chunk;
    float rate = 44100;
    int block = 1024;
    std::atomic<bool> redisplay{false};   // Processor::paramsChanged, served on the editor's idle

    // MIDI out: Processor::sendMidi collects into these during processReplacing, and the block goes
    // to the host in one audioMasterProcessEvents afterwards. Fixed sizes keep the audio thread free
    // of allocation; events past kMaxOut, or sysex past kSysexBytes, in one block are dropped.
    static constexpr int32_t kMaxOut = 512;
    union OutEvent { VstMidiEvent midi; VstMidiSysexEvent sysex; };
    struct { int32_t numEvents; intptr_t reserved; VstEvent* events[kMaxOut]; } outList{};   // VstEvents layout
    std::vector<OutEvent> outEvents;
    std::vector<char> sysexBytes;
    size_t sysexUsed = 0;
    int32_t frames = 0;   // the current block's length; events are kept inside it

    explicit Effect(AudioMasterCallback m);

    void sendMidi(int frame, const uint8_t* bytes, int size) {
        if (size <= 0 || outList.numEvents >= kMaxOut) return;
        frame = std::clamp(frame, 0, std::max(frames - 1, 0));
        OutEvent& e = outEvents[(size_t)outList.numEvents];
        std::memset(&e, 0, sizeof e);
        if (size <= 3) {
            e.midi.type = kVstMidiType;
            e.midi.byteSize = sizeof e.midi;
            e.midi.deltaFrames = frame;
            std::memcpy(e.midi.midiData, bytes, (size_t)size);
        } else {
            if (sysexUsed + (size_t)size > sysexBytes.size()) return;
            std::memcpy(&sysexBytes[sysexUsed], bytes, (size_t)size);
            e.sysex.type = kVstSysExType;
            e.sysex.byteSize = sizeof e.sysex;
            e.sysex.deltaFrames = frame;
            e.sysex.dumpBytes = size;
            e.sysex.sysexDump = &sysexBytes[sysexUsed];
            sysexUsed += (size_t)size;
        }
        outList.events[outList.numEvents++] = (VstEvent*)&e;
    }

    intptr_t call(int32_t op, int32_t i = 0, intptr_t v = 0, float o = 0) { return master(&eff, op, i, v, nullptr, o); }
    bool valid(int32_t k) const { return k >= 0 && (size_t)k < state->hostCount(); }

    // Editor::Host, on the GUI thread, with State indices; the host gets their VST2 index.
    void beginEdit(size_t i) override { if (state->hostSlot(i) >= 0) call(audioMasterBeginEdit, state->hostSlot(i)); }
    void edit(size_t i, double plain) override {
        if (state->hostSlot(i) >= 0) call(audioMasterAutomate, state->hostSlot(i), 0, (float)state->toNormal(i, plain));
    }
    void endEdit(size_t i) override { if (state->hostSlot(i) >= 0) call(audioMasterEndEdit, state->hostSlot(i)); }
    bool resize(int w, int h) override { return call(audioMasterSizeWindow, w, h) != 0; }

    Editor* ensureEditor() {
        if (!editor && skin) editor = std::make_unique<Editor>(skin, *state, *this);
        return editor.get();
    }

    // MIDI played on the editor, at offset 0. Called before the host's events of a block
    // (effProcessEvents) and again in processReplacing for blocks that have none. State's queue has
    // one consumer, so this assumes the host sends both on its audio thread, as hosts do.
    void guiMidi() {
        uint8_t m[3];
        for (int n; state->popMidi(m, n);) proc->midi(0, m, n);
    }

    // MIDI from the host: State sees it (key display, MIDI learn), then the processor.
    void hostMidi(int t, const uint8_t* b, int n) {
        state->midiIn(b, n);
        proc->midi(t, b, n);
    }

    void events(const VstEvents* ev) {
        guiMidi();
        for (int32_t k = 0; ev && k < ev->numEvents; ++k) {
            const VstEvent* e = ev->events[k];
            if (e && e->type == kVstMidiType) {
                auto* m = (const VstMidiEvent*)e;
                hostMidi(m->deltaFrames, (const uint8_t*)m->midiData, midiSize((uint8_t)m->midiData[0]));
            } else if (e && e->type == kVstSysExType) {
                auto* x = (const VstMidiSysexEvent*)e;
                hostMidi(x->deltaFrames, (const uint8_t*)x->sysexDump, x->dumpBytes);
            }
        }
    }

    intptr_t canDo(const char* s) const {
        const Info& info = pluginInfo();
        if (!std::strcmp(s, "receiveVstEvents") || !std::strcmp(s, "receiveVstMidiEvent")) return info.midiIn ? 1 : -1;
        if (!std::strcmp(s, "sendVstEvents") || !std::strcmp(s, "sendVstMidiEvent")) return info.midiOut ? 1 : -1;
        return 0;
    }

    intptr_t dispatch(int32_t op, int32_t idx, intptr_t val, void* ptr, float opt) {
        const Info& info = pluginInfo();
        const size_t p = valid(idx) ? state->hostParam((size_t)idx) : 0;   // the State index of param idx
        switch (op) {
            case effClose: delete this; return 1;
            case effGetProgramName: copy(ptr, "Default", 24); return 1;
            case effGetParamLabel: if (!valid(idx)) return 0; copy(ptr, state->def(p).unit, 8); return 1;
            case effGetParamDisplay: {   // the unit goes to effGetParamLabel, so leave it off here
                if (!valid(idx)) return 0;
                std::string t = state->text(p, state->get(p)), u = " " + state->def(p).unit;
                if (u.size() > 1 && t.size() >= u.size() && t.compare(t.size() - u.size(), u.size(), u) == 0) t.resize(t.size() - u.size());
                copy(ptr, t, 24);
                return 1;
            }
            case effGetParamName: if (!valid(idx)) return 0; copy(ptr, state->def(p).name, 32); return 1;
            case effSetSampleRate: rate = opt; return 1;
            case effSetBlockSize: block = (int)val; return 1;
            case effMainsChanged: if (val) proc->prepare(rate, block); return 1;
            case effEditGetRect: {
                const Editor* e = ensureEditor();
                if (!e || !ptr) return 0;
                rect = {0, 0, (int16_t)e->height(), (int16_t)e->width()};
                *(ERect**)ptr = &rect;
                return 1;
            }
            case effEditOpen: return ensureEditor() && editor->attach(ptr) ? 1 : 0;
            case effEditClose: editor.reset(); return 1;
            case effEditIdle:
                if (redisplay.exchange(false)) call(audioMasterUpdateDisplay);
                return 1;
            // A host that keeps the keyboard to itself may offer keys only here, and reads 0 as "not the plug-in's" and then acts on the key itself. idx is
            // the ASCII character, val a VstVirtualKey, opt the VstModifierKey bits.
            case effEditKeyDown: {
                keyLog("effEditKeyDown idx=%d val=%d opt=%g editor=%d", (int)idx, (int)val, (double)opt, editor ? 1 : 0);
                if (!editor) return 0;
                const int mods = (int)opt;
                return editor->key(vstVirtualKey((int)val), (unsigned)idx, (mods & kVstModShift) != 0,
                                   (mods & (kVstModControl | kVstModCommand)) != 0, (mods & kVstModAlternate) != 0)
                           ? 1 : 0;
            }
            case effEditKeyUp: return 0;   // nothing in the editor acts on a release
            case effKeysRequired: return 0;   // inverted by convention: 0 means the editor wants keys
            case effGetChunk:
                if (!ptr) return 0;
                proc->saving();
                chunk = state->save();
                *(void**)ptr = chunk.data();
                return (intptr_t)chunk.size();
            case effSetChunk: return ptr && state->load(std::string((const char*)ptr, (size_t)val)) ? 1 : 0;
            case effProcessEvents: events((const VstEvents*)ptr); return 1;
            case effCanBeAutomated: return valid(idx) ? 1 : 0;
            case effString2Parameter: {
                double plain = 0;
                if (!valid(idx) || !ptr || !state->parse(p, (const char*)ptr, plain)) return 0;
                state->set(p, plain);
                return 1;
            }
            case effGetPlugCategory: return info.instrument ? kPlugCategSynth : kPlugCategEffect;
            case effGetEffectName: copy(ptr, info.name, 32); return 1;
            case effGetVendorString: copy(ptr, info.vendor, 64); return 1;
            case effGetProductString: copy(ptr, info.name, 64); return 1;
            case effGetVendorVersion: return versionCode(info.version);
            case effCanDo: return ptr ? canDo((const char*)ptr) : 0;
            case effGetVstVersion: return 2400;
            case effGetNumMidiInputChannels: return info.midiIn ? 16 : 0;
            case effGetNumMidiOutputChannels: return info.midiOut ? 16 : 0;
            default: return 0;
        }
    }
};

Effect* self(AEffect* e) { return (Effect*)e->object; }

Effect::Effect(AudioMasterCallback m) : master(m) {
    const Info& info = pluginInfo();
    skin = loadSkin();
    state = std::make_unique<State>(skin ? skinParams(*skin) : std::vector<ParamDef>{});
    if (skin) applyLearnDefaults(*state, *skin);
    proc = createProcessor(*state);
    if (!proc) proc = std::make_unique<Processor>();

    eff.magic = kEffectMagic;
    eff.dispatcher = [](AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt) { return self(e)->dispatch(op, idx, val, ptr, opt); };
    // The accumulating process() predates VST 2.4 and is not supported; 2.4 hosts use processReplacing.
    eff.process = [](AEffect*, float**, float**, int32_t) {};
    eff.setParameter = [](AEffect* e, int32_t k, float v) {
        Effect* s = self(e);
        if (!s->valid(k)) return;
        const size_t i = s->state->hostParam((size_t)k);
        s->state->set(i, s->state->fromNormal(i, v));
    };
    eff.getParameter = [](AEffect* e, int32_t k) {
        const Effect* s = self(e);
        if (!s->valid(k)) return 0.0f;
        const size_t i = s->state->hostParam((size_t)k);
        return (float)s->state->toNormal(i, s->state->get(i));
    };
    eff.processReplacing = [](AEffect* e, float** in, float** out, int32_t n) {
        Effect* s = self(e);
        const auto t0 = std::chrono::steady_clock::now();
        for (int32_t c = 0; c < e->numOutputs; ++c) std::memset(out[c], 0, sizeof(float) * (size_t)n);
        s->guiMidi();
        s->frames = n;
        s->proc->process(in, out, n);
        float pk[2] = {0, 0};
        for (int32_t c = 0; c < e->numOutputs && c < 2; ++c)
            for (int32_t k = 0; k < n; ++k) pk[c] = std::max(pk[c], std::fabs(out[c][k]));
        s->state->peak(pk[0], e->numOutputs > 1 ? pk[1] : pk[0]);
        if (s->outList.numEvents) {
            s->master(e, audioMasterProcessEvents, 0, 0, &s->outList, 0);
            s->outList.numEvents = 0;
            s->sysexUsed = 0;
        }
        if (n > 0) s->state->setLoad(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * s->rate / n);
    };
    if (info.midiOut) {
        outEvents.resize(kMaxOut);
        sysexBytes.resize(16384);
        proc->sendMidi = [this](int frame, const uint8_t* b, int n) { sendMidi(frame, b, n); };
    }
    proc->paramsChanged = [this] { redisplay = true; };
    eff.numPrograms = 1;
    eff.numParams = (int32_t)state->hostCount();
    eff.numInputs = info.inputs;
    eff.numOutputs = info.outputs;
    eff.flags = effFlagsCanReplacing | effFlagsProgramChunks | (skin ? effFlagsHasEditor : 0) | (info.instrument ? effFlagsIsSynth : 0);
    eff.object = this;
    eff.uniqueID = (int32_t)(fnv1a(info.id) & 0x7fffffff);
    eff.version = versionCode(info.version);
}

} // namespace
} // namespace hollow

#ifdef _WIN32
#define HOLLOW_VST_EXPORT extern "C" __declspec(dllexport)
#else
#define HOLLOW_VST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

HOLLOW_VST_EXPORT AEffect* VSTPluginMain(AudioMasterCallback master) {
    if (!master || !master(nullptr, audioMasterVersion, 0, 0, nullptr, 0)) return nullptr;
    return &(new hollow::Effect(master))->eff;
}

// Hosts from before VST 2.4 look for "main" (Windows) or "main_macho" (macOS).
#if defined(__APPLE__)
HOLLOW_VST_EXPORT AEffect* main_macho(AudioMasterCallback master) { return VSTPluginMain(master); }
#elif defined(__linux__)
HOLLOW_VST_EXPORT AEffect* mainLinux(AudioMasterCallback master) __asm__("main");
HOLLOW_VST_EXPORT AEffect* mainLinux(AudioMasterCallback master) { return VSTPluginMain(master); }
#elif defined(_WIN64)
#pragma comment(linker, "/EXPORT:main=VSTPluginMain")
#elif defined(_WIN32)
#pragma comment(linker, "/EXPORT:main=_VSTPluginMain")
#endif
