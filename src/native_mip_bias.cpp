// SM2 VR (port of SpidyVR to Marvel's Spider-Man 2), GPLv3; SpidyVR by Ilya Mezerowsky.
// The game's texture mip bias in the eyes (spidy/mip_bias.hpp). Part of the
// render-memory module, which the launcher loads while the game starts, before
// it creates its D3D12 device. No game address: D3D12CreateDevice is d3d12.dll's
// export, and the device's methods are its COM vtable's (DirectX-Headers
// d3d12.idl: ID3D12Device CreateRootSignature 16, CreateSampler 22;
// ID3D12Device11 CreateSampler2 79, the first after ID3D12Device10's 79).
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>
#include <windows.h>
#include <d3d12.h>
#include "spidy/mip_bias.hpp"
using namespace spidy;
using namespace spidy::mip_bias;
extern "C" {
__declspec(dllexport) Data SpidyMipBiasData;
}
namespace {
// D3D12_SAMPLER_DESC2 (Agility SDK 1.7xx): D3D12_SAMPLER_DESC's fields, then Flags.
struct SamplerDesc2 {
    D3D12_SAMPLER_DESC desc;
    UINT flags;
};
static_assert(sizeof(SamplerDesc2) == 56 && offsetof(D3D12_SAMPLER_DESC, MipLODBias) == 16);
// The static samplers of root signature 1.0/1.1 (D3D12_STATIC_SAMPLER_DESC)
// and 1.2 (D3D12_STATIC_SAMPLER_DESC1: the same, then Flags); 1.2's
// D3D12_ROOT_SIGNATURE_DESC2 has DESC1's layout.
static_assert(sizeof(D3D12_STATIC_SAMPLER_DESC) == 52 && offsetof(D3D12_STATIC_SAMPLER_DESC, MipLODBias) == 16);
constexpr size_t biasOffset = 16, anisotropyOffset = 20; // Filter is at 0 in both
static_assert(offsetof(D3D12_SAMPLER_DESC, MaxAnisotropy) == 20 && offsetof(D3D12_STATIC_SAMPLER_DESC, MaxAnisotropy) == 20);
const GUID device11 = {0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
using CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CreateSampler = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateSampler2 = void(STDMETHODCALLTYPE*)(ID3D12Device*, const SamplerDesc2*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateRootSignature = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
using Deserialize = HRESULT(WINAPI*)(const void*, SIZE_T, REFIID, void**);
using Serialize = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
CreateDevice originalCreateDevice{};
CreateSampler originalSampler{};
CreateSampler2 originalSampler2{};
CreateRootSignature originalRootSignature{};
Deserialize deserialize{};
Serialize serialize{};
void* createDeviceHook{};
void* deviceHooks[3]{};
std::mutex lifecycle;
std::atomic<bool> change{};
std::atomic<float> bias{};
std::atomic<uint32_t> anisotropy{};
std::atomic<int32_t> mostNegativeBits{}; // float bits; 0 is 0.0f
bool started{};

void count(uint64_t& field, uint64_t n = 1) {
    InterlockedAdd64(reinterpret_cast<volatile LONG64*>(&field), static_cast<LONG64>(n));
}
void seen(float game) {
    if (!(game < 0.f))
        return;
    auto bits = mostNegativeBits.load();
    float current;
    do {
        std::memcpy(&current, &bits, 4);
        if (game >= current)
            return;
        int32_t next;
        std::memcpy(&next, &game, 4);
        if (mostNegativeBits.compare_exchange_weak(bits, next)) {
            SpidyMipBiasData.mostNegative = game;
            return;
        }
    } while (true);
}
float samplerBias(const void* desc) {
    float b;
    std::memcpy(&b, static_cast<const uint8_t*>(desc) + biasOffset, 4);
    return b;
}
// A sampler: the game's bias is reported, and a negative one replaced; a
// negatively biased one's filtering raised to `anisotropy` (raiseAnisotropy).
// True when changed.
bool replaceBias(void* desc) {
    auto* d = static_cast<uint8_t*>(desc);
    const float game = samplerBias(desc);
    seen(game);
    bool changed = false;
    uint32_t filter, most;
    std::memcpy(&filter, d, 4);
    std::memcpy(&most, d + anisotropyOffset, 4);
    if (game < 0.f) {
        if (filter == anisotropic) {
            count(SpidyMipBiasData.anisotropicSamplers);
            for (auto seenMost = SpidyMipBiasData.mostAnisotropy; most > seenMost;
                 seenMost = SpidyMipBiasData.mostAnisotropy)
                if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&SpidyMipBiasData.mostAnisotropy),
                                               static_cast<LONG>(most), static_cast<LONG>(seenMost)) ==
                    static_cast<LONG>(seenMost))
                    break;
        } else if (filter == trilinear) {
            count(SpidyMipBiasData.trilinearSamplers);
        }
        if (raiseAnisotropy(game, filter, most, anisotropy.load())) {
            std::memcpy(d, &filter, 4);
            std::memcpy(d + anisotropyOffset, &most, 4);
            count(SpidyMipBiasData.raisedSamplers);
            changed = true;
        }
    }
    const float wanted = adjusted(game, change.load(), bias.load());
    if (wanted == game)
        return changed;
    std::memcpy(d + biasOffset, &wanted, 4);
    return true;
}
void STDMETHODCALLTYPE sampler(ID3D12Device* device, const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE to) {
    if (!desc)
        return originalSampler(device, desc, to);
    count(SpidyMipBiasData.samplers);
    D3D12_SAMPLER_DESC copy = *desc;
    if (copy.MipLODBias < 0.f)
        count(SpidyMipBiasData.negativeSamplers);
    if (replaceBias(&copy)) {
        count(SpidyMipBiasData.changedSamplers);
        return originalSampler(device, &copy, to);
    }
    originalSampler(device, desc, to);
}
void STDMETHODCALLTYPE sampler2(ID3D12Device* device, const SamplerDesc2* desc, D3D12_CPU_DESCRIPTOR_HANDLE to) {
    if (!desc)
        return originalSampler2(device, desc, to);
    count(SpidyMipBiasData.samplers);
    SamplerDesc2 copy = *desc;
    if (copy.desc.MipLODBias < 0.f)
        count(SpidyMipBiasData.negativeSamplers);
    if (replaceBias(&copy)) {
        count(SpidyMipBiasData.changedSamplers);
        return originalSampler2(device, &copy, to);
    }
    originalSampler2(device, desc, to);
}
// A root signature's static samplers, rewritten through d3d12.dll's own
// deserializer and serializer so the blob stays valid. Anything unexpected
// leaves the game's blob as it is.
HRESULT STDMETHODCALLTYPE rootSignature(ID3D12Device* device, UINT mask, const void* blob, SIZE_T size, REFIID iid,
                                        void** out) {
    count(SpidyMipBiasData.rootSignatures);
    const bool standalone = standaloneRootSignature(blob, size);
    ID3D12VersionedRootSignatureDeserializer* reader{};
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc{};
    if (!deserialize || !blob ||
        FAILED(deserialize(blob, size, IID_PPV_ARGS(&reader))) ||
        !(desc = reader->GetUnconvertedRootSignatureDesc())) {
        if (reader)
            reader->Release();
        if (standalone)
            count(SpidyMipBiasData.failedRootSignatures);
        return originalRootSignature(device, mask, blob, size, iid, out);
    }
    // 1.1's and 1.2's descriptions have the same layout; only the samplers' size differs.
    const uint32_t version = desc->Version;
    const size_t stride = version == 3 ? 56 : 52;
    const auto& d = desc->Desc_1_1;
    const UINT samplers = version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? desc->Desc_1_0.NumStaticSamplers : d.NumStaticSamplers;
    const auto* first = reinterpret_cast<const uint8_t*>(
        version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? desc->Desc_1_0.pStaticSamplers : d.pStaticSamplers);
    uint64_t negative = 0;
    for (UINT i = 0; first && i < samplers; ++i)
        negative += samplerBias(first + i * stride) < 0.f;
    if (!standalone) {
        count(SpidyMipBiasData.embeddedRootSignatures);
        count(SpidyMipBiasData.negativeEmbeddedSamplers, negative);
        for (UINT i = 0; first && i < samplers; ++i)
            seen(samplerBias(first + i * stride));
        reader->Release();
        return originalRootSignature(device, mask, blob, size, iid, out);
    }
    count(SpidyMipBiasData.staticSamplers, samplers);
    count(SpidyMipBiasData.negativeStaticSamplers, negative);
    std::vector<uint8_t> copies(first ? samplers * stride : 0);
    if (!copies.empty())
        std::memcpy(copies.data(), first, copies.size());
    bool changed = false;
    for (UINT i = 0; i < samplers && first; ++i)
        changed = replaceBias(copies.data() + i * stride) || changed;
    ID3DBlob* rewritten{};
    HRESULT result;
    if (changed && version >= 1 && version <= 3 && serialize) {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy = *desc;
        if (version == D3D_ROOT_SIGNATURE_VERSION_1_0)
            copy.Desc_1_0.pStaticSamplers = reinterpret_cast<const D3D12_STATIC_SAMPLER_DESC*>(copies.data());
        else
            copy.Desc_1_1.pStaticSamplers = reinterpret_cast<const D3D12_STATIC_SAMPLER_DESC*>(copies.data());
        ID3DBlob* errors{};
        if (SUCCEEDED(serialize(&copy, &rewritten, &errors)) && rewritten) {
            result = originalRootSignature(device, mask, rewritten->GetBufferPointer(), rewritten->GetBufferSize(),
                                           iid, out);
            if (SUCCEEDED(result)) {
                count(SpidyMipBiasData.changedRootSignatures);
            } else { // as the game made it, rather than none
                count(SpidyMipBiasData.failedRootSignatures);
                result = originalRootSignature(device, mask, blob, size, iid, out);
            }
        } else {
            count(SpidyMipBiasData.failedRootSignatures);
            result = originalRootSignature(device, mask, blob, size, iid, out);
        }
        if (errors)
            errors->Release();
    } else {
        result = originalRootSignature(device, mask, blob, size, iid, out);
    }
    if (rewritten)
        rewritten->Release();
    reader->Release();
    return result;
}
// The device's methods: one implementation for every device of the process.
void watch(ID3D12Device* device) {
    std::lock_guard lock(lifecycle);
    count(SpidyMipBiasData.devices);
    if (deviceHooks[0])
        return;
    auto** table = *reinterpret_cast<void***>(device);
    void* targets[3] = {table[16], table[22], nullptr};
    if (IUnknown* newer{}; SUCCEEDED(device->QueryInterface(device11, reinterpret_cast<void**>(&newer))) && newer) {
        targets[2] = (*reinterpret_cast<void***>(newer))[79];
        newer->Release();
    }
    void* details[3] = {reinterpret_cast<void*>(rootSignature), reinterpret_cast<void*>(sampler),
                        reinterpret_cast<void*>(sampler2)};
    void** originals[3] = {reinterpret_cast<void**>(&originalRootSignature), reinterpret_cast<void**>(&originalSampler),
                           reinterpret_cast<void**>(&originalSampler2)};
    for (unsigned i = 0; i < 3; ++i) {
        if (!targets[i])
            continue;
        auto s = MH_CreateHook(targets[i], details[i], originals[i]);
        if (s == MH_OK)
            s = MH_EnableHook(targets[i]);
        if (s != MH_OK) {
            MH_RemoveHook(targets[i]);
            SpidyMipBiasData.error = 8300 + 10 * i + s;
            continue;
        }
        deviceHooks[i] = targets[i];
    }
    SpidyMipBiasData.status = deviceHooks[0] || deviceHooks[1] ? 2 : 4;
}
HRESULT WINAPI createDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** out) {
    const auto result = originalCreateDevice(adapter, level, iid, out);
    // Without `out` the game only asks whether a device could be made.
    if (SUCCEEDED(result) && out && *out) {
        ID3D12Device* device{};
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&device))) && device) {
            watch(device);
            device->Release();
        }
    }
    return result;
}
} // namespace
// `parameter`: spidy::mip_bias::encode (bias and anisotropy), 0 to only report. Once, while the game starts.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyMipBiasStart(void* parameter) {
    std::lock_guard lock(lifecycle);
    if (started)
        return 0;
    bool wantChange;
    float wanted;
    uint32_t wantedAnisotropy;
    if (!decode(reinterpret_cast<uintptr_t>(parameter), wantChange, wanted, wantedAnisotropy))
        return 8001;
    change = wantChange;
    bias = wanted;
    anisotropy = wantedAnisotropy;
    SpidyMipBiasData.anisotropy = wantedAnisotropy;
    SpidyMipBiasData.change = wantChange;
    SpidyMipBiasData.bias = wanted;
    // A device made before now (the game started without the launcher) is not seen.
    const bool late = GetModuleHandleW(L"D3D12Core.dll") != nullptr;
    const auto d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12) {
        SpidyMipBiasData.status = 4;
        return SpidyMipBiasData.error = 8002;
    }
    deserialize = reinterpret_cast<Deserialize>(GetProcAddress(d3d12, "D3D12CreateVersionedRootSignatureDeserializer"));
    serialize = reinterpret_cast<Serialize>(GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature"));
    createDeviceHook = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    auto s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        SpidyMipBiasData.status = 4;
        return SpidyMipBiasData.error = 8100 + s;
    }
    if (!createDeviceHook ||
        (s = MH_CreateHook(createDeviceHook, reinterpret_cast<void*>(createDevice),
                           reinterpret_cast<void**>(&originalCreateDevice))) != MH_OK ||
        (s = MH_EnableHook(createDeviceHook)) != MH_OK) {
        SpidyMipBiasData.status = 4;
        return SpidyMipBiasData.error = 8200 + s;
    }
    started = true;
    SpidyMipBiasData.status = late ? 3 : 1;
    return 0;
}
