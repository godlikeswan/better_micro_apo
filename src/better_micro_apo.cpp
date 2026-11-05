// Better Micro APO - a minimal Windows capture APO.
//
// DSP chain: input gain -> linked peak compressor -> makeup gain -> clip ->
// optional mono copy (left channel to all channels).
//
// COM plumbing follows Microsoft's SwapAPO sample: ATL owns the class factory,
// the object map and the aggregation rules. Only the DSP and the registry
// writes are ours.

#include <atlbase.h>
#include <atlcom.h>

#include "better_micro_apo.h"
#include "audio_format_ids.h"

#include <mmdeviceapi.h>
#include <objbase.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>

namespace
{
    HMODULE g_module = nullptr;

    constexpr wchar_t kLogDirectory[] = L"C:\\ProgramData\\better_micro_apo";
    constexpr wchar_t kLogPath[] = L"C:\\ProgramData\\better_micro_apo\\better_micro_apo.log";

    // Same flags Equalizer APO uses for its in-place system-effect APOs.
    // Channel count is not required to match because the DSP is per-sample.
    constexpr APO_FLAG kApoFlags = static_cast<APO_FLAG>(
        APO_FLAG_INPLACE |
        APO_FLAG_FRAMESPERSECOND_MUST_MATCH |
        APO_FLAG_BITSPERSAMPLE_MUST_MATCH);

    // Only called from setup paths (Initialize / LockForProcess / registration),
    // never from APOProcess, so file I/O is acceptable here.
    void Log(const wchar_t* format, ...)
    {
        wchar_t message[1024]{};
        va_list args;
        va_start(args, format);
        vswprintf_s(message, std::size(message), format, args);
        va_end(args);

        SYSTEMTIME now{};
        GetLocalTime(&now);

        wchar_t line[1200]{};
        swprintf_s(line, std::size(line),
                   L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\n",
                   now.wYear, now.wMonth, now.wDay,
                   now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                   message);

        OutputDebugStringW(L"[Better Micro APO] ");
        OutputDebugStringW(line);

        CreateDirectoryW(kLogDirectory, nullptr);

        HANDLE file = CreateFileW(kLogPath, FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }

        char utf8[2400]{};
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8,
                                              static_cast<int>(sizeof(utf8)),
                                              nullptr, nullptr);
        if (bytes > 1)
        {
            DWORD written = 0;
            WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
        }
        CloseHandle(file);
    }

    bool GuidToString(REFGUID guid, wchar_t (&text)[64])
    {
        return StringFromGUID2(guid, text, static_cast<int>(std::size(text))) > 0;
    }

    // IMMDevice::GetId() returns "{0.0.0.00000000}.{endpoint-guid}". The last
    // GUID is the same key the GUI uses under MMDevices\Audio\Capture.
    std::wstring EndpointIdFromDeviceId(const wchar_t* deviceId)
    {
        if (deviceId == nullptr)
        {
            return {};
        }

        const wchar_t* brace = wcsrchr(deviceId, L'{');
        return (brace != nullptr) ? std::wstring(brace) : std::wstring(deviceId);
    }

    std::wstring EndpointIdFromInitialize(UINT32 cbDataSize, BYTE* pbyData)
    {
        // APOInitSystemEffects and APOInitSystemEffects2 share this prefix:
        // pDeviceCollection sits at the same offset in both. Accept either
        // size instead of assuming the newer structure.
        if (pbyData == nullptr || cbDataSize < sizeof(APOInitSystemEffects))
        {
            return {};
        }

        const auto* init =
            reinterpret_cast<const APOInitSystemEffects*>(pbyData);
        if (init->APOInit.cbSize < sizeof(APOInitSystemEffects) ||
            init->pDeviceCollection == nullptr)
        {
            return {};
        }

        // The last item in the collection is always the audio endpoint.
        UINT count = 0;
        if (FAILED(init->pDeviceCollection->GetCount(&count)) || count == 0)
        {
            return {};
        }

        CComPtr<IMMDevice> device;
        if (FAILED(init->pDeviceCollection->Item(count - 1, &device)) ||
            device == nullptr)
        {
            return {};
        }

        LPWSTR deviceId = nullptr;
        if (FAILED(device->GetId(&deviceId)) || deviceId == nullptr)
        {
            return {};
        }

        std::wstring endpointId = EndpointIdFromDeviceId(deviceId);
        CoTaskMemFree(deviceId);
        return endpointId;
    }
}

