#include "dsp/dsp.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace apo::dsp {

const wchar_t* FormatName(SampleFormat format)
{
    switch (format)
    {
    case SampleFormat::Float32: return L"float32";
    case SampleFormat::Pcm16:   return L"pcm16";
    case SampleFormat::Pcm24:   return L"pcm24";
    case SampleFormat::Pcm32:   return L"pcm32";
    default:                    return L"unknown";
    }
}

double DecodeSample(SampleFormat format, const BYTE* buffer, size_t index)
{
    switch (format)
    {
    case SampleFormat::Float32:
        return static_cast<double>(
            reinterpret_cast<const float*>(buffer)[index]);

    case SampleFormat::Pcm16:
        return static_cast<double>(
                   reinterpret_cast<const int16_t*>(buffer)[index]) /
               32768.0;

    case SampleFormat::Pcm24:
    {
        const BYTE* sample = buffer + index * 3;
        int32_t value = static_cast<int32_t>(sample[0]) |
                        (static_cast<int32_t>(sample[1]) << 8) |
                        (static_cast<int32_t>(sample[2]) << 16);
        if ((value & 0x00800000) != 0)
        {
            value |= ~0x00FFFFFF;
        }
        return static_cast<double>(value) / 8388608.0;
    }

    case SampleFormat::Pcm32:
        return static_cast<double>(
                   reinterpret_cast<const int32_t*>(buffer)[index]) /
               2147483648.0;

    case SampleFormat::Unknown:
    default:
        return 0.0;
    }
}

void EncodeSample(SampleFormat format, BYTE* buffer, size_t index,
                  double value)
{
    // Keep the output inside the nominal [-1, 1] range for every format.
    if (value > 1.0)
    {
        value = 1.0;
    }
    else if (value < -1.0)
    {
        value = -1.0;
    }

    switch (format)
    {
    case SampleFormat::Float32:
        reinterpret_cast<float*>(buffer)[index] = static_cast<float>(value);
        break;

    case SampleFormat::Pcm16:
    {
        long scaled = std::lrint(value * 32768.0);
        if (scaled > 32767)
        {
            scaled = 32767;
        }
        else if (scaled < -32768)
        {
            scaled = -32768;
        }
        reinterpret_cast<int16_t*>(buffer)[index] =
            static_cast<int16_t>(scaled);
        break;
    }

    case SampleFormat::Pcm24:
    {
        long scaled = std::lrint(value * 8388608.0);
        if (scaled > 8388607)
        {
            scaled = 8388607;
        }
        else if (scaled < -8388608)
        {
            scaled = -8388608;
        }

        BYTE* sample = buffer + index * 3;
        sample[0] = static_cast<BYTE>(scaled & 0xFF);
        sample[1] = static_cast<BYTE>((scaled >> 8) & 0xFF);
        sample[2] = static_cast<BYTE>((scaled >> 16) & 0xFF);
        break;
    }

    case SampleFormat::Pcm32:
    {
        long long scaled = std::llrint(value * 2147483648.0);
        if (scaled > 2147483647LL)
        {
            scaled = 2147483647LL;
        }
        else if (scaled < -2147483648LL)
        {
            scaled = -2147483648LL;
        }
        reinterpret_cast<int32_t*>(buffer)[index] =
            static_cast<int32_t>(scaled);
        break;
    }

    case SampleFormat::Unknown:
    default:
        break;
    }
}

FftPlan::FftPlan(size_t size) : m_size(size)
{
    if (size < 2)
    {
        m_size = 0;
        return;
    }

    size_t bits = 0;
    while ((size_t{1} << bits) < size)
    {
        ++bits;
    }
    if ((size_t{1} << bits) != size)
    {
        // Not a power of two; leave the plan empty rather than mis-transform.
        m_size = 0;
        return;
    }

    m_reverse.resize(size);
    for (size_t index = 0; index < size; ++index)
    {
        size_t reversed = 0;
        size_t value = index;
        for (size_t bit = 0; bit < bits; ++bit)
        {
            reversed = (reversed << 1) | (value & 1);
            value >>= 1;
        }
        m_reverse[index] = reversed;
    }

    constexpr double kPi = 3.14159265358979323846;
    m_cos.resize(size / 2);
    m_sin.resize(size / 2);
    for (size_t index = 0; index < size / 2; ++index)
    {
        const double angle =
            2.0 * kPi * static_cast<double>(index) / static_cast<double>(size);
        m_cos[index] = std::cos(angle);
        m_sin[index] = std::sin(angle);
    }
}

