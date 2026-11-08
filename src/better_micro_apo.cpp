// Better Micro APO - a minimal Windows capture APO.
//
// DSP chain: optional gate -> linked peak compressor -> makeup gain ->
// optional mono copy (left channel to all channels).
//
// The stages themselves live in src/dsp. COM plumbing follows Microsoft's
// SwapAPO sample: ATL owns the class factory, the object map and the
// aggregation rules. Only the DSP and the registry writes are ours.

#include <atlbase.h>
#include <atlcom.h>

#include "better_micro_apo.h"
#include "audio_format_ids.h"
#include "dsp/stages.h"

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
    m_chain.Reset();
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

    apo::dsp::AudioFormat info;
    info.channels = format.dwSamplesPerFrame;
    info.sampleRate = static_cast<UINT32>(format.fFramesPerSecond + 0.5f);
    info.bytesPerSample = format.dwBytesPerSampleContainer;
    info.maxFrames = ppInputConnections[0]->u32MaxFrameCount;

    if (IsEqualGUID(format.guidFormatType, SUBTYPE_IEEE_FLOAT))
    {
        info.sample = apo::dsp::SampleFormat::Float32;
    }
    else if (info.bytesPerSample == 2)
    {
        info.sample = apo::dsp::SampleFormat::Pcm16;
    }
    else if (info.bytesPerSample == 3)
    {
        info.sample = apo::dsp::SampleFormat::Pcm24;
    }
    else
    {
        info.sample = apo::dsp::SampleFormat::Pcm32;
    }

    m_settings = apo::ApoSettings{};
    m_noiseProfile = apo::NoiseProfile{};
    if (!m_endpointId.empty())
    {
        apo::LoadSettings(m_endpointId, m_settings);
        if (m_settings.noise.enabled)
        {
            apo::LoadNoiseProfile(m_endpointId, m_noiseProfile);
        }
    }
    apo::ClampSettings(m_settings);

    m_formatInfo = info;
    m_maxFrames = (info.maxFrames > 0) ? info.maxFrames : 1;

    try
    {
        m_scratch.assign(
            static_cast<size_t>(m_maxFrames) * info.channels, 0.0);
        ConfigureDsp();
    }
    catch (...)
    {
        CBaseAudioProcessingObject::UnlockForProcess();
        m_chain.Clear();
        m_formatInfo = apo::dsp::AudioFormat{};
        m_maxFrames = 0;
        std::vector<double>().swap(m_scratch);
        Log(L"LockForProcess: could not allocate DSP buffers");
        return E_OUTOFMEMORY;
    }

    Log(L"LockForProcess ok: format=%s channels=%u rate=%u containerBytes=%u maxFrames=%u endpoint=%s noise=%d noiseReduction=%.2fdB noiseProfile=%d gate=%d gateThreshold=%.2fdB gateRange=%.2fdB gateAttack=%.2fms gateHold=%.2fms gateRelease=%.2fms gateLookahead=%.2fms compressor=%d threshold=%.2fdB ratio=%.2f:1 attack=%.2fms release=%.2fms makeup=%d makeupGain=%.2fdB mono=%d",
        apo::dsp::FormatName(m_formatInfo.sample), m_formatInfo.channels,
        m_formatInfo.sampleRate, m_formatInfo.bytesPerSample, m_maxFrames,
        m_endpointId.empty() ? L"<unknown>" : m_endpointId.c_str(),
        m_settings.noise.enabled ? 1 : 0, m_settings.noise.reductionDb,
        m_noiseProfile.valid ? 1 : 0,
        m_settings.gate.enabled ? 1 : 0, m_settings.gate.thresholdDb,
        m_settings.gate.rangeDb, m_settings.gate.attackMs,
        m_settings.gate.holdMs, m_settings.gate.releaseMs,
        m_settings.gate.lookaheadMs,
        m_settings.compressor.enabled ? 1 : 0,
        m_settings.compressor.thresholdDb, m_settings.compressor.ratio,
        m_settings.compressor.attackMs, m_settings.compressor.releaseMs,
        m_settings.makeupEnabled ? 1 : 0, m_settings.makeupDb,
        m_settings.mono ? 1 : 0);
    return S_OK;
}

STDMETHODIMP BetterMicroApo::UnlockForProcess()
{
    const HRESULT hr = CBaseAudioProcessingObject::UnlockForProcess();

    m_chain.Clear();
    m_noiseProfile = apo::NoiseProfile{};
    m_formatInfo = apo::dsp::AudioFormat{};
    m_maxFrames = 0;
    std::vector<double>().swap(m_scratch);

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

    if (!m_bIsLocked ||
        m_formatInfo.sample == apo::dsp::SampleFormat::Unknown ||
        input->u32BufferFlags == BUFFER_INVALID)
    {
        output->u32ValidFrameCount = 0;
        output->u32BufferFlags = BUFFER_INVALID;
        return;
    }

    if (input->u32ValidFrameCount > m_maxFrames)
    {
        output->u32ValidFrameCount = 0;
        output->u32BufferFlags = BUFFER_INVALID;
        return;
    }

    BYTE* destination = reinterpret_cast<BYTE*>(output->pBuffer);
    const BYTE* source = reinterpret_cast<const BYTE*>(input->pBuffer);

    if (input->u32BufferFlags == BUFFER_SILENT)
    {
        // A stage with lookahead buffers audio, so run the silent buffer
        // through the chain to flush its tail instead of replaying stale audio
        // when sound resumes.
        if (destination != nullptr && m_chain.NeedsSilence())
        {
            m_chain.ProcessSilence(destination, input->u32ValidFrameCount,
                                   m_scratch.data());
            output->u32BufferFlags = BUFFER_VALID;
        }
        else
        {
            output->u32BufferFlags = BUFFER_SILENT;

            // If the engine did not give us an in-place buffer, make sure the
            // silent output is actually zeroed.
            if (destination != nullptr && destination != source)
            {
                ZeroMemory(destination,
                           static_cast<size_t>(input->u32ValidFrameCount) *
                               m_formatInfo.channels *
                               m_formatInfo.bytesPerSample);
            }
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
    using namespace apo::dsp;

    m_chain.Clear();

    // Noise reduction must see the raw input, so it is the first stage.
    if (m_settings.noise.enabled && m_noiseProfile.valid)
    {
        m_chain.Add(
            std::make_unique<NoiseStage>(m_settings.noise, m_noiseProfile));
    }
    if (m_settings.gate.enabled)
    {
        m_chain.Add(std::make_unique<GateStage>(m_settings.gate));
    }
    if (m_settings.compressor.enabled)
    {
        m_chain.Add(std::make_unique<CompressorStage>(m_settings.compressor));
    }
    if (m_settings.makeupEnabled && m_settings.makeupDb != 0.0)
    {
        m_chain.Add(std::make_unique<GainStage>(m_settings.makeupDb));
    }
    if (m_settings.mono)
    {
        m_chain.Add(std::make_unique<MonoStage>());
    }

    m_chain.Configure(m_formatInfo);
    m_chain.Reset();
}

void BetterMicroApo::ProcessBuffer(const BYTE* source, BYTE* destination, UINT32 frames)
{
    m_chain.Process(source, destination, frames, m_scratch.data());
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
