// The DXi (32-bit Windows only): a DirectShow filter speaking Cakewalk's MFX interfaces, built into
// the 32-bit VST2 DLL. The host pushes audio buffers through the input pin and MIDI through
// IMfxSoftSynth; each buffer is rendered by this module's own VST2 AEffect (formats/vst2), so the DXi
// gets the same parameters, state and editor as the VST2.
#define NOMINMAX
#include <streams.h>
#include <mmreg.h>
#include <olectl.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <hollow/hollow.h>
#include <vst2.h>
#include "mfx.h"

extern "C" AEffect* VSTPluginMain(AudioMasterCallback host);   // formats/vst2, this module

// HOLLOW_DXI_CLSID and HOLLOW_DXI_PAGE_CLSID come from hollow_add_plugin (name-based UUIDs of the
// product id unless the product passes DXI_CLSID).
const GUID kClsid = HOLLOW_DXI_CLSID;
const GUID kPageClsid = HOLLOW_DXI_PAGE_CLSID;
// Private: the page asks the object the host hands it for this to reach the filter behind it.
const GUID& kSelfIid = kClsid;

namespace {

constexpr int kMaxBlock = 1024;    // frames per render call; host buffers are split
constexpr int kMaxEvents = 512;    // MIDI events handed over per chunk
wchar_t kName[64];                 // Info::name, set in DllMain

// The stream format both pins carry: the plug-in's stereo output is mixed or spread to the host's channels.
struct Fmt {
    int rate = 44100, ch = 2, blockAlign = 8;
    bool isFloat = true;
};

bool parseFmt(const CMediaType* mt, Fmt& f) {
    if (!mt || *mt->Type() != MEDIATYPE_Audio || *mt->FormatType() != FORMAT_WaveFormatEx) return false;
    if (mt->FormatLength() < sizeof(WAVEFORMATEX)) return false;
    auto* w = (const WAVEFORMATEX*)mt->Format();
    DWORD tag = w->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && mt->FormatLength() >= sizeof(WAVEFORMATEXTENSIBLE))
        tag = ((const WAVEFORMATEXTENSIBLE*)w)->SubFormat.Data1;   // KSDATAFORMAT_SUBTYPE_* start with the tag
    const bool isFloat = tag == WAVE_FORMAT_IEEE_FLOAT && w->wBitsPerSample == 32;
    const bool isPcm16 = tag == WAVE_FORMAT_PCM && w->wBitsPerSample == 16;
    if (!(isFloat || isPcm16) || w->nChannels < 1 || w->nChannels > 8 || !w->nSamplesPerSec) return false;
    if (w->nBlockAlign != w->nChannels * w->wBitsPerSample / 8) return false;
    f.rate = (int)w->nSamplesPerSec; f.ch = w->nChannels; f.blockAlign = w->nBlockAlign; f.isFloat = isFloat;
    return true;
}

// One MIDI short message waiting for its sample time on the synth clock.
struct Midi { LONGLONG t; BYTE s, d1, d2; };

// Per-track state the host sets through IMfxNotify (track Key+/Vel+, forced channel, mute).
struct ChannelState {
    int midiChannel = -1;
    BYTE mute = 0;
    int velOfs = 0, velTrim = 0, keyOfs = 0, keyTrim = 0;
};

class Dxi;

class InPin : public CBaseInputPin {
public:
    InPin(Dxi* f, CCritSec* lock, HRESULT* phr);
    HRESULT CheckMediaType(const CMediaType* mt) override { Fmt f; return parseFmt(mt, f) ? S_OK : VFW_E_TYPE_NOT_ACCEPTED; }
    HRESULT SetMediaType(const CMediaType* mt) override;
    STDMETHODIMP Receive(IMediaSample* ms) override;
    STDMETHODIMP EndOfStream() override;
    STDMETHODIMP BeginFlush() override;
    STDMETHODIMP EndFlush() override;
    STDMETHODIMP NewSegment(REFERENCE_TIME t0, REFERENCE_TIME t1, double rate) override;
    const CMediaType& mediaType() const { return m_mt; }
private:
    Dxi* m_f;
};

class OutPin : public CBaseOutputPin {
public:
    OutPin(Dxi* f, CCritSec* lock, HRESULT* phr);
    HRESULT CheckMediaType(const CMediaType* mt) override;
    HRESULT GetMediaType(int i, CMediaType* mt) override;
    HRESULT DecideBufferSize(IMemAllocator* a, ALLOCATOR_PROPERTIES* p) override;
private:
    Dxi* m_f;
};

