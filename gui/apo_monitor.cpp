#include "apo_monitor.h"

#include "apo.h"
#include "stage_monitor.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <future>
#include <limits>
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

void ListenLog(const wchar_t* format, ...)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, L"C:\\ProgramData\\better_micro_apo\\listen.log",
                  L"a, ccs=UTF-8") != 0 ||
        file == nullptr)
    {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    fwprintf(file, L"[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute,
             now.wSecond, now.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfwprintf(file, format, args);
    va_end(args);
    fwprintf(file, L"\n");
    fclose(file);
}

std::wstring FormatText(const WAVEFORMATEX* format)
{
    if (format == nullptr) {
        return L"null";
    }
    wchar_t text[128]{};
    swprintf_s(text, L"tag=%u ch=%u rate=%u bits=%u block=%u",
               format->wFormatTag, format->nChannels,
               format->nSamplesPerSec, format->wBitsPerSample,
               format->nBlockAlign);
    return text;
}

FormatPtr CloneFormat(const WAVEFORMATEX* format)
{
    if (format == nullptr) {
        return {};
    }
    const size_t bytes = sizeof(WAVEFORMATEX) +
                         (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                              ? format->cbSize
                              : 0u);
    void* memory = CoTaskMemAlloc(bytes);
    if (memory == nullptr) {
        return {};
    }
    std::memcpy(memory, format, bytes);
    return FormatPtr(static_cast<WAVEFORMATEX*>(memory));
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

BlockLevel FloatBlockLevel(const float* data, UINT32 frames, UINT32 channels)
{
    BlockLevel level;
    if (data == nullptr || frames == 0 || channels == 0) {
        return level;
    }

    const size_t count = static_cast<size_t>(frames) * channels;
    level.samples = static_cast<unsigned long long>(count);
    for (size_t i = 0; i < count; ++i) {
        const double value = static_cast<double>(data[i]);
        const double magnitude = std::fabs(value);
        level.sumSquares += value * value;
        if (magnitude > level.peak) {
            level.peak = magnitude;
        }
    }
    return level;
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
    // Post-DSP floats can reach full scale even when the render format cannot,
    // so treat near-full-scale as clipping too.
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

void WriteRenderSample(BYTE* destination, double value, UINT32 sampleBytes,
                       bool isFloat)
{
    value = std::clamp(value, -1.0, 1.0);
    if (isFloat && sampleBytes == 4) {
        float sample = static_cast<float>(value);
        std::memcpy(destination, &sample, sizeof(sample));
    } else if (sampleBytes == 2) {
        const int32_t raw = static_cast<int32_t>(
            std::llround(value * 32767.0));
        const int16_t sample = static_cast<int16_t>(
            std::clamp<int32_t>(raw, -32768, 32767));
        std::memcpy(destination, &sample, sizeof(sample));
    } else if (sampleBytes == 3) {
        const int32_t raw = static_cast<int32_t>(
            std::llround(value * 8388607.0));
        const uint32_t bits = static_cast<uint32_t>(std::clamp<int32_t>(
            raw, -8388608, 8388607)) & 0x00FFFFFFu;
        destination[0] = static_cast<BYTE>(bits & 0xFFu);
        destination[1] = static_cast<BYTE>((bits >> 8) & 0xFFu);
        destination[2] = static_cast<BYTE>((bits >> 16) & 0xFFu);
    } else if (sampleBytes == 4) {
        const int64_t raw = static_cast<int64_t>(
            std::llround(value * 2147483647.0));
        const int32_t sample = static_cast<int32_t>(std::clamp<int64_t>(
            raw, std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::max()));
        std::memcpy(destination, &sample, sizeof(sample));
    }
}

double TapChannelAt(const std::vector<float>& input, size_t frameIndex,
                    size_t outputChannel, size_t outputChannels)
{
    // The APO tap always publishes interleaved stereo floats; mono capture is
    // duplicated into both channels by StageMonitor::WriteAudio.
    const float* frame = input.data() + frameIndex * kStageMonitorAudioChannels;
    if (outputChannels == 1) {
        return (static_cast<double>(frame[0]) + frame[1]) * 0.5;
    }
    return outputChannel < kStageMonitorAudioChannels
               ? frame[outputChannel]
               : 0.0;
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
    // Audition audio comes from the APO's post-DSP ring. The bootstrap capture
    // stream exists only to make Windows lock the APO when no other app is
    // using the selected endpoint; it is drained and discarded, never read for
    // Listen. Opening a capture stream on Line 1/2 can unlock/rebuild the APO,
    // so it is started only when the tap is not already live.
    ComPtr<IAudioClient> bootstrapClient;
    ComPtr<IAudioCaptureClient> bootstrap;
    StageMonitorView tap;
    ComPtr<IAudioClient> renderClient;
    ComPtr<IAudioRenderClient> render;
    FormatPtr renderFormat;
    UINT32 sourceRate = 0;
    UINT32 sourceChannels = 0;
    UINT32 renderFrames = 0;
    UINT32 renderRate = 0;
    UINT32 renderFrameBytes = 0;
    UINT32 renderChannels = 0;
    UINT32 renderSampleBytes = 0;
    bool renderIsFloat = false;

    ~Monitor()
    {
        if (bootstrapClient) {
            bootstrapClient->Stop();
        }
        if (renderClient) {
            renderClient->Stop();
        }
    }
};

std::wstring OpenBootstrapCapture(const std::wstring& endpointId,
                                  IMMDeviceEnumerator* enumerator,
                                  Monitor& monitor)
{
    if (enumerator == nullptr) {
        return L"The audio device enumerator is unavailable.";
    }

    ListenLog(L"Opening APO bootstrap capture endpoint=%s", endpointId.c_str());

    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE,
                                                collection.put());
    if (FAILED(hr)) {
        return HresultText(L"Could not enumerate capture devices", hr);
    }

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return L"Could not read the capture device list.";
    }
    ListenLog(L"Active capture devices: %u", count);

    ComPtr<IMMDevice> captureDevice;
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
        reinterpret_cast<void**>(monitor.bootstrapClient.put()));
    if (FAILED(hr)) {
        return HresultText(L"Could not open the capture device", hr);
    }

    WAVEFORMATEX* rawFormat = nullptr;
    hr = monitor.bootstrapClient->GetMixFormat(&rawFormat);
    if (FAILED(hr) || rawFormat == nullptr) {
        return HresultText(L"Could not read the capture format", hr);
    }
    FormatPtr format(rawFormat);
    ListenLog(L"Bootstrap capture format: %s", FormatText(format.get()).c_str());

    hr = monitor.bootstrapClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, format.get(), nullptr);
    ListenLog(L"Bootstrap capture Init hr=0x%08X", static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        return HresultText(L"Could not start the capture stream", hr);
    }

    hr = monitor.bootstrapClient->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(monitor.bootstrap.put()));
    if (FAILED(hr)) {
        return HresultText(L"Could not open the capture client", hr);
    }

    hr = monitor.bootstrapClient->Start();
    ListenLog(L"Bootstrap capture Start hr=0x%08X", static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        return HresultText(L"Could not start the capture stream", hr);
    }
    return {};
}