void FftPlan::Transform(double* real, double* imag, bool inverse) const
{
    if (m_size == 0)
    {
        return;
    }

    for (size_t index = 0; index < m_size; ++index)
    {
        const size_t reversed = m_reverse[index];
        if (reversed > index)
        {
            std::swap(real[index], real[reversed]);
            std::swap(imag[index], imag[reversed]);
        }
    }

    for (size_t length = 2; length <= m_size; length <<= 1)
    {
        const size_t half = length >> 1;
        const size_t step = m_size / length;
        for (size_t start = 0; start < m_size; start += length)
        {
            for (size_t offset = 0; offset < half; ++offset)
            {
                const size_t twiddle = offset * step;
                const double wr = m_cos[twiddle];
                const double wi = inverse ? m_sin[twiddle] : -m_sin[twiddle];
                const size_t a = start + offset;
                const size_t b = a + half;
                const double tr = real[b] * wr - imag[b] * wi;
                const double ti = real[b] * wi + imag[b] * wr;
                real[b] = real[a] - tr;
                imag[b] = imag[a] - ti;
                real[a] += tr;
                imag[a] += ti;
            }
        }
    }

    if (inverse)
    {
        const double scale = 1.0 / static_cast<double>(m_size);
        for (size_t index = 0; index < m_size; ++index)
        {
            real[index] *= scale;
            imag[index] *= scale;
        }
    }
}

void FftPlan::Forward(double* real, double* imag) const
{
    Transform(real, imag, false);
}

void FftPlan::Inverse(double* real, double* imag) const
{
    Transform(real, imag, true);
}

std::vector<double> AverageMagnitudeSpectrum(const float* samples,
                                             size_t frames, UINT32 channels)
{
    if (samples == nullptr || frames == 0 || channels == 0)
    {
        return {};
    }

    constexpr size_t kSize = kNoiseFftSize;
    constexpr size_t kHop = kNoiseHopSize;
    constexpr double kPi = 3.14159265358979323846;

    FftPlan fft(kSize);
    std::vector<double> window(kSize);
    for (size_t index = 0; index < kSize; ++index)
    {
        const double hann =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(index) /
                                 static_cast<double>(kSize));
        window[index] = std::sqrt(std::max(0.0, hann));
    }

    std::vector<double> real(kSize, 0.0);
    std::vector<double> imag(kSize, 0.0);
    std::vector<double> sum(kSize / 2 + 1, 0.0);
    size_t measured = 0;

    for (size_t start = 0; start < frames; start += kHop)
    {
        const size_t available = std::min(kSize, frames - start);
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            std::fill(real.begin(), real.end(), 0.0);
            std::fill(imag.begin(), imag.end(), 0.0);
            for (size_t index = 0; index < available; ++index)
            {
                real[index] =
                    static_cast<double>(
                        samples[(start + index) * channels + channel]) *
                    window[index];
            }

            fft.Forward(real.data(), imag.data());
            for (size_t bin = 0; bin <= kSize / 2; ++bin)
            {
                sum[bin] += std::sqrt(real[bin] * real[bin] +
                                      imag[bin] * imag[bin]);
            }
            ++measured;
        }
    }

    if (measured == 0)
    {
        return {};
    }

    for (double& value : sum)
    {
        value /= static_cast<double>(measured);
    }
    return sum;
}

void Chain::Add(std::unique_ptr<Stage> stage)
{
    if (stage)
    {
        m_stages.push_back(std::move(stage));
    }
}

void Chain::Clear()
{
    m_stages.clear();
    m_format = AudioFormat{};
}

void Chain::Configure(const AudioFormat& format)
{
    m_format = format;
    for (auto& stage : m_stages)
    {
        stage->Configure(m_format);
    }
}

void Chain::Reset()
{
    for (auto& stage : m_stages)
    {
        stage->Reset();
    }
}

bool Chain::NeedsSilence() const
{
    for (const auto& stage : m_stages)
    {
        if (stage->NeedsSilence())
        {
            return true;
        }
    }
    return false;
}

void Chain::Process(const BYTE* source, BYTE* destination, UINT32 frames,
                    double* scratch) const
{
    if (m_format.sample == SampleFormat::Unknown || m_format.channels == 0)
    {
        return;
    }

    const size_t count = static_cast<size_t>(frames) * m_format.channels;
    if (count == 0)
    {
        return;
    }

    if (m_stages.empty())
    {
        if (destination != source)
        {
            std::memcpy(destination, source,
                        count * m_format.bytesPerSample);
        }
        return;
    }

    for (size_t i = 0; i < count; ++i)
    {
        scratch[i] = DecodeSample(m_format.sample, source, i);
    }

    for (auto& stage : m_stages)
    {
        stage->Process(scratch, frames, m_format.channels);
    }

    for (size_t i = 0; i < count; ++i)
    {
        EncodeSample(m_format.sample, destination, i, scratch[i]);
    }
}

void Chain::ProcessSilence(BYTE* destination, UINT32 frames,
                           double* scratch) const
{
    if (destination == nullptr ||
        m_format.sample == SampleFormat::Unknown || m_format.channels == 0)
    {
        return;
    }

    const size_t count = static_cast<size_t>(frames) * m_format.channels;
    if (count == 0)
    {
        return;
    }

    if (m_stages.empty())
    {
        std::memset(destination, 0, count * m_format.bytesPerSample);
        return;
    }

    // Run zeros through the stages so buffered stages (such as a gate with
    // lookahead) flush their tail instead of replaying stale audio later.
    std::fill(scratch, scratch + count, 0.0);
    for (auto& stage : m_stages)
    {
        stage->Process(scratch, frames, m_format.channels);
    }

    for (size_t i = 0; i < count; ++i)
    {
        EncodeSample(m_format.sample, destination, i, scratch[i]);
    }
}

} // namespace apo::dsp
