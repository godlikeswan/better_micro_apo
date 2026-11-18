#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dsp/dsp.h"

namespace apo {

// Capture control block shared by the GUI and the running APO. The file is
// created by whichever side opens it first and lives under the runtime folder.
// Only 32-bit control fields are touched with Interlocked operations, so the
// real-time APO path never takes a lock.
//
// Request flow:
//   GUI writes target/duration, then Requested.
//   APO inserts a tap before the selected Noise stage and sets Capturing.
//   APO publishes magnitude bins and sets Done.
//   GUI saves the profile, writes resultAck, and waits for Idle.
//   APO updates the live profile, removes the tap, and sets Idle.
enum StageCaptureState : LONG
{
    StageCaptureIdle = 0,
    StageCaptureRequested = 1,
    StageCaptureCapturing = 2,
    StageCaptureDone = 3,
    StageCaptureFailed = 4,
};

inline constexpr std::uint32_t kStageCaptureMagic = 0x50414353u; // "SCAP"
inline constexpr std::uint32_t kStageCaptureVersion = 1u;
inline constexpr size_t kStageCaptureBins = apo::dsp::kNoiseFftSize / 2 + 1;

enum StageCaptureError : ULONG
{
    StageCaptureErrorNone = 0,
    StageCaptureErrorInvalidTarget = 1,
    StageCaptureErrorBadDuration = 2,
    StageCaptureErrorNotConfigured = 3,
};

inline constexpr LONG kStageCaptureAckNone = 0;
inline constexpr LONG kStageCaptureAckSaved = 1;
inline constexpr LONG kStageCaptureAckFailed = 2;

struct StageCaptureFile
{
    volatile LONG initialized = 0;
    ULONG magic = 0;
    ULONG version = 0;
    ULONG structSize = 0;

    volatile LONG state = StageCaptureIdle;
    volatile LONG requestId = 0;
    LONG presetIndex = -1;
    LONG effectId = -1;
    ULONG durationMs = 0;
    ULONG sampleRate = 0;
    ULONG channels = 0;
    ULONG framesTarget = 0;
    ULONG framesDone = 0;
    ULONG error = 0;
    volatile LONG resultAck = kStageCaptureAckNone;

    double bins[kStageCaptureBins]{};
};

// RAII handle for the file-backed mapping. The APO keeps one open for the life
// of a locked stream; the GUI opens it only while a capture is in flight.
class StageCaptureView
{
public:
    StageCaptureView() = default;
    ~StageCaptureView() { Close(); }

    StageCaptureView(const StageCaptureView&) = delete;
    StageCaptureView& operator=(const StageCaptureView&) = delete;

    bool Open(const std::wstring& endpointId, bool create);
    void Close();
    StageCaptureFile* Get() const
    {
        return static_cast<StageCaptureFile*>(m_view);
    }

private:
    HANDLE m_file = nullptr;
    HANDLE m_mapping = nullptr;
    void* m_view = nullptr;
};

std::wstring StageCapturePath(const std::wstring& endpointId);

// GUI side: ask the APO for a stage-local capture, save the returned bins to
// the endpoint's durable .noise profile, and acknowledge the result. No
// settings are changed and the audio service is left alone.
bool RequestStageCapture(const std::wstring& endpointId, int presetIndex,
                         int effectId, unsigned durationMs,
                         std::wstring& error);

// A stage inserted just before the selected Noise effect. It leaves the audio
// untouched while measuring the signal that reaches that effect. Configure()
// allocates its FFT state; Process() is allocation-free.
class StageCaptureTap final : public apo::dsp::Stage
{
public:
    void Configure(const apo::dsp::AudioFormat& format) override;

    bool Begin(ULONG framesTarget);
    bool Complete() const { return m_complete; }
    ULONG FramesDone() const { return m_framesDone; }
    void Finish(double* bins, size_t count) const;

    void Reset() override;
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    void AnalyzeHop();

    apo::dsp::FftPlan m_fft{apo::dsp::kNoiseFftSize};
    std::vector<double> m_window;
    std::vector<double> m_history;
    std::vector<double> m_frameReal;
    std::vector<double> m_frameImag;
    std::vector<double> m_sum;

    UINT32 m_channels = 0;
    ULONG m_framesTarget = 0;
    ULONG m_framesDone = 0;
    size_t m_cursor = 0;
    size_t m_measured = 0;
    bool m_active = false;
    bool m_complete = false;
};

} // namespace apo
