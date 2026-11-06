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
