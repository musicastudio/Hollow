// The CLAP plug-in: one product instance on hollow::State, Editor and Processor. clap-wrapper turns
// this same plug-in into the VST3, the AUv2 and the standalone. entry.cpp exports it.
#include <clap/clap.h>
#include <hollow/hollow.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace hollow {
namespace {

#if defined(_WIN32)
const char* const kWindowApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
const char* const kWindowApi = CLAP_WINDOW_API_COCOA;
#else
const char* const kWindowApi = CLAP_WINDOW_API_X11;
#endif

const clap_plugin_descriptor_t* descriptor() {
    static const char* features[3] = {};
    static const clap_plugin_descriptor_t d = [] {
        const Info& i = pluginInfo();
        features[0] = i.instrument ? CLAP_PLUGIN_FEATURE_INSTRUMENT : CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
        features[1] = i.outputs == 2 ? CLAP_PLUGIN_FEATURE_STEREO : i.outputs == 1 ? CLAP_PLUGIN_FEATURE_MONO : nullptr;
        return clap_plugin_descriptor_t{CLAP_VERSION_INIT, i.id, i.name, i.vendor, i.url, "", "", i.version, i.description, features};
    }();
    return &d;
}

int midiSize(uint8_t status) {
    if (status >= 0xf8 || status == 0xf6) return 1;
    if ((status & 0xe0) == 0xc0 || status == 0xf1 || status == 0xf3) return 2;   // program, pressure
    return 3;
}

// An edit made in the editor, on its way to the host.
struct GuiEvent {
    uint16_t type;   // CLAP_EVENT_PARAM_GESTURE_BEGIN, _VALUE or _GESTURE_END
    uint32_t index;
    double plain;
};

struct Plugin final : Editor::Host {
    clap_plugin_t clap{};
    const clap_host_t* host;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_gui_t* hostGui = nullptr;
    std::shared_ptr<Skin> skin;
    std::unique_ptr<State> state;
    std::unique_ptr<Processor> proc;
    std::unique_ptr<Editor> editor;
    // GUI edits wait here for the next process() or flush(). The audio side only try-locks, so a
    // busy GUI delays its events by one block at most.
    std::mutex queueLock;
    std::vector<GuiEvent> queue, draining;
    std::atomic<bool> processing{false};
    const clap_output_events_t* midiOut = nullptr;   // the current process() call's output, for Processor::sendMidi
    int midiFrames = 0;                              // and its length; events are kept inside it
    double rate = 44100;                             // from activate, for State::setLoad
    std::atomic<bool> rescan{false};                 // Processor::paramsChanged, served on the main thread

    explicit Plugin(const clap_host_t* h) : host(h) {}

