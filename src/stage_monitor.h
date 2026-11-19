#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "apo_settings.h"

namespace apo {

// Continuous APO -> GUI telemetry. The APO owns the writer and publishes one
// fixed-size file per endpoint under runtime\{id}.monitor. The GUI maps it
// read-only and polls the selected effect's slot from a timer. This channel is
// deliberately separate from settings and from the one-shot noise capture, so
// sampling the meters never changes settings, rebuilds the chain, or stops
// audio.
inline constexpr std::uint32_t kStageMonitorMagic = 0x544F4D42u; // "BMOT"
inline constexpr std::uint32_t kStageMonitorVersion = 7u;
inline constexpr int kStageMonitorFftSize = 256;
inline constexpr int kStageMonitorBins = kStageMonitorFftSize / 2;
// One spare slot covers the temporary capture tap, another covers future chain
// padding without changing the shared layout.
inline constexpr int kStageMonitorSlots = apo::kMaxEffects + 2;
inline constexpr ULONG kStageMonitorFlagObserved = 1u;

// Identifies the effect currently occupying one chain slot. `slot` is the
// position in Chain, not a persistent id: a rebuild can move effects. The GUI
// looks slots up by effectId, which is stable for the active preset.
struct StageMonitorBinding
{
    int effectId = 0;
    apo::EffectType type = apo::EffectType::Count;
    bool spectrum = false;
};

// Allocation-free sink called from Chain::Process. Metrics are computed by the
// chain from the scratch buffer before and after each stage; the sink only
// observes them and publishes its own smoothed spectrum state.
class StageMonitorSink
{
public:
    virtual ~StageMonitorSink() = default;

    virtual void BeginBuffer() = 0;
    virtual void ObserveStage(size_t slot, const StageMonitorBinding& binding,
                              const double* samples, UINT32 frames,
                              UINT32 channels, float inputPeak, float inputRms,
                              float outputPeak, float outputRms,
                              float engagement, float dynamicGainDb) = 0;
    // Any slot not observed during a buffer is cleared here, so silence and
    // disabled effects never leave stale meter values on screen.
    virtual void EndBuffer() = 0;
};

struct StageMonitorSlot
{
    volatile LONG sequence = 0;
    LONG effectId = 0;
    LONG type = 0;
    ULONG flags = 0;
    float inputPeak = 0.0f;
    float inputRms = 0.0f;
    float outputPeak = 0.0f;
    float outputRms = 0.0f;
    // Cross-buffer short holds keep live overlays from missing transients
    // between GUI polls; the longer peak hold remains as the meter line.
    float inputPeakShortHold = 0.0f;
    float inputRmsShortHold = 0.0f;
    float outputRmsShortHold = 0.0f;
    float outputPeakShortHold = 0.0f;
    float outputPeakHold = 0.0f;
    float engagement = 0.0f;
    // Signed dynamic gain in dB for the Equalizer's live response overlay.
    float dynamicGainDb = 0.0f;
    float spectrum[kStageMonitorBins]{};
};

struct StageMonitorFile
{
    ULONG magic = 0;
    ULONG version = 0;
    ULONG structSize = 0;
    ULONG sampleRate = 0;
    ULONG slotCount = 0;
    ULONG reserved = 0;
    StageMonitorSlot slots[kStageMonitorSlots];
};

struct StageMonitorSnapshot
{
    bool valid = false;
    int effectId = 0;
    apo::EffectType type = apo::EffectType::Count;
    ULONG sampleRate = 0;
    float inputPeak = 0.0f;
    float inputRms = 0.0f;
    float outputPeak = 0.0f;
    float outputRms = 0.0f;
    float inputPeakShortHold = 0.0f;
    float inputRmsShortHold = 0.0f;
    float outputRmsShortHold = 0.0f;
    float outputPeakShortHold = 0.0f;
    float outputPeakHold = 0.0f;
    float engagement = 0.0f;
    // Signed dynamic gain in dB for the Equalizer's live response overlay.
    float dynamicGainDb = 0.0f;
    float spectrum[kStageMonitorBins]{};
};

// Read-only mapping used by the GUI. Opening is cheap but not free, so callers
// keep one view open for the selected endpoint.
class StageMonitorView
{
public:
    StageMonitorView() = default;
    ~StageMonitorView() { Close(); }

    StageMonitorView(const StageMonitorView&) = delete;
    StageMonitorView& operator=(const StageMonitorView&) = delete;

    bool Open(const std::wstring& endpointId, bool create);
    void Close();

    bool Read(int slot, StageMonitorSnapshot& snapshot) const;
    bool Find(int effectId, apo::EffectType type,
              StageMonitorSnapshot& snapshot) const;

    const std::wstring& EndpointId() const { return m_endpointId; }

private:
    std::wstring m_endpointId;
    HANDLE m_file = nullptr;
    HANDLE m_mapping = nullptr;
    const StageMonitorFile* m_view = nullptr;
};

std::wstring StageMonitorPath(const std::wstring& endpointId);

struct StageMonitorImpl;

// APO-side writer. Open/Close run on setup paths; BeginBuffer, ObserveStage and
// EndBuffer run from APOProcess and never allocate or take a lock.
class StageMonitor final : public StageMonitorSink
{
public:
    StageMonitor();
    ~StageMonitor() override;

    StageMonitor(const StageMonitor&) = delete;
    StageMonitor& operator=(const StageMonitor&) = delete;

    bool Open(const std::wstring& endpointId, UINT32 sampleRate);
    void Close();

    void BeginBuffer() override;
    void ObserveStage(size_t slot, const StageMonitorBinding& binding,
                      const double* samples, UINT32 frames, UINT32 channels,
                      float inputPeak, float inputRms, float outputPeak,
                      float outputRms, float engagement,
                      float dynamicGainDb) override;
    void EndBuffer() override;

private:
    void AnalyzeSpectrum(size_t slot, const double* samples, UINT32 frames,
                         UINT32 channels);
    void WriteSlot(size_t slot, const StageMonitorBinding& binding,
                   float inputPeak, float inputRms, float outputPeak,
                   float outputRms, float engagement, float dynamicGainDb,
                   float shortDecay, float peakDecay);
    void ClearSlot(size_t slot);

    HANDLE m_file = nullptr;
    HANDLE m_mapping = nullptr;
    StageMonitorFile* m_view = nullptr;
    std::array<bool, kStageMonitorSlots> m_observed{};
    std::unique_ptr<StageMonitorImpl> m_impl;
};

} // namespace apo
