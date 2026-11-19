#pragma once

#include <cmath>

namespace apo::pitch {

// MIDI note numbers use the usual A4 = 69 convention, so C0 is 12 and C8 is 96.
inline constexpr double kMidiA4 = 69.0;
inline constexpr double kMidiC0 = 12.0;
inline constexpr double kMidiC8 = 96.0;

inline double MidiToFrequency(double midi, double referenceHz)
{
    if (!std::isfinite(midi) || !std::isfinite(referenceHz) ||
        referenceHz <= 0.0) {
        return 0.0;
    }
    return referenceHz * std::pow(2.0, (midi - kMidiA4) / 12.0);
}

inline double FrequencyToMidi(double frequencyHz, double referenceHz)
{
    if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0 ||
        !std::isfinite(referenceHz) || referenceHz <= 0.0) {
        return kMidiA4;
    }
    return kMidiA4 + 12.0 * std::log2(frequencyHz / referenceHz);
}

inline int NearestMidiNote(double midi)
{
    return static_cast<int>(std::lround(midi));
}

inline double CentsFromNearest(double midi)
{
    return (midi - static_cast<double>(NearestMidiNote(midi))) * 100.0;
}

inline const wchar_t* NoteName(int midi)
{
    static const wchar_t* names[12] = {
        L"C", L"C#", L"D", L"D#", L"E", L"F",
        L"F#", L"G", L"G#", L"A", L"A#", L"B",
    };
    int index = midi % 12;
    if (index < 0) {
        index += 12;
    }
    return names[index];
}

inline int OctaveOf(int midi)
{
    return midi / 12 - 1;
}

} // namespace apo::pitch