class Dxi : public CBaseFilter, public CPersistStream, public ISpecifyPropertyPages,
            public mfx::ISoftSynth2, public mfx::INotify, public mfx::IInputPort {
    friend class InPin;
    friend class OutPin;
public:
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN outer, HRESULT* phr) {
        auto* d = new Dxi(outer, phr);
        if (FAILED(*phr)) { delete d; return nullptr; }
        return d;
    }
    Dxi(LPUNKNOWN outer, HRESULT* phr);
    ~Dxi();

    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv) override;

    // CBaseFilter
    int GetPinCount() override { return 2; }
    CBasePin* GetPin(int n) override { return n == 0 ? (CBasePin*)&m_in : n == 1 ? (CBasePin*)&m_out : nullptr; }
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME t) override;
    STDMETHODIMP GetClassID(CLSID* id) override { if (!id) return E_POINTER; *id = kClsid; return S_OK; }

    // CPersistStream: the project state is the plug-in's chunk (State::save).
    int SizeMax() override;
    HRESULT WriteToStream(IStream* s) override;
    HRESULT ReadFromStream(IStream* s) override;

    // ISpecifyPropertyPages: the page hosts the editor.
    STDMETHODIMP GetPages(CAUUID* pages) override;

    // IMfxSoftSynth2
    STDMETHODIMP Connect(IUnknown* ctx) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP OnStart(LONG t) override;
    STDMETHODIMP OnLoop(LONG restart, LONG stop) override;
    STDMETHODIMP OnStop(LONG t) override;
    STDMETHODIMP OnEvents(LONG from, LONG thru, mfx::Channel ch, mfx::IEventQueue* q) override;
    STDMETHODIMP OnInput(mfx::Channel ch, mfx::IDataQueue* q) override;
    STDMETHODIMP GetBanksForPatchNames(int**, int*) override { return E_NOTIMPL; }
    STDMETHODIMP GetIsDrumPatch(int, int) override { return S_FALSE; }
    STDMETHODIMP GetIsDiatonicNoteNames(int, int) override { return S_FALSE; }
    STDMETHODIMP GetPatchNames(int, IUnknown**) override { return E_NOTIMPL; }
    STDMETHODIMP GetNoteNames(int, int, IUnknown**) override { return E_NOTIMPL; }
    STDMETHODIMP GetControllerNames(IUnknown**) override { return E_NOTIMPL; }
    STDMETHODIMP GetRpnNames(IUnknown**) override { return E_NOTIMPL; }
    STDMETHODIMP GetNrpnNames(IUnknown**) override { return E_NOTIMPL; }
    STDMETHODIMP GetInstrument(int, IUnknown**) override { return E_NOTIMPL; }
    // IMfxNotify
    STDMETHODIMP OnMfxNotify(mfx::NotifyMsg* m) override;
    // IMfxInputPort: a DXi cannot send MIDI back to the host, so the callback is only kept.
    STDMETHODIMP SetInputCallback(IUnknown* cb) override;
    STDMETHODIMP GetInputCallback(IUnknown** cb) override;

    intptr_t dispatch(int32_t op, int32_t idx = 0, intptr_t val = 0, void* ptr = nullptr, float opt = 0) {
        return m_eff->dispatcher(m_eff, op, idx, val, ptr, opt);
    }
    HWND m_editor = nullptr;   // the page window the editor lives in, while open

private:
    HRESULT render(IMediaSample* in);
    void renderFrames(BYTE* dst, long n);
    int collectEvents(long n);
    void push(LONGLONG t, BYTE s, BYTE d1, BYTE d2);
    void releaseNotes(LONGLONG t);
    LONGLONG ticksToSamples(LONG ticks);
    LONGLONG toClock(LONG ticks) { return ticksToSamples(ticks) - m_sampStart + m_loopUnroll; }
    void allocate();
    void freeResources();
    ChannelState& chan(mfx::Channel c);

    CCritSec m_lock;              // filter state
    CCritSec m_rx;                // streaming (Receive), taken by Stop to wait out a render
    InPin m_in;
    OutPin m_out;
    Fmt m_fmt;
    AEffect* m_eff = nullptr;
    bool m_resources = false;

    // MIDI, written by the host's sequencer thread and drained by the streaming thread.
    std::mutex m_q;
    std::vector<Midi> m_pending;
    std::vector<std::pair<mfx::Channel, ChannelState>> m_chans;
    uint64_t m_sounding[16][2] = {};   // notes held, so a stop can release them
    std::atomic<LONGLONG> m_clock{0};  // frames rendered since OnStart
    LONGLONG m_sampStart = 0, m_loopUnroll = 0;
    mfx::ITempoMap* m_tempo = nullptr;
    mfx::ITimeConverter* m_conv = nullptr;
    IUnknown* m_inputCb = nullptr;

    // Scratch for one chunk: the plug-in's planar buffers and the VstEvents it is handed.
    std::vector<float> m_bufs;
    std::vector<float*> m_ins, m_outs;
    std::vector<VstMidiEvent> m_mev;
    std::vector<char> m_evBlock;
    std::string m_chunk;
};

Dxi* g_latest = nullptr;   // newest instance, for a page asked its size before it has one