// ---------------------------------------------------------------------------
// BetterMicroApo
// ---------------------------------------------------------------------------

#pragma warning(push)
#pragma warning(disable : 4815) // zero-sized array in CRegAPOProperties (same as SwapAPO)
const CRegAPOProperties<1> BetterMicroApo::sm_RegProperties(
    CLSID_BetterMicroApo,
    L"Better Micro APO",
    L"Better Micro APO",
    1,
    0,
    __uuidof(IAudioProcessingObject),
    kApoFlags);
#pragma warning(pop)

HRESULT BetterMicroApo::FinalConstruct()
{
    Log(L"FinalConstruct");
    return S_OK;
}

void BetterMicroApo::FinalRelease()
{
    Log(L"FinalRelease");
}

STDMETHODIMP BetterMicroApo::Reset()
{
    const HRESULT hr = CBaseAudioProcessingObject::Reset();
    m_envelopeDb = 0.0;
    return hr;
}

STDMETHODIMP BetterMicroApo::Initialize(UINT32 cbDataSize, BYTE* pbyData)
{
    // The base class validates the initialization structure and sets the
    // initialized flag the engine relies on.
    const HRESULT hr = CBaseAudioProcessingObject::Initialize(cbDataSize, pbyData);

    m_endpointId.clear();
    if (SUCCEEDED(hr))
    {
        m_endpointId = EndpointIdFromInitialize(cbDataSize, pbyData);
    }

    wchar_t clsidText[64] = L"<none>";
    if (pbyData != nullptr && cbDataSize >= sizeof(APOInitBaseStruct))
    {
        const auto* base = reinterpret_cast<const APOInitBaseStruct*>(pbyData);
        GuidToString(base->clsid, clsidText);
    }

    Log(L"Initialize hr=0x%08X cbDataSize=%u clsid=%s endpoint=%s", hr,
        cbDataSize, clsidText,
        m_endpointId.empty() ? L"<unknown>" : m_endpointId.c_str());
    return hr;
}

bool BetterMicroApo::IsSupportedFormat(const UNCOMPRESSEDAUDIOFORMAT& format)
{
    if (format.dwSamplesPerFrame < AUDIO_MIN_CHANNELS ||
        format.dwSamplesPerFrame > AUDIO_MAX_CHANNELS)
    {
        return false;
    }

    if (!(format.fFramesPerSecond >= AUDIO_MIN_FRAMERATE &&
          format.fFramesPerSecond <= AUDIO_MAX_FRAMERATE))
    {
        return false;
    }

    if (IsEqualGUID(format.guidFormatType, SUBTYPE_IEEE_FLOAT))
    {
        return format.dwBytesPerSampleContainer == 4 &&
               format.dwValidBitsPerSample == 32;
    }

    if (IsEqualGUID(format.guidFormatType, SUBTYPE_PCM))
    {
        // Only accept containers whose valid bits fill the whole container.
        // 24-in-32 is a possible engine format and can be added later.
        if (format.dwValidBitsPerSample != format.dwBytesPerSampleContainer * 8)
        {
            return false;
        }

        return format.dwBytesPerSampleContainer == 2 ||
               format.dwBytesPerSampleContainer == 3 ||
               format.dwBytesPerSampleContainer == 4;
    }

    return false;
}

const wchar_t* BetterMicroApo::FormatName(SampleFormat format)
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

