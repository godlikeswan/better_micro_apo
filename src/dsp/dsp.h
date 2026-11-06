#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

// Format conversion and the stage/chain framework used by the APO. Keeping
// this separate from the COM plumbing makes adding or reordering a DSP stage a
// matter of writing one Stage subclass and appending it in ConfigureDsp().
namespace apo::dsp {

enum class SampleFormat
{
    Unknown,
    Float32,
    Pcm16,
    Pcm24,
    Pcm32
};

const wchar_t* FormatName(SampleFormat format);

struct AudioFormat
{
    SampleFormat sample = SampleFormat::Unknown;
    UINT32 channels = 0;
    UINT32 sampleRate = 0;
    UINT32 bytesPerSample = 0;
    UINT32 maxFrames = 0;
};

// Converts one interleaved sample to and from the normalized [-1, 1] range.
double DecodeSample(SampleFormat format, const BYTE* buffer, size_t index);
void EncodeSample(SampleFormat format, BYTE* buffer, size_t index, double value);

inline double DecibelsToLinear(double decibels)
{
    return std::pow(10.0, decibels / 20.0);
}

// One-pole smoothing coefficient for a time constant in milliseconds.
inline double TimeCoefficient(double milliseconds, double sampleRate)
{
    const double seconds = std::max(milliseconds, 0.001) * 0.001;
    return 1.0 - std::exp(-1.0 / (seconds * sampleRate));
}

class Stage
{
public:
    virtual ~Stage() = default;

    // Called from LockForProcess only. Implementations precompute coefficients
    // and reset their state.
    virtual void Configure(const AudioFormat& format) { (void)format; }

    // Clears internal state without touching the configuration.
    virtual void Reset() {}

    // True when the stage buffers audio across calls, so the chain must still
    // be run for silent input buffers to flush the buffered tail.
    virtual bool NeedsSilence() const { return false; }

    // Processes `frames` frames in place. `samples` holds
    // frames * channels interleaved values in [-1, 1]. Real-time safe.
    virtual void Process(double* samples, UINT32 frames, UINT32 channels) = 0;
};

// An ordered list of stages. The chain owns the stages, so rebuilding it is a
// setup-time operation and never happens on the real-time path.
class Chain
{
public:
    void Add(std::unique_ptr<Stage> stage);
    void Clear();
    void Configure(const AudioFormat& format);
    void Reset();
    bool NeedsSilence() const;
    void Process(const BYTE* source, BYTE* destination, UINT32 frames,
                 double* scratch) const;
    void ProcessSilence(BYTE* destination, UINT32 frames,
                        double* scratch) const;

    bool Empty() const { return m_stages.empty(); }

private:
    AudioFormat m_format;
    std::vector<std::unique_ptr<Stage>> m_stages;
};

} // namespace apo::dsp