// Editor scale changed with the page open: resize the page, and the windows above it by the same
// amount so the whole editor stays in view. Neither DXi, MFX nor IPropertyPage lets a page ask its
// frame for room, so every parent keeps its margins around the page, up to the top-level window.
// It stops at an MDI client, and before any parent that holds much more than the page (over 100 px
// of other content), whose layout is not ours to change.
void resizeWithHost(HWND page, int w, int h) {
    RECT r;
    GetWindowRect(page, &r);
    int cw = r.right - r.left, ch = r.bottom - r.top;   // the child's size before this resize
    const int dx = w - cw, dy = h - ch;
    SetWindowPos(page, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!dx && !dy) return;
    for (HWND a = GetAncestor(page, GA_PARENT); a && a != GetDesktopWindow(); a = GetAncestor(a, GA_PARENT)) {
        wchar_t cls[32] = {};
        GetClassNameW(a, cls, 31);
        if (!_wcsicmp(cls, L"MDIClient")) break;
        GetWindowRect(a, &r);
        const int aw = r.right - r.left, ah = r.bottom - r.top;
        if (aw - cw > 100 || ah - ch > 100) break;
        SetWindowPos(a, nullptr, 0, 0, aw + dx, ah + dy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        cw = aw;
        ch = ah;
    }
}

// The host callback the DXi gives the AEffect it hosts. Edits from the editor stay in the plug-in's
// State and reach the project through the saved chunk; DXi automation is not implemented. A DXi has
// no MIDI output, so the MIDI a product sends (Processor::sendMidi, which the VST2 layer sets for
// Info::midiOut) arrives here as audioMasterProcessEvents and is dropped.
intptr_t VSTCALLBACK dxiMaster(AEffect* e, int32_t op, int32_t idx, intptr_t val, void*, float) {
    Dxi* d = e ? (Dxi*)e->user : nullptr;   // unset until VSTPluginMain returns
    switch (op) {
        case audioMasterVersion: return 2400;
        case audioMasterCurrentId: return e ? e->uniqueID : 0;
        case audioMasterSizeWindow:
            if (d && d->m_editor) resizeWithHost(d->m_editor, idx, (int)val);
            return 1;
        default: return 0;
    }
}

InPin::InPin(Dxi* f, CCritSec* lock, HRESULT* phr) : CBaseInputPin(NAME("Hollow DXi in"), f, lock, phr, L"Input"), m_f(f) {}

HRESULT InPin::SetMediaType(const CMediaType* mt) {
    HRESULT hr = CBaseInputPin::SetMediaType(mt);
    if (FAILED(hr)) return hr;
    parseFmt(mt, m_f->m_fmt);
    return S_OK;
}

STDMETHODIMP InPin::Receive(IMediaSample* ms) {
    CAutoLock lock(&m_f->m_rx);
    HRESULT hr = CBaseInputPin::Receive(ms);
    return hr == S_OK ? m_f->render(ms) : hr;
}
STDMETHODIMP InPin::EndOfStream() {
    CAutoLock lock(&m_f->m_rx);
    HRESULT hr = CheckStreaming();
    return hr == S_OK ? m_f->m_out.DeliverEndOfStream() : hr;
}
STDMETHODIMP InPin::BeginFlush() {
    HRESULT hr = CBaseInputPin::BeginFlush();
    return SUCCEEDED(hr) ? m_f->m_out.DeliverBeginFlush() : hr;
}
STDMETHODIMP InPin::EndFlush() {
    HRESULT hr = m_f->m_out.DeliverEndFlush();
    return SUCCEEDED(hr) ? CBaseInputPin::EndFlush() : hr;
}
STDMETHODIMP InPin::NewSegment(REFERENCE_TIME t0, REFERENCE_TIME t1, double rate) {
    CBasePin::NewSegment(t0, t1, rate);
    return m_f->m_out.DeliverNewSegment(t0, t1, rate);
}

OutPin::OutPin(Dxi* f, CCritSec* lock, HRESULT* phr) : CBaseOutputPin(NAME("Hollow DXi out"), f, lock, phr, L"Output"), m_f(f) {}

// The output carries exactly the input's format (the host sets both), so a buffer in makes a
// buffer out of the same size.
HRESULT OutPin::CheckMediaType(const CMediaType* mt) {
    Fmt f;
    if (!parseFmt(mt, f)) return VFW_E_TYPE_NOT_ACCEPTED;
    if (m_f->m_in.IsConnected() && *mt != m_f->m_in.mediaType()) return VFW_E_TYPE_NOT_ACCEPTED;
    return S_OK;
}
HRESULT OutPin::GetMediaType(int i, CMediaType* mt) {
    if (i < 0) return E_INVALIDARG;
    if (i > 0) return VFW_S_NO_MORE_ITEMS;
    if (!m_f->m_in.IsConnected()) return E_UNEXPECTED;
    *mt = m_f->m_in.mediaType();
    return S_OK;
}
HRESULT OutPin::DecideBufferSize(IMemAllocator* a, ALLOCATOR_PROPERTIES* p) {
    ALLOCATOR_PROPERTIES in{};
    IMemAllocator* ia = nullptr;
    if (m_f->m_in.GetAllocator(&ia) == S_OK) { ia->GetProperties(&in); ia->Release(); }
    p->cBuffers = std::max<long>(p->cBuffers, 2);
    p->cbBuffer = std::max<long>(p->cbBuffer, std::max<long>(in.cbBuffer, kMaxBlock * m_f->m_fmt.blockAlign));
    ALLOCATOR_PROPERTIES got{};
    HRESULT hr = a->SetProperties(p, &got);
    if (FAILED(hr)) return hr;
    return got.cbBuffer < p->cbBuffer ? E_FAIL : S_OK;
}

Dxi::Dxi(LPUNKNOWN outer, HRESULT* phr)
    : CBaseFilter(NAME("Hollow DXi"), outer, &m_lock, kClsid),
      CPersistStream(outer, phr), m_in(this, &m_lock, phr), m_out(this, &m_lock, phr) {
    if (FAILED(*phr)) return;
    m_eff = VSTPluginMain(&dxiMaster);
    if (!m_eff || m_eff->magic != kEffectMagic) { m_eff = nullptr; *phr = E_FAIL; return; }
    m_eff->user = this;
    g_latest = this;
    dispatch(effOpen);
    m_pending.reserve(4096);
    m_mev.resize(kMaxEvents);
    m_evBlock.resize(offsetof(VstEvents, events) + kMaxEvents * sizeof(VstEvent*));
    const int nIn = std::max(m_eff->numInputs, 0), nOut = std::max(m_eff->numOutputs, 2);
    m_bufs.assign((size_t)(nIn + nOut) * kMaxBlock, 0.0f);
    for (int i = 0; i < nIn; ++i) m_ins.push_back(&m_bufs[(size_t)i * kMaxBlock]);
    for (int i = 0; i < nOut; ++i) m_outs.push_back(&m_bufs[(size_t)(nIn + i) * kMaxBlock]);
}

Dxi::~Dxi() {
    if (g_latest == this) g_latest = nullptr;
    freeResources();
    Disconnect();
    if (m_eff) { dispatch(effClose); m_eff = nullptr; }
}

STDMETHODIMP Dxi::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    CheckPointer(ppv, E_POINTER);
    if (riid == kSelfIid) { *ppv = this; AddRef(); return S_OK; }
    if (riid == IID_IPersistStream) return GetInterface((IPersistStream*)this, ppv);
    if (riid == IID_ISpecifyPropertyPages) return GetInterface((ISpecifyPropertyPages*)this, ppv);
    if (riid == mfx::IID_IMfxSoftSynth || riid == mfx::IID_IMfxSoftSynth2) return GetInterface((mfx::ISoftSynth2*)this, ppv);
    if (riid == mfx::IID_IMfxNotify) return GetInterface((mfx::INotify*)this, ppv);
    if (riid == mfx::IID_IMfxInputPort) return GetInterface((mfx::IInputPort*)this, ppv);
    return CBaseFilter::NonDelegatingQueryInterface(riid, ppv);
}