STDMETHODIMP BetterMicroApo::LockForProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_DESCRIPTOR** ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_DESCRIPTOR** ppOutputConnections)
{
    // The base class validates connection counts, formats and registration
    // flags, caches the format information and sets m_bIsLocked.
    HRESULT hr = CBaseAudioProcessingObject::LockForProcess(
        u32NumInputConnections, ppInputConnections,
        u32NumOutputConnections, ppOutputConnections);
    if (FAILED(hr))
    {
        Log(L"LockForProcess failed: hr=0x%08X", hr);
        return hr;
    }

    UNCOMPRESSEDAUDIOFORMAT format{};
    hr = ppInputConnections[0]->pFormat->GetUncompressedAudioFormat(&format);
    if (FAILED(hr) || !IsSupportedFormat(format))
    {
        CBaseAudioProcessingObject::UnlockForProcess();
        Log(L"LockForProcess: unsupported format hr=0x%08X", hr);
        return APOERR_INVALID_CONNECTION_FORMAT;
    }

    m_channels = format.dwSamplesPerFrame;
    m_sampleRate = static_cast<UINT32>(format.fFramesPerSecond + 0.5f);
    m_bytesPerSample = format.dwBytesPerSampleContainer;

    if (IsEqualGUID(format.guidFormatType, SUBTYPE_IEEE_FLOAT))
    {
        m_format = SampleFormat::Float32;
    }
    else if (m_bytesPerSample == 2)
    {
        m_format = SampleFormat::Pcm16;
    }
    else if (m_bytesPerSample == 3)
    {
        m_format = SampleFormat::Pcm24;
    }
    else
    {
        m_format = SampleFormat::Pcm32;
    }

    m_settings = apo::ApoSettings{};
    if (!m_endpointId.empty())
    {
        apo::LoadSettings(m_endpointId, m_settings);
    }
    apo::ClampSettings(m_settings);

    ConfigureDsp();
    m_envelopeDb = 0.0;

    Log(L"LockForProcess ok: format=%s channels=%u rate=%u containerBytes=%u maxFrames=%u endpoint=%s inputGain=%.2fdB compressor=%d threshold=%.2fdB ratio=%.2f:1 attack=%.2fms release=%.2fms makeup=%.2fdB mono=%d",
        FormatName(m_format), m_channels, m_sampleRate, m_bytesPerSample,
        ppInputConnections[0]->u32MaxFrameCount,
        m_endpointId.empty() ? L"<unknown>" : m_endpointId.c_str(),
        m_settings.inputGainDb, m_settings.compressorEnabled ? 1 : 0,
        m_settings.thresholdDb, m_settings.ratio, m_settings.attackMs,
        m_settings.releaseMs, m_settings.makeupDb, m_settings.mono ? 1 : 0);
    return S_OK;
}

STDMETHODIMP BetterMicroApo::UnlockForProcess()
{
    const HRESULT hr = CBaseAudioProcessingObject::UnlockForProcess();

    m_format = SampleFormat::Unknown;
    m_channels = 0;
    m_sampleRate = 0;
    m_bytesPerSample = 0;
    m_envelopeDb = 0.0;

    Log(L"UnlockForProcess hr=0x%08X", hr);
    return hr;
}

STDMETHODIMP_(void) BetterMicroApo::APOProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_PROPERTY** ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_PROPERTY** ppOutputConnections)
{
    // Real-time path: no allocation, no locks, no logging.
    if (u32NumInputConnections != 1 || u32NumOutputConnections != 1 ||
        ppInputConnections == nullptr || ppOutputConnections == nullptr)
    {
        return;
    }

    APO_CONNECTION_PROPERTY* input = ppInputConnections[0];
    APO_CONNECTION_PROPERTY* output = ppOutputConnections[0];
    if (input == nullptr || output == nullptr)
    {
        return;
    }

    output->u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
    output->u32ValidFrameCount = input->u32ValidFrameCount;

    if (!m_bIsLocked || m_format == SampleFormat::Unknown ||
        input->u32BufferFlags == BUFFER_INVALID)
    {
        output->u32ValidFrameCount = 0;
        output->u32BufferFlags = BUFFER_INVALID;
        return;
    }

    BYTE* destination = reinterpret_cast<BYTE*>(output->pBuffer);
    const BYTE* source = reinterpret_cast<const BYTE*>(input->pBuffer);

    if (input->u32BufferFlags == BUFFER_SILENT)
    {
        output->u32BufferFlags = BUFFER_SILENT;

        // If the engine did not give us an in-place buffer, make sure the
        // silent output is actually zeroed.
        if (destination != nullptr && destination != source)
        {
            ZeroMemory(destination,
                       static_cast<size_t>(input->u32ValidFrameCount) *
                           m_channels * m_bytesPerSample);
        }
        return;
    }

    if (source == nullptr || destination == nullptr)
    {
        output->u32ValidFrameCount = 0;
        output->u32BufferFlags = BUFFER_INVALID;
        return;
    }

    ProcessBuffer(source, destination, input->u32ValidFrameCount);
    output->u32BufferFlags = BUFFER_VALID;
}

