#include "stage_capture.h"

#include "runtime_settings.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace apo {
namespace {

constexpr double kPi = 3.14159265358979323846;
volatile LONG g_nextCaptureRequest = 0;

LONG LoadState(StageCaptureFile* capture)
{
    return InterlockedCompareExchange(&capture->state, StageCaptureIdle,
                                      StageCaptureIdle);
}

LONG LoadRequestId(StageCaptureFile* capture)
{
    return InterlockedCompareExchange(&capture->requestId, 0, 0);
}

std::wstring CaptureErrorText(ULONG error)
{
    switch (error) {
    case StageCaptureErrorInvalidTarget:
        return L"The selected effect is not an enabled Noise effect in the active preset.";
    case StageCaptureErrorBadDuration:
        return L"The capture duration is invalid for this audio format.";
    case StageCaptureErrorNotConfigured:
        return L"The audio chain is not ready to capture a profile.";
    default:
        return L"Noise profile capture failed.";
    }
}

} // namespace

std::wstring StageCapturePath(const std::wstring& endpointId)
{
    return RuntimeSettingsDirectory() + L"\\" + SafeEndpointId(endpointId) +
           L".capture";
}

bool StageCaptureView::Open(const std::wstring& endpointId, bool create)
{
    Close();

    const std::wstring directory = RuntimeSettingsDirectory();
    CreateDirectoryW(directory.c_str(), nullptr);

    HANDLE file = CreateFileW(
        StageCapturePath(endpointId).c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    HANDLE mapping = CreateFileMappingW(
        file, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(StageCaptureFile)), nullptr);
    if (mapping == nullptr) {
        CloseHandle(file);
        return false;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                               sizeof(StageCaptureFile));
    if (view == nullptr) {
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }

    auto* capture = static_cast<StageCaptureFile*>(view);
    for (;;) {
        const LONG value =
            InterlockedCompareExchange(&capture->initialized, 1, 0);
        if (value == 0) {
            capture->magic = kStageCaptureMagic;
            capture->version = kStageCaptureVersion;
            capture->structSize = static_cast<ULONG>(sizeof(StageCaptureFile));
            capture->state = StageCaptureIdle;
            capture->requestId = 0;
            capture->presetIndex = -1;
            capture->effectId = -1;
            capture->durationMs = 0;
            capture->sampleRate = 0;
            capture->channels = 0;
            capture->binCount = 0;
            capture->framesTarget = 0;
            capture->framesDone = 0;
            capture->error = StageCaptureErrorNone;
            capture->resultAck = kStageCaptureAckNone;
            std::fill(std::begin(capture->bins), std::end(capture->bins), 0.0);
            InterlockedExchange(&capture->initialized, 2);
            break;
        }

        while (InterlockedCompareExchange(&capture->initialized, 2, 2) != 2) {
            Sleep(1);
        }
        if (capture->magic == kStageCaptureMagic &&
            capture->version == kStageCaptureVersion &&
            capture->structSize == sizeof(StageCaptureFile)) {
            break;
        }

        // A stale file from an older build cannot be read; let either side
        // claim it again and write the current layout.
        InterlockedExchange(&capture->initialized, 0);
    }

    m_file = file;
    m_mapping = mapping;
    m_view = view;
    return true;
}