// The plug-in is switched on when the graph leaves Stopped and off when it returns.
void Dxi::allocate() {
    if (m_resources) return;
    dispatch(effSetSampleRate, 0, 0, nullptr, (float)m_fmt.rate);
    dispatch(effSetBlockSize, 0, kMaxBlock);
    dispatch(effMainsChanged, 0, 1);
    dispatch(effStartProcess);
    m_resources = true;
}
void Dxi::freeResources() {
    if (!m_resources || !m_eff) return;
    dispatch(effStopProcess);
    dispatch(effMainsChanged, 0, 0);
    m_resources = false;
    std::lock_guard<std::mutex> g(m_q);
    std::memset(m_sounding, 0, sizeof m_sounding);   // mains off silences every voice
}

STDMETHODIMP Dxi::Stop() {
    CAutoLock lock(&m_lock);
    if (m_State == State_Stopped) return S_OK;
    HRESULT hr = CBaseFilter::Stop();   // pins go inactive, so Receive stops accepting samples
    CAutoLock rx(&m_rx);                // wait out a render in flight
    freeResources();
    return hr;
}
STDMETHODIMP Dxi::Pause() {
    CAutoLock lock(&m_lock);
    if (m_State == State_Stopped) allocate();
    HRESULT hr = CBaseFilter::Pause();
    if (!m_in.IsConnected()) m_out.DeliverEndOfStream();
    return hr;
}
STDMETHODIMP Dxi::Run(REFERENCE_TIME t) {
    CAutoLock lock(&m_lock);
    HRESULT hr = CBaseFilter::Run(t);
    if (!m_in.IsConnected()) m_out.DeliverEndOfStream();
    return hr;
}

// One host buffer in, one buffer out of the same size, rendered in chunks of kMaxBlock.
HRESULT Dxi::render(IMediaSample* in) {
    if (!m_out.IsConnected() || !m_eff) return S_OK;
    long n = in->GetActualDataLength() / m_fmt.blockAlign;
    IMediaSample* out = nullptr;
    HRESULT hr = m_out.GetDeliveryBuffer(&out, nullptr, nullptr, 0);
    if (FAILED(hr)) return hr;
    REFERENCE_TIME t0, t1;
    if (in->GetTime(&t0, &t1) == S_OK) out->SetTime(&t0, &t1);
    out->SetSyncPoint(in->IsSyncPoint() == S_OK);
    out->SetDiscontinuity(in->IsDiscontinuity() == S_OK);
    out->SetPreroll(in->IsPreroll() == S_OK);
    BYTE* dst = nullptr;
    out->GetPointer(&dst);
    n = std::min<long>(n, out->GetSize() / m_fmt.blockAlign);
    renderFrames(dst, n);
    out->SetActualDataLength(n * m_fmt.blockAlign);
    hr = m_out.Deliver(out);
    out->Release();
    return hr;
}

// The VST2 layer does the State bookkeeping for each chunk: MIDI played on the editor goes to the
// processor ahead of the chunk's own events, those events go through State::midiIn, and
// processReplacing reports State::setLoad.
void Dxi::renderFrames(BYTE* dst, long n) {
    const int ch = m_fmt.ch;
    for (long done = 0; done < n;) {
        const long c = std::min<long>(n - done, kMaxBlock);
        if (const int k = collectEvents(c)) {
            auto* ev = (VstEvents*)m_evBlock.data();
            ev->numEvents = k;
            ev->reserved = 0;
            dispatch(effProcessEvents, 0, 0, ev);
        }
        for (float* o : m_outs) std::memset(o, 0, sizeof(float) * c);
        m_eff->processReplacing(m_eff, m_ins.data(), m_outs.data(), c);
        const float* L = m_outs[0];
        const float* R = m_outs[1];
        for (long i = 0; i < c; ++i) {
            const float l = L[i], r = R[i];
            for (int k = 0; k < ch; ++k) {
                const float v = ch == 1 ? 0.5f * (l + r) : k == 0 ? l : k == 1 ? r : 0.0f;
                const size_t at = (size_t)(done + i) * ch + k;
                if (m_fmt.isFloat) ((float*)dst)[at] = v;
                else ((short*)dst)[at] = (short)std::clamp(lroundf(v * 32767.0f), -32768L, 32767L);
            }
        }
        m_clock += c;
        done += c;
    }
}