void BetterMicroApo::ConfigureDsp()
{
    m_inputGain = std::pow(10.0, m_settings.inputGainDb / 20.0);
    m_makeupGain = std::pow(10.0, m_settings.makeupDb / 20.0);
    m_thresholdLinear = std::pow(10.0, m_settings.thresholdDb / 20.0);

    const double sampleRate = (m_sampleRate > 0) ? static_cast<double>(m_sampleRate) : 48000.0;
    const double attackSeconds =
        (m_settings.attackMs > 0.0) ? (m_settings.attackMs * 0.001) : 0.001;
    const double releaseSeconds =
        (m_settings.releaseMs > 0.0) ? (m_settings.releaseMs * 0.001) : 0.001;

    m_attackCoeff = 1.0 - std::exp(-1.0 / (attackSeconds * sampleRate));
    m_releaseCoeff = 1.0 - std::exp(-1.0 / (releaseSeconds * sampleRate));
}

void BetterMicroApo::UpdateEnvelope(double peak)
{
    double targetDb = 0.0;

    if (peak > m_thresholdLinear)
    {
        const double levelDb = 20.0 * std::log10(peak);
        targetDb = (m_settings.thresholdDb - levelDb) *
                   (1.0 - 1.0 / m_settings.ratio);
        if (targetDb > 0.0)
        {
            targetDb = 0.0;
        }
    }

    // Attack when we need more reduction, release when we need less.
    const double coeff = (targetDb < m_envelopeDb) ? m_attackCoeff : m_releaseCoeff;
    m_envelopeDb += (targetDb - m_envelopeDb) * coeff;
}

double BetterMicroApo::ReadSample(const BYTE* buffer, size_t index) const
{
    switch (m_format)
    {
    case SampleFormat::Float32:
        return static_cast<double>(reinterpret_cast<const float*>(buffer)[index]);

    case SampleFormat::Pcm16:
        return static_cast<double>(reinterpret_cast<const int16_t*>(buffer)[index]) / 32768.0;

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
        return static_cast<double>(reinterpret_cast<const int32_t*>(buffer)[index]) / 2147483648.0;

    case SampleFormat::Unknown:
    default:
        return 0.0;
    }
}

void BetterMicroApo::WriteSample(BYTE* buffer, size_t index, double value) const
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

    switch (m_format)
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
        reinterpret_cast<int16_t*>(buffer)[index] = static_cast<int16_t>(scaled);
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
        reinterpret_cast<int32_t*>(buffer)[index] = static_cast<int32_t>(scaled);
        break;
    }

    case SampleFormat::Unknown:
    default:
        break;
    }
}

