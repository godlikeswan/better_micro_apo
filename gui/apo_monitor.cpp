#include "apo_monitor.h"

#include "apo.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace apo {
namespace {

std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<double> g_peak{0.0};
std::atomic<double> g_sumSquares{0.0};
std::atomic<unsigned long long> g_samples{0};
std::atomic<bool> g_clipped{false};
std::atomic<unsigned> g_drops{0};

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_)
    {
        other.ptr_ = nullptr;
    }

    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T** put()
    {
        reset();
        return &ptr_;
    }

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void reset()
    {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

struct CoTaskMemDeleter {
    void operator()(void* value) const { CoTaskMemFree(value); }
};

using FormatPtr = std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter>;

std::wstring HresultText(const wchar_t* action, HRESULT hr)
{
    wchar_t code[16]{};
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(hr));
    return std::wstring(action) + L" (" + code + L").";
}

bool IsFloatFormat(const WAVEFORMATEX* format)
{
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extended =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return extended->SubFormat.Data1 == 3; // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    }
    return false;
}

struct BlockLevel {
    double peak = 0.0;
    double sumSquares = 0.0;
    unsigned long long samples = 0;
};

BlockLevel BufferLevel(const BYTE* data, UINT32 frames, UINT32 channels,
                       UINT32 sampleBytes, bool isFloat)
{
    BlockLevel level;
    if (data == nullptr || frames == 0 || channels == 0 ||
        sampleBytes == 0) {
        return level;
    }

    const size_t count = static_cast<size_t>(frames) * channels;
    level.samples = static_cast<unsigned long long>(count);

    if (isFloat && sampleBytes == 4) {
        const float* samples = reinterpret_cast<const float*>(data);
        for (size_t i = 0; i < count; ++i) {
            const double value = static_cast<double>(samples[i]);
            const double magnitude = std::fabs(value);
            level.sumSquares += value * value;
            if (magnitude > level.peak) {
                level.peak = magnitude;
            }
        }
    } else if (sampleBytes == 2) {
        const int16_t* samples = reinterpret_cast<const int16_t*>(data);
        for (size_t i = 0; i < count; ++i) {
            const double value = static_cast<double>(samples[i]) / 32768.0;
            const double magnitude = std::fabs(value);
            level.sumSquares += value * value;
            if (magnitude > level.peak) {
                level.peak = magnitude;
            }
        }
    } else if (sampleBytes == 3) {
        for (size_t i = 0; i < count; ++i) {
            const BYTE* sample = data + i * 3;
            int32_t raw = static_cast<int32_t>(sample[0]) |
                          (static_cast<int32_t>(sample[1]) << 8) |
                          (static_cast<int32_t>(sample[2]) << 16);
            if ((raw & 0x00800000) != 0) {
                raw |= ~0x00FFFFFF;
            }
            const double value = static_cast<double>(raw) / 8388608.0;
            const double magnitude = std::fabs(value);
            level.sumSquares += value * value;
            if (magnitude > level.peak) {
                level.peak = magnitude;
            }
        }
    } else if (sampleBytes == 4) {
        const int32_t* samples = reinterpret_cast<const int32_t*>(data);
        for (size_t i = 0; i < count; ++i) {
            const double value =
                static_cast<double>(samples[i]) / 2147483648.0;
            const double magnitude = std::fabs(value);
            level.sumSquares += value * value;
            if (magnitude > level.peak) {
                level.peak = magnitude;
            }
        }
    } else {
        level.samples = 0;
    }

    return level;
}

void AppendSamplesAsFloat(const BYTE* data, UINT32 frames, UINT32 channels,
                          UINT32 sampleBytes, bool isFloat,
                          std::vector<float>& out)
{
    const size_t count = static_cast<size_t>(frames) * channels;
    out.reserve(out.size() + count);

    if (isFloat && sampleBytes == 4) {
        const float* samples = reinterpret_cast<const float*>(data);
        out.insert(out.end(), samples, samples + count);
    } else if (sampleBytes == 2) {
        const int16_t* samples = reinterpret_cast<const int16_t*>(data);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(static_cast<float>(
                static_cast<double>(samples[i]) / 32768.0));
        }
    } else if (sampleBytes == 3) {
        for (size_t i = 0; i < count; ++i) {
            const BYTE* sample = data + i * 3;
            int32_t raw = static_cast<int32_t>(sample[0]) |
                          (static_cast<int32_t>(sample[1]) << 8) |
                          (static_cast<int32_t>(sample[2]) << 16);
            if ((raw & 0x00800000) != 0) {
                raw |= ~0x00FFFFFF;
            }
            out.push_back(static_cast<float>(
                static_cast<double>(raw) / 8388608.0));
        }
    } else if (sampleBytes == 4) {
        const int32_t* samples = reinterpret_cast<const int32_t*>(data);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(static_cast<float>(
                static_cast<double>(samples[i]) / 2147483648.0));
        }
    } else {
        out.insert(out.end(), count, 0.0f);
    }
}

