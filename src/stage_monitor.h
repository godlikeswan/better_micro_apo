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
inline constexpr std::uint32_t kStageMonitorVersion = 12u;
// Post-DSP audio tap used by Listen and noise capture. It keeps audition out of
// the capture endpoint's WASAPI client list, which otherwise reloads the APO
// chain on some devices. 32768 stereo frames is about 0.68 s at 48 kHz.
inline constexpr std::uint32_t kStageMonitorAudioFrames = 32768u;
inline constexpr std::uint32_t kStageMonitorAudioChannels = 2u;
inline constexpr int kStageMonitorFftSize = 256;
inline constexpr int kStageMonitorBins = kStageMonitorFftSize / 2;
// One spare slot covers the temporary capture tap, another covers future chain
// padding without changing the shared layout.
inline constexpr int kStageMonitorSlots = apo::kMaxEffects + 2;
inline constexpr ULONG kStageMonitorFlagObserved = 1u;
inline constexpr ULONG kStageMonitorFlagTunerValid = 2u;

// Pitch reported by a Tuner stage. The GUI derives note name and cents from
// `frequencyHz` using the effect's Reference setting, so only the measurement
// itself crosses the shared-memory boundary.
struct TunerMonitorData
{
    float frequencyHz = 0.0f;
    float confidence = 0.0f;
    bool valid = false;
};

// Per-track transport state published by a Looper stage. `states` holds
// apo::LooperState values as integers so the shared layout stays plain data.
struct LooperMonitorData
{
    int states[apo::kLooperTrackCount]{};
};

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
                              float engagement, float dynamicGainDb,
                              const TunerMonitorData& tuner,
                              const LooperMonitorData& looper) = 0;
    // Any slot not observed during a buffer is cleared here, so silence and
    // disabled effects never leave stale meter values on screen.
    virtual void EndBuffer() = 0;
    // Copies the final processed buffer into the audition ring. This runs on the
    // audio thread and must never allocate or take a lock.
    virtual void WriteAudio(const double* samples, UINT32 frames,
                            UINT32 channels) = 0;
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
    // Tuner measurement; validity is also flagged in `flags`.
    float tunerFrequencyHz = 0.0f;
    float tunerConfidence = 0.0f;
    int looperStates[apo::kLooperTrackCount]{};
    float spectrum[kStageMonitorBins]{};
};

struct StageMonitorFile
{
    ULONG magic = 0;
    ULONG version = 0;
    ULONG structSize = 0;
    ULONG sampleRate = 0;
    ULONG slotCount = 0;
    // Incremented by the APO every time it opens the monitor. The GUI uses it
    // to detect an audio restart and discard optimistic Looper state.
    ULONG generation = 0;
    // Post-DSP audio ring. `audioWriteFrames` counts frames written since the
    // stream started; readers keep their own cursor so multiple GUI views can
    // tap the same stream without interfering.
    volatile LONG audioWriteFrames = 0;
    ULONG audioCapacityFrames = 0;
    ULONG audioSampleRate = 0;
    ULONG audioChannels = 0;
    float audioSamples[kStageMonitorAudioFrames *
                       kStageMonitorAudioChannels]{};
    StageMonitorSlot slots[kStageMonitorSlots];
};

struct StageMonitorSnapshot
{
    bool valid = false;
    ULONG flags = 0;
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
    // Tuner measurement; validity is also flagged in `flags`.
    float tunerFrequencyHz = 0.0f;
    float tunerConfidence = 0.0f;
    int looperStates[apo::kLooperTrackCount]{};
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

    // Reads interleaved stereo floats from the post-DSP audition ring. Starts
    // at the live edge when the view opens, so starting Listen does not replay
    // old audio. Returns false if the APO has not published an audio format.
    bool AudioFormat(ULONG& sampleRate, ULONG& channels) const;
    // Frames currently queued in the audition ring, capped at its capacity.
    // Used only for latency diagnostics; it does not move the read cursor.
    ULONG AudioAvailableFrames() const;
    bool ReadAudio(float* frames, ULONG maxFrames, ULONG& framesRead);
    // Skips to the live write edge after the APO publishes a new generation.
    void RebaseAudioRead();

    const std::wstring& EndpointId() const { return m_endpointId; }
    ULONG Generation() const { return m_view != nullptr ? m_view->generation : 0; }

private:
    std::wstring m_endpointId;
    HANDLE m_file = nullptr;
    HANDLE m_mapping = nullptr;
    StageMonitorFile* m_view = nullptr;
    ULONG m_audioReadFrames = 0;
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

    bool Open(const std::wstring& endpointId, UINT32 sampleRate,
              UINT32 channels);
    void Close();

    void BeginBuffer() override;
    void ObserveStage(size_t slot, const StageMonitorBinding& binding,
                      const double* samples, UINT32 frames, UINT32 channels,
                      float inputPeak, float inputRms, float outputPeak,
                      float outputRms, float engagement, float dynamicGainDb,
                      const TunerMonitorData& tuner,
                      const LooperMonitorData& looper) override;
    void EndBuffer() override;
    void WriteAudio(const double* samples, UINT32 frames,
                    UINT32 channels) override;

private:
    void AnalyzeSpectrum(size_t slot, const double* samples, UINT32 frames,
                         UINT32 channels);
    void WriteSlot(size_t slot, const StageMonitorBinding& binding,
                   float inputPeak, float inputRms, float outputPeak,
                   float outputRms, float engagement, float dynamicGainDb,
                   const TunerMonitorData& tuner,
                   const LooperMonitorData& looper, float shortDecay,
                   float peakDecay);
    void ClearSlot(size_t slot);

    HANDLE m_file = nullptr;
    HANDLE m_mapping = nullptr;
    StageMonitorFile* m_view = nullptr;
    std::array<bool, kStageMonitorSlots> m_observed{};
    std::unique_ptr<StageMonitorImpl> m_impl;
};

} // namespace apo