void DrainBootstrapCapture(Monitor& monitor)
{
    if (!monitor.bootstrap) {
        return;
    }

    UINT32 packet = 0;
    while (SUCCEEDED(monitor.bootstrap->GetNextPacketSize(&packet)) &&
           packet != 0)
    {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(monitor.bootstrap->GetBuffer(&data, &frames, &flags,
                                                nullptr, nullptr)))
        {
            return;
        }
        // Keep the capture stream moving so the APO stays locked. Its samples
        // are deliberately discarded; Listen reads the APO's audio ring.
        monitor.bootstrap->ReleaseBuffer(frames);
    }
}

std::wstring OpenMonitor(const std::wstring& endpointId, Monitor& monitor)
{
    const auto fail = [](const std::wstring& message) {
        ListenLog(L"FAIL: %s", message.c_str());
        return message;
    };
    ListenLog(L"OpenMonitor endpoint=%s", endpointId.c_str());

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(enumerator.put()));
    if (FAILED(hr)) {
        return fail(HresultText(L"Could not access the audio devices", hr));
    }

    // The APO publishes the post-DSP audio ring only while its processor is
    // locked. If another app is already using the endpoint, use that live tap
    // without opening a second capture stream.
    ULONG tapRate = 0;
    ULONG tapChannels = 0;
    bool tapReady = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (monitor.tap.Open(endpointId, false) &&
            monitor.tap.AudioFormat(tapRate, tapChannels))
        {
            tapReady = true;
            break;
        }
        Sleep(20);
    }
    ListenLog(L"Initial APO tap ready=%d", tapReady ? 1 : 0);

    if (!tapReady) {
        // Windows does not lock a capture APO just because it is enabled. A
        // short-lived shared stream here bootstraps LockForProcess; its audio
        // is drained and discarded, while Listen renders from the APO tap.
        const std::wstring bootstrapError =
            OpenBootstrapCapture(endpointId, enumerator.get(), monitor);
        if (!bootstrapError.empty()) {
            ListenLog(L"Bootstrap capture failed: %s", bootstrapError.c_str());
        }
        for (int attempt = 0; attempt < 50; ++attempt) {
            DrainBootstrapCapture(monitor);
            if (monitor.tap.Open(endpointId, false) &&
                monitor.tap.AudioFormat(tapRate, tapChannels))
            {
                tapReady = true;
                break;
            }
            Sleep(20);
        }
        if (!tapReady) {
            std::wstring message =
                L"The APO did not start audio for this device. Check Enable and try again.";
            if (!bootstrapError.empty()) {
                message += L" " + bootstrapError;
            }
            return fail(message);
        }
    }
    ListenLog(L"APO tap opened rate=%lu ch=%lu generation=%lu", tapRate,
              tapChannels, monitor.tap.Generation());

    // Listen always renders through the default console output. Its samples
    // come from the APO tap, never from the bootstrap capture client.
    ComPtr<IMMDevice> renderDevice;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                             renderDevice.put());
    if (FAILED(hr)) {
        return fail(HresultText(L"Could not find the default output device", hr));
    }

    LPWSTR renderId = nullptr;
    if (SUCCEEDED(renderDevice->GetId(&renderId)) && renderId != nullptr) {
        ListenLog(L"Default render endpoint: %s", renderId);
        CoTaskMemFree(renderId);
    }

    hr = renderDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(monitor.renderClient.put()));
    if (FAILED(hr)) {
        return fail(HresultText(L"Could not open the default output device", hr));
    }

    FormatPtr renderNative;
    {
        WAVEFORMATEX* raw = nullptr;
        hr = monitor.renderClient->GetMixFormat(&raw);
        if (FAILED(hr) || raw == nullptr) {
            return fail(HresultText(L"Could not read the output format", hr));
        }
        renderNative.reset(raw);
    }
    ListenLog(L"Render mix format: %s", FormatText(renderNative.get()).c_str());

    monitor.renderFormat = CloneFormat(renderNative.get());
    if (!monitor.renderFormat) {
        return fail(L"Could not copy the output format.");
    }

    hr = monitor.renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                          2000000, 0,
                                          monitor.renderFormat.get(), nullptr);
    ListenLog(L"Render Init hr=0x%08X", static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        return fail(HresultText(L"Could not start the output stream", hr));
    }

    hr = monitor.renderClient->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(monitor.render.put()));
    ListenLog(L"Render GetService hr=0x%08X", static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        return fail(HresultText(L"Could not open the output client", hr));
    }

    hr = monitor.renderClient->GetBufferSize(&monitor.renderFrames);
    if (FAILED(hr) || monitor.renderFrames == 0) {
        return fail(HresultText(L"Could not size the output buffer", hr));
    }
    ListenLog(L"Render buffer frames=%u", monitor.renderFrames);

    monitor.renderFrameBytes = monitor.renderFormat->nBlockAlign;
    monitor.renderChannels = monitor.renderFormat->nChannels;
    if (monitor.renderFrameBytes == 0 || monitor.renderChannels == 0 ||
        monitor.renderFormat->nSamplesPerSec == 0)
    {
        return fail(L"The output format has no frame size.");
    }
    monitor.renderSampleBytes =
        monitor.renderFrameBytes / monitor.renderChannels;
    monitor.renderRate = monitor.renderFormat->nSamplesPerSec;
    monitor.renderIsFloat = IsFloatFormat(monitor.renderFormat.get());
    monitor.sourceRate = tapRate;
    monitor.sourceChannels = tapChannels;

    ListenLog(L"Source layout rate=%u ch=%u", monitor.sourceRate,
              monitor.sourceChannels);
    ListenLog(L"Render layout rate=%u ch=%u frame=%u sample=%u float=%d",
              monitor.renderRate, monitor.renderChannels,
              monitor.renderFrameBytes, monitor.renderSampleBytes,
              monitor.renderIsFloat ? 1 : 0);

    hr = monitor.renderClient->Start();
    ListenLog(L"Render Start hr=0x%08X", static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        return fail(HresultText(L"Could not start the output stream", hr));
    }

    ListenLog(L"OpenMonitor success");
    return {};
}