void AddSumSquares(double value)
{
    double current = g_sumSquares.load(std::memory_order_relaxed);
    while (!g_sumSquares.compare_exchange_weak(
        current, current + value, std::memory_order_relaxed)) {
    }
}

void ReportLevel(const BlockLevel& level)
{
    // PCM formats cannot represent exactly 1.0, so treat near-full-scale as
    // clipping too.
    if (level.peak >= 0.9999) {
        g_clipped.store(true, std::memory_order_relaxed);
    }

    double current = g_peak.load(std::memory_order_relaxed);
    while (level.peak > current &&
           !g_peak.compare_exchange_weak(current, level.peak,
                                          std::memory_order_relaxed)) {
    }

    if (level.samples != 0) {
        AddSumSquares(level.sumSquares);
        g_samples.fetch_add(level.samples, std::memory_order_relaxed);
    }
}

struct ComInit {
    HRESULT hr = E_FAIL;

    ComInit() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComInit()
    {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
};

struct Monitor {
    ComPtr<IAudioClient> captureClient;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioClient> renderClient;
    ComPtr<IAudioRenderClient> render;
    FormatPtr format;
    UINT32 renderFrames = 0;
    UINT32 frameBytes = 0;
    UINT32 channels = 0;
    UINT32 sampleBytes = 0;
    bool isFloat = false;

    ~Monitor()
    {
        if (captureClient) {
            captureClient->Stop();
        }
        if (renderClient) {
            renderClient->Stop();
        }
    }
};

std::wstring OpenMonitor(const std::wstring& endpointId, Monitor& monitor)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(enumerator.put()));
    if (FAILED(hr)) {
        return HresultText(L"Could not access the audio devices", hr);
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE,
                                        collection.put());
    if (FAILED(hr)) {
        return HresultText(L"Could not enumerate the capture devices", hr);
    }

    ComPtr<IMMDevice> captureDevice;
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return L"Could not read the capture device list.";
    }

    for (UINT i = 0; i < count && !captureDevice; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put())) || !device) {
            continue;
        }

        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
            const bool match = std::wcsstr(id, endpointId.c_str()) != nullptr;
            CoTaskMemFree(id);
            if (match) {
                captureDevice = std::move(device);
            }
        }
    }

    if (!captureDevice) {
        return L"The selected capture device is not active.";
    }

    hr = captureDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(monitor.captureClient.put()));
    if (FAILED(hr)) {
        return HresultText(L"Could not open the capture device", hr);
    }

    WAVEFORMATEX* rawFormat = nullptr;
    hr = monitor.captureClient->GetMixFormat(&rawFormat);
    if (FAILED(hr) || rawFormat == nullptr) {
        return HresultText(L"Could not read the capture format", hr);
    }
    monitor.format.reset(rawFormat);

    hr = monitor.captureClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, monitor.format.get(), nullptr);
    if (FAILED(hr)) {
        return HresultText(L"Could not start the capture stream", hr);
    }

    hr = monitor.captureClient->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(monitor.capture.put()));
    if (FAILED(hr)) {
        return HresultText(L"Could not open the capture client", hr);
    }

    ComPtr<IMMDevice> renderDevice;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                             renderDevice.put());
    if (FAILED(hr)) {
        return HresultText(L"Could not find the default output device", hr);
    }

    hr = renderDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(monitor.renderClient.put()));
    if (FAILED(hr)) {
        return HresultText(L"Could not open the default output device", hr);
    }

    // Let the audio engine convert the capture format to the output mix
    // format, so capture and render devices do not need matching rates.
    const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = monitor.renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                          2000000, 0, monitor.format.get(),
                                          nullptr);
    if (FAILED(hr)) {
        return HresultText(L"Could not start the output stream", hr);
    }

    hr = monitor.renderClient->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(monitor.render.put()));
    if (FAILED(hr)) {
        return HresultText(L"Could not open the output client", hr);
    }

    hr = monitor.renderClient->GetBufferSize(&monitor.renderFrames);
    if (FAILED(hr) || monitor.renderFrames == 0) {
        return HresultText(L"Could not size the output buffer", hr);
    }

    monitor.frameBytes = monitor.format->nBlockAlign;
    monitor.channels = monitor.format->nChannels;
    if (monitor.frameBytes == 0 || monitor.channels == 0) {
        return L"The capture format has no frame size.";
    }
    monitor.sampleBytes = monitor.frameBytes / monitor.channels;
    monitor.isFloat = IsFloatFormat(monitor.format.get());

    hr = monitor.renderClient->Start();
    if (FAILED(hr)) {
        return HresultText(L"Could not start the output stream", hr);
    }

    hr = monitor.captureClient->Start();
    if (FAILED(hr)) {
        return HresultText(L"Could not start the capture stream", hr);
    }

    return {};
}

