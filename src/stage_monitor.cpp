#include "stage_monitor.h"

#include "dsp/dsp.h"
#include "runtime_settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace apo {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kSpectrumSmoothing = 0.18f;
// Half-lives of the two published peak holds. Decay is measured in audio time,
// not buffer count, so the meter behaves the same for short and long engine
// buffers and a transient survives until the next GUI poll. The short hold
// drives the fill; the longer one remains as the peak line.
constexpr float kShortPeakHalfLifeSeconds = 0.06f;
constexpr float kPeakHalfLifeSeconds = 0.6f;

} // namespace

struct StageMonitorImpl
{
    StageMonitorImpl() : fft(kStageMonitorFftSize)
    {
        for (int i = 0; i < kStageMonitorFftSize; ++i) {
            const double hann =
                0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                     static_cast<double>(kStageMonitorFftSize));
            window[static_cast<size_t>(i)] = hann;
        }
    }

    apo::dsp::FftPlan fft;
    std::array<double, kStageMonitorFftSize> window{};
    std::array<double, kStageMonitorFftSize> real{};
    std::array<double, kStageMonitorFftSize> imag{};
    std::array<std::array<double, kStageMonitorBins>, kStageMonitorSlots>
        spectrum{};
    std::array<std::array<double, kStageMonitorFftSize>, kStageMonitorSlots>
        history{};
    std::array<int, kStageMonitorSlots> historyWrite{};
    std::array<int, kStageMonitorSlots> hopCounter{};
    std::array<float, kStageMonitorSlots> inputPeakShortHold{};
    std::array<float, kStageMonitorSlots> inputRmsShortHold{};
    std::array<float, kStageMonitorSlots> outputRmsShortHold{};
    std::array<float, kStageMonitorSlots> outputPeakShortHold{};
    std::array<float, kStageMonitorSlots> peakHold{};
};

std::wstring StageMonitorPath(const std::wstring& endpointId)
{
    return RuntimeSettingsDirectory() + L"\\" + SafeEndpointId(endpointId) +
           L".monitor";
}

namespace {

bool HeaderValid(const StageMonitorFile* file)
{
    return file != nullptr &&
           file->magic == kStageMonitorMagic &&
           file->version == kStageMonitorVersion &&
           file->structSize == static_cast<ULONG>(sizeof(StageMonitorFile)) &&
           file->slotCount == static_cast<ULONG>(kStageMonitorSlots);
}

void InitializeHeader(StageMonitorFile* file, UINT32 sampleRate)
{
    std::memset(file, 0, sizeof(StageMonitorFile));
    file->magic = kStageMonitorMagic;
    file->version = kStageMonitorVersion;
    file->structSize = static_cast<ULONG>(sizeof(StageMonitorFile));
    file->sampleRate = sampleRate;
    file->slotCount = static_cast<ULONG>(kStageMonitorSlots);
}

} // namespace

bool StageMonitorView::Open(const std::wstring& endpointId, bool create)
{
    Close();
    if (endpointId.empty()) {
        return false;
    }

    const std::wstring path = StageMonitorPath(endpointId);
    m_file = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_file == nullptr || m_file == INVALID_HANDLE_VALUE) {
        m_file = nullptr;
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(m_file, &size) ||
        size.QuadPart < static_cast<LONGLONG>(sizeof(StageMonitorFile))) {
        Close();
        return false;
    }

    m_mapping = CreateFileMappingW(m_file, nullptr, PAGE_READONLY, 0,
                                   static_cast<DWORD>(sizeof(StageMonitorFile)),
                                   nullptr);
    if (m_mapping == nullptr) {
        Close();
        return false;
    }

    m_view = static_cast<const StageMonitorFile*>(
        MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0,
                      sizeof(StageMonitorFile)));
    if (m_view == nullptr || !HeaderValid(m_view)) {
        Close();
        return false;
    }

    m_endpointId = endpointId;
    return true;
}

void StageMonitorView::Close()
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
    m_endpointId.clear();
}

