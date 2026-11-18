#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "apo_settings.h"
#include "transfer.h"

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

// One-pole smoothing coefficient for a time constant in milliseconds.
inline double TimeCoefficient(double milliseconds, double sampleRate)
{
    const double seconds = std::max(milliseconds, 0.001) * 0.001;
    return 1.0 - std::exp(-1.0 / (seconds * sampleRate));
}

// Noise reduction offers several analysis sizes. Buffers are allocated for the
// largest one at bind time; the active size is retuned from the effect settings
// without allocating.
constexpr size_t kNoiseMaxBins =
    static_cast<size_t>(apo::kNoiseFftSizes.back()) / 2 + 1;

// Precomputed radix-2 FFT. Construction allocates; the transforms themselves
// are allocation-free and safe to call from the real-time path.
class FftPlan
{
public:
    explicit FftPlan(size_t size);

    size_t Size() const { return m_size; }

    // In-place transforms. `real` and `imag` must hold Size() values.
    void Forward(double* real, double* imag) const;
    void Inverse(double* real, double* imag) const;

private:
    void Transform(double* real, double* imag, bool inverse) const;

    size_t m_size = 0;
    std::vector<size_t> m_reverse;
    std::vector<double> m_cos;
    std::vector<double> m_sin;
};

// Average magnitude spectrum (bins 0..fftSize/2) of interleaved float
// samples, measured with the same window and hop as the noise stage. Returns
// an empty vector for invalid input.
std::vector<double> AverageMagnitudeSpectrum(const float* samples,
                                             size_t frames, UINT32 channels,
                                             int fftSize);

class Stage
{
public:
    virtual ~Stage() = default;

    // Called from LockForProcess only. Implementations precompute coefficients
    // and reset their state.
    virtual void Configure(const AudioFormat& format) { (void)format; }

    // Retunes the stage from one effect instance without allocating. Called
    // from the real-time path when the GUI publishes a new generation.
    virtual void Update(const EffectInstance& effect, const AudioFormat& format)
    {
        (void)effect;
        (void)format;
    }

    // Clears internal state without touching the configuration.
    virtual void Reset() {}

    // True when the stage buffers audio across calls, so the chain must still
    // be run for silent input buffers to flush the buffered tail.
    virtual bool NeedsSilence() const { return false; }

    // Processes `frames` frames in place. `samples` holds
    // frames * channels interleaved values in [-1, 1]. Real-time safe.
    virtual void Process(double* samples, UINT32 frames, UINT32 channels) = 0;
};

// An ordered list of stages. The chain only points at stages owned by
// StagePool, so rebuilding it never allocates or frees memory on the real-time
// path. Capacity is reserved for the largest effect list.
class Chain
{
public:
    Chain() { m_stages.reserve(kMaxEffects + 1); }

    void Add(Stage* stage);
    // Inserts at `index`, clamped to the current end. Used by the temporary
    // stage-local capture tap; the extra slot is reserved up front.
    void Insert(size_t index, Stage* stage);
    void Clear();
    void SetFormat(const AudioFormat& format) { m_format = format; }
    void Reset();
    bool NeedsSilence() const;
    void Process(const BYTE* source, BYTE* destination, UINT32 frames,
                 double* scratch) const;
    void ProcessSilence(BYTE* destination, UINT32 frames,
                        double* scratch) const;

    bool Empty() const { return m_stages.empty(); }

private:
    AudioFormat m_format;
    std::vector<Stage*> m_stages;
};

} // namespace apo::dsp