void RunMonitor(Monitor& monitor)
{
    std::vector<BYTE> pending;
    pending.reserve(static_cast<size_t>(monitor.frameBytes) *
                    monitor.format->nSamplesPerSec / 4);

    // Keep at most one second queued; drop the oldest audio if output stalls.
    const size_t maxPending = static_cast<size_t>(monitor.frameBytes) *
                              monitor.format->nSamplesPerSec;

    while (g_running.load()) {
        UINT32 packet = 0;
        while (g_running.load() &&
               SUCCEEDED(monitor.capture->GetNextPacketSize(&packet)) &&
               packet != 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(monitor.capture->GetBuffer(&data, &frames, &flags,
                                                  nullptr, nullptr))) {
                break;
            }

            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                g_drops.fetch_add(1, std::memory_order_relaxed);
            }

            if (frames != 0) {
                const size_t bytes =
                    static_cast<size_t>(frames) * monitor.frameBytes;
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ||
                    data == nullptr) {
                    // Count silent frames as zero-energy samples so the RMS
                    // window decays instead of holding the last level.
                    BlockLevel silence;
                    silence.samples = static_cast<unsigned long long>(frames) *
                                      monitor.channels;
                    ReportLevel(silence);
                    pending.insert(pending.end(), bytes, 0);
                } else {
                    ReportLevel(BufferLevel(data, frames, monitor.channels,
                                            monitor.sampleBytes,
                                            monitor.isFloat));
                    pending.insert(pending.end(), data, data + bytes);
                }
            }
            monitor.capture->ReleaseBuffer(frames);
        }

        if (pending.size() > maxPending) {
            const size_t drop = pending.size() - maxPending;
            g_drops.fetch_add(
                static_cast<unsigned>(drop / monitor.frameBytes),
                std::memory_order_relaxed);
            pending.erase(pending.begin(), pending.begin() + drop);
        }

        UINT32 padding = 0;
        if (!pending.empty() &&
            SUCCEEDED(monitor.renderClient->GetCurrentPadding(&padding))) {
            const UINT32 available =
                (padding < monitor.renderFrames)
                    ? (monitor.renderFrames - padding)
                    : 0;
            if (available != 0) {
                const size_t pendingFrames =
                    pending.size() / monitor.frameBytes;
                const UINT32 frames = static_cast<UINT32>(
                    std::min<size_t>(available, pendingFrames));
                if (frames != 0) {
                    BYTE* output = nullptr;
                    if (SUCCEEDED(
                            monitor.render->GetBuffer(frames, &output)) &&
                        output != nullptr) {
                        const size_t bytes =
                            static_cast<size_t>(frames) * monitor.frameBytes;
                        std::memcpy(output, pending.data(), bytes);
                        monitor.render->ReleaseBuffer(frames, 0);
                        pending.erase(pending.begin(),
                                      pending.begin() + bytes);
                    }
                }
            }
        }

        Sleep(5);
    }
}

struct CaptureResult {
    std::wstring error;
    std::vector<float> samples;
    unsigned sampleRate = 0;
    unsigned channels = 0;
};