void StageCaptureView::Close()
{
    if (m_view != nullptr) {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    if (m_mapping != nullptr) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
    if (m_file != nullptr) {
        CloseHandle(m_file);
        m_file = nullptr;
    }
}

bool RequestStageCapture(const std::wstring& endpointId, int presetIndex,
                         int effectId, unsigned durationMs,
                         std::wstring& error)
{
    StageCaptureView view;
    if (!view.Open(endpointId, true)) {
        error = L"Could not open the audio capture channel.";
        return false;
    }
    StageCaptureFile* capture = view.Get();

    const ULONGLONG deadline = GetTickCount64() + durationMs + 10000;
    for (;;) {
        const LONG state = LoadState(capture);
        if (state == StageCaptureIdle || state == StageCaptureFailed ||
            state == StageCaptureDone) {
            // A done state with no acknowledge means the previous GUI exited
            // mid-capture. Let the APO drop its tap and reuse the block.
            InterlockedExchange(&capture->resultAck, kStageCaptureAckFailed);
            InterlockedExchange(&capture->state, StageCaptureIdle);
            break;
        }
        if (GetTickCount64() >= deadline) {
            error = L"The audio chain is busy with another noise capture.";
            InterlockedExchange(&capture->state, StageCaptureIdle);
            return false;
        }
        Sleep(10);
    }

    LONG requestId = InterlockedIncrement(&g_nextCaptureRequest);
    if (requestId == 0) {
        requestId = InterlockedIncrement(&g_nextCaptureRequest);
    }

    capture->presetIndex = presetIndex;
    capture->effectId = effectId;
    capture->durationMs = durationMs;
    capture->error = StageCaptureErrorNone;
    capture->binCount = 0;
    capture->framesTarget = 0;
    capture->framesDone = 0;
    capture->sampleRate = 0;
    capture->channels = 0;
    capture->resultAck = kStageCaptureAckNone;
    std::fill(std::begin(capture->bins), std::end(capture->bins), 0.0);
    InterlockedExchange(&capture->requestId, requestId);
    InterlockedExchange(&capture->state, StageCaptureRequested);

    for (;;) {
        const LONG state = LoadState(capture);
        const LONG active = LoadRequestId(capture);
        if ((state == StageCaptureDone || state == StageCaptureFailed) &&
            active == requestId) {
            break;
        }
        if (GetTickCount64() >= deadline) {
            error = L"The audio chain did not finish the noise capture.";
            InterlockedExchange(&capture->state, StageCaptureIdle);
            return false;
        }
        Sleep(10);
    }

    if (LoadState(capture) == StageCaptureFailed) {
        error = CaptureErrorText(capture->error);
        InterlockedExchange(&capture->state, StageCaptureIdle);
        return false;
    }

    const std::uint32_t sampleRate = capture->sampleRate;
    const ULONG binCount = capture->binCount;
    const int profileFft = (binCount >= 2)
        ? static_cast<int>((binCount - 1) * 2)
        : 0;
    const bool validBins = binCount > 1 && binCount <= dsp::kNoiseMaxBins &&
                           profileFft > 0 &&
                           ValidNoiseFftSize(profileFft) == profileFft;
    const std::vector<double> bins = validBins
        ? std::vector<double>(capture->bins, capture->bins + binCount)
        : std::vector<double>{};
    const bool saved = sampleRate != 0 && capture->framesDone != 0 &&
                       SaveNoiseProfileBins(endpointId, sampleRate, bins);
    if (!saved) {
        error = L"Could not save the noise profile file.";
    }

    // The APO applies the new profile only after a successful save. It then
    // removes the tap and returns the control block to idle.
    InterlockedExchange(&capture->resultAck,
                        saved ? kStageCaptureAckSaved
                              : kStageCaptureAckFailed);
    const ULONGLONG ackDeadline = GetTickCount64() + 10000;
    while (LoadState(capture) != StageCaptureIdle) {
        if (GetTickCount64() >= ackDeadline) {
            InterlockedExchange(&capture->state, StageCaptureIdle);
            break;
        }
        Sleep(10);
    }
    return saved;
}

void StageCaptureTap::Configure(const apo::dsp::AudioFormat& format)
{
    m_channels = format.channels;

    const size_t planCount = kNoiseFftSizes.size();
    m_fftPlans.clear();
    m_fftPlans.reserve(planCount);
    m_windows.assign(planCount, {});
    for (size_t index = 0; index < planCount; ++index) {
        const size_t size = static_cast<size_t>(kNoiseFftSizes[index]);
        m_fftPlans.emplace_back(size);
        m_windows[index].resize(size);
        for (size_t sample = 0; sample < size; ++sample) {
            const double hann =
                0.5 - 0.5 * std::cos(2.0 * kPi *
                                     static_cast<double>(sample) /
                                     static_cast<double>(size));
            m_windows[index][sample] = std::sqrt(std::max(0.0, hann));
        }
    }

    const size_t maxSize = static_cast<size_t>(kNoiseFftSizes.back());
    m_history.assign(maxSize * m_channels, 0.0);
    m_frameReal.assign(maxSize, 0.0);
    m_frameImag.assign(maxSize, 0.0);
    m_sum.assign(dsp::kNoiseMaxBins, 0.0);
    Reset();
}

void StageCaptureTap::Reset()
{
    std::fill(m_history.begin(), m_history.end(), 0.0);
    std::fill(m_sum.begin(), m_sum.end(), 0.0);
    m_framesTarget = 0;
    m_framesDone = 0;
    m_cursor = 0;
    m_measured = 0;
    m_active = false;
    m_complete = false;
}

bool StageCaptureTap::Begin(ULONG framesTarget, int fftSize)
{
    const int size = ValidNoiseFftSize(fftSize);
    if (m_channels == 0 || m_sum.empty() || framesTarget == 0 ||
        size != fftSize) {
        return false;
    }

    m_sizeIndex = static_cast<std::size_t>(NoiseFftSizeIndex(size));
    m_fftSize = static_cast<size_t>(size);
    m_hopSize = m_fftSize / 2;
    m_binCount = static_cast<ULONG>(m_fftSize / 2 + 1);
    Reset();
    m_framesTarget = framesTarget;
    m_active = true;
    return true;
}

void StageCaptureTap::AnalyzeHop()
{
    const size_t N = m_fftSize;
    const size_t H = m_hopSize;

    if (m_channels == 0 || m_sum.size() < N / 2 + 1) {
        return;
    }

    for (UINT32 channel = 0; channel < m_channels; ++channel) {
        const double* history = m_history.data() +
                                static_cast<size_t>(channel) * N;
        for (size_t index = 0; index < N; ++index) {
            m_frameReal[index] = history[index] * m_windows[m_sizeIndex][index];
            m_frameImag[index] = 0.0;
        }
        m_fftPlans[m_sizeIndex].Forward(m_frameReal.data(),
                                        m_frameImag.data());
        for (size_t bin = 0; bin <= N / 2; ++bin) {
            m_sum[bin] += std::sqrt(m_frameReal[bin] * m_frameReal[bin] +
                                    m_frameImag[bin] * m_frameImag[bin]);
        }
        ++m_measured;
    }

    for (UINT32 channel = 0; channel < m_channels; ++channel) {
        double* history = m_history.data() +
                          static_cast<size_t>(channel) * N;
        std::memmove(history, history + H, (N - H) * sizeof(double));
        std::fill(history + (N - H), history + N, 0.0);
    }
    m_cursor = N - H;
}

void StageCaptureTap::Finish(double* bins, size_t count) const
{
    if (bins == nullptr || m_measured == 0) {
        return;
    }
    const size_t available =
        std::min({count, m_sum.size(), static_cast<size_t>(m_binCount)});
    for (size_t index = 0; index < available; ++index) {
        bins[index] = m_sum[index] / static_cast<double>(m_measured);
    }
}

void StageCaptureTap::Process(double* samples, UINT32 frames, UINT32 channels)
{
    const size_t N = m_fftSize;

    if (samples == nullptr || frames == 0 || channels == 0 ||
        channels != m_channels || m_history.empty()) {
        return;
    }

    if (!m_active || m_framesDone >= m_framesTarget) {
        return;
    }

    const ULONG take = static_cast<ULONG>(
        std::min<size_t>(frames, m_framesTarget - m_framesDone));
    for (ULONG frame = 0; frame < take; ++frame) {
        const double* values = samples + static_cast<size_t>(frame) * channels;
        for (UINT32 channel = 0; channel < channels; ++channel) {
            m_history[static_cast<size_t>(channel) * N + m_cursor] =
                values[channel];
        }
        ++m_cursor;
        if (m_cursor >= N) {
            AnalyzeHop();
        }
    }

    m_framesDone += take;
    if (m_framesDone >= m_framesTarget) {
        m_active = false;
        m_complete = true;
    }
}

} // namespace apo