bool StageMonitorView::Read(int slot, StageMonitorSnapshot& snapshot) const
{
    snapshot = StageMonitorSnapshot{};
    if (m_view == nullptr || slot < 0 || slot >= kStageMonitorSlots) {
        return false;
    }

    const StageMonitorSlot& source = m_view->slots[slot];
    LONG sequence = 0;
    for (;;) {
        sequence = source.sequence;
        std::atomic_thread_fence(std::memory_order_acquire);
        if ((sequence & 1) != 0) {
            continue;
        }

        snapshot.effectId = source.effectId;
        snapshot.type = static_cast<apo::EffectType>(source.type);
        snapshot.sampleRate = m_view->sampleRate;
        snapshot.inputPeak = source.inputPeak;
        snapshot.inputRms = source.inputRms;
        snapshot.outputPeak = source.outputPeak;
        snapshot.outputRms = source.outputRms;
        snapshot.inputPeakShortHold = source.inputPeakShortHold;
        snapshot.inputRmsShortHold = source.inputRmsShortHold;
        snapshot.outputRmsShortHold = source.outputRmsShortHold;
        snapshot.outputPeakShortHold = source.outputPeakShortHold;
        snapshot.outputPeakHold = source.outputPeakHold;
        snapshot.engagement = source.engagement;
        snapshot.dynamicGainDb = source.dynamicGainDb;
        snapshot.tunerFrequencyHz = source.tunerFrequencyHz;
        snapshot.tunerConfidence = source.tunerConfidence;
        snapshot.flags = source.flags;
        snapshot.valid = (source.flags & kStageMonitorFlagObserved) != 0;
        std::memcpy(snapshot.spectrum, source.spectrum,
                    sizeof(snapshot.spectrum));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (source.sequence == sequence) {
            break;
        }
    }

    return snapshot.valid;
}

bool StageMonitorView::Find(int effectId, apo::EffectType type,
                            StageMonitorSnapshot& snapshot) const
{
    for (int slot = 0; slot < kStageMonitorSlots; ++slot) {
        if (Read(slot, snapshot) && snapshot.effectId == effectId &&
            snapshot.type == type) {
            return true;
        }
    }
    snapshot = StageMonitorSnapshot{};
    return false;
}

StageMonitor::StageMonitor() : m_impl(std::make_unique<StageMonitorImpl>()) {}

StageMonitor::~StageMonitor() = default;

bool StageMonitor::Open(const std::wstring& endpointId, UINT32 sampleRate)
{
    Close();
    if (endpointId.empty()) {
        return false;
    }

    const std::wstring path = StageMonitorPath(endpointId);
    m_file = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_file == nullptr || m_file == INVALID_HANDLE_VALUE) {
        m_file = nullptr;
        return false;
    }

    LARGE_INTEGER desired{};
    desired.QuadPart = static_cast<LONGLONG>(sizeof(StageMonitorFile));
    if (!SetFilePointerEx(m_file, desired, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(m_file)) {
        Close();
        return false;
    }

    m_mapping = CreateFileMappingW(m_file, nullptr, PAGE_READWRITE, 0,
                                   static_cast<DWORD>(sizeof(StageMonitorFile)),
                                   nullptr);
    if (m_mapping == nullptr) {
        Close();
        return false;
    }

    m_view = static_cast<StageMonitorFile*>(
        MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0,
                      sizeof(StageMonitorFile)));
    if (m_view == nullptr) {
        Close();
        return false;
    }

    if (!HeaderValid(m_view)) {
        InitializeHeader(m_view, sampleRate);
    } else if (m_view->sampleRate != sampleRate) {
        // The stream format changed. Old spectrum bins are in the wrong
        // frequency scale, so drop the published values.
        m_view->sampleRate = sampleRate;
        for (int slot = 0; slot < kStageMonitorSlots; ++slot) {
            m_view->slots[slot].flags = 0;
        }
    }

    m_observed.fill(false);
    for (auto& bins : m_impl->spectrum) {
        bins.fill(0.0);
    }
    for (auto& values : m_impl->history) {
        values.fill(0.0);
    }
    m_impl->historyWrite.fill(0);
    m_impl->hopCounter.fill(0);
    m_impl->inputPeakShortHold.fill(0.0f);
    m_impl->inputRmsShortHold.fill(0.0f);
    m_impl->outputRmsShortHold.fill(0.0f);
    m_impl->outputPeakShortHold.fill(0.0f);
    m_impl->peakHold.fill(0.0f);
    return true;
}

