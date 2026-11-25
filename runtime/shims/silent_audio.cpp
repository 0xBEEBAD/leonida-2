// The silent output endpoint (silent_audio.h): IMMDeviceEnumerator, IMMDevice, IPropertyStore, IAudioClient3 and its
// services, all in-process. Nothing here opens a host audio device; the samples a title writes go to a scratch buffer.
//
// No function-local statics or thread_local here: like com_shim.cpp, this runs on threads the title made.
#include "silent_audio.h"

#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#include <propsys.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>
#include <vector>

namespace rt::silent_audio {
namespace {

constexpr HRESULT kNotFound = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);  // 0x80070490

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT and KSDATAFORMAT_SUBTYPE_PCM.
constexpr GUID kSubtypeFloat{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypePcm{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Property keys (functiondiscoverykeys_devpkey.h, mmdeviceapi.h), spelled out so no INITGUID unit is needed.
constexpr GUID kDeviceFmtid{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}};
constexpr GUID kInterfaceFmtid{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}};
constexpr GUID kEngineFmtid{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}};
constexpr GUID kOemFmtid{0xe4870e26, 0x3cc5, 0x4cd2, {0xba, 0x46, 0xca, 0x0a, 0x9a, 0x70, 0xed, 0x04}};
constexpr GUID kEndpointFmtid{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}};
constexpr PROPERTYKEY kKeyFriendlyName{kDeviceFmtid, 14};        // PKEY_Device_FriendlyName
constexpr PROPERTYKEY kKeyDeviceDesc{kDeviceFmtid, 2};           // PKEY_Device_DeviceDesc
constexpr PROPERTYKEY kKeyInterfaceName{kInterfaceFmtid, 2};     // PKEY_DeviceInterface_FriendlyName
constexpr PROPERTYKEY kKeyDeviceFormat{kEngineFmtid, 0};         // PKEY_AudioEngine_DeviceFormat
constexpr PROPERTYKEY kKeyOemFormat{kOemFmtid, 3};               // PKEY_AudioEngine_OEMFormat
constexpr PROPERTYKEY kKeyFormFactor{kEndpointFmtid, 0};         // PKEY_AudioEndpoint_FormFactor
constexpr PROPERTYKEY kKeyPhysicalSpeakers{kEndpointFmtid, 3};   // PKEY_AudioEndpoint_PhysicalSpeakers
constexpr PROPERTYKEY kKeyEndpointGuid{kEndpointFmtid, 4};       // PKEY_AudioEndpoint_GUID
constexpr PROPERTYKEY kKeys[] = {kKeyFriendlyName, kKeyDeviceDesc, kKeyInterfaceName, kKeyDeviceFormat,
                                 kKeyOemFormat, kKeyFormFactor, kKeyPhysicalSpeakers, kKeyEndpointGuid};
constexpr uint32_t kFormFactorSpeakers = 1;
constexpr wchar_t kEndpointGuid[] = L"{5a4c2a5e-1e0d-4c1b-9a51-6c656f6e6964}";

constexpr uint32_t kMaxChannels = 32;

LONG g_served = 0;
LONG g_logged = 0;
SRWLOCK g_iid_lock = SRWLOCK_INIT;
GUID g_iids_logged[16]{};
uint32_t g_iid_count = 0;

bool same_key(const PROPERTYKEY& a, const PROPERTYKEY& b) {
    return a.pid == b.pid && IsEqualGUID(a.fmtid, b.fmtid);
}

// One [rt] line per interface the silent endpoint does not have.
void log_missing_iid_once(const char* where, REFIID riid) {
    AcquireSRWLockExclusive(&g_iid_lock);
    bool seen = false;
    for (uint32_t i = 0; i < g_iid_count; ++i) seen = seen || IsEqualGUID(g_iids_logged[i], riid);
    if (!seen && g_iid_count < 16) g_iids_logged[g_iid_count++] = riid;
    ReleaseSRWLockExclusive(&g_iid_lock);
    if (seen) return;
    wchar_t text[64]{};
    StringFromGUID2(riid, text, 64);
    std::fprintf(stderr, "[rt] audio: the silent output endpoint has no %ls (%s); E_NOINTERFACE\n", text, where);
}

void note_served(const char* reason) {
    InterlockedIncrement(&g_served);
    if (InterlockedExchange(&g_logged, 1) == 0) {
        std::fprintf(stderr,
                     "[rt] audio: serving the silent output endpoint (%s): 48 kHz float 7.1, every sample discarded, "
                     "no host audio device opened\n",
                     reason);
    }
}