    // Processor::sendMidi: short messages as MIDI events, longer ones as sysex, on note-out port 0.
    // The host copies the sysex bytes during try_push, so they need not outlive the call.
    void sendMidi(int frame, const uint8_t* bytes, int size) {
        if (!midiOut || size <= 0) return;
        frame = std::clamp(frame, 0, std::max(midiFrames - 1, 0));
        if (size <= 3) {
            clap_event_midi_t m{};
            m.header = {sizeof m, (uint32_t)frame, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            std::memcpy(m.data, bytes, (size_t)size);
            midiOut->try_push(midiOut, &m.header);
        } else {
            clap_event_midi_sysex_t x{};
            x.header = {sizeof x, (uint32_t)frame, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0};
            x.buffer = bytes;
            x.size = (uint32_t)size;
            midiOut->try_push(midiOut, &x.header);
        }
    }

    // Stepped params are shown to the host as their step index 0..steps, the rest as plain values.
    double toClap(size_t i, double plain) const {
        const int steps = state->def(i).steps;
        return steps ? std::round(state->toNormal(i, plain) * steps) : plain;
    }
    double fromClap(size_t i, double v) const {
        const int steps = state->def(i).steps;
        return steps ? state->fromNormal(i, v / steps) : v;
    }

    // The State index of a param the host may see, by its CLAP id; -1 for unknown and host: false.
    int hostIndex(clap_id id) const {
        const int i = state->indexOfHostId(id);
        return i >= 0 && state->hostSlot((size_t)i) >= 0 ? i : -1;
    }

    // Editor::Host, on the GUI thread, with State indices; drain() sends them by CLAP id.
    void beginEdit(size_t i) override { push({CLAP_EVENT_PARAM_GESTURE_BEGIN, (uint32_t)i, 0}); }
    void edit(size_t i, double plain) override { push({CLAP_EVENT_PARAM_VALUE, (uint32_t)i, plain}); }
    void endEdit(size_t i) override { push({CLAP_EVENT_PARAM_GESTURE_END, (uint32_t)i, 0}); }
    bool resize(int w, int h) override { return hostGui && hostGui->request_resize(host, (uint32_t)w, (uint32_t)h); }

    void push(const GuiEvent& e) {
        if (state->hostSlot(e.index) < 0) return;   // host: false params never reach the host
        {
            std::lock_guard<std::mutex> g(queueLock);
            queue.push_back(e);
        }
        if (!processing && hostParams) hostParams->request_flush(host);
    }

    void drain(const clap_output_events_t* out) {
        {
            std::unique_lock<std::mutex> g(queueLock, std::try_to_lock);
            if (!g.owns_lock()) return;
            draining.swap(queue);
        }
        for (const GuiEvent& e : draining) {
            const clap_id id = state->def(e.index).hostId;
            if (e.type == CLAP_EVENT_PARAM_VALUE) {
                clap_event_param_value_t v{};
                v.header = {sizeof v, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
                v.param_id = id;
                v.note_id = -1;
                v.port_index = v.channel = v.key = -1;
                v.value = toClap(e.index, e.plain);
                out->try_push(out, &v.header);
            } else {
                clap_event_param_gesture_t g{};
                g.header = {sizeof g, 0, CLAP_CORE_EVENT_SPACE_ID, e.type, 0};
                g.param_id = id;
                out->try_push(out, &g.header);
            }
        }
        draining.clear();
    }

    // MIDI from the host: State sees it (key display, MIDI learn), then the processor.
    void hostMidi(int t, const uint8_t* b, int n) {
        state->midiIn(b, n);
        proc->midi(t, b, n);
    }

    void events(const clap_input_events_t* in) {
        const uint32_t n = in->size(in);
        for (uint32_t k = 0; k < n; ++k) {
            const clap_event_header_t* h = in->get(in, k);
            if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
            const int t = (int)h->time;
            switch (h->type) {
                case CLAP_EVENT_PARAM_VALUE: {
                    auto* e = (const clap_event_param_value_t*)h;
                    const int i = hostIndex(e->param_id);
                    if (i >= 0) state->set((size_t)i, fromClap((size_t)i, e->value));
                    break;
                }
                case CLAP_EVENT_MIDI: {
                    auto* e = (const clap_event_midi_t*)h;
                    hostMidi(t, e->data, midiSize(e->data[0]));
                    break;
                }
                case CLAP_EVENT_MIDI_SYSEX: {
                    auto* e = (const clap_event_midi_sysex_t*)h;
                    hostMidi(t, e->buffer, (int)e->size);
                    break;
                }
                case CLAP_EVENT_NOTE_ON:
                case CLAP_EVENT_NOTE_OFF: {
                    auto* e = (const clap_event_note_t*)h;
                    if (e->key < 0) break;
                    const bool on = h->type == CLAP_EVENT_NOTE_ON;
                    const int vel = std::clamp((int)std::lround(e->velocity * 127), on ? 1 : 0, 127);
                    const uint8_t m[3] = {uint8_t((on ? 0x90 : 0x80) | (e->channel < 0 ? 0 : e->channel & 15)), uint8_t(e->key & 127), uint8_t(vel)};
                    hostMidi(t, m, 3);
                    break;
                }
                default: break;
            }
        }
    }

    clap_process_status process(const clap_process_t* p) {
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t b = 0; b < p->audio_outputs_count; ++b)
            if (float** d = p->audio_outputs[b].data32)
                for (uint32_t c = 0; c < p->audio_outputs[b].channel_count; ++c) std::memset(d[c], 0, sizeof(float) * p->frames_count);
        uint8_t m[3];   // MIDI played on the editor goes first, at offset 0
        for (int n; state->popMidi(m, n);) proc->midi(0, m, n);
        events(p->in_events);
        drain(p->out_events);   // GUI edits go out at time 0, before any MIDI the processor sends
        const float* const* in = p->audio_inputs_count ? p->audio_inputs[0].data32 : nullptr;
        midiOut = p->out_events;
        midiFrames = (int)p->frames_count;
        if (p->audio_outputs_count && p->audio_outputs[0].data32) {
            float* const* out = p->audio_outputs[0].data32;
            proc->process(in, out, (int)p->frames_count);
            const uint32_t chans = p->audio_outputs[0].channel_count;
            float pk[2] = {0, 0};
            for (uint32_t c = 0; c < chans && c < 2; ++c)
                for (uint32_t k = 0; k < p->frames_count; ++k) pk[c] = std::max(pk[c], std::fabs(out[c][k]));
            state->peak(pk[0], chans > 1 ? pk[1] : pk[0]);
        }
        midiOut = nullptr;
        if (p->frames_count)
            state->setLoad(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * rate / p->frames_count);
        return CLAP_PROCESS_CONTINUE;
    }

    bool paramInfo(uint32_t k, clap_param_info_t* p) const {
        if (k >= state->hostCount()) return false;
        const size_t index = state->hostParam(k);
        const ParamDef& d = state->def(index);
        *p = {};
        p->id = d.hostId;
        p->flags = CLAP_PARAM_IS_AUTOMATABLE;
        if (d.steps) p->flags |= CLAP_PARAM_IS_STEPPED;
        if (d.steps && d.labels.size() == (size_t)d.steps + 1) p->flags |= CLAP_PARAM_IS_ENUM;
        std::snprintf(p->name, sizeof p->name, "%s", d.name.c_str());
        std::string module = d.id.substr(0, d.id.rfind('.') == std::string::npos ? 0 : d.id.rfind('.'));
        std::replace(module.begin(), module.end(), '.', '/');
        std::snprintf(p->module, sizeof p->module, "%s", module.c_str());
        p->min_value = d.steps ? 0 : d.min;
        p->max_value = d.steps ? d.steps : d.max;
        p->default_value = toClap(index, d.def);
        return true;
    }

    bool save(const clap_ostream_t* s) const {
        proc->saving();
        const std::string b = state->save();
        for (size_t at = 0; at < b.size();) {
            const int64_t n = s->write(s, b.data() + at, b.size() - at);
            if (n <= 0) return false;
            at += (size_t)n;
        }
        return true;
    }
    bool load(const clap_istream_t* s) {
        std::string b;
        char buf[4096];
        for (int64_t n; (n = s->read(s, buf, sizeof buf)) != 0;) {
            if (n < 0) return false;
            b.append(buf, (size_t)n);
        }
        if (!state->load(b)) return false;
        if (hostParams) hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);   // the host re-reads every value
        return true;
    }
};

Plugin* self(const clap_plugin_t* p) { return (Plugin*)p->plugin_data; }

const clap_plugin_params_t kParams = {
    [](const clap_plugin_t* p) { return (uint32_t)self(p)->state->hostCount(); },
    [](const clap_plugin_t* p, uint32_t i, clap_param_info_t* info) { return self(p)->paramInfo(i, info); },
    [](const clap_plugin_t* p, clap_id id, double* v) {
        const Plugin* s = self(p);
        const int i = s->hostIndex(id);
        if (i < 0) return false;
        *v = s->toClap((size_t)i, s->state->get((size_t)i));
        return true;
    },
    [](const clap_plugin_t* p, clap_id id, double v, char* out, uint32_t size) {
        const Plugin* s = self(p);
        const int i = s->hostIndex(id);
        if (i < 0 || !size) return false;
        std::snprintf(out, size, "%s", s->state->text((size_t)i, s->fromClap((size_t)i, v)).c_str());
        return true;
    },
    [](const clap_plugin_t* p, clap_id id, const char* text, double* v) {
        const Plugin* s = self(p);
        const int i = s->hostIndex(id);
        double plain = 0;
        if (i < 0 || !s->state->parse((size_t)i, text, plain)) return false;
        *v = s->toClap((size_t)i, plain);
        return true;
    },
    [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
        self(p)->events(in);
        self(p)->drain(out);
    },
};

const clap_plugin_state_t kState = {
    [](const clap_plugin_t* p, const clap_ostream_t* s) { return self(p)->save(s); },
    [](const clap_plugin_t* p, const clap_istream_t* s) { return self(p)->load(s); },
};

const clap_plugin_gui_t kGui = {
    [](const clap_plugin_t*, const char* api, bool floating) { return !floating && !std::strcmp(api, kWindowApi); },
    [](const clap_plugin_t*, const char** api, bool* floating) { *api = kWindowApi; *floating = false; return true; },
    [](const clap_plugin_t* p, const char* api, bool floating) {
        Plugin* s = self(p);
        if (floating || std::strcmp(api, kWindowApi) || !s->skin) return false;
        s->editor = std::make_unique<Editor>(s->skin, *s->state, *s);
        return true;
    },
    [](const clap_plugin_t* p) { self(p)->editor.reset(); },
    [](const clap_plugin_t*, double) { return false; },   // the editor keeps its own integer scale
    [](const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
        const Editor* e = self(p)->editor.get();
        if (!e) return false;
        *w = (uint32_t)e->width();
        *h = (uint32_t)e->height();
        return true;
    },
    [](const clap_plugin_t*) { return false; },
    [](const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; },
    [](const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
        const Editor* e = self(p)->editor.get();
        if (!e) return false;
        *w = (uint32_t)e->width();
        *h = (uint32_t)e->height();
        return true;
    },
    [](const clap_plugin_t* p, uint32_t w, uint32_t h) {
        const Editor* e = self(p)->editor.get();
        return e && w == (uint32_t)e->width() && h == (uint32_t)e->height();
    },
#if defined(_WIN32) || defined(__APPLE__)
    [](const clap_plugin_t* p, const clap_window_t* w) { return self(p)->editor && self(p)->editor->attach(w->ptr); },
#else
    [](const clap_plugin_t* p, const clap_window_t* w) { return self(p)->editor && self(p)->editor->attach((void*)(uintptr_t)w->x11); },
#endif
    [](const clap_plugin_t*, const clap_window_t*) { return false; },
    [](const clap_plugin_t*, const char*) {},
    [](const clap_plugin_t*) { return true; },
    [](const clap_plugin_t*) { return true; },
};

const clap_plugin_audio_ports_t kAudioPorts = {
    [](const clap_plugin_t*, bool input) -> uint32_t { return (input ? pluginInfo().inputs : pluginInfo().outputs) > 0; },
    [](const clap_plugin_t*, uint32_t index, bool input, clap_audio_port_info_t* p) {
        const int ch = input ? pluginInfo().inputs : pluginInfo().outputs;
        if (index || ch <= 0) return false;
        *p = {};
        p->id = 0;
        std::snprintf(p->name, sizeof p->name, "%s", input ? "Input" : "Output");
        p->flags = CLAP_AUDIO_PORT_IS_MAIN;
        p->channel_count = (uint32_t)ch;
        p->port_type = ch == 2 ? CLAP_PORT_STEREO : ch == 1 ? CLAP_PORT_MONO : nullptr;
        p->in_place_pair = CLAP_INVALID_ID;
        return true;
    },
};

const clap_plugin_note_ports_t kNotePorts = {
    [](const clap_plugin_t*, bool input) -> uint32_t { return input ? pluginInfo().midiIn : pluginInfo().midiOut; },
    [](const clap_plugin_t*, uint32_t index, bool input, clap_note_port_info_t* p) {
        if (index || !(input ? pluginInfo().midiIn : pluginInfo().midiOut)) return false;
        *p = {};
        p->id = 0;
        p->supported_dialects = input ? CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI : CLAP_NOTE_DIALECT_MIDI;
        p->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
        std::snprintf(p->name, sizeof p->name, "%s", input ? "MIDI In" : "MIDI Out");
        return true;
    },
};

const clap_plugin_t* create(const clap_host_t* host) {
    auto* s = new Plugin(host);
    clap_plugin_t& c = s->clap;
    c.desc = descriptor();
    c.plugin_data = s;
    c.init = [](const clap_plugin_t* p) {
        Plugin* s = self(p);
        s->hostParams = (const clap_host_params_t*)s->host->get_extension(s->host, CLAP_EXT_PARAMS);
        s->hostGui = (const clap_host_gui_t*)s->host->get_extension(s->host, CLAP_EXT_GUI);
        s->skin = loadSkin();
        s->state = std::make_unique<State>(s->skin ? skinParams(*s->skin) : std::vector<ParamDef>{});
        if (s->skin) applyLearnDefaults(*s->state, *s->skin);
        s->proc = createProcessor(*s->state);
        if (!s->proc) s->proc = std::make_unique<Processor>();
        if (pluginInfo().midiOut) s->proc->sendMidi = [s](int frame, const uint8_t* b, int n) { s->sendMidi(frame, b, n); };
        s->proc->paramsChanged = [s] {
            if (!s->rescan.exchange(true)) s->host->request_callback(s->host);
        };
        s->queue.reserve(256);
        s->draining.reserve(256);
        return true;
    };
    c.destroy = [](const clap_plugin_t* p) {
        Plugin* s = self(p);
        s->editor.reset();
        delete s;
    };
    c.activate = [](const clap_plugin_t* p, double rate, uint32_t, uint32_t maxFrames) {
        self(p)->rate = rate;
        self(p)->proc->prepare(rate, (int)maxFrames);
        return true;
    };
    c.deactivate = [](const clap_plugin_t*) {};
    c.start_processing = [](const clap_plugin_t* p) { self(p)->processing = true; return true; };
    c.stop_processing = [](const clap_plugin_t* p) { self(p)->processing = false; };
    c.reset = [](const clap_plugin_t*) {};
    c.process = [](const clap_plugin_t* p, const clap_process_t* pr) { return self(p)->process(pr); };
    c.get_extension = [](const clap_plugin_t*, const char* id) -> const void* {
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
        if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
        if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
        if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS) && (pluginInfo().midiIn || pluginInfo().midiOut)) return &kNotePorts;
        if (!std::strcmp(id, CLAP_EXT_GUI) && *kWindowApi) return &kGui;
        return nullptr;
    };
    c.on_main_thread = [](const clap_plugin_t* p) {
        Plugin* s = self(p);
        if (s->rescan.exchange(false) && s->hostParams) s->hostParams->rescan(s->host, CLAP_PARAM_RESCAN_VALUES);
    };
    return &c;
}

const clap_plugin_factory_t kFactory = {
    [](const clap_plugin_factory_t*) -> uint32_t { return 1; },
    [](const clap_plugin_factory_t*, uint32_t i) { return i ? nullptr : descriptor(); },
    [](const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
        return clap_version_is_compatible(host->clap_version) && !std::strcmp(id, descriptor()->id) ? create(host) : nullptr;
    },
};

} // namespace

// entry.cpp puts these in each module's clap_entry.
bool clapInit(const char*) { return true; }
void clapDeinit() {}
const void* clapFactory(const char* id) { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &kFactory; }

} // namespace hollow