void StageMonitor::Close()
{
    if (m_view != nullptr) {
        // Publishing an empty set on shutdown keeps the GUI from showing the
        // last buffer forever after the audio stream unbinds.
        for (int slot = 0; slot < kStageMonitorSlots; ++slot) {
            ClearSlot(static_cast<size_t>(slot));
        }
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
    m_observed.fill(false);
}

void StageMonitor::BeginBuffer()
{
    m_observed.fill(false);
}

void StageMonitor::ObserveStage(size_t slot, const StageMonitorBinding& binding,
                                const double* samples, UINT32 frames,
                                UINT32 channels, float inputPeak,
                                float inputRms, float outputPeak,
                                float outputRms, float engagement,
                                float dynamicGainDb, const TunerMonitorData& tuner)
{
    if (m_view == nullptr || slot >= static_cast<size_t>(kStageMonitorSlots)) {
        return;
    }

    m_observed[slot] = true;
    if (binding.spectrum && samples != nullptr && frames > 0 && channels > 0) {
        AnalyzeSpectrum(slot, samples, frames, channels);
    }

    const float seconds =
        m_view->sampleRate > 0
            ? static_cast<float>(frames) / static_cast<float>(m_view->sampleRate)
            : 0.0f;
    WriteSlot(slot, binding, inputPeak, inputRms, outputPeak, outputRms,
              engagement, dynamicGainDb, tuner,
              std::pow(0.5f, seconds / kShortPeakHalfLifeSeconds),
              std::pow(0.5f, seconds / kPeakHalfLifeSeconds));
}

void StageMonitor::EndBuffer()
{
    if (m_view == nullptr) {
        return;
    }
    for (int slot = 0; slot < kStageMonitorSlots; ++slot) {
        if (!m_observed[static_cast<size_t>(slot)]) {
            ClearSlot(static_cast<size_t>(slot));
        }
    }
}

void StageMonitor::AnalyzeSpectrum(size_t slot, const double* samples,
                                   UINT32 frames, UINT32 channels)
{
    StageMonitorImpl& impl = *m_impl;
    auto& history = impl.history[slot];
    int& write = impl.historyWrite[slot];
    int& hop = impl.hopCounter[slot];
    const int hopSize = kStageMonitorFftSize / 2;

    for (UINT32 frame = 0; frame < frames; ++frame) {
        double value = 0.0;
        const double* frameSamples =
            samples + static_cast<size_t>(frame) * channels;
        for (UINT32 channel = 0; channel < channels; ++channel) {
            value += frameSamples[channel];
        }
        value /= static_cast<double>(channels);

        history[static_cast<size_t>(write)] = value;
        write = (write + 1) % kStageMonitorFftSize;
        if (++hop < hopSize) {
            continue;
        }
        hop = 0;

        for (int i = 0; i < kStageMonitorFftSize; ++i) {
            const size_t index =
                static_cast<size_t>((write + i) % kStageMonitorFftSize);
            impl.real[static_cast<size_t>(i)] =
                history[index] * impl.window[static_cast<size_t>(i)];
            impl.imag[static_cast<size_t>(i)] = 0.0;
        }
        impl.fft.Forward(impl.real.data(), impl.imag.data());

        auto& smoothed = impl.spectrum[slot];
        for (int bin = 0; bin < kStageMonitorBins; ++bin) {
            const double magnitude =
                std::sqrt(impl.real[static_cast<size_t>(bin)] *
                              impl.real[static_cast<size_t>(bin)] +
                          impl.imag[static_cast<size_t>(bin)] *
                              impl.imag[static_cast<size_t>(bin)]) *
                4.0 / static_cast<double>(kStageMonitorFftSize);
            smoothed[static_cast<size_t>(bin)] +=
                (magnitude - smoothed[static_cast<size_t>(bin)]) *
                kSpectrumSmoothing;
        }
    }
}

void StageMonitor::WriteSlot(size_t slot, const StageMonitorBinding& binding,
                             float inputPeak, float inputRms, float outputPeak,
                             float outputRms, float engagement,
                             float dynamicGainDb, const TunerMonitorData& tuner, float shortDecay,
                             float peakDecay)
{
    StageMonitorSlot& target = m_view->slots[slot];
    InterlockedExchange(&target.sequence, 1);
    std::atomic_thread_fence(std::memory_order_release);

    target.effectId = binding.effectId;
    target.type = static_cast<LONG>(binding.type);
    target.flags = kStageMonitorFlagObserved |
                   (tuner.valid ? kStageMonitorFlagTunerValid : 0u);
    target.inputPeak = inputPeak;
    target.inputRms = inputRms;
    target.outputPeak = outputPeak;
    target.outputRms = outputRms;

    // Accumulate the maximum across buffers, not just the latest one: a short
    // transient can happen between two GUI polls and would otherwise be lost.
    float& inputPeakShort = m_impl->inputPeakShortHold[slot];
    inputPeakShort = std::max(inputPeak, inputPeakShort * shortDecay);
    target.inputPeakShortHold = inputPeakShort;
    float& inputRmsShort = m_impl->inputRmsShortHold[slot];
    inputRmsShort = std::max(inputRms, inputRmsShort * shortDecay);
    target.inputRmsShortHold = inputRmsShort;
    float& outputRmsShort = m_impl->outputRmsShortHold[slot];
    outputRmsShort = std::max(outputRms, outputRmsShort * shortDecay);
    target.outputRmsShortHold = outputRmsShort;
    float& outputPeakShort = m_impl->outputPeakShortHold[slot];
    outputPeakShort = std::max(outputPeak, outputPeakShort * shortDecay);
    target.outputPeakShortHold = outputPeakShort;
    float& peakHold = m_impl->peakHold[slot];
    peakHold = std::max(outputPeak, peakHold * peakDecay);
    target.outputPeakHold = peakHold;

    target.engagement = engagement;
    target.dynamicGainDb = dynamicGainDb;
    target.tunerFrequencyHz = tuner.frequencyHz;
    target.tunerConfidence = tuner.confidence;

    if (binding.spectrum) {
        const auto& bins = m_impl->spectrum[slot];
        for (int bin = 0; bin < kStageMonitorBins; ++bin) {
            target.spectrum[static_cast<size_t>(bin)] =
                static_cast<float>(bins[static_cast<size_t>(bin)]);
        }
    } else {
        std::memset(target.spectrum, 0, sizeof(target.spectrum));
    }

    std::atomic_thread_fence(std::memory_order_release);
    InterlockedExchange(&target.sequence, 2);
}

void StageMonitor::ClearSlot(size_t slot)
{
    StageMonitorSlot& target = m_view->slots[slot];
    InterlockedExchange(&target.sequence, 1);
    std::atomic_thread_fence(std::memory_order_release);

    target.flags = 0;
    target.inputPeak = 0.0f;
    target.inputRms = 0.0f;
    target.outputPeak = 0.0f;
    target.outputRms = 0.0f;
    target.inputPeakShortHold = 0.0f;
    target.inputRmsShortHold = 0.0f;
    target.outputRmsShortHold = 0.0f;
    target.outputPeakShortHold = 0.0f;
    target.outputPeakHold = 0.0f;
    target.engagement = 0.0f;
    target.dynamicGainDb = 0.0f;
    target.tunerFrequencyHz = 0.0f;
    target.tunerConfidence = 0.0f;
    std::memset(target.spectrum, 0, sizeof(target.spectrum));

    auto& bins = m_impl->spectrum[slot];
    bins.fill(0.0);
    m_impl->hopCounter[slot] = 0;
    m_impl->inputPeakShortHold[slot] = 0.0f;
    m_impl->inputRmsShortHold[slot] = 0.0f;
    m_impl->outputRmsShortHold[slot] = 0.0f;
    m_impl->outputPeakShortHold[slot] = 0.0f;
    m_impl->peakHold[slot] = 0.0f;

    std::atomic_thread_fence(std::memory_order_release);
    InterlockedExchange(&target.sequence, 2);
}

} // namespace apo