void RunCapture(Monitor& monitor, unsigned durationMs, CaptureResult& result)
{
    const unsigned rate =
        (monitor.format != nullptr) ? monitor.format->nSamplesPerSec : 0;
    if (rate == 0 || monitor.channels == 0) {
        result.error = L"The capture format has no sample rate.";
        return;
    }

    result.sampleRate = rate;
    result.channels = monitor.channels;
    const size_t targetFrames =
        static_cast<size_t>(rate) * durationMs / 1000;
    if (targetFrames == 0) {
        result.error = L"The capture duration is too short.";
        return;
    }

    const ULONGLONG deadline =
        GetTickCount64() + durationMs + 5000;
    while (result.samples.size() / result.channels < targetFrames &&
           GetTickCount64() < deadline) {
        UINT32 packet = 0;
        while (SUCCEEDED(monitor.capture->GetNextPacketSize(&packet)) &&
               packet != 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(monitor.capture->GetBuffer(&data, &frames, &flags,
                                                  nullptr, nullptr))) {
                break;
            }

            if (frames != 0) {
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ||
                    data == nullptr) {
                    result.samples.insert(
                        result.samples.end(),
                        static_cast<size_t>(frames) * result.channels, 0.0f);
                } else {
                    AppendSamplesAsFloat(data, frames, result.channels,
                                         monitor.sampleBytes, monitor.isFloat,
                                         result.samples);
                }
            }
            monitor.capture->ReleaseBuffer(frames);

            if (result.samples.size() / result.channels >= targetFrames) {
                break;
            }
        }

        // Keep the output stream fed with silence so the capture device is
        // never played back into the room.
        UINT32 padding = 0;
        if (SUCCEEDED(monitor.renderClient->GetCurrentPadding(&padding))) {
            const UINT32 available =
                (padding < monitor.renderFrames)
                    ? (monitor.renderFrames - padding)
                    : 0;
            if (available != 0) {
                BYTE* output = nullptr;
                if (SUCCEEDED(
                        monitor.render->GetBuffer(available, &output)) &&
                    output != nullptr) {
                    monitor.render->ReleaseBuffer(
                        available, AUDCLNT_BUFFERFLAGS_SILENT);
                }
            }
        }

        Sleep(5);
    }

    if (result.samples.size() / result.channels < targetFrames) {
        result.error = L"Could not capture enough audio from the device.";
        return;
    }

    result.samples.resize(targetFrames * result.channels);
}

void CaptureLoop(std::wstring endpointId, unsigned durationMs,
                 std::promise<CaptureResult> ready)
{
    CaptureResult result;

    ComInit com;
    if (FAILED(com.hr)) {
        result.error = HresultText(L"Could not initialize COM", com.hr);
        ready.set_value(std::move(result));
        return;
    }

    Monitor monitor;
    result.error = OpenMonitor(endpointId, monitor);
    if (!result.error.empty()) {
        ready.set_value(std::move(result));
        return;
    }

    RunCapture(monitor, durationMs, result);
    ready.set_value(std::move(result));
}

void MonitorLoop(std::wstring endpointId, std::promise<std::wstring> ready)
{
    ComInit com;
    if (FAILED(com.hr)) {
        ready.set_value(HresultText(L"Could not initialize COM", com.hr));
        return;
    }

    Monitor monitor;
    std::wstring error = OpenMonitor(endpointId, monitor);
    if (!error.empty()) {
        ready.set_value(std::move(error));
        return;
    }

    ready.set_value(std::wstring());
    RunMonitor(monitor);
    g_running = false;
}

} // namespace

std::wstring StartMonitor(const std::wstring& endpointId)
{
    StopMonitor();

    g_peak.store(0.0, std::memory_order_relaxed);
    g_sumSquares.store(0.0, std::memory_order_relaxed);
    g_samples.store(0, std::memory_order_relaxed);
    g_clipped.store(false, std::memory_order_relaxed);
    g_drops.store(0, std::memory_order_relaxed);

    g_running = true;
    std::promise<std::wstring> promise;
    std::future<std::wstring> future = promise.get_future();
    g_thread = std::thread(MonitorLoop, endpointId, std::move(promise));

    std::wstring error = future.get();
    if (!error.empty()) {
        g_running = false;
        if (g_thread.joinable()) {
            g_thread.join();
        }
    }
    return error;
}

void StopMonitor()
{
    g_running = false;
    if (g_thread.joinable()) {
        g_thread.join();
    }
}

bool IsMonitorActive()
{
    return g_running.load();
}

MonitorLevel TakeMonitorLevel()
{
    MonitorLevel level;
    level.peak = g_peak.exchange(0.0, std::memory_order_relaxed);
    level.sumSquares = g_sumSquares.exchange(0.0, std::memory_order_relaxed);
    level.samples = g_samples.exchange(0, std::memory_order_relaxed);
    level.clipped = g_clipped.exchange(false, std::memory_order_relaxed);
    level.drops = g_drops.exchange(0, std::memory_order_relaxed);
    return level;
}

std::wstring CaptureNoise(const std::wstring& endpointId, unsigned durationMs,
                          std::vector<float>& samples, unsigned& sampleRate,
                          unsigned& channels)
{
    StopMonitor();

    samples.clear();
    sampleRate = 0;
    channels = 0;

    g_running = true;
    std::promise<CaptureResult> promise;
    std::future<CaptureResult> future = promise.get_future();
    g_thread =
        std::thread(CaptureLoop, endpointId, durationMs, std::move(promise));

    CaptureResult result = future.get();
    g_running = false;
    if (g_thread.joinable()) {
        g_thread.join();
    }

    samples = std::move(result.samples);
    sampleRate = result.sampleRate;
    channels = result.channels;
    return result.error;
}

} // namespace apo