// Hand over every pending message that falls inside the next c frames, at its offset in the chunk.
// A message already late plays at the start of the chunk.
int Dxi::collectEvents(long c) {
    std::lock_guard<std::mutex> g(m_q);
    const LONGLONG now = m_clock, end = now + c;
    auto** ptrs = (VstEvent**)(m_evBlock.data() + offsetof(VstEvents, events));
    size_t i = 0;
    int k = 0;
    for (; i < m_pending.size() && m_pending[i].t < end && k < kMaxEvents; ++i, ++k) {
        const Midi& m = m_pending[i];
        VstMidiEvent& e = m_mev[k];
        std::memset(&e, 0, sizeof e);
        e.type = kVstMidiType;
        e.byteSize = sizeof e;
        e.deltaFrames = (int32_t)std::max<LONGLONG>(0, m.t - now);
        e.midiData[0] = (char)m.s; e.midiData[1] = (char)m.d1; e.midiData[2] = (char)m.d2;
        ptrs[k] = (VstEvent*)&e;
        const int ch = m.s & 15, kind = m.s & 0xf0;
        const uint64_t bit = 1ull << (m.d1 & 63);
        if (kind == 0x90 && m.d2) m_sounding[ch][m.d1 >> 6] |= bit;
        else if (kind == 0x80 || kind == 0x90) m_sounding[ch][m.d1 >> 6] &= ~bit;
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + i);
    return k;
}

// Insert keeping m_pending ordered by time (stable, so same-time messages keep host order).
void Dxi::push(LONGLONG t, BYTE s, BYTE d1, BYTE d2) {
    auto at = std::upper_bound(m_pending.begin(), m_pending.end(), t, [](LONGLONG v, const Midi& m) { return v < m.t; });
    m_pending.insert(at, Midi{t, s, d1, d2});
}

// Release every held note (transport stop): note-offs plus sustain off. Caller holds m_q.
void Dxi::releaseNotes(LONGLONG t) {
    for (int ch = 0; ch < 16; ++ch) {
        bool any = false;
        for (int key = 0; key < 128; ++key)
            if (m_sounding[ch][key >> 6] & (1ull << (key & 63))) { push(t, (BYTE)(0x80 | ch), (BYTE)key, 64); any = true; }
        if (any) push(t, (BYTE)(0xb0 | ch), 64, 0);
    }
}

LONGLONG Dxi::ticksToSamples(LONG ticks) {
    if (m_conv) {
        mfx::Time t{};
        t.format = mfx::TF_TICKS;
        t.ticks = ticks;
        if (SUCCEEDED(m_conv->ConvertMfxTime(&t, mfx::TF_SAMPLES))) return t.samples;
    }
    if (m_tempo) return (LONGLONG)m_tempo->TicksToMsecs(ticks) * m_fmt.rate / 1000;
    return (LONGLONG)ticks * m_fmt.rate / 1920;   // no host map: 120 BPM at 960 PPQ
}

ChannelState& Dxi::chan(mfx::Channel c) {
    for (auto& p : m_chans) if (p.first == c) return p.second;
    m_chans.push_back({c, ChannelState{}});
    return m_chans.back().second;
}

// MFX hands the context over with a reference the synth must drop.
STDMETHODIMP Dxi::Connect(IUnknown* ctx) {
    if (!ctx) return E_POINTER;
    HRESULT hr = E_UNEXPECTED;   // connected already
    {
        std::lock_guard<std::mutex> g(m_q);
        if (!m_tempo) {
            hr = ctx->QueryInterface(mfx::IID_IMfxTempoMap, (void**)&m_tempo);
            if (SUCCEEDED(hr)) ctx->QueryInterface(mfx::IID_IMfxTimeConverter, (void**)&m_conv);
        }
    }
    ctx->Release();
    return hr;
}
STDMETHODIMP Dxi::Disconnect() {
    std::lock_guard<std::mutex> g(m_q);
    if (m_tempo) m_tempo->Release(), m_tempo = nullptr;
    if (m_conv) m_conv->Release(), m_conv = nullptr;
    if (m_inputCb) m_inputCb->Release(), m_inputCb = nullptr;
    return S_OK;
}

STDMETHODIMP Dxi::OnStart(LONG t) {
    std::lock_guard<std::mutex> g(m_q);
    m_pending.clear();
    releaseNotes(m_clock);
    m_sampStart = ticksToSamples(t);
    m_loopUnroll = 0;
    m_clock = 0;
    for (Midi& m : m_pending) m.t = 0;   // the release goes out at the top of the new clock
    return S_OK;
}
STDMETHODIMP Dxi::OnLoop(LONG restart, LONG stop) {
    std::lock_guard<std::mutex> g(m_q);
    m_loopUnroll += ticksToSamples(stop) - ticksToSamples(restart);
    return S_OK;
}
STDMETHODIMP Dxi::OnStop(LONG) {
    std::lock_guard<std::mutex> g(m_q);
    m_pending.clear();
    releaseNotes(m_clock);
    return S_OK;
}

BYTE clamp7(int v) { return (BYTE)std::clamp(v, 0, 127); }