void mix_format(WAVEFORMATEXTENSIBLE* f) {
    std::memset(f, 0, sizeof *f);
    f->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f->Format.nChannels = kChannels;
    f->Format.nSamplesPerSec = kSampleRate;
    f->Format.wBitsPerSample = 32;
    f->Format.nBlockAlign = static_cast<WORD>(kChannels * 4);
    f->Format.nAvgBytesPerSec = kSampleRate * kChannels * 4;
    f->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f->Samples.wValidBitsPerSample = 32;
    f->dwChannelMask = kChannelMask;
    f->SubFormat = kSubtypeFloat;
}

WAVEFORMATEX* alloc_mix_format() {
    auto* f = static_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (f) mix_format(f);
    return reinterpret_cast<WAVEFORMATEX*>(f);
}

bool is_float(const WAVEFORMATEX* f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    return f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22 &&
           IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat, kSubtypeFloat);
}

// A format a shared-mode stream could carry: PCM or float, 1..32 channels, 8..384 kHz, a consistent block.
bool valid_format(const WAVEFORMATEX* f) {
    if (f->nChannels == 0 || f->nChannels > kMaxChannels) return false;
    if (f->nSamplesPerSec < 8000 || f->nSamplesPerSec > 384000) return false;
    if (f->wBitsPerSample == 0 || f->wBitsPerSample % 8 != 0 || f->wBitsPerSample > 64) return false;
    if (f->nBlockAlign != f->nChannels * (f->wBitsPerSample / 8)) return false;
    if (f->wFormatTag == WAVE_FORMAT_PCM || f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag != WAVE_FORMAT_EXTENSIBLE || f->cbSize < 22) return false;
    const GUID& sub = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat;
    return IsEqualGUID(sub, kSubtypeFloat) || IsEqualGUID(sub, kSubtypePcm);
}

bool is_mix_format(const WAVEFORMATEX* f) {
    return f->nSamplesPerSec == kSampleRate && f->nChannels == kChannels && f->wBitsPerSample == 32 && is_float(f);
}

int64_t qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t qpc_frequency() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

wchar_t* co_task_string(const wchar_t* s) {
    const size_t n = std::wcslen(s) + 1;
    auto* out = static_cast<wchar_t*>(CoTaskMemAlloc(n * sizeof(wchar_t)));
    if (out) std::wmemcpy(out, s, n);
    return out;
}

// IUnknown for an object with one interface.
template <class Iface>
class Unknown : public Iface {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!IsEqualIID(riid, __uuidof(IUnknown)) && !answers(riid)) return E_NOINTERFACE;
        *ppv = static_cast<Iface*>(this);
        AddRef();
        return S_OK;
    }

protected:
    virtual ~Unknown() = default;
    virtual bool answers(REFIID riid) const { return IsEqualIID(riid, __uuidof(Iface)); }

private:
    LONG refs_ = 1;
};

// ---- IAudioClient3 --------------------------------------------------------------------------------------------