void RunMonitor(Monitor& monitor)
{
    ULONG sourceRate = monitor.sourceRate;
    ULONG generation = monitor.tap.Generation();
    const UINT32 renderRate = monitor.renderRate;
    const UINT32 renderChannels = monitor.renderChannels;
    if (sourceRate == 0 || renderRate == 0 || renderChannels == 0 ||
        monitor.renderFrameBytes == 0 || monitor.renderSampleBytes == 0)
    {
        ListenLog(L"RunMonitor: invalid format layout");
        g_running.store(false);
        return;
    }

    // The render device keeps its native mix format. Resample the APO tap in
    // software so WASAPI never has to reconfigure either endpoint's APO chain.
    double sourceStep =
        static_cast<double>(sourceRate) / static_cast<double>(renderRate);
    const size_t maxInputFrames = sourceRate; // one second
    constexpr ULONG kTapFrames = 4096;
    std::vector<float> block(kTapFrames * kStageMonitorAudioChannels);
    std::vector<float> input;
    input.reserve(maxInputFrames * kStageMonitorAudioChannels);
    size_t inputBase = 0;    // absolute tap frame index of input[0]
    double sourcePos = 0.0;  // next render frame's position in tap frames

    while (g_running.load()) {
        // Keep the bootstrap stream alive and moving; its samples are not part
        // of the audition path.
        DrainBootstrapCapture(monitor);

        ULONG tapRate = 0;
        ULONG tapChannels = 0;
        if (monitor.tap.AudioFormat(tapRate, tapChannels) &&
            (tapRate != sourceRate ||
             tapChannels != monitor.sourceChannels))
        {
            ListenLog(L"APO format changed rate=%lu ch=%lu", tapRate,
                      tapChannels);
            sourceRate = tapRate;
            monitor.sourceChannels = tapChannels;
            sourceStep = static_cast<double>(sourceRate) /
                         static_cast<double>(renderRate);
            input.clear();
            inputBase = 0;
            sourcePos = 0.0;
        }

        const ULONG currentGeneration = monitor.tap.Generation();
        if (currentGeneration != generation) {
            // A fresh APO instance restarts its ring at zero. Drop frames from
            // the old stream instead of trying to splice the two timelines.
            ListenLog(L"APO restart generation=%lu", currentGeneration);
            generation = currentGeneration;
            monitor.tap.RebaseAudioRead();
            input.clear();
            inputBase = 0;
            sourcePos = 0.0;
        }

        ULONG frames = 0;
        if (monitor.tap.ReadAudio(block.data(), kTapFrames, frames) &&
            frames != 0)
        {
            ReportLevel(FloatBlockLevel(block.data(), frames,
                                        kStageMonitorAudioChannels));
            input.insert(input.end(), block.begin(),
                         block.begin() +
                             static_cast<size_t>(frames) *
                                 kStageMonitorAudioChannels);
        }

        size_t inputFrames = input.size() / kStageMonitorAudioChannels;
        if (inputFrames > maxInputFrames) {
            const size_t dropFrames = inputFrames - maxInputFrames;
            input.erase(input.begin(),
                        input.begin() +
                            static_cast<std::ptrdiff_t>(
                                dropFrames * kStageMonitorAudioChannels));
            inputBase += dropFrames;
            if (sourcePos < static_cast<double>(inputBase)) {
                sourcePos = static_cast<double>(inputBase);
            }
            g_drops.fetch_add(static_cast<unsigned>(dropFrames),
                              std::memory_order_relaxed);
            inputFrames = maxInputFrames;
        }

        UINT32 padding = 0;
        if (SUCCEEDED(monitor.renderClient->GetCurrentPadding(&padding))) {
            const UINT32 available =
                (padding < monitor.renderFrames)
                    ? (monitor.renderFrames - padding)
                    : 0;
            if (available != 0) {
                BYTE* output = nullptr;
                if (SUCCEEDED(monitor.render->GetBuffer(available, &output)) &&
                    output != nullptr)
                {
                    UINT32 produced = 0;
                    while (produced < available) {
                        const size_t absoluteIndex =
                            static_cast<size_t>(sourcePos);
                        const size_t totalFrames = inputBase + inputFrames;
                        if (absoluteIndex < inputBase ||
                            absoluteIndex + 1 >= totalFrames)
                        {
                            break;
                        }
                        const size_t relative = absoluteIndex - inputBase;
                        const double fraction =
                            sourcePos - static_cast<double>(absoluteIndex);
                        for (UINT32 channel = 0; channel < renderChannels;
                             ++channel)
                        {
                            const double left = TapChannelAt(
                                input, relative, channel, renderChannels);
                            const double right = TapChannelAt(
                                input, relative + 1, channel, renderChannels);
                            const double value =
                                left + (right - left) * fraction;
                            WriteRenderSample(
                                output +
                                    static_cast<size_t>(produced) *
                                        monitor.renderFrameBytes +
                                    channel * monitor.renderSampleBytes,
                                value, monitor.renderSampleBytes,
                                monitor.renderIsFloat);
                        }
                        ++produced;
                        sourcePos += sourceStep;
                    }

                    if (produced != 0) {
                        monitor.render->ReleaseBuffer(produced, 0);
                        // Keep the frame at floor(sourcePos) as the left side
                        // of the next linear interpolation.
                        const size_t consumed = static_cast<size_t>(sourcePos);
                        if (consumed > inputBase) {
                            const size_t dropFrames = std::min(
                                consumed - inputBase,
                                input.size() / kStageMonitorAudioChannels);
                            if (dropFrames != 0) {
                                input.erase(
                                    input.begin(),
                                    input.begin() +
                                        static_cast<std::ptrdiff_t>(
                                            dropFrames *
                                            kStageMonitorAudioChannels));
                                inputBase += dropFrames;
                            }
                        }
                    } else {
                        // No tap audio yet; keep the render clock moving with
                        // silence so the device does not stall.
                        monitor.render->ReleaseBuffer(
                            available, AUDCLNT_BUFFERFLAGS_SILENT);
                    }
                }
            }
        }

        Sleep(5);
    }
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

void LogListenEvent(const std::wstring& text)
{
    ListenLog(L"%s", text.c_str());
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

} // namespace apo