// Playback: the host hands over a window of track events ahead of time, in ticks.
STDMETHODIMP Dxi::OnEvents(LONG, LONG, mfx::Channel ch, mfx::IEventQueue* q) {
    if (!q) return E_POINTER;
    int n = 0;
    HRESULT hr = q->GetCount(&n);
    if (FAILED(hr)) return hr;
    std::lock_guard<std::mutex> g(m_q);
    ChannelState& cs = chan(ch);
    for (int i = 0; i < n; ++i) {
        mfx::Event e{};
        if (FAILED(q->GetAt(i, &e))) break;
        const BYTE c = (BYTE)((cs.midiChannel >= 0 ? cs.midiChannel : e.chan) & 15);
        const LONGLONG t = toClock(e.time);
        switch (e.type) {
            case mfx::Event::Note: {
                if (cs.mute) break;
                const BYTE key = clamp7(e.u.note.key + cs.keyOfs + cs.keyTrim);
                push(t, (BYTE)(0x90 | c), key, (BYTE)std::clamp(e.u.note.vel + cs.velOfs + cs.velTrim, 1, 127));
                if (e.u.note.duration != 0xffffffff)
                    push(toClock(e.time + (LONG)e.u.note.duration), (BYTE)(0x80 | c), key, e.u.note.velOff ? e.u.note.velOff : 64);
                break;
            }
            case mfx::Event::KeyAft:  push(t, (BYTE)(0xa0 | c), e.u.keyAft.key, e.u.keyAft.amt); break;
            case mfx::Event::Control: push(t, (BYTE)(0xb0 | c), e.u.control.num, e.u.control.val); break;
            case mfx::Event::Patch:   push(t, (BYTE)(0xc0 | c), e.u.patch.patch, 0); break;
            case mfx::Event::ChanAft: push(t, (BYTE)(0xd0 | c), e.u.chanAft.amt, 0); break;
            case mfx::Event::Wheel: {
                const int v = std::clamp(e.u.wheel.val + 8192, 0, 16383);
                push(t, (BYTE)(0xe0 | c), (BYTE)(v & 127), (BYTE)(v >> 7));
                break;
            }
            case mfx::Event::RPN:
            case mfx::Event::NRPN: {
                const bool rpn = e.type == mfx::Event::RPN;
                push(t, (BYTE)(0xb0 | c), rpn ? 101 : 99, (BYTE)(e.u.rpn.num >> 7 & 127));
                push(t, (BYTE)(0xb0 | c), rpn ? 100 : 98, (BYTE)(e.u.rpn.num & 127));
                push(t, (BYTE)(0xb0 | c), 6, (BYTE)(e.u.rpn.val >> 7 & 127));
                push(t, (BYTE)(0xb0 | c), 38, (BYTE)(e.u.rpn.val & 127));
                break;
            }
            case mfx::Event::ShortMsg: {
                const DWORD m = e.u.shortMsg;
                if ((m & 0xf0) >= 0x80 && (m & 0xf0) < 0xf0) push(t, (BYTE)m, (BYTE)(m >> 8 & 127), (BYTE)(m >> 16 & 127));
                break;
            }
            case mfx::Event::MuteMask: cs.mute = (BYTE)((cs.mute & ~e.chan) | e.port); break;
            case mfx::Event::VelOfs:  cs.velOfs = (char)e.port; break;
            case mfx::Event::VelTrim: cs.velTrim = (char)e.port; break;
            case mfx::Event::KeyOfs:  cs.keyOfs = (char)e.port; break;
            case mfx::Event::KeyTrim: cs.keyTrim = (char)e.port; break;
            default: break;   // sysex, text and lyrics are not passed on
        }
    }
    return S_OK;
}

// Live playing: raw MIDI, due now.
STDMETHODIMP Dxi::OnInput(mfx::Channel ch, mfx::IDataQueue* q) {
    if (!q) return E_POINTER;
    int n = 0;
    HRESULT hr = q->GetCount(&n);
    if (FAILED(hr)) return hr;
    std::lock_guard<std::mutex> g(m_q);
    ChannelState& cs = chan(ch);
    const LONGLONG now = m_clock;
    for (int i = 0; i < n; ++i) {
        mfx::Data d{};
        if (FAILED(q->GetAt(i, &d))) break;
        const int kind = d.status & 0xf0;
        if (kind < 0x80 || kind == 0xf0) continue;
        const BYTE s = (BYTE)(kind | ((cs.midiChannel >= 0 ? cs.midiChannel : d.status) & 15));
        BYTE d1 = d.data1 & 127, d2 = d.data2 & 127;
        if (kind == 0x80 || kind == 0x90) {
            d1 = clamp7(d1 + cs.keyOfs + cs.keyTrim);
            if (kind == 0x90 && d2) d2 = (BYTE)std::clamp(d2 + cs.velOfs + cs.velTrim, 1, 127);
        }
        push(now, s, d1, d2);
    }
    return S_OK;
}

STDMETHODIMP Dxi::OnMfxNotify(mfx::NotifyMsg* m) {
    if (!m) return E_POINTER;
    std::lock_guard<std::mutex> g(m_q);
    ChannelState& cs = chan(m->channel);
    switch (m->type) {
        case mfx::NotifyMsg::ChannelMidiChannel: cs.midiChannel = m->a; break;
        case mfx::NotifyMsg::ChannelMuteMask: cs.mute = (BYTE)((cs.mute & ~(BYTE)m->b) | (BYTE)m->a); break;
        case mfx::NotifyMsg::ChannelVelOfs: cs.velOfs = m->a; break;
        case mfx::NotifyMsg::ChannelVelTrim: cs.velTrim = m->a; break;
        case mfx::NotifyMsg::ChannelKeyOfs: cs.keyOfs = m->a; break;
        case mfx::NotifyMsg::ChannelKeyTrim: cs.keyTrim = m->a; break;
        default: break;
    }
    return S_OK;
}