void BetterMicroApo::ProcessBuffer(const BYTE* source, BYTE* destination, UINT32 frames)
{
    const size_t channels = m_channels;

    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        const size_t base = static_cast<size_t>(frame) * channels;

        // First pass: find the peak of this frame after input gain. Using the
        // peak across all channels keeps the gain linked for stereo sources.
        double peak = 0.0;
        for (size_t channel = 0; channel < channels; ++channel)
        {
            const double sample = ReadSample(source, base + channel) * m_inputGain;
            const double magnitude = std::fabs(sample);
            if (magnitude > peak)
            {
                peak = magnitude;
            }
        }

        double compressorGain = 1.0;
        if (m_settings.compressorEnabled)
        {
            UpdateEnvelope(peak);
            compressorGain = std::pow(10.0, m_envelopeDb / 20.0);
        }

        const double totalGain = compressorGain * m_makeupGain;

        // Second pass: apply input gain, compressor gain and makeup gain.
        // Reading and writing the same index is safe for in-place buffers.
        for (size_t channel = 0; channel < channels; ++channel)
        {
            const double sample = ReadSample(source, base + channel) * m_inputGain;
            WriteSample(destination, base + channel, sample * totalGain);
        }
    }

    // Mono mode copies the processed left channel to every other channel, so a
    // mono source on input 1 is heard on both sides.
    if (m_settings.mono && channels > 1)
    {
        for (UINT32 frame = 0; frame < frames; ++frame)
        {
            const size_t base = static_cast<size_t>(frame) * channels;
            const BYTE* left = destination + base * m_bytesPerSample;
            for (size_t channel = 1; channel < channels; ++channel)
            {
                std::memcpy(destination + (base + channel) * m_bytesPerSample,
                            left, m_bytesPerSample);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ATL module: class factory, object map and lifetime
// ---------------------------------------------------------------------------

class BetterMicroApoModule : public CAtlDllModuleT<BetterMicroApoModule>
{
};

BetterMicroApoModule g_moduleObject;

OBJECT_ENTRY_AUTO(CLSID_BetterMicroApo, BetterMicroApo)

namespace
{

    HRESULT SetStringValue(HKEY key, const wchar_t* name, const wchar_t* value)
    {
        const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
        const LSTATUS status = RegSetValueExW(
            key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value), bytes);
        return (status == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(status);
    }

    HRESULT SetDwordValue(HKEY key, const wchar_t* name, DWORD value)
    {
        const LSTATUS status = RegSetValueExW(
            key, name, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value));
        return (status == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(status);
    }

    // Windows only considers an APO whose CLSID is listed here. RegisterAPO()
    // normally creates this key, but the SDK import library does not expose it,
    // so the same values are written directly.
    HRESULT RegisterApoProperties()
    {
        wchar_t clsidText[64]{};
        if (!GuidToString(CLSID_BetterMicroApo, clsidText))
        {
            return E_UNEXPECTED;
        }

        std::wstring keyPath =
            L"Software\\Classes\\AudioEngine\\AudioProcessingObjects\\";
        keyPath += clsidText;

        HKEY key = nullptr;
        const LSTATUS status = RegCreateKeyExW(
            HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
            nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS)
        {
            return HRESULT_FROM_WIN32(status);
        }

        HRESULT hr = SetStringValue(key, L"FriendlyName", L"Better Micro APO");
        if (SUCCEEDED(hr)) hr = SetStringValue(key, L"Copyright", L"Better Micro APO");
        if (SUCCEEDED(hr)) hr = SetDwordValue(key, L"MajorVersion", 1);
        if (SUCCEEDED(hr)) hr = SetDwordValue(key, L"MinorVersion", 0);
        if (SUCCEEDED(hr))
        {
            hr = SetDwordValue(key, L"Flags", static_cast<DWORD>(kApoFlags));
        }
        if (SUCCEEDED(hr)) hr = SetDwordValue(key, L"MinInputConnections", 1);
        if (SUCCEEDED(hr)) hr = SetDwordValue(key, L"MaxInputConnections", 1);
        if (SUCCEEDED(hr)) hr = SetDwordValue(key, L"MinOutputConnections", 1);
        if (SUCCEEDED(hr)) hr = SetDwordValue(key, L"MaxOutputConnections", 1);
        if (SUCCEEDED(hr)) hr = SetDwordValue(key, L"MaxInstances", 0xFFFFFFFFu);
        if (SUCCEEDED(hr)) hr = SetDwordValue(key, L"NumAPOInterfaces", 1);
        if (SUCCEEDED(hr))
        {
            wchar_t interfaceText[64]{};
            if (!GuidToString(__uuidof(IAudioProcessingObject), interfaceText))
            {
                hr = E_UNEXPECTED;
            }
            else
            {
                hr = SetStringValue(key, L"APOInterface0", interfaceText);
            }
        }

        RegCloseKey(key);
        return hr;
    }

    HRESULT RegisterComServer()
    {
        wchar_t modulePath[32768]{};
        const DWORD length = GetModuleFileNameW(
            g_module, modulePath, static_cast<DWORD>(std::size(modulePath)));
        if (length == 0 || length >= std::size(modulePath))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        wchar_t clsidText[64]{};
        if (!GuidToString(CLSID_BetterMicroApo, clsidText))
        {
            return E_UNEXPECTED;
        }

        std::wstring clsidKeyPath = L"Software\\Classes\\CLSID\\";
        clsidKeyPath += clsidText;

        HKEY clsidKey = nullptr;
        LSTATUS status = RegCreateKeyExW(
            HKEY_LOCAL_MACHINE, clsidKeyPath.c_str(), 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
            nullptr, &clsidKey, nullptr);
        if (status != ERROR_SUCCESS)
        {
            return HRESULT_FROM_WIN32(status);
        }

        HRESULT hr = SetStringValue(clsidKey, nullptr, L"Better Micro APO");
        if (SUCCEEDED(hr))
        {
            HKEY inprocKey = nullptr;
            status = RegCreateKeyExW(
                clsidKey, L"InprocServer32", 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
                nullptr, &inprocKey, nullptr);
            if (status == ERROR_SUCCESS)
            {
                hr = SetStringValue(inprocKey, nullptr, modulePath);
                if (SUCCEEDED(hr))
                {
                    hr = SetStringValue(inprocKey, L"ThreadingModel", L"Both");
                }
                RegCloseKey(inprocKey);
            }
            else
            {
                hr = HRESULT_FROM_WIN32(status);
            }
        }

        RegCloseKey(clsidKey);

        if (SUCCEEDED(hr))
        {
            hr = RegisterApoProperties();
        }

        if (SUCCEEDED(hr))
        {
            Log(L"DllRegisterServer ok: %s -> %s", clsidText, modulePath);
        }
        return hr;
    }

    HRESULT UnregisterComServer()
    {
        wchar_t clsidText[64]{};
        if (!GuidToString(CLSID_BetterMicroApo, clsidText))
        {
            return E_UNEXPECTED;
        }

        std::wstring clsidKeyPath = L"Software\\Classes\\CLSID\\";
        clsidKeyPath += clsidText;

        LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidKeyPath.c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        {
            return HRESULT_FROM_WIN32(status);
        }

        status = RegDeleteKeyExW(
            HKEY_LOCAL_MACHINE, clsidKeyPath.c_str(), KEY_WOW64_64KEY, 0);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        {
            return HRESULT_FROM_WIN32(status);
        }

        std::wstring apoKeyPath =
            L"Software\\Classes\\AudioEngine\\AudioProcessingObjects\\";
        apoKeyPath += clsidText;

        status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, apoKeyPath.c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        {
            return HRESULT_FROM_WIN32(status);
        }

        status = RegDeleteKeyExW(
            HKEY_LOCAL_MACHINE, apoKeyPath.c_str(), KEY_WOW64_64KEY, 0);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        {
            return HRESULT_FROM_WIN32(status);
        }

        Log(L"DllUnregisterServer ok: %s", clsidText);
        return S_OK;
    }
}

// ---------------------------------------------------------------------------
// DLL exports
// ---------------------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        // Diagnostic only: proves the DLL was mapped into the hosting process
        // even if no COM entry point is ever called.
        Log(L"DllMain attach pid=%lu", GetCurrentProcessId());
    }
    return g_moduleObject.DllMain(reason, reserved);
}

extern "C" HRESULT WINAPI DllCanUnloadNow()
{
    return g_moduleObject.DllCanUnloadNow();
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid,
                                            void** ppvObject)
{
    {
        wchar_t clsidText[64]{};
        wchar_t riidText[64]{};
        GuidToString(rclsid, clsidText);
        GuidToString(riid, riidText);
        Log(L"DllGetClassObject clsid=%s riid=%s", clsidText, riidText);
    }

    return g_moduleObject.DllGetClassObject(rclsid, riid, ppvObject);
}

extern "C" HRESULT WINAPI DllRegisterServer()
{
    return RegisterComServer();
}

extern "C" HRESULT WINAPI DllUnregisterServer()
{
    return UnregisterComServer();
}