class AudioClient final : public Unknown<IAudioClient3> {
public:
    // IAudioClient
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME buffer_hns,
                                         REFERENCE_TIME, const WAVEFORMATEX* format, LPCGUID) override {
        return init(mode, flags, buffer_hns, 0, format);
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override {
        if (!frames) return E_POINTER;
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        *frames = buffer_frames_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* latency) override {
        if (!latency) return E_POINTER;
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        *latency = kDefaultPeriodHns;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* padding) override {
        if (!padding) return E_POINTER;
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        advance(qpc_now());
        *padding = static_cast<UINT32>(written_ - played_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE mode, const WAVEFORMATEX* format,
                                                WAVEFORMATEX** closest) override {
        if (closest) *closest = nullptr;
        if (!format) return E_POINTER;
        if (mode == AUDCLNT_SHAREMODE_EXCLUSIVE) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (mode != AUDCLNT_SHAREMODE_SHARED) return E_INVALIDARG;
        if (!closest) return E_POINTER;
        if (!valid_format(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (is_mix_format(format)) return S_OK;
        *closest = alloc_mix_format();
        return *closest ? S_FALSE : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override {
        if (!format) return E_POINTER;
        *format = alloc_mix_format();
        return *format ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* default_period, REFERENCE_TIME* minimum) override {
        if (!default_period && !minimum) return E_POINTER;
        if (default_period) *default_period = kDefaultPeriodHns;
        if (minimum) *minimum = kMinimumPeriodHns;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Start() override {
        HANDLE signal = nullptr;
        {
            Lock l(lock_);
            if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
            if (running_) return AUDCLNT_E_NOT_STOPPED;
            if (event_mode_ && !event_) return AUDCLNT_E_EVENTHANDLE_NOT_SET;
            anchor_qpc_ = qpc_now();
            played_at_anchor_ = played_;
            running_ = true;
            signal = event_;
        }
        if (signal) SetEvent(signal);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override {
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        if (!running_) return S_FALSE;
        advance(qpc_now());
        running_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        if (running_) return AUDCLNT_E_NOT_STOPPED;
        if (pending_) return AUDCLNT_E_BUFFER_OPERATION_PENDING;
        if (written_ == 0 && played_ == 0) return S_FALSE;
        written_ = played_ = played_at_anchor_ = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE event) override {
        if (!event || event == INVALID_HANDLE_VALUE) return E_INVALIDARG;
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        if (!event_mode_) return AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
        event_ = event;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetService(REFIID riid, void** ppv) override;

    // IAudioClient2
    HRESULT STDMETHODCALLTYPE IsOffloadCapable(AUDIO_STREAM_CATEGORY, BOOL* capable) override {
        if (!capable) return E_POINTER;
        *capable = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetClientProperties(const AudioClientProperties* properties) override {
        if (!properties) return E_POINTER;
        Lock l(lock_);
        return initialized_ ? AUDCLNT_E_ALREADY_INITIALIZED : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBufferSizeLimits(const WAVEFORMATEX* format, BOOL, REFERENCE_TIME* min_hns,
                                                  REFERENCE_TIME* max_hns) override {
        if (!format || !min_hns || !max_hns) return E_POINTER;
        if (!valid_format(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        *min_hns = kDefaultPeriodHns;
        *max_hns = 20000000;  // 2 s
        return S_OK;
    }

    // IAudioClient3: one engine period only, 10 ms.
    HRESULT STDMETHODCALLTYPE GetSharedModeEnginePeriod(const WAVEFORMATEX* format, UINT32* default_frames,
                                                        UINT32* fundamental, UINT32* min_frames,
                                                        UINT32* max_frames) override {
        if (!format || !default_frames || !fundamental || !min_frames || !max_frames) return E_POINTER;
        if (!valid_format(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        const UINT32 period = static_cast<UINT32>(format->nSamplesPerSec / 100);
        *default_frames = *fundamental = *min_frames = *max_frames = period;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentSharedModeEnginePeriod(WAVEFORMATEX** format, UINT32* frames) override {
        if (!format || !frames) return E_POINTER;
        *format = alloc_mix_format();
        if (!*format) return E_OUTOFMEMORY;
        *frames = kSampleRate / 100;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InitializeSharedAudioStream(DWORD flags, UINT32 period_frames,
                                                          const WAVEFORMATEX* format, LPCGUID) override {
        if (!format) return E_POINTER;
        const REFERENCE_TIME hns = format->nSamplesPerSec
                                       ? static_cast<REFERENCE_TIME>(period_frames) * 10000000 / format->nSamplesPerSec
                                       : 0;
        return init(AUDCLNT_SHAREMODE_SHARED, flags, 0, hns, format);
    }

    // For the services.
    HRESULT get_buffer(UINT32 frames, BYTE** data) {
        if (!data) return E_POINTER;
        *data = nullptr;
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        if (pending_) return AUDCLNT_E_OUT_OF_ORDER;
        advance(qpc_now());
        const uint64_t padding = written_ - played_;
        if (frames > buffer_frames_ - padding) return AUDCLNT_E_BUFFER_TOO_LARGE;
        pending_ = true;
        pending_frames_ = frames;
        *data = frames ? scratch_.data() : nullptr;
        return S_OK;
    }
    HRESULT release_buffer(UINT32 frames) {
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        if (!pending_) return frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
        if (frames > pending_frames_) return AUDCLNT_E_INVALID_SIZE;
        // A write after the stream ran dry plays from now on (advance re-anchors), as a device fed silence in the gap.
        advance(qpc_now());
        written_ += frames;
        pending_ = false;
        pending_frames_ = 0;
        return S_OK;
    }
    void position(UINT64* frames, UINT64* qpc_hns) {
        Lock l(lock_);
        const int64_t now = qpc_now();
        advance(now);
        *frames = played_;
        if (qpc_hns) {
            const int64_t f = qpc_frequency();
            *qpc_hns = static_cast<UINT64>((now / f) * 10000000 + (now % f) * 10000000 / f);
        }
    }
    uint32_t rate() const { return rate_; }
    uint32_t channels() const { return channels_; }
    float volume(uint32_t i) {
        Lock l(lock_);
        return i < kMaxChannels ? channel_volume_[i] : 0.0f;
    }
    void set_volume(uint32_t i, float v) {
        Lock l(lock_);
        if (i < kMaxChannels) channel_volume_[i] = v;
    }
    float master_ = 1.0f;
    BOOL mute_ = FALSE;

private:
    struct Lock {
        explicit Lock(SRWLOCK& s) : s_(s) { AcquireSRWLockExclusive(&s_); }
        ~Lock() { ReleaseSRWLockExclusive(&s_); }
        SRWLOCK& s_;
    };

    ~AudioClient() override {
        if (thread_) {
            SetEvent(stop_);
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
        }
        if (stop_) CloseHandle(stop_);
    }

    bool answers(REFIID riid) const override {
        return IsEqualIID(riid, __uuidof(IAudioClient)) || IsEqualIID(riid, __uuidof(IAudioClient2)) ||
               IsEqualIID(riid, __uuidof(IAudioClient3));
    }

    HRESULT init(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME buffer_hns, REFERENCE_TIME period_hns,
                 const WAVEFORMATEX* format) {
        if (!format) return E_POINTER;
        if (mode == AUDCLNT_SHAREMODE_EXCLUSIVE) return AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
        if (mode != AUDCLNT_SHAREMODE_SHARED) return E_INVALIDARG;
        if (flags & AUDCLNT_STREAMFLAGS_LOOPBACK) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        // Any PCM or float format is taken, not only the mix format: the samples are thrown away, so there is
        // nothing to convert, and a title never fails here for asking without AUTOCONVERTPCM.
        if (!valid_format(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        Lock l(lock_);
        if (initialized_) return AUDCLNT_E_ALREADY_INITIALIZED;
        rate_ = format->nSamplesPerSec;
        channels_ = format->nChannels;
        block_ = format->nBlockAlign;
        period_hns_ = period_hns > 0 ? period_hns : kDefaultPeriodHns;
        const REFERENCE_TIME hns = buffer_hns > 2 * period_hns_ ? buffer_hns : 2 * period_hns_;
        buffer_frames_ = static_cast<uint32_t>((hns * rate_ + 9999999) / 10000000);
        scratch_.assign(static_cast<size_t>(buffer_frames_) * block_, 0);
        for (float& v : channel_volume_) v = 1.0f;
        event_mode_ = (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;
        if (event_mode_) {
            stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            thread_ = stop_ ? CreateThread(nullptr, 64 * 1024, &AudioClient::clock_thread, this, 0, nullptr) : nullptr;
            if (!thread_) return E_OUTOFMEMORY;
        }
        initialized_ = true;
        if (InterlockedExchange(&g_stream_logged, 1) == 0) {
            std::fprintf(stderr, "[rt] audio: silent stream: %u ch, %u Hz, %u-byte frames, %u-frame buffer, %s\n",
                         channels_, rate_, block_, buffer_frames_, event_mode_ ? "event-driven" : "polled");
        }
        return S_OK;
    }

    // Frames the device has consumed since Start, at the stream's rate, clamped to what was written.
    void advance(int64_t now) {
        if (!running_) return;
        const int64_t f = qpc_frequency();
        const int64_t elapsed = now > anchor_qpc_ ? now - anchor_qpc_ : 0;
        const uint64_t frames = static_cast<uint64_t>((elapsed / f) * rate_ + (elapsed % f) * rate_ / f);
        const uint64_t candidate = played_at_anchor_ + frames;
        if (candidate >= written_) {
            // Ran dry: the device plays silence until more is written.
            played_ = written_;
            anchor_qpc_ = now;
            played_at_anchor_ = written_;
        } else {
            played_ = candidate;
        }
    }

    void tick() {
        HANDLE signal = nullptr;
        {
            Lock l(lock_);
            if (!running_) return;
            advance(qpc_now());
            signal = event_;
        }
        if (signal) SetEvent(signal);
    }

    static DWORD WINAPI clock_thread(void* self) {
        auto* c = static_cast<AudioClient*>(self);
        const DWORD period_ms = static_cast<DWORD>((c->period_hns_ + 9999) / 10000);
        // A fixed schedule, so a late wake-up does not push the next ones back.
        const int64_t f = qpc_frequency();
        const int64_t step = c->period_hns_ * f / 10000000;
        int64_t next = qpc_now() + step;
        for (;;) {
            const int64_t now = qpc_now();
            DWORD wait = 0;
            if (next > now) {
                wait = static_cast<DWORD>((next - now) * 1000 / f);
                if (wait > period_ms) wait = period_ms;
            }
            if (WaitForSingleObject(c->stop_, wait) != WAIT_TIMEOUT) break;
            if (qpc_now() < next) continue;
            next += step;
            if (next < qpc_now()) next = qpc_now() + step;
            c->tick();
        }
        return 0;
    }

    static inline LONG g_stream_logged = 0;

    SRWLOCK lock_ = SRWLOCK_INIT;
    bool initialized_ = false;
    bool event_mode_ = false;
    bool running_ = false;
    bool pending_ = false;
    uint32_t pending_frames_ = 0;
    uint32_t rate_ = kSampleRate;
    uint32_t channels_ = kChannels;
    uint32_t block_ = kChannels * 4;
    uint32_t buffer_frames_ = 0;
    REFERENCE_TIME period_hns_ = kDefaultPeriodHns;
    std::vector<BYTE> scratch_;
    HANDLE event_ = nullptr;  // the title's; not owned
    HANDLE stop_ = nullptr;
    HANDLE thread_ = nullptr;
    uint64_t written_ = 0;
    uint64_t played_ = 0;
    uint64_t played_at_anchor_ = 0;
    int64_t anchor_qpc_ = 0;
    float channel_volume_[kMaxChannels]{};
};

// The services hold their client.
template <class Iface>
class Service : public Unknown<Iface> {
public:
    explicit Service(AudioClient* client) : client_(client) { client_->AddRef(); }

protected:
    ~Service() override { client_->Release(); }
    AudioClient* client_;
};

class RenderClient final : public Service<IAudioRenderClient> {
public:
    using Service::Service;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames, BYTE** data) override { return client_->get_buffer(frames, data); }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames, DWORD) override { return client_->release_buffer(frames); }
};

class Clock final : public Service<IAudioClock> {
public:
    using Service::Service;
    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* frequency) override {
        if (!frequency) return E_POINTER;
        *frequency = client_->rate();  // the position counts frames
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPosition(UINT64* position, UINT64* qpc_hns) override {
        if (!position) return E_POINTER;
        client_->position(position, qpc_hns);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* characteristics) override {
        if (!characteristics) return E_POINTER;
        *characteristics = AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ;
        return S_OK;
    }
};

class SimpleVolume final : public Service<ISimpleAudioVolume> {
public:
    using Service::Service;
    HRESULT STDMETHODCALLTYPE SetMasterVolume(float level, LPCGUID) override {
        if (level < 0.0f || level > 1.0f) return E_INVALIDARG;
        client_->master_ = level;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMasterVolume(float* level) override {
        if (!level) return E_POINTER;
        *level = client_->master_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMute(BOOL mute, LPCGUID) override {
        client_->mute_ = mute;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMute(BOOL* mute) override {
        if (!mute) return E_POINTER;
        *mute = client_->mute_;
        return S_OK;
    }
};

class StreamVolume final : public Service<IAudioStreamVolume> {
public:
    using Service::Service;
    HRESULT STDMETHODCALLTYPE GetChannelCount(UINT32* count) override {
        if (!count) return E_POINTER;
        *count = client_->channels();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetChannelVolume(UINT32 index, const float level) override {
        if (index >= client_->channels() || level < 0.0f || level > 1.0f) return E_INVALIDARG;
        client_->set_volume(index, level);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetChannelVolume(UINT32 index, float* level) override {
        if (!level) return E_POINTER;
        if (index >= client_->channels()) return E_INVALIDARG;
        *level = client_->volume(index);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetAllVolumes(UINT32 count, const float* levels) override {
        if (!levels) return E_POINTER;
        if (count != client_->channels()) return E_INVALIDARG;
        for (UINT32 i = 0; i < count; ++i) {
            if (levels[i] < 0.0f || levels[i] > 1.0f) return E_INVALIDARG;
        }
        for (UINT32 i = 0; i < count; ++i) client_->set_volume(i, levels[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAllVolumes(UINT32 count, float* levels) override {
        if (!levels) return E_POINTER;
        if (count != client_->channels()) return E_INVALIDARG;
        for (UINT32 i = 0; i < count; ++i) levels[i] = client_->volume(i);
        return S_OK;
    }
};

HRESULT AudioClient::GetService(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    {
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
    }
    IUnknown* made = nullptr;
    if (IsEqualIID(riid, __uuidof(IAudioRenderClient))) made = new (std::nothrow) RenderClient(this);
    else if (IsEqualIID(riid, __uuidof(IAudioClock))) made = new (std::nothrow) Clock(this);
    else if (IsEqualIID(riid, __uuidof(ISimpleAudioVolume))) made = new (std::nothrow) SimpleVolume(this);
    else if (IsEqualIID(riid, __uuidof(IAudioStreamVolume))) made = new (std::nothrow) StreamVolume(this);
    else {
        log_missing_iid_once("IAudioClient::GetService", riid);
        return E_NOINTERFACE;
    }
    if (!made) return E_OUTOFMEMORY;
    const HRESULT hr = made->QueryInterface(riid, ppv);
    made->Release();
    return hr;
}

// ---- IPropertyStore -------------------------------------------------------------------------------------------

class PropertyStore final : public Unknown<IPropertyStore> {
public:
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) override {
        if (!count) return E_POINTER;
        *count = static_cast<DWORD>(sizeof kKeys / sizeof kKeys[0]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD index, PROPERTYKEY* key) override {
        if (!key) return E_POINTER;
        if (index >= sizeof kKeys / sizeof kKeys[0]) return E_INVALIDARG;
        *key = kKeys[index];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override {
        if (!value) return E_POINTER;
        PropVariantInit(value);
        if (same_key(key, kKeyFriendlyName) || same_key(key, kKeyDeviceDesc) || same_key(key, kKeyInterfaceName) ||
            same_key(key, kKeyEndpointGuid)) {
            value->pwszVal = co_task_string(same_key(key, kKeyEndpointGuid) ? kEndpointGuid : kFriendlyName);
            if (!value->pwszVal) return E_OUTOFMEMORY;
            value->vt = VT_LPWSTR;
        } else if (same_key(key, kKeyDeviceFormat) || same_key(key, kKeyOemFormat)) {
            value->blob.pBlobData = reinterpret_cast<BYTE*>(alloc_mix_format());
            if (!value->blob.pBlobData) return E_OUTOFMEMORY;
            value->blob.cbSize = sizeof(WAVEFORMATEXTENSIBLE);
            value->vt = VT_BLOB;
        } else if (same_key(key, kKeyFormFactor) || same_key(key, kKeyPhysicalSpeakers)) {
            value->ulVal = same_key(key, kKeyFormFactor) ? kFormFactorSpeakers : kChannelMask;
            value->vt = VT_UI4;
        }
        return S_OK;  // any other key: VT_EMPTY, as a host store answers a key it does not hold
    }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Commit() override { return STG_E_ACCESSDENIED; }
};

// ---- IMMDevice ------------------------------------------------------------------------------------------------

class Device final : public IMMDevice, public IMMEndpoint {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IMMDevice))) {
            *ppv = static_cast<IMMDevice*>(this);
        } else if (IsEqualIID(riid, __uuidof(IMMEndpoint))) {
            *ppv = static_cast<IMMEndpoint*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    // IMMDevice
    HRESULT STDMETHODCALLTYPE Activate(REFIID riid, DWORD, PROPVARIANT*, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!IsEqualIID(riid, __uuidof(IAudioClient)) && !IsEqualIID(riid, __uuidof(IAudioClient2)) &&
            !IsEqualIID(riid, __uuidof(IAudioClient3))) {
            log_missing_iid_once("IMMDevice::Activate", riid);
            return E_NOINTERFACE;
        }
        auto* client = new (std::nothrow) AudioClient();
        if (!client) return E_OUTOFMEMORY;
        const HRESULT hr = client->QueryInterface(riid, ppv);
        client->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD access, IPropertyStore** store) override {
        if (!store) return E_POINTER;
        *store = nullptr;
        if (access != STGM_READ && access != STGM_WRITE && access != STGM_READWRITE) return E_INVALIDARG;
        if (access != STGM_READ) return E_ACCESSDENIED;
        *store = new (std::nothrow) PropertyStore();
        return *store ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR* id) override {
        if (!id) return E_POINTER;
        *id = co_task_string(kDeviceId);
        return *id ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetState(DWORD* state) override {
        if (!state) return E_POINTER;
        *state = DEVICE_STATE_ACTIVE;
        return S_OK;
    }

    // IMMEndpoint
    HRESULT STDMETHODCALLTYPE GetDataFlow(EDataFlow* flow) override {
        if (!flow) return E_POINTER;
        *flow = eRender;
        return S_OK;
    }

private:
    ~Device() = default;
    LONG refs_ = 1;
};

IMMDevice* make_device(const char* reason) {
    auto* d = new (std::nothrow) Device();
    if (d) note_served(reason);
    return d;
}

// ---- IMMDeviceCollection --------------------------------------------------------------------------------------

class Collection final : public Unknown<IMMDeviceCollection> {
public:
    void add(IMMDevice* device) { devices_.push_back(device); }  // takes the reference
    HRESULT STDMETHODCALLTYPE GetCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = static_cast<UINT>(devices_.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Item(UINT index, IMMDevice** device) override {
        if (!device) return E_POINTER;
        *device = nullptr;
        if (index >= devices_.size()) return E_INVALIDARG;
        *device = devices_[index];
        (*device)->AddRef();
        return S_OK;
    }

private:
    ~Collection() override {
        for (IMMDevice* d : devices_) d->Release();
    }
    std::vector<IMMDevice*> devices_;
};

// Appends the host's devices of one flow; returns the result of the host's call.
HRESULT append_host(IMMDeviceEnumerator* host, EDataFlow flow, DWORD mask, Collection* out) {
    IMMDeviceCollection* c = nullptr;
    const HRESULT hr = host->EnumAudioEndpoints(flow, mask, &c);
    if (FAILED(hr) || !c) return hr;
    UINT n = 0;
    c->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* d = nullptr;
        if (SUCCEEDED(c->Item(i, &d)) && d) out->add(d);
    }
    c->Release();
    return hr;
}

// ---- IMMDeviceEnumerator --------------------------------------------------------------------------------------

class Enumerator final : public Unknown<IMMDeviceEnumerator> {
public:
    Enumerator(IMMDeviceEnumerator* host, HRESULT host_hr, Mode mode, HostFactory factory)
        : host_(host), host_hr_(host_hr), mode_(mode), factory_(factory) {
        if (host_) host_->AddRef();
    }

    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow flow, DWORD mask, IMMDeviceCollection** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (flow != eRender && flow != eCapture && flow != eAll) return E_INVALIDARG;
        if (mask & ~DEVICE_STATEMASK_ALL) return E_INVALIDARG;
        auto* c = new (std::nothrow) Collection();
        if (!c) return E_OUTOFMEMORY;
        if (flow == eRender || flow == eAll) {
            char reason[96];
            if (!host_render(reason, sizeof reason)) {
                if (mask & DEVICE_STATE_ACTIVE) {
                    if (IMMDevice* d = make_device(reason)) c->add(d);
                }
            } else {
                append_host(host_, eRender, mask, c);
            }
        }
        if (flow == eCapture || flow == eAll) {
            if (IMMDeviceEnumerator* h = host()) append_host(h, eCapture, mask, c);
        }
        *out = c;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (role != eConsole && role != eMultimedia && role != eCommunications) return E_INVALIDARG;
        if (flow == eCapture) {
            IMMDeviceEnumerator* h = host();
            return h ? h->GetDefaultAudioEndpoint(flow, role, out) : kNotFound;
        }
        if (flow != eRender) return E_INVALIDARG;
        char reason[96];
        if (mode_ == Mode::Auto && host_) {
            const HRESULT hr = host_->GetDefaultAudioEndpoint(eRender, role, out);
            if (SUCCEEDED(hr) && *out) return hr;
            if (*out) (*out)->Release();
            *out = nullptr;
            std::snprintf(reason, sizeof reason, "no host render endpoint: GetDefaultAudioEndpoint 0x%08lx",
                          static_cast<unsigned long>(hr));
        } else {
            why_silent(reason, sizeof reason);
        }
        *out = make_device(reason);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, IMMDevice** out) override {
        if (!id || !out) return E_POINTER;
        *out = nullptr;
        if (std::wcscmp(id, kDeviceId) == 0) {
            char reason[96];
            why_silent(reason, sizeof reason);
            *out = make_device(reason);
            return *out ? S_OK : E_OUTOFMEMORY;
        }
        IMMDeviceEnumerator* h = host();
        return h ? h->GetDevice(id, out) : kNotFound;
    }
    // Accepted and never called: the silent endpoint never changes, and no host notification is forwarded.
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* client) override {
        return client ? S_OK : E_POINTER;
    }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* client) override {
        return client ? S_OK : E_POINTER;
    }

private:
    ~Enumerator() override {
        if (host_) host_->Release();
    }

    // The host's enumerator for capture questions: made on first use in Mode::Silent.
    IMMDeviceEnumerator* host() {
        AcquireSRWLockExclusive(&lock_);
        if (!host_ && factory_ && !factory_tried_) {
            factory_tried_ = true;
            host_ = factory_();
        }
        IMMDeviceEnumerator* h = host_;
        ReleaseSRWLockExclusive(&lock_);
        return h;
    }

    void why_silent(char* reason, size_t size) const {
        if (mode_ == Mode::Silent) std::snprintf(reason, size, "LEONIDA_AUDIO=silent");
        else if (!host_) std::snprintf(reason, size, "no host device enumerator: 0x%08lx",
                                       static_cast<unsigned long>(host_hr_));
        else std::snprintf(reason, size, "no host render endpoint");
    }

    // True when render questions go to the host: Mode::Auto and a host with a default render endpoint.
    bool host_render(char* reason, size_t size) {
        if (mode_ == Mode::Auto && host_) {
            IMMDevice* d = nullptr;
            const HRESULT hr = host_->GetDefaultAudioEndpoint(eRender, eConsole, &d);
            if (d) d->Release();
            if (SUCCEEDED(hr) && d) return true;
            std::snprintf(reason, size, "no host render endpoint: GetDefaultAudioEndpoint 0x%08lx",
                          static_cast<unsigned long>(hr));
            return false;
        }
        why_silent(reason, size);
        return false;
    }

    SRWLOCK lock_ = SRWLOCK_INIT;
    IMMDeviceEnumerator* host_;
    HRESULT host_hr_;
    Mode mode_;
    HostFactory factory_;
    bool factory_tried_ = false;
};

IMMDeviceEnumerator* make_host_enumerator() {
    IMMDeviceEnumerator* e = nullptr;
    if (FAILED(::CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&e)))) {
        return nullptr;
    }
    return e;
}

}  // namespace

Mode mode_from_env() {
    char v[16]{};
    const DWORD n = GetEnvironmentVariableA("LEONIDA_AUDIO", v, sizeof v);
    if (n == 0 || n >= sizeof v) return Mode::Auto;
    return _stricmp(v, "silent") == 0 ? Mode::Silent : Mode::Auto;
}

HRESULT serve_enumerator(IMMDeviceEnumerator* host, HRESULT host_hr, Mode mode, HostFactory factory, REFIID riid,
                         void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (mode == Mode::Auto && host) {
        IMMDevice* d = nullptr;
        const HRESULT hr = host->GetDefaultAudioEndpoint(eRender, eConsole, &d);
        if (d) d->Release();
        if (SUCCEEDED(hr) && d) return host->QueryInterface(riid, ppv);  // the host has an output: unchanged
    }
    auto* e = new (std::nothrow) Enumerator(host, host ? S_OK : host_hr, mode, factory);
    if (!e) return E_OUTOFMEMORY;
    const HRESULT hr = e->QueryInterface(riid, ppv);
    e->Release();
    return hr;
}

HRESULT create_enumerator(DWORD cls_context, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    const Mode mode = mode_from_env();
    if (mode == Mode::Silent) return serve_enumerator(nullptr, S_OK, mode, &make_host_enumerator, riid, ppv);
    IMMDeviceEnumerator* host = nullptr;
    const HRESULT hr = ::CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, cls_context,
                                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&host));
    if (FAILED(hr)) host = nullptr;
    const HRESULT served = serve_enumerator(host, hr, mode, nullptr, riid, ppv);
    if (host) host->Release();
    return served;
}

uint32_t devices_served() { return static_cast<uint32_t>(InterlockedCompareExchange(&g_served, 0, 0)); }

}  // namespace rt::silent_audio