STDMETHODIMP Dxi::SetInputCallback(IUnknown* cb) {
    std::lock_guard<std::mutex> g(m_q);
    if (m_inputCb) m_inputCb->Release();
    m_inputCb = cb;
    if (cb) cb->AddRef();
    return S_OK;
}
STDMETHODIMP Dxi::GetInputCallback(IUnknown** cb) {
    if (!cb) return E_POINTER;
    std::lock_guard<std::mutex> g(m_q);
    *cb = m_inputCb;
    if (m_inputCb) m_inputCb->AddRef();
    return S_OK;
}

int Dxi::SizeMax() {
    void* p = nullptr;
    return (int)sizeof(DWORD) + (int)dispatch(effGetChunk, 0, 0, &p);
}
HRESULT Dxi::WriteToStream(IStream* s) {
    CAutoLock lock(&m_lock);
    void* p = nullptr;
    const DWORD n = (DWORD)dispatch(effGetChunk, 0, 0, &p);
    HRESULT hr = s->Write(&n, sizeof n, nullptr);
    if (SUCCEEDED(hr) && n) hr = s->Write(p, n, nullptr);
    return hr;
}
HRESULT Dxi::ReadFromStream(IStream* s) {
    CAutoLock lock(&m_lock);
    DWORD n = 0;
    HRESULT hr = s->Read(&n, sizeof n, nullptr);
    if (FAILED(hr) || !n || n > (64u << 20)) return FAILED(hr) ? hr : S_OK;
    m_chunk.resize(n);
    ULONG got = 0;
    hr = s->Read(m_chunk.data(), n, &got);
    if (FAILED(hr) || got != n) return FAILED(hr) ? hr : E_FAIL;
    dispatch(effSetChunk, 0, (intptr_t)n, m_chunk.data());
    return S_OK;
}

STDMETHODIMP Dxi::GetPages(CAUUID* pages) {
    if (!pages) return E_POINTER;
    pages->cElems = 1;
    pages->pElems = (GUID*)CoTaskMemAlloc(sizeof(GUID));
    if (!pages->pElems) return E_OUTOFMEMORY;
    pages->pElems[0] = kPageClsid;
    return S_OK;
}

// The property page: a plain child window in the host's property frame, with the editor opened
// inside it.
class Page : public CUnknown, public IPropertyPage {
public:
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN outer, HRESULT* phr) { return new Page(outer, phr); }
    Page(LPUNKNOWN outer, HRESULT* phr) : CUnknown(NAME("Hollow DXi page"), outer, phr) {}
    ~Page() { Deactivate(); SetObjects(0, nullptr); SetPageSite(nullptr); }

    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IPropertyPage) return GetInterface((IPropertyPage*)this, ppv);
        return CUnknown::NonDelegatingQueryInterface(riid, ppv);
    }

    STDMETHODIMP SetPageSite(IPropertyPageSite* site) override {
        if (m_site) m_site->Release();
        m_site = site;
        if (site) site->AddRef();
        return S_OK;
    }
    STDMETHODIMP Activate(HWND parent, LPCRECT rc, BOOL) override {
        if (!m_dxi || m_wnd) return E_UNEXPECTED;
        m_wnd = CreateWindowExW(0, windowClass(), kName, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, rc->left, rc->top,
                                rc->right - rc->left, rc->bottom - rc->top, parent, nullptr, g_hInst, this);
        if (!m_wnd) return E_FAIL;
        // Some hosts' DX windows do not clip their children, so any repaint of their own would land
        // on the editor. Clip every window up to the top-level one while the page is up (GA_PARENT
        // never reaches an owner, so the host's main window is never touched).
        for (HWND w = parent; w && w != GetDesktopWindow(); w = GetAncestor(w, GA_PARENT)) {
            const LONG_PTR st = GetWindowLongPtrW(w, GWL_STYLE);
            if (st & WS_CLIPCHILDREN) continue;
            SetWindowLongPtrW(w, GWL_STYLE, st | WS_CLIPCHILDREN);
            m_clipped.push_back(w);
        }
        m_dxi->dispatch(effEditOpen, 0, 0, m_wnd);
        m_dxi->m_editor = m_wnd;
        SetTimer(m_wnd, 1, 30, nullptr);
        return S_OK;
    }
    STDMETHODIMP Deactivate() override {
        if (!m_wnd) return S_OK;
        KillTimer(m_wnd, 1);
        m_onScreen = false;
        if (m_dxi) { m_dxi->m_editor = nullptr; m_dxi->dispatch(effEditClose); }
        DestroyWindow(m_wnd);
        m_wnd = nullptr;
        for (HWND w : m_clipped)
            if (IsWindow(w)) SetWindowLongPtrW(w, GWL_STYLE, GetWindowLongPtrW(w, GWL_STYLE) & ~WS_CLIPCHILDREN);
        m_clipped.clear();
        return S_OK;
    }
    STDMETHODIMP GetPageInfo(PROPPAGEINFO* pi) override {
        if (!pi) return E_POINTER;
        const size_t cb = (wcslen(kName) + 1) * sizeof(wchar_t);
        pi->cb = sizeof *pi;
        pi->pszTitle = (LPOLESTR)CoTaskMemAlloc(cb);
        if (pi->pszTitle) std::memcpy(pi->pszTitle, kName, cb);
        // Property frames ask for the size before SetObjects, so fall back on the newest instance:
        // the editor is the same size in every instance.
        pi->size = {800, 600};
        ERect* r = nullptr;
        Dxi* d = m_dxi ? m_dxi : g_latest;
        if (d && d->dispatch(effEditGetRect, 0, 0, &r) && r) pi->size = {r->right - r->left, r->bottom - r->top};
        pi->pszDocString = nullptr;
        pi->pszHelpFile = nullptr;
        pi->dwHelpContext = 0;
        return S_OK;
    }
    STDMETHODIMP SetObjects(ULONG n, IUnknown** objs) override {
        if (m_dxi) { Deactivate(); m_dxi->Release(); m_dxi = nullptr; }
        if (n && objs && objs[0]) objs[0]->QueryInterface(kSelfIid, (void**)&m_dxi);
        return !n || m_dxi ? S_OK : E_NOINTERFACE;
    }
    STDMETHODIMP Show(UINT cmd) override { if (m_wnd) ShowWindow(m_wnd, cmd); return S_OK; }
    STDMETHODIMP Move(LPCRECT rc) override {
        if (m_wnd) MoveWindow(m_wnd, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, TRUE);
        return S_OK;
    }
    STDMETHODIMP IsPageDirty() override { return S_FALSE; }
    STDMETHODIMP Apply() override { return S_OK; }
    STDMETHODIMP Help(LPCOLESTR) override { return E_NOTIMPL; }
    STDMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }

private:
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        auto* page = (Page*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (msg == WM_TIMER && page && page->m_dxi) {
            // Some hosts activate and show the page while their window has redraw switched off, then
            // repaint only themselves, so the editor never gets a full paint. Give it one each time
            // the page actually comes on screen.
            const bool onScreen = IsWindowVisible(h) != FALSE;
            if (onScreen && !page->m_onScreen) RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            page->m_onScreen = onScreen;
            page->m_dxi->dispatch(effEditIdle);
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
    static const wchar_t* windowClass() {
        static const wchar_t* name = [] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = &proc;
            wc.hInstance = g_hInst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);   // any area a host gives beyond the editor
            wc.lpszClassName = L"HollowDxiPage";
            RegisterClassW(&wc);
            return wc.lpszClassName;
        }();
        return name;
    }

    Dxi* m_dxi = nullptr;
    IPropertyPageSite* m_site = nullptr;
    HWND m_wnd = nullptr;
    std::vector<HWND> m_clipped;   // host windows we gave WS_CLIPCHILDREN, handed back on Deactivate
    bool m_onScreen = false;       // the page was actually visible at the last idle tick
};

const AMOVIESETUP_MEDIATYPE kPinTypes = {&MEDIATYPE_Audio, &MEDIASUBTYPE_NULL};
const AMOVIESETUP_PIN kPins[] = {
    {(LPWSTR)L"Input", FALSE, FALSE, FALSE, FALSE, &CLSID_NULL, nullptr, 1, &kPinTypes},
    {(LPWSTR)L"Output", FALSE, TRUE, FALSE, FALSE, &CLSID_NULL, nullptr, 1, &kPinTypes},
};
const AMOVIESETUP_FILTER kFilterSetup = {&kClsid, kName, MERIT_DO_NOT_USE, 2, kPins};

// A DXi host lists soft synths from HKCR\MfxSoftSynths\{clsid}, on top of the usual COM and
// DirectShow filter registration.
std::wstring synthKey() {
    wchar_t id[64] = {};
    StringFromGUID2(kClsid, id, 64);
    return std::wstring(L"MfxSoftSynths\\") + id;
}

} // namespace

// The DirectShow BaseClasses' class factory (DllGetClassObject) serves these two objects.
CFactoryTemplate g_Templates[] = {
    {kName, &kClsid, Dxi::CreateInstance, nullptr, &kFilterSetup},
    {L"Hollow DXi editor", &kPageClsid, Page::CreateInstance, nullptr, nullptr},
};
int g_cTemplates = sizeof g_Templates / sizeof g_Templates[0];

STDAPI DllRegisterServer() {
    HRESULT hr = AMovieDllRegisterServer2(TRUE);
    if (FAILED(hr)) return hr;
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, synthKey().c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return SELFREG_E_CLASS;
    RegSetValueExW(k, L"Description", 0, REG_SZ, (const BYTE*)kName, (DWORD)((wcslen(kName) + 1) * sizeof(wchar_t)));
    RegSetValueExW(k, L"HelpFilePath", 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t));
    RegSetValueExW(k, L"HelpFileTopic", 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t));
    RegCloseKey(k);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    RegDeleteKeyW(HKEY_CLASSES_ROOT, synthKey().c_str());
    return AMovieDllRegisterServer2(FALSE);
}

// The BaseClasses need the module handle, and the DXi its display name, before COM asks for anything.
extern "C" BOOL WINAPI _DllEntryPoint(HINSTANCE, ULONG, LPVOID);
BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH)
        MultiByteToWideChar(CP_UTF8, 0, hollow::pluginInfo().name, -1, kName, (int)(sizeof kName / sizeof kName[0]) - 1);
    return _DllEntryPoint(h, reason, reserved);
}

// COM finds these by their plain names; on x86 STDAPI decorates them.
#pragma comment(linker, "/EXPORT:DllGetClassObject=_DllGetClassObject@12,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow=_DllCanUnloadNow@0,PRIVATE")
#pragma comment(linker, "/EXPORT:DllRegisterServer=_DllRegisterServer@0,PRIVATE")
#pragma comment(linker, "/EXPORT:DllUnregisterServer=_DllUnregisterServer@0,PRIVATE")
