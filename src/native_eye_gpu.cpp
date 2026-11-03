#include "spidy/native_eye_gpu.hpp"
#include "spidy/copy_eye_texture.hpp"
#include "spidy/eye_pair_state.hpp"
#include "spidy/native_hud.hpp"
#include "spidy/eye_sharpen.hpp"
#include "spidy/eye_debug.hpp"
#include "spidy/monitor_rate.hpp"
#include "spidy/presentation_gate.hpp"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using namespace spidy;
extern "C" IMAGE_DOS_HEADER __ImageBase;
extern "C" {
__declspec(dllexport) native_gpu::Data SpidyGpuData;
__declspec(dllexport) native_gpu::ScreenData SpidyScreenData;
__declspec(dllexport) native_gpu::HistoryData SpidyEyeHistoryData;
}
namespace spidy::native_gpu {
namespace {
using Barrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*,
                                          ID3D12PipelineState*);
using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using EndQuery = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE,
                                          UINT);
using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
Barrier originalBarrier{};
Reset originalReset{};
Execute originalExecute{};
EndQuery originalEndQuery{};
Present originalPresent{};
Present1 originalPresent1{};
std::array<void*, 4> hooks{};
// DXGI Present and Present1; absent when no swapchain could be made to find them.
std::array<void*, 2> presentHooks{};
std::atomic<bool> enabled{};
std::atomic<uint32_t> fault{};
std::mutex lifecycle, submission, telemetry;
// Recursive: under Proton (vkd3d-proton) one function serves every command list, so the hooks below
// also see Spidy's own D3D12 calls and can re-enter on a thread that already holds this lock. A plain
// std::mutex then throws resource_deadlock_would_occur and the game closes.
std::recursive_mutex recording;
std::condition_variable copyReady;
uint64_t deadline{};
bool hooked{};
bool copyToXr{}, captureImages{}, staged{};
uint64_t requestedSerial{}, requestFence{};
std::array<ComPtr<ID3D12Resource>, 2> targets;
struct Marker {
    unsigned eye{};
    EyePairState::Stamp stamp{};
    bool ended{}, valid{};
};
thread_local Marker currentMarker{};
struct Event {
    unsigned eye{};
    bool marker{}, ended{};
    EyePairState::Stamp stamp{};
    D3D12_RESOURCE_TRANSITION_BARRIER transition{};
    D3D12_RESOURCE_BARRIER_FLAGS flags{};
};
std::unordered_map<ID3D12CommandList*, std::vector<Event>> lists;
EyePairState pair;
uint64_t executes{}, matchedMarkers{}, pairs{}, lastGeneration{}, lastSerial{}, fenceValue{};
std::atomic<uint64_t> markers{};
uint64_t begins[2]{}, ends[2]{};
// Eye images the game replaced during the session (registerEye: its graphics menu).
std::atomic<uint32_t> eyeReplacements{};
ComPtr<ID3D12Device> device;
ComPtr<ID3D12CommandQueue> queue;
std::array<ComPtr<ID3D12Resource>, 2> resources, readbacks;
std::array<std::atomic<ID3D12Resource*>, 2> watched{};
std::array<D3D12_RESOURCE_STATES, 2> states{};
std::array<bool, 2> known{};
std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> layouts{};
std::array<std::vector<uint8_t>, 2> pixels;
ComPtr<ID3D12CommandAllocator> allocator;
ComPtr<ID3D12GraphicsCommandList> command;
ComPtr<ID3D12Fence> fence;
HANDLE fenceEvent{};
UINT width{}, height{};
// The headset's eye size (setEyeOutput; 0: the eyes' own). Above it the eyes
// are supersampled and the staged images are filtered down to it (drawAa).
std::atomic<uint32_t> wantedOutWidth{}, wantedOutHeight{};
UINT outWidth{}, outHeight{};
bool downsampling() { return outWidth != width || outHeight != height; }
// The size of the images read back (captureImage) and published (Data).
UINT imageWidth() { return staged ? outWidth : width; }
UINT imageHeight() { return staged ? outHeight : height; }
struct CopyCommands {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t fence{};
};
std::array<CopyCommands, 3> captureCommands, presentCommands, screenCommands, mirrorCommands, shownCommands,
    hudCommands;
std::array<ComPtr<ID3D12Resource>, 2> stagedEyes;
EyePairState::Stamp capturedStamp;
uint64_t capturedPairs{}, reusedPairs{}, capturedMs{};
// copyTiming (submission lock): headset copies not yet seen finished, with
// when they and their pair were submitted (QueryPerformanceCounter).
int64_t capturedQpc{};
struct PendingCopy {
    uint64_t fence{};
    int64_t submitted{}, paired{};
};
std::array<PendingCopy, 16> pendingCopies;
unsigned pendingCount{};
TimingMetric copyGpu, copyPair;
std::mutex copyTimingLock;
double qpcMs(int64_t from, int64_t to) {
    static const double perMs = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    return static_cast<double>(to - from) / perMs;
}
int64_t qpcNow() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}
void noteCopies(uint64_t completed) { // submission lock held by caller
    if (!pendingCount || completed == UINT64_MAX)
        return;
    const auto now = qpcNow();
    unsigned kept = 0;
    std::lock_guard lock(copyTimingLock);
    for (unsigned i = 0; i < pendingCount; ++i) {
        const auto& p = pendingCopies[i];
        if (p.fence <= completed) {
            copyGpu.add(qpcMs(p.submitted, now));
            if (p.paired)
                copyPair.add(qpcMs(p.paired, now));
        } else
            pendingCopies[kept++] = p;
    }
    pendingCount = kept;
}
// The game's presented frame (submission lock). A texture replaced by a new
// size stays alive for a few seconds, for draws still reading it.
std::atomic<bool> screenWanted{};
bool screenWantedFlag() {
    return screenWanted.load();
}
Screen screen;
uint64_t screenMs{};
struct Retired {
    ComPtr<ID3D12Resource> texture;
    uint64_t releaseMs{};
};
std::vector<Retired> retiredScreens;
std::vector<uint8_t> screenPixels; // probe readback of the newest copy
std::atomic<uint64_t> screenPresents{};
// Every Present of any swapchain in the process since the hooks went in.
std::atomic<uint32_t> presentCalls{};
// The left eye on the game's window (mirrorEye): wanted while the game does
// not render its own view, and how many presents got it.
std::atomic<bool> mirrorWanted{};
std::atomic<uint32_t> mirrorCalls{};
// While the window shows the left eye, at most this many of the game's
// presents a second reach DXGI (monitor_rate.hpp; 0: all of them), the time
// the next one is due, and how many were held back.
std::atomic<uint32_t> mirrorRate{}, presentsHeld{};
std::atomic<int64_t> mirrorDue{};
// The left eye as the headset showed it, the overlay's drawings on it
// (keepShownEye; COPY_SOURCE between uses, submission lock), and when it was
// copied. shownWanted: the window has drawn since, so the next one is copied.
ComPtr<ID3D12Resource> shownEye;
uint64_t shownMs{};
bool shownFailed{};
std::atomic<bool> shownWanted{true};
std::atomic<uint32_t> shownMirrors{};
struct MirrorPipeline {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> state;
    ComPtr<ID3D12DescriptorHeap> source, target;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool failed{};
} mirrorPipeline;
std::atomic<uint32_t> screenSkipped{}, backBufferFormat{};
// MilesVR 0.1.80: the game's HUD texture (recording lock), its state after the barriers the game has
// submitted, and the copy of it made at each wanted Present (submission lock).
ComPtr<ID3D12Resource> hudResource;
std::atomic<ID3D12Resource*> hudWatched{};
D3D12_RESOURCE_STATES hudState{};
bool hudKnown{};
std::atomic<bool> hudWanted{};
Screen hud;
uint64_t hudMs{};
std::vector<Retired> retiredHuds;
std::atomic<uint32_t> hudCopied{}, hudSkipped{};
constexpr unsigned hudEvent = 2; // Event::eye for the HUD texture
void publishScreen(uint32_t status, const void* rows, uint32_t rowBytes) {
    std::lock_guard lock(telemetry);
    auto& d = SpidyScreenData;
    InterlockedIncrement64(&d.sequence);
    if (status)
        d.status = status;
    d.frames = screen.frame;
    d.width = screen.width;
    d.height = screen.height;
    d.format = screen.view;
    if (rows) {
        d.pixels = reinterpret_cast<uint64_t>(rows);
        d.rowBytes = rowBytes;
    }
    d.presents = screenPresents;
    d.skip = screenSkipped;
    d.backBufferFormat = backBufferFormat;
    InterlockedIncrement64(&d.sequence);
}
bool read(uintptr_t p, void* out, size_t n) {
    __try {
        if (p < 0x10000)
            return false;
        std::memcpy(out, reinterpret_cast<void*>(p), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void publish() { // submission and recording held by caller
    std::lock_guard lock(telemetry);
    auto& d = SpidyGpuData;
    InterlockedIncrement64(&d.sequence);
    d.error = fault.load();
    d.executes = executes;
    d.markers = markers;
    d.matchedMarkers = matchedMarkers;
    d.pairs = pairs;
    d.serial = lastSerial;
    d.generation = lastGeneration;
    d.fence = fenceValue;
    d.completed = fence ? fence->GetCompletedValue() : 0;
    if (fence)
        noteCopies(d.completed);
    d.width = imageWidth();
    d.height = imageHeight();
    d.captured = capturedPairs;
    d.reused = reusedPairs;
    d.capturedSerial = capturedStamp.serial;
    for (unsigned i = 0; i < 2; ++i) {
        d.begins[i] = begins[i];
        d.ends[i] = ends[i];
        d.resources[i] = reinterpret_cast<uint64_t>(resources[i].Get());
        d.states[i] = states[i];
        d.known[i] = known[i];
        d.pixels[i] = pixels[i].empty() ? 0 : reinterpret_cast<uint64_t>(pixels[i].data());
    }
    InterlockedIncrement64(&d.sequence);
}
void add(ID3D12CommandList* list, const Event& event) {
    auto found = lists.find(list);
    if (found == lists.end()) {
        if (lists.size() >= 4096) {
            fault = 3101;
            return;
        }
        found = lists.emplace(list, std::vector<Event>{}).first;
    }
    if (found->second.size() >= 4096) {
        fault = 3102;
        return;
    }
    found->second.push_back(event);
}
void STDMETHODCALLTYPE barrier(ID3D12GraphicsCommandList* list, UINT count,
                               const D3D12_RESOURCE_BARRIER* bs) {
    if (enabled) {
        const auto* left = watched[0].load();
        const auto* right = watched[1].load();
        const auto* hudTexture = hudWatched.load();
        bool relevant{};
        for (UINT n = 0; n < count; ++n)
            relevant |= bs[n].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                        (bs[n].Transition.pResource == left || bs[n].Transition.pResource == right ||
                         (hudTexture && bs[n].Transition.pResource == hudTexture));
        if (!relevant) {
            originalBarrier(list, count, bs);
            return;
        }
        std::lock_guard lock(recording);
        for (UINT n = 0; n < count; ++n)
            if (bs[n].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                for (unsigned i = 0; i < 2; ++i)
                    if (resources[i] && bs[n].Transition.pResource == resources[i].Get()) {
                        Event e{};
                        e.eye = i;
                        e.transition = bs[n].Transition;
                        e.flags = bs[n].Flags;
                        add(list, e);
                    }
        for (UINT n = 0; n < count; ++n)
            if (bs[n].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && hudResource &&
                bs[n].Transition.pResource == hudResource.Get()) {
                Event e{};
                e.eye = hudEvent;
                e.transition = bs[n].Transition;
                e.flags = bs[n].Flags;
                add(list, e);
            }
    }
    originalBarrier(list, count, bs);
}
HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* a,
                                ID3D12PipelineState* p) {
    auto result = originalReset(list, a, p);
    if (enabled && SUCCEEDED(result)) {
        std::lock_guard lock(recording);
        lists.erase(list);
    }
    return result;
}
void STDMETHODCALLTYPE endQuery(ID3D12GraphicsCommandList* list, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type,
                                UINT index) {
    originalEndQuery(list, heap, type, index);
    if (enabled) {
        markers.fetch_add(1, std::memory_order_relaxed);
        if (!currentMarker.valid || type != D3D12_QUERY_TYPE_TIMESTAMP)
            return;
        std::lock_guard lock(recording);
        if (currentMarker.valid && type == D3D12_QUERY_TYPE_TIMESTAMP) {
            ++matchedMarkers;
            Event e{};
            e.eye = currentMarker.eye;
            e.marker = true;
            e.ended = currentMarker.ended;
            e.stamp = currentMarker.stamp;
            add(list, e);
        }
    }
}
bool makeReadbacks() {
    if (width && (!captureImages || (readbacks[0] && readbacks[1])))
        return true;
    if (!resources[0] || !resources[1])
        return false;
    for (unsigned i = 0; i < 2; ++i) {
        const auto desc = resources[i]->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.MipLevels != 1 || desc.DepthOrArraySize != 1 ||
            desc.SampleDesc.Count != 1 || !validEyeSize(desc.Width) || !validEyeSize(desc.Height) ||
            (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) {
            fault = 3201;
            return false;
        }
        if (i && (width != desc.Width || height != desc.Height)) {
            fault = 3202;
            return false;
        }
        width = static_cast<UINT>(desc.Width);
        height = desc.Height;
        outWidth = wantedOutWidth.load() ? wantedOutWidth.load() : width;
        outHeight = wantedOutHeight.load() ? wantedOutHeight.load() : height;
        if (!captureImages)
            continue;
        // Staged, the images read back are the headset's (copyLatest), at its size.
        auto image = desc;
        image.Width = imageWidth();
        image.Height = imageHeight();
        UINT64 bytes{};
        device->GetCopyableFootprints(&image, 0, 1, 0, &layouts[i], nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&readbacks[i])))) {
            fault = 3203;
            return false;
        }
        pixels[i].resize(static_cast<size_t>(imageWidth()) * imageHeight() * 4);
    }
    return true;
}
CopyCommands* readyCommands(std::array<CopyCommands, 3>& pool) {
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        fault = 3301;
        return nullptr;
    }
    for (auto& slot : pool) {
        if (slot.fence > completed)
            continue;
        if (!slot.allocator) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&slot.allocator))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(),
                                                 nullptr, IID_PPV_ARGS(&slot.list))) ||
                FAILED(slot.list->Close())) {
                fault = 3801;
                return nullptr;
            }
        }
        if (FAILED(slot.allocator->Reset()) ||
            FAILED(originalReset(slot.list.Get(), slot.allocator.Get(), nullptr))) {
            fault = 3802;
            return nullptr;
        }
        return &slot;
    }
    return nullptr;
}
bool submitCommands(CopyCommands& slot) {
    if (FAILED(slot.list->Close())) {
        fault = 3803;
        return false;
    }
    ID3D12CommandList* list = slot.list.Get();
    originalExecute(queue.Get(), 1, &list);
    slot.fence = ++fenceValue;
    if (FAILED(queue->Signal(fence.Get(), slot.fence))) {
        fault = 3804;
        return false;
    }
    return true;
}
// Update 16: each eye pass's newest GPU time, for the wrist hologram's FPS
// page (eyePassesGpuMs): plain atomics, so reading it takes no lock.
struct PassClock {
    std::atomic<float> ms{};
    std::atomic<uint64_t> tick{}; // GetTickCount64 when noted; 0 never
    void note(double value) {
        ms = static_cast<float>(value);
        tick = GetTickCount64();
    }
};
PassClock aaClock, historyClock, sharpenClock;
// Eye anti-aliasing (native_eye_gpu.hpp, setEyeAA). The pass is built on the
// first pair that wants it, under the submission and recording locks.
std::atomic<uint32_t> aaMode{}, aaFault{};
struct AaPipeline {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> state, down; // FXAA; the downsample filter
    ComPtr<ID3D12DescriptorHeap> views, targets;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> stamps; // readback, mapped while the bridge lives
    const volatile uint64_t* stampData{};
    double ticksPerMs{};
    std::array<bool, 3> timed{}; // a captureCommands slot holds a measured pass
    bool ready{};
} aa;
TimingMetric aaGpu; // copyTimingLock
std::atomic<uint64_t> aaPairs{};
// FXAA 3.11 quality (Timothy Lottes, NVIDIA), PC path, preset 29 steps, luma
// computed from RGB. The eyes hold display-encoded values, as FXAA expects.
const char* fxaaShader = R"(
Texture2D source : register(t0);
SamplerState bilinear : register(s0);
cbuffer Frame : register(b0) { float2 rcpFrame; float subpix; float edgeThreshold; };
static const float edgeThresholdMin = 0.0625;
static const float steps[12] = {1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float lumaAt(float2 p) { return luma(source.SampleLevel(bilinear, p, 0).rgb); }
float lumaOff(float2 p, int2 o) { return luma(source.SampleLevel(bilinear, p, 0, o).rgb); }
float4 vs(uint id : SV_VertexID) : SV_POSITION {
    const float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 position : SV_POSITION) : SV_TARGET {
    float2 posM = position.xy * rcpFrame;
    const float4 rgbyM = source.SampleLevel(bilinear, posM, 0);
    const float lumaM = luma(rgbyM.rgb);
    float lumaS = lumaOff(posM, int2(0, 1));
    const float lumaE = lumaOff(posM, int2(1, 0));
    float lumaN = lumaOff(posM, int2(0, -1));
    const float lumaW = lumaOff(posM, int2(-1, 0));
    const float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    const float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    const float range = rangeMax - rangeMin;
    if (range < max(edgeThresholdMin, rangeMax * edgeThreshold))
        return rgbyM;
    const float lumaNW = lumaOff(posM, int2(-1, -1));
    const float lumaSE = lumaOff(posM, int2(1, 1));
    const float lumaNE = lumaOff(posM, int2(1, -1));
    const float lumaSW = lumaOff(posM, int2(-1, 1));
    const float lumaNS = lumaN + lumaS;
    const float lumaWE = lumaW + lumaE;
    const float subpixRcpRange = 1.0 / range;
    const float subpixNSWE = lumaNS + lumaWE;
    const float edgeHorz1 = -2.0 * lumaM + lumaNS;
    const float edgeVert1 = -2.0 * lumaM + lumaWE;
    const float lumaNESE = lumaNE + lumaSE;
    const float lumaNWNE = lumaNW + lumaNE;
    const float edgeHorz2 = -2.0 * lumaE + lumaNESE;
    const float edgeVert2 = -2.0 * lumaN + lumaNWNE;
    const float lumaNWSW = lumaNW + lumaSW;
    const float lumaSWSE = lumaSW + lumaSE;
    const float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
    const float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    const float edgeHorz3 = -2.0 * lumaW + lumaNWSW;
    const float edgeVert3 = -2.0 * lumaS + lumaSWSE;
    const float edgeHorz = abs(edgeHorz3) + edgeHorz4;
    const float edgeVert = abs(edgeVert3) + edgeVert4;
    const float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = rcpFrame.x;
    const bool horzSpan = edgeHorz >= edgeVert;
    const float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) {
        lumaN = lumaW;
        lumaS = lumaE;
    } else
        lengthSign = rcpFrame.y;
    const float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    const float gradientN = lumaN - lumaM;
    const float gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM;
    const float lumaSS = lumaS + lumaM;
    const bool pairN = abs(gradientN) >= abs(gradientS);
    const float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN)
        lengthSign = -lengthSign;
    const float subpixC = saturate(abs(subpixB) * subpixRcpRange);
    float2 posB = posM;
    const float2 offNP = horzSpan ? float2(rcpFrame.x, 0.0) : float2(0.0, rcpFrame.y);
    if (!horzSpan)
        posB.x += lengthSign * 0.5;
    else
        posB.y += lengthSign * 0.5;
    float2 posN = posB - offNP * steps[0];
    float2 posP = posB + offNP * steps[0];
    const float subpixD = -2.0 * subpixC + 3.0;
    float lumaEndN = lumaAt(posN);
    const float subpixE = subpixC * subpixC;
    float lumaEndP = lumaAt(posP);
    if (!pairN)
        lumaNN = lumaSS;
    const float gradientScaled = gradient * 0.25;
    const float lumaMM = lumaM - lumaNN * 0.5;
    const float subpixF = subpixD * subpixE;
    const bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN)
        posN -= offNP * steps[1];
    if (!doneP)
        posP += offNP * steps[1];
    for (int i = 2; i < 12; ++i) {
        if (doneN && doneP)
            break;
        if (!doneN)
            lumaEndN = lumaAt(posN) - lumaNN * 0.5;
        if (!doneP)
            lumaEndP = lumaAt(posP) - lumaNN * 0.5;
        doneN = abs(lumaEndN) >= gradientScaled;
        doneP = abs(lumaEndP) >= gradientScaled;
        if (!doneN)
            posN -= offNP * steps[i];
        if (!doneP)
            posP += offNP * steps[i];
    }
    float dstN = posM.x - posN.x;
    float dstP = posP.x - posM.x;
    if (!horzSpan) {
        dstN = posM.y - posN.y;
        dstP = posP.y - posM.y;
    }
    const bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    const bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    const float spanLengthRcp = 1.0 / (dstP + dstN);
    const bool directionN = dstN < dstP;
    const float dst = min(dstN, dstP);
    const bool goodSpan = directionN ? goodSpanN : goodSpanP;
    const float subpixG = subpixF * subpixF;
    const float pixelOffset = dst * -spanLengthRcp + 0.5;
    const float subpixH = subpixG * subpix;
    const float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    const float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan)
        posM.x += pixelOffsetSubpix * lengthSign;
    else
        posM.y += pixelOffsetSubpix * lengthSign;
    return float4(source.SampleLevel(bilinear, posM, 0).rgb, rgbyM.a);
}
)";
// The downsample for supersampled eyes (setEyeOutput): each headset pixel is
// the Mitchell-Netravali (B = C = 1/3) filtered average of the eye's pixels
// under it, out to `reach` headset pixels from its centre, in linear light
// (the eyes hold sRGB-encoded values). A plain or bilinear copy at that ratio
// keeps the aliasing of patterns finer than the headset's pixels (the curved
// moire on rows of fins); this filter removes them with little blur.
const char* downsampleShader = R"(
Texture2D source : register(t0);
cbuffer Frame : register(b0) { float2 scale; float reach; float unused; };
float4 vs(uint id : SV_VertexID) : SV_POSITION {
    const float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}
float mitchell(float x) {
    x = abs(x);
    if (x >= 2.0)
        return 0.0;
    const float x2 = x * x, x3 = x2 * x;
    if (x < 1.0)
        return (7.0 * x3 - 12.0 * x2 + 16.0 / 3.0) / 6.0;
    return (-7.0 / 3.0 * x3 + 12.0 * x2 - 20.0 * x + 32.0 / 3.0) / 6.0;
}
float3 toLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 toDisplay(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float4 ps(float4 position : SV_POSITION) : SV_TARGET {
    uint w, h;
    source.GetDimensions(w, h);
    const float2 centre = position.xy * scale;     // in the eye's pixels
    const float2 step = max(scale, 1.0);           // eye pixels per filter unit
    const float2 radius = min(reach, 2.0) * step;
    const int2 lo = max(int2(floor(centre - radius)), 0);
    const int2 hi = min(int2(ceil(centre + radius)), int2(w, h) - 1);
    float3 sum = 0;
    float total = 0;
    [loop] for (int y = lo.y; y <= hi.y; ++y) {
        const float wy = mitchell((y + 0.5 - centre.y) / step.y);
        if (wy == 0.0)
            continue;
        [loop] for (int x = lo.x; x <= hi.x; ++x) {
            const float weight = wy * mitchell((x + 0.5 - centre.x) / step.x);
            sum += weight * toLinear(source.Load(int3(x, y, 0)).rgb);
            total += weight;
        }
    }
    const float3 colour = total > 0.0 ? saturate(sum / total) : toLinear(source.Load(int3(min(int2(centre), int2(w, h) - 1), 0)).rgb);
    return float4(toDisplay(colour), source.Load(int3(min(int2(centre), int2(w, h) - 1), 0)).a);
}
)";
// The Mitchell filter's full reach, in headset pixels.
constexpr float downsampleRadius = 2.f;
// FXAA's quality settings: sub-pixel smoothing 0.5 (its default 0.75 softens
// the eyes' fine detail more than a headset wants) and edge threshold 0.125
// (its "high quality" value).
constexpr float aaSubpix = 0.5f, aaEdgeThreshold = 0.125f;
// The AA pass's view of eye image i (prepareAa; again when the game makes a new one, registerEye).
void writeEyeView(unsigned i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    auto view = aa.views->GetCPUDescriptorHandleForHeapStart();
    view.ptr += static_cast<SIZE_T>(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) * i;
    device->CreateShaderResourceView(resources[i].Get(), &sv, view);
}
bool failAa(uint32_t code) {
    aaFault = code;
    aa.ready = false;
    return false;
}
// The AA pass for the eye images and stagedEyes (both fixed for the bridge's
// life, so the descriptors are written once). Submission and recording held.
bool prepareAa() {
    if (aaFault)
        return false;
    if (aa.ready)
        return true;
    for (unsigned i = 0; i < 2; ++i)
        if (resources[i]->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
            return failAa(5);
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 4;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> signature, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error)) ||
        FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                           IID_PPV_ARGS(&aa.root))))
        return failAa(1);
    // FXAA or, for supersampled eyes, the downsample filter: the same root.
    const bool down = downsampling();
    const char* shader = down ? downsampleShader : fxaaShader;
    const char* name = down ? "spidy_eye_downsample" : "spidy_eye_fxaa";
    ComPtr<ID3DBlob> vs, ps;
    if (FAILED(D3DCompile(shader, std::strlen(shader), name, nullptr, nullptr, "vs", "vs_5_0",
                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &error)) ||
        FAILED(D3DCompile(shader, std::strlen(shader), name, nullptr, nullptr, "ps", "ps_5_0",
                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &error)))
        return failAa(2);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = aa.root.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    auto& blend = d.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(down ? &aa.down : &aa.state))))
        return failAa(3);
    D3D12_DESCRIPTOR_HEAP_DESC views{};
    views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    views.NumDescriptors = 2;
    views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC targetViews{};
    targetViews.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    targetViews.NumDescriptors = 2;
    if (FAILED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&aa.views))) ||
        FAILED(device->CreateDescriptorHeap(&targetViews, IID_PPV_ARGS(&aa.targets))))
        return failAa(4);
    const auto targetStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (unsigned i = 0; i < 2; ++i) {
        writeEyeView(i);
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        auto target = aa.targets->GetCPUDescriptorHandleForHeapStart();
        target.ptr += static_cast<SIZE_T>(targetStep) * i;
        device->CreateRenderTargetView(stagedEyes[i].Get(), &rv, target);
    }
    // The GPU time is optional: without timestamps the pass still runs.
    UINT64 frequency{};
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2 * static_cast<UINT>(captureCommands.size());
    D3D12_HEAP_PROPERTIES readback{};
    readback.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = sizeof(uint64_t) * qd.Count;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped{};
    if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency &&
        SUCCEEDED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&aa.queries))) &&
        SUCCEEDED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&aa.stamps))) &&
        SUCCEEDED(aa.stamps->Map(0, nullptr, &mapped))) {
        aa.stampData = static_cast<const volatile uint64_t*>(mapped);
        aa.ticksPerMs = static_cast<double>(frequency) / 1000.0;
    } else {
        aa.queries.Reset();
        aa.stamps.Reset();
    }
    aa.ready = true;
    return true;
}
// The measured pass that last used this slot, now that its fence passed.
void noteAaTime(size_t slot) {
    if (!aa.timed[slot] || !aa.stampData)
        return;
    aa.timed[slot] = false;
    const uint64_t begin = aa.stampData[2 * slot], end = aa.stampData[2 * slot + 1];
    if (end > begin) {
        std::lock_guard lock(copyTimingLock);
        aaGpu.add(static_cast<double>(end - begin) / aa.ticksPerMs);
    }
    if (end > begin)
        aaClock.note(static_cast<double>(end - begin) / aa.ticksPerMs);
}
// Both eyes through FXAA, or the supersampled eyes through the downsample
// filter, into stagedEyes, which enter and leave COPY_SOURCE;
// the eye images return to the states the game left them in.
void drawAa(ID3D12GraphicsCommandList* list, size_t slot) {
    const auto first = static_cast<UINT>(2 * slot);
    if (aa.queries)
        originalEndQuery(list, aa.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first);
    D3D12_RESOURCE_BARRIER b[4]{};
    UINT count = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (!(states[i] & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
            b[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[count++].Transition = {resources[i].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, states[i],
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        }
        b[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[count++].Transition = {stagedEyes[i].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
    }
    originalBarrier(list, count, b);
    // FXAA: one over the eye's size, and its settings. The downsample: the
    // eye's pixels per headset pixel across and down, and the filter's reach.
    const bool down = downsampling();
    const float frame[4] = {down ? static_cast<float>(width) / static_cast<float>(outWidth)
                                 : 1.f / static_cast<float>(width),
                            down ? static_cast<float>(height) / static_cast<float>(outHeight)
                                 : 1.f / static_cast<float>(height),
                            down ? downsampleRadius : aaSubpix, down ? 0.f : aaEdgeThreshold};
    list->SetPipelineState(down ? aa.down.Get() : aa.state.Get());
    list->SetGraphicsRootSignature(aa.root.Get());
    ID3D12DescriptorHeap* heaps[] = {aa.views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRoot32BitConstants(1, 4, frame, 0);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(outWidth), static_cast<float>(outHeight), 0, 1};
    const D3D12_RECT rect{0, 0, static_cast<LONG>(outWidth), static_cast<LONG>(outHeight)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &rect);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto viewStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto targetStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (unsigned i = 0; i < 2; ++i) {
        auto view = aa.views->GetGPUDescriptorHandleForHeapStart();
        view.ptr += static_cast<UINT64>(viewStep) * i;
        auto target = aa.targets->GetCPUDescriptorHandleForHeapStart();
        target.ptr += static_cast<SIZE_T>(targetStep) * i;
        list->SetGraphicsRootDescriptorTable(0, view);
        list->OMSetRenderTargets(1, &target, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
    }
    for (UINT n = 0; n < count; ++n)
        std::swap(b[n].Transition.StateBefore, b[n].Transition.StateAfter);
    originalBarrier(list, count, b);
    if (aa.queries) {
        originalEndQuery(list, aa.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
        list->ResolveQueryData(aa.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2, aa.stamps.Get(),
                               sizeof(uint64_t) * first);
        aa.timed[slot] = true;
    }
}
// Update 16: eye sharpening (native_eye_gpu.hpp, setEyeSharpen;
// eye_sharpen.hpp). For the headset only: instead of copying stagedEyes into
// the headset's images (copyLatest), the pass draws them there sharpened, so
// it adds no copy of its own, and the web/HUD overlay is drawn over it as
// before. Built on the first headset frame that wants it, under the
// submission and recording locks. If anything is missing (the compile, the
// pipeline, a headset image that is not a render target) it stops for the
// session (sharpenFault) and copyLatest copies as before: the eyes never go
// without a picture.
std::atomic<uint32_t> sharpenPercent{}, sharpenFault{};
std::atomic<bool> sharpening{}; // the last headset frame was drawn sharpened
// Update 24: live.txt eye_debug (eye_debug.hpp): 1 both headset images from the left eye's picture.
std::atomic<uint32_t> eyeDebugMode{};
struct SharpenPipeline {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> state, srgb; // to UNORM targets; to UNORM_SRGB ones
    ComPtr<ID3D12DescriptorHeap> views, targets;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> stamps;
    const volatile uint64_t* stampData{};
    double ticksPerMs{};
    std::array<bool, 3> timed{}; // a presentCommands slot holds a measured pass
    bool ready{};
} sharp;
TimingMetric sharpenGpu; // copyTimingLock
std::atomic<uint64_t> sharpenPairs{};
// CAS (eye_sharpen.hpp sharpenChannel, the same arithmetic): the eyes hold
// display-encoded values and are sharpened as such, like FXAA. To a typed
// sRGB target (whose writes are encoded by the GPU) the result goes out as
// light (linearOut), so the stored bytes are the same as a UNORM target's.
// No vector ?: (dxc's HLSL 2021 rejects it; checked with dxc on the box).
const char* sharpenShader = R"(
Texture2D source : register(t0);
cbuffer Frame : register(b0) { float peak; float linearOut; float lift; float unused; };
float4 vs(uint id : SV_VertexID) : SV_POSITION {
    const float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 toLinear(float3 c) {
    return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045));
}
float4 ps(float4 position : SV_POSITION) : SV_TARGET {
    uint w, h;
    source.GetDimensions(w, h);
    const int2 p = int2(position.xy);
    const int2 hi = int2(w, h) - 1;
    const float3 a = source.Load(int3(clamp(p + int2(-1, -1), 0, hi), 0)).rgb;
    const float3 b = source.Load(int3(clamp(p + int2(0, -1), 0, hi), 0)).rgb;
    const float3 c = source.Load(int3(clamp(p + int2(1, -1), 0, hi), 0)).rgb;
    const float3 d = source.Load(int3(clamp(p + int2(-1, 0), 0, hi), 0)).rgb;
    const float4 centre = source.Load(int3(clamp(p, 0, hi), 0));
    const float3 e = centre.rgb;
    const float3 f = source.Load(int3(clamp(p + int2(1, 0), 0, hi), 0)).rgb;
    const float3 g = source.Load(int3(clamp(p + int2(-1, 1), 0, hi), 0)).rgb;
    const float3 hh = source.Load(int3(clamp(p + int2(0, 1), 0, hi), 0)).rgb;
    const float3 i = source.Load(int3(clamp(p + int2(1, 1), 0, hi), 0)).rgb;
    float3 mn = min(min(min(d, e), min(f, b)), hh);
    const float3 mn2 = min(min(mn, a), min(min(c, g), i));
    mn += mn2;
    float3 mx = max(max(max(d, e), max(f, b)), hh);
    const float3 mx2 = max(max(mx, a), max(max(c, g), i));
    mx += mx2;
    const float3 guard = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));
    const float3 amp = guard + (1.0 - guard) * saturate(lift); // Update 21: 101-200%
    const float3 wt = amp * peak;
    const float3 o = saturate((b * wt + d * wt + f * wt + hh * wt + e) / (1.0 + 4.0 * wt));
    return float4(lerp(o, toLinear(o), linearOut), centre.a);
}
)";
bool failSharpen(uint32_t code) {
    sharpenFault = code;
    sharpenPercent = 0; // the copy from now on; the slider shows UNAVAILABLE
    sharp.ready = false;
    sharpening = false;
    return false;
}
// The pass, for stagedEyes into whichever headset images copyLatest is given
// (their target views are written each frame). Submission and recording held.
bool prepareSharpen() {
    if (sharpenFault)
        return false;
    if (sharp.ready)
        return true;
    for (unsigned i = 0; i < 2; ++i)
        if (!stagedEyes[i] || (stagedEyes[i]->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            return failSharpen(5);
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 4;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> signature, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error)) ||
        FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                           IID_PPV_ARGS(&sharp.root))))
        return failSharpen(1);
    ComPtr<ID3DBlob> vs, ps;
    const char* shader = sharpenShader;
    if (FAILED(D3DCompile(shader, std::strlen(shader), "spidy_eye_sharpen", nullptr, nullptr, "vs", "vs_5_0",
                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &error)) ||
        FAILED(D3DCompile(shader, std::strlen(shader), "spidy_eye_sharpen", nullptr, nullptr, "ps", "ps_5_0",
                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &error)))
        return failSharpen(2);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = sharp.root.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.SampleDesc.Count = 1;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    auto& blend = d.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&sharp.state))))
        return failSharpen(3);
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&sharp.srgb))))
        return failSharpen(3);
    D3D12_DESCRIPTOR_HEAP_DESC views{};
    views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    views.NumDescriptors = 2;
    views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC targetViews{};
    targetViews.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    targetViews.NumDescriptors = 2;
    if (FAILED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&sharp.views))) ||
        FAILED(device->CreateDescriptorHeap(&targetViews, IID_PPV_ARGS(&sharp.targets))))
        return failSharpen(4);
    const auto viewStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (unsigned i = 0; i < 2; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        auto view = sharp.views->GetCPUDescriptorHandleForHeapStart();
        view.ptr += static_cast<SIZE_T>(viewStep) * i;
        device->CreateShaderResourceView(stagedEyes[i].Get(), &sv, view);
    }
    // The GPU time is optional, as the AA pass's.
    UINT64 frequency{};
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2 * static_cast<UINT>(presentCommands.size());
    D3D12_HEAP_PROPERTIES readback{};
    readback.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = sizeof(uint64_t) * qd.Count;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped{};
    if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency &&
        SUCCEEDED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&sharp.queries))) &&
        SUCCEEDED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&sharp.stamps))) &&
        SUCCEEDED(sharp.stamps->Map(0, nullptr, &mapped))) {
        sharp.stampData = static_cast<const volatile uint64_t*>(mapped);
        sharp.ticksPerMs = static_cast<double>(frequency) / 1000.0;
    } else {
        sharp.queries.Reset();
        sharp.stamps.Reset();
    }
    sharp.ready = true;
    return true;
}
void noteSharpenTime(size_t slot) {
    if (slot >= sharp.timed.size() || !sharp.timed[slot] || !sharp.stampData)
        return;
    sharp.timed[slot] = false;
    const uint64_t begin = sharp.stampData[2 * slot], end = sharp.stampData[2 * slot + 1];
    if (end > begin) {
        std::lock_guard lock(copyTimingLock);
        sharpenGpu.add(static_cast<double>(end - begin) / sharp.ticksPerMs);
    }
    if (end > begin)
        sharpenClock.note(static_cast<double>(end - begin) / sharp.ticksPerMs);
}
// The target view for a headset image: its own UNORM or UNORM_SRGB, the
// UNORM one for a typeless image; UNKNOWN when it cannot be drawn to.
DXGI_FORMAT sharpenTarget(const D3D12_RESOURCE_DESC& desc) {
    if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || desc.MipLevels > 1 ||
        desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1)
        return DXGI_FORMAT_UNKNOWN;
    switch (desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
// Both staged eyes, sharpened, into the headset's images (RENDER_TARGET, as
// the copy leaves them). stagedEyes leave and re-enter COPY_SOURCE. False
// (nothing recorded) when an image cannot be drawn to: then the copy.
bool drawSharpen(ID3D12GraphicsCommandList* list, size_t slot, ID3D12Resource* const* incoming, uint32_t percent,
                 uint32_t debug) {
    DXGI_FORMAT formats[2]{};
    for (unsigned i = 0; i < 2; ++i)
        if ((formats[i] = sharpenTarget(incoming[i]->GetDesc())) == DXGI_FORMAT_UNKNOWN)
            return failSharpen(6);
    if (formats[0] != formats[1])
        return failSharpen(6);
    const auto first = static_cast<UINT>(2 * slot);
    if (sharp.queries)
        originalEndQuery(list, sharp.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first);
    D3D12_RESOURCE_BARRIER b[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition = {stagedEyes[i].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    }
    originalBarrier(list, 2, b);
    const bool srgb = formats[0] == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const float frame[4] = {eye_sharpen::peak(eye_sharpen::sharpness(percent)), srgb ? 1.f : 0.f,
                            eye_sharpen::boost(percent), 0};
    list->SetPipelineState(srgb ? sharp.srgb.Get() : sharp.state.Get());
    list->SetGraphicsRootSignature(sharp.root.Get());
    ID3D12DescriptorHeap* heaps[] = {sharp.views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRoot32BitConstants(1, 4, frame, 0);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(outWidth), static_cast<float>(outHeight), 0, 1};
    const D3D12_RECT rect{0, 0, static_cast<LONG>(outWidth), static_cast<LONG>(outHeight)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &rect);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto viewStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto targetStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (unsigned i = 0; i < 2; ++i) {
        // Render target views are read when recorded: rewriting them next
        // frame, for that frame's images, is safe.
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = formats[i];
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        auto target = sharp.targets->GetCPUDescriptorHandleForHeapStart();
        target.ptr += static_cast<SIZE_T>(targetStep) * i;
        device->CreateRenderTargetView(incoming[i], &rv, target);
        auto view = sharp.views->GetGPUDescriptorHandleForHeapStart();
        // Update 24: eye_debug same reads the left eye's picture for both.
        view.ptr += static_cast<UINT64>(viewStep) * eye_debug::eyeDebugSource(debug, i);
        list->SetGraphicsRootDescriptorTable(0, view);
        list->OMSetRenderTargets(1, &target, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
    }
    for (auto& barrier : b)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    originalBarrier(list, 2, b);
    if (sharp.queries) {
        originalEndQuery(list, sharp.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
        list->ResolveQueryData(sharp.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2, sharp.stamps.Get(),
                               sizeof(uint64_t) * first);
        sharp.timed[slot] = true;
    }
    return true;
}
// Eye history (native_eye_gpu.hpp, setEyeHistory; eye_history.hpp): each new
// staged pair blended with the one before it, turned to the new pair's view.
// Built on the first pair that wants it, under the submission and recording
// locks, like the AA pass.
std::atomic<uint32_t> historyPercent{}, historyFault{};
std::mutex viewLock;
struct NotedViews {
    uint64_t serial{};
    std::array<eye_history::EyeView, 2> eyes{};
};
std::array<NotedViews, 32> notedViews{}; // viewLock
unsigned nextNoted{};                    // viewLock
struct HistoryPipeline {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> state;
    ComPtr<ID3D12DescriptorHeap> views, targets;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> stamps;
    const volatile uint64_t* stampData{};
    double ticksPerMs{};
    std::array<bool, 3> timed{};
    // Per eye, the picture so far and the one being drawn (parity is read).
    std::array<std::array<ComPtr<ID3D12Resource>, 2>, 2> pictures;
    unsigned parity{};
    bool ready{}, valid{}; // valid: pictures[*][parity] hold `shown`
    std::array<eye_history::EyeView, 2> shown{};
} history;
const char* historyShader = R"(
Texture2D current : register(t0);
Texture2D previous : register(t1);
SamplerState bilinear : register(s0);
cbuffer Frame : register(b0) { float4 row0; float4 row1; float4 row2; float4 extra; };
float4 vs(uint id : SV_VertexID) : SV_POSITION {
    const float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 position : SV_POSITION) : SV_TARGET {
    uint w, h;
    current.GetDimensions(w, h);
    const int2 p = int2(position.xy);
    const int2 last = int2(w, h) - 1;
    const float4 here = current.Load(int3(p, 0));
    float3 lo = here.rgb, hi = here.rgb;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float3 n = current.Load(int3(clamp(p + int2(x, y), int2(0, 0), last), 0)).rgb;
            lo = min(lo, n);
            hi = max(hi, n);
        }
    const float3 uv = float3(position.xy * extra.yz, 1);
    const float3 q = float3(dot(row0.xyz, uv), dot(row1.xyz, uv), dot(row2.xyz, uv));
    float keep = extra.x;
    float2 old = float2(-1, -1);
    if (q.z > 1e-5)
        old = q.xy / q.z;
    if (keep <= 0 || any(old < 0) || any(old > 1))
        return here;
    // Update 21: extra.w widens the clamp (eye_history.hpp historyLoosen).
    const float3 give = (hi - lo) * saturate(extra.w);
    const float3 before = clamp(previous.SampleLevel(bilinear, old, 0).rgb, max(lo - give, 0), hi + give);
    return float4(lerp(here.rgb, before, keep), here.a);
}
)";
bool failHistory(uint32_t code) {
    historyFault = code;
    history.ready = false;
    std::lock_guard lock(copyTimingLock);
    SpidyEyeHistoryData.fault = code;
    return false;
}
bool prepareHistory() {
    if (historyFault)
        return false;
    if (history.ready)
        return true;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 16;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> signature, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error)) ||
        FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                           IID_PPV_ARGS(&history.root))))
        return failHistory(1);
    ComPtr<ID3DBlob> vs, ps;
    const auto length = std::strlen(historyShader);
    if (FAILED(D3DCompile(historyShader, length, "spidy_eye_history", nullptr, nullptr, "vs", "vs_5_0",
                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &error)) ||
        FAILED(D3DCompile(historyShader, length, "spidy_eye_history", nullptr, nullptr, "ps", "ps_5_0",
                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &error)))
        return failHistory(2);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = history.root.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    auto& blend = d.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&history.state))))
        return failHistory(3);
    // Two pictures an eye, the headset's size, in the staged images' format.
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned k = 0; k < 2; ++k) {
            auto desc = stagedEyes[i]->GetDesc();
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                       IID_PPV_ARGS(&history.pictures[i][k]))))
                return failHistory(5);
        }
    // Views: for eye i read from picture k, the table at (2i + k) * 2 holds
    // the staged image (t0) and that picture (t1). Targets: picture (i, k) at 2i + k.
    D3D12_DESCRIPTOR_HEAP_DESC views{};
    views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    views.NumDescriptors = 8;
    views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC targetViews{};
    targetViews.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    targetViews.NumDescriptors = 4;
    if (FAILED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&history.views))) ||
        FAILED(device->CreateDescriptorHeap(&targetViews, IID_PPV_ARGS(&history.targets))))
        return failHistory(4);
    const auto viewStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto targetStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned k = 0; k < 2; ++k) {
            auto view = history.views->GetCPUDescriptorHandleForHeapStart();
            view.ptr += static_cast<SIZE_T>(viewStep) * ((2 * i + k) * 2);
            device->CreateShaderResourceView(stagedEyes[i].Get(), &sv, view);
            view.ptr += viewStep;
            device->CreateShaderResourceView(history.pictures[i][k].Get(), &sv, view);
            auto target = history.targets->GetCPUDescriptorHandleForHeapStart();
            target.ptr += static_cast<SIZE_T>(targetStep) * (2 * i + k);
            device->CreateRenderTargetView(history.pictures[i][k].Get(), &rv, target);
        }
    UINT64 frequency{};
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2 * static_cast<UINT>(captureCommands.size());
    D3D12_HEAP_PROPERTIES readback{};
    readback.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = sizeof(uint64_t) * qd.Count;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped{};
    if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency &&
        SUCCEEDED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&history.queries))) &&
        SUCCEEDED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&history.stamps))) &&
        SUCCEEDED(history.stamps->Map(0, nullptr, &mapped))) {
        history.stampData = static_cast<const volatile uint64_t*>(mapped);
        history.ticksPerMs = static_cast<double>(frequency) / 1000.0;
    } else {
        history.queries.Reset();
        history.stamps.Reset();
    }
    history.ready = true;
    history.valid = false;
    return true;
}
void noteHistoryTime(size_t slot) {
    if (!history.timed[slot] || !history.stampData)
        return;
    history.timed[slot] = false;
    const uint64_t begin = history.stampData[2 * slot], end = history.stampData[2 * slot + 1];
    if (end > begin) {
        std::lock_guard lock(copyTimingLock);
        SpidyEyeHistoryData.gpuMs += static_cast<double>(end - begin) / history.ticksPerMs;
        ++SpidyEyeHistoryData.timed;
    }
    if (end > begin)
        historyClock.note(static_cast<double>(end - begin) / history.ticksPerMs);
}
bool findViews(uint64_t serial, std::array<eye_history::EyeView, 2>& out) {
    std::lock_guard lock(viewLock);
    for (const auto& noted : notedViews)
        if (noted.serial == serial && serial) {
            out = noted.eyes;
            return true;
        }
    return false;
}
// The staged pair (in COPY_SOURCE) blended with the history, turned to its
// view: drawn into the other picture, which is copied back into the staged
// image and kept for the next pair.
void drawHistory(ID3D12GraphicsCommandList* list, size_t slot, uint64_t serial) {
    std::array<eye_history::EyeView, 2> now{};
    const bool viewed = findViews(serial, now);
    const uint32_t percent = historyPercent.load();
    float frame[2][16]{};
    bool blended = false;
    for (unsigned i = 0; i < 2; ++i) {
        std::array<float, 9> h{};
        const bool both = viewed && history.valid &&
                          eye_history::historyReprojection(history.shown[i], now[i], h);
        const float turn = both ? eye_history::historyTurn(history.shown[i], now[i]) : 0;
        // Update 23: less of the old picture, held less loosely, while the head turns.
        const float keep = eye_history::turnedWeight(eye_history::historyWeight(percent, both, turn), turn);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                frame[i][r * 4 + c] = h[r * 3 + c];
        frame[i][12] = keep;
        frame[i][13] = 1.f / static_cast<float>(outWidth);
        frame[i][14] = 1.f / static_cast<float>(outHeight);
        frame[i][15] = eye_history::turnedLoosen(eye_history::historyLoosen(percent), turn);
        blended = blended || keep > 0;
    }
    const unsigned read = history.parity, write = 1 - read;
    const auto first = static_cast<UINT>(2 * slot);
    if (history.queries)
        originalEndQuery(list, history.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first);
    D3D12_RESOURCE_BARRIER b[4]{};
    auto transition = [&](unsigned n, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[n].Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    };
    for (unsigned i = 0; i < 2; ++i) {
        transition(2 * i, stagedEyes[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(2 * i + 1, history.pictures[i][write].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    originalBarrier(list, 4, b);
    list->SetPipelineState(history.state.Get());
    list->SetGraphicsRootSignature(history.root.Get());
    ID3D12DescriptorHeap* heaps[] = {history.views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(outWidth), static_cast<float>(outHeight), 0, 1};
    const D3D12_RECT rect{0, 0, static_cast<LONG>(outWidth), static_cast<LONG>(outHeight)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &rect);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto viewStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto targetStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (unsigned i = 0; i < 2; ++i) {
        auto view = history.views->GetGPUDescriptorHandleForHeapStart();
        view.ptr += static_cast<UINT64>(viewStep) * ((2 * i + read) * 2);
        auto target = history.targets->GetCPUDescriptorHandleForHeapStart();
        target.ptr += static_cast<SIZE_T>(targetStep) * (2 * i + write);
        list->SetGraphicsRootDescriptorTable(0, view);
        list->SetGraphicsRoot32BitConstants(1, 16, frame[i], 0);
        list->OMSetRenderTargets(1, &target, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
    }
    for (unsigned i = 0; i < 2; ++i) {
        transition(2 * i, stagedEyes[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        transition(2 * i + 1, history.pictures[i][write].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    originalBarrier(list, 4, b);
    for (unsigned i = 0; i < 2; ++i)
        list->CopyResource(stagedEyes[i].Get(), history.pictures[i][write].Get());
    for (unsigned i = 0; i < 2; ++i) {
        transition(2 * i, stagedEyes[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(2 * i + 1, history.pictures[i][write].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    originalBarrier(list, 4, b);
    if (history.queries) {
        originalEndQuery(list, history.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
        list->ResolveQueryData(history.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2, history.stamps.Get(),
                               sizeof(uint64_t) * first);
        history.timed[slot] = true;
    }
    history.parity = write;
    // Without its view the picture is still kept, but it cannot be turned to
    // the next pair's: that one starts again.
    history.valid = viewed;
    history.shown = now;
    std::lock_guard lock(copyTimingLock);
    if (blended)
        ++SpidyEyeHistoryData.blended;
    else
        ++SpidyEyeHistoryData.restarted;
    if (!viewed)
        ++SpidyEyeHistoryData.unviewed;
}
void stagePair(EyePairState::Stamp stamp) {
    if (!known[0] || !known[1] || !makeReadbacks())
        return;
    for (unsigned i = 0; i < 2; ++i)
        if (!stagedEyes[i]) {
            auto desc = resources[i]->GetDesc();
            // A render target too, for the AA pass (drawAa); the headset's
            // size, which is smaller than the eye's when it is supersampled.
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            desc.Width = outWidth;
            desc.Height = outHeight;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                                       IID_PPV_ARGS(&stagedEyes[i])))) {
                fault = 3805;
                return;
            }
        }
    auto* slot = readyCommands(captureCommands);
    if (!slot)
        return;
    const auto index = static_cast<size_t>(slot - captureCommands.data());
    noteAaTime(index);
    noteHistoryTime(index);
    // Supersampled eyes cannot be copied: they are always filtered down,
    // without FXAA (the extra samples already smooth the edges).
    const bool down = downsampling();
    const bool smooth = (down || aaMode.load() == 1) && prepareAa();
    if (down && !smooth) {
        fault = 3806; // the headset would get nothing: say why (aaFault)
        return;
    }
    if (smooth)
        drawAa(slot->list.Get(), index);
    else
        for (unsigned i = 0; i < 2; ++i)
            copyEyeTexture(slot->list.Get(), resources[i].Get(), states[i], stagedEyes[i].Get(), originalBarrier,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
    // The history after AA: it blends what the headset is shown.
    if (historyPercent.load() && prepareHistory())
        drawHistory(slot->list.Get(), index, stamp.serial);
    else
        history.valid = false;
    if (!submitCommands(*slot)) {
        history.valid = false;
        return;
    }
    aaPairs += smooth;
    capturedStamp = stamp;
    capturedMs = GetTickCount64();
    capturedQpc = qpcNow();
    ++capturedPairs;
}
void captureImage(ID3D12GraphicsCommandList* list, ID3D12Resource* image, unsigned eye) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_COPY_SOURCE};
    originalBarrier(list, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = image;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readbacks[eye].Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = layouts[eye];
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    originalBarrier(list, 1, &b);
}
void capturePair(EyePairState::Stamp stamp) {
    if (!known[0] || !known[1] || !makeReadbacks())
        return;
    if (copyToXr)
        for (unsigned i = 0; i < 2; ++i) {
            if (!targets[i]) {
                fault = 3305;
                return;
            }
            const auto destination = targets[i]->GetDesc();
            if (destination.Width != width || destination.Height != height) {
                fault = 3306;
                return;
            }
        }
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        fault = 3301;
        return;
    }
    if (completed < fenceValue)
        return; // previous allocator and readbacks still in use
    if (FAILED(allocator->Reset()) || FAILED(originalReset(command.Get(), allocator.Get(), nullptr))) {
        fault = 3302;
        return;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (copyToXr) {
            copyEyeTexture(command.Get(), resources[i].Get(), states[i], targets[i].Get(), originalBarrier);
        }
        if (!captureImages)
            continue;
        // Read the acquired XR image after its copy, so diagnostics show the
        // actual submitted texture rather than another native render generation.
        auto* captured = copyToXr ? targets[i].Get() : resources[i].Get();
        const auto capturedState = copyToXr ? D3D12_RESOURCE_STATE_RENDER_TARGET : states[i];
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {captured, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, capturedState,
                        D3D12_RESOURCE_STATE_COPY_SOURCE};
        if (capturedState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            originalBarrier(command.Get(), 1, &b);
        {
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = captured;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = readbacks[i].Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = layouts[i];
            command->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        if (capturedState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            originalBarrier(command.Get(), 1, &b);
    }
    if (FAILED(command->Close())) {
        fault = 3303;
        return;
    }
    ID3D12CommandList* list = command.Get();
    originalExecute(queue.Get(), 1, &list);
    if (FAILED(queue->Signal(fence.Get(), ++fenceValue))) {
        fault = 3304;
        return;
    }
    ++pairs;
    lastSerial = stamp.serial;
    lastGeneration = stamp.generation;
    if (copyToXr)
        requestFence = fenceValue;
    copyReady.notify_all();
}
void STDMETHODCALLTYPE execute(ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* commands) {
    if (!enabled) {
        originalExecute(q, count, commands);
        return;
    }
    // Runtime/overlay queues must not serialize behind the game submission lock.
    if (q != queue.Get()) {
        originalExecute(q, count, commands);
        std::lock_guard recordLock(recording);
        for (UINT n = 0; n < count; ++n)
            if (auto found = lists.find(commands[n]); found != lists.end())
                for (const auto& e : found->second) {
                    if (e.eye == hudEvent) {
                        hudKnown = false; // its state now changes off the game's queue
                        continue;
                    }
                    fault = 3401;
                    copyReady.notify_all();
                    break;
                }
        return;
    }
    std::lock_guard submitLock(submission);
    if (pendingCount && fence)
        noteCopies(fence->GetCompletedValue());
    originalExecute(q, count, commands);
    std::lock_guard recordLock(recording);
    ++executes;
    bool relevant{};
    for (UINT n = 0; n < count; ++n)
        if (auto found = lists.find(commands[n]); found != lists.end())
            for (const auto& e : found->second) {
                if (e.eye == hudEvent) {
                    // The HUD's: the game's own word on its state from here on in the queue. A split
                    // barrier or one subresource leaves it unknown (it has one).
                    if (q != queue.Get() || !hudResource || e.transition.pResource != hudResource.Get())
                        continue;
                    hudKnown = e.flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                               (e.transition.Subresource == 0 ||
                                e.transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
                    hudState = e.transition.StateAfter;
                    continue;
                }
                relevant = true;
                if (q != queue.Get()) {
                    fault = 3401;
                    continue;
                }
                if (e.marker) {
                    if (e.ended) {
                        pair.end(e.eye, e.stamp);
                        ++ends[e.eye];
                    } else {
                        pair.begin(e.eye, e.stamp);
                        ++begins[e.eye];
                    }
                } else {
                    if (e.flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
                        (e.transition.Subresource != 0 &&
                         e.transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)) {
                        fault = 3402;
                        known[e.eye] = false;
                        continue;
                    }
                    if (known[e.eye] && states[e.eye] != e.transition.StateBefore) {
                        fault = 3403;
                        known[e.eye] = false;
                        continue;
                    }
                    states[e.eye] = e.transition.StateAfter;
                    known[e.eye] = true;
                }
            }
    if (!relevant)
        return;
    const auto ready = pair.ready();
    if (staged) {
        if (enabled && !fault && GetTickCount64() < deadline && ready.serial &&
            ready.generation > capturedStamp.generation)
            stagePair(ready);
    } else if (enabled && !fault && GetTickCount64() < deadline && ready.serial &&
               ready.generation > lastGeneration &&
               (!copyToXr || (requestedSerial == ready.serial && !requestFence)))
        capturePair(ready);
    publish();
    if (fault)
        copyReady.notify_all();
}
DXGI_FORMAT typelessFamily(DXGI_FORMAT view) {
    switch (view) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    default:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    }
}
// Present thread: copies the back buffer about to be shown on the game's
// render queue, after the frame it rendered there and before the queue renders
// into that buffer again. The swapchain reports the queue it was created with,
// which with Streamline is a wrapper of that queue, so the game's swapchain is
// recognized by its back buffer's device.
ScreenSkip copyScreen(IDXGISwapChain* swapchain) {
    ComPtr<IDXGISwapChain3> chain;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&chain))) ||
        FAILED(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer))))
        return screenNoBuffer;
    ComPtr<ID3D12Device> owner;
    ComPtr<IUnknown> ownerIdentity, gameIdentity;
    if (FAILED(buffer->GetDevice(IID_PPV_ARGS(&owner))) || FAILED(owner.As(&ownerIdentity)) ||
        FAILED(device.As(&gameIdentity)) || ownerIdentity.Get() != gameIdentity.Get())
        return screenOtherDevice;
    const auto desc = buffer->GetDesc();
    backBufferFormat = desc.Format;
    bool linear{};
    const auto view = screenView(desc.Format, linear);
    if (view == DXGI_FORMAT_UNKNOWN)
        return screenFormat;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || !validEyeSize(desc.Width) || !validEyeSize(desc.Height))
        return screenShape;
    std::lock_guard submitLock(submission);
    std::lock_guard recordLock(recording);
    if (!enabled || fault || GetTickCount64() >= deadline)
        return screenStopped;
    const auto now = GetTickCount64();
    std::erase_if(retiredScreens, [&](const Retired& old) { return now >= old.releaseMs; });
    if (!screen.texture || screen.width != desc.Width || screen.height != desc.Height || screen.view != view) {
        if (screen.texture)
            retiredScreens.push_back({screen.texture, now + 3000});
        screen = {};
        auto copy = desc;
        copy.Format = typelessFamily(view);
        copy.Flags = D3D12_RESOURCE_FLAG_NONE;
        copy.Alignment = 0;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&screen.texture))))
            return screenTexture;
        screen.view = view;
        screen.linear = linear;
        screen.width = static_cast<uint32_t>(desc.Width);
        screen.height = desc.Height;
    }
    auto* slot = readyCommands(screenCommands);
    if (!slot)
        return screenBusy; // earlier copies still in flight: skip this frame
    D3D12_RESOURCE_BARRIER b[2]{};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {buffer.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_SOURCE};
    b[1].Transition = {screen.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
    originalBarrier(slot->list.Get(), 2, b);
    slot->list->CopyResource(screen.texture.Get(), buffer.Get());
    for (auto& barrier : b)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    originalBarrier(slot->list.Get(), 2, b);
    if (!submitCommands(*slot))
        return screenStopped;
    ++screen.frame;
    screenMs = now;
    return screenCopied;
}
// The pipeline that draws a part of one texture over a whole target of
// `format`: a screen-covering triangle and one linear sample (submission lock).
bool prepareMirror(DXGI_FORMAT format) {
    auto& m = mirrorPipeline;
    if (m.failed)
        return false;
    if (m.state && m.format == format)
        return true;
    m.failed = true; // cleared at the end: a failure is not tried again every frame
    if (!m.root) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = {1, &range};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants.Num32BitValues = 8;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> signature, error;
        D3D12_DESCRIPTOR_HEAP_DESC views{};
        views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        views.NumDescriptors = 1;
        views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        D3D12_DESCRIPTOR_HEAP_DESC targetsHeap{};
        targetsHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        targetsHeap.NumDescriptors = 1;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error)) ||
            FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                               IID_PPV_ARGS(&m.root))) ||
            FAILED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&m.source))) ||
            FAILED(device->CreateDescriptorHeap(&targetsHeap, IID_PPV_ARGS(&m.target)))) {
            m.root.Reset();
            return false;
        }
    }
    // Both images hold display-encoded values and neither view is sRGB, so
    // what is sampled is what is stored.
    const char* shader = R"(
Texture2D source : register(t0);
SamplerState scaled : register(s0);
cbuffer Part : register(b0) { float2 targetPixel; float2 offset; float2 scale; float2 unused; };
float4 vs(uint id : SV_VertexID) : SV_POSITION {
    const float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 position : SV_POSITION) : SV_TARGET {
    return float4(source.SampleLevel(scaled, offset + position.xy * targetPixel * scale, 0).rgb, 1);
}
)";
    ComPtr<ID3DBlob> vs, ps, error;
    if (FAILED(D3DCompile(shader, std::strlen(shader), "spidy_mirror", nullptr, nullptr, "vs", "vs_5_0",
                          D3DCOMPILE_ENABLE_STRICTNESS, 0, &vs, &error)) ||
        FAILED(D3DCompile(shader, std::strlen(shader), "spidy_mirror", nullptr, nullptr, "ps", "ps_5_0",
                          D3DCOMPILE_ENABLE_STRICTNESS, 0, &ps, &error)))
        return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = m.root.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = format;
    d.SampleDesc.Count = 1;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    auto& blend = d.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    m.state.Reset();
    if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&m.state))))
        return false;
    m.format = format;
    m.failed = false;
    return true;
}
// Present thread: the newest left eye over the back buffer about to be shown,
// on the game's queue. While the game does not render its own view (the stereo
// module's views option 32), nothing else draws there, and the window would
// show whichever old frames its buffers still hold. The eye fills the window's
// width or height and is cut to its shape about the middle; it is not
// stretched. A float (HDR) back buffer is left alone. The eye is the one the
// headset showed, Spidy's drawings on it, while a copy of that is fresh
// (keepShownEye), and the game's own image of it otherwise.
void mirrorEye(IDXGISwapChain* swapchain) {
    ComPtr<IDXGISwapChain3> chain;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&chain))) ||
        FAILED(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer))))
        return;
    ComPtr<ID3D12Device> owner;
    ComPtr<IUnknown> ownerIdentity, gameIdentity;
    if (FAILED(buffer->GetDevice(IID_PPV_ARGS(&owner))) || FAILED(owner.As(&ownerIdentity)) ||
        FAILED(device.As(&gameIdentity)) || ownerIdentity.Get() != gameIdentity.Get())
        return;
    const auto desc = buffer->GetDesc();
    bool linear{};
    const auto view = screenView(desc.Format, linear);
    if (view == DXGI_FORMAT_UNKNOWN || linear || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.SampleDesc.Count != 1 || !desc.Width || !desc.Height)
        return;
    std::lock_guard submitLock(submission);
    std::lock_guard recordLock(recording);
    const auto now = GetTickCount64();
    if (!enabled || fault || now >= deadline || !staged || !stagedEyes[0] || !capturedStamp.serial ||
        now - capturedMs > imageHoldMs || !width || !height || !prepareMirror(view))
        return;
    auto* slot = readyCommands(mirrorCommands);
    if (!slot)
        return; // earlier draws still in flight: this present keeps what the buffer holds
    // Update 16: while sharpening, the unsharpened stagedEyes[0] (the monitor
    // gets no sharpening and no copy is added for it).
    const bool shown = !sharpening && shownEye && shownEyeFresh(shownMs, now, mirrorRate.load());
    auto* eye = shown ? shownEye.Get() : stagedEyes[0].Get();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(eye, &sv, mirrorPipeline.source->GetCPUDescriptorHandleForHeapStart());
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = view;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const auto rtv = mirrorPipeline.target->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(buffer.Get(), &rv, rtv);
    D3D12_RESOURCE_BARRIER b[2]{};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {buffer.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_RENDER_TARGET};
    b[1].Transition = {eye, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    auto* list = slot->list.Get();
    originalBarrier(list, 2, b);
    // The part of the eye with the window's shape, about its middle.
    const float window = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
    const float image = static_cast<float>(outWidth) / static_cast<float>(outHeight);
    const float across = window < image ? window / image : 1, down = window < image ? 1 : image / window;
    const float part[8] = {1.f / static_cast<float>(desc.Width), 1.f / static_cast<float>(desc.Height),
                           (1 - across) / 2, (1 - down) / 2, across, down, 0, 0};
    list->SetPipelineState(mirrorPipeline.state.Get());
    list->SetGraphicsRootSignature(mirrorPipeline.root.Get());
    ID3D12DescriptorHeap* heaps[] = {mirrorPipeline.source.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(0, mirrorPipeline.source->GetGPUDescriptorHandleForHeapStart());
    list->SetGraphicsRoot32BitConstants(1, 8, part, 0);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
    const D3D12_RECT rect{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &rect);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    for (auto& barrier : b)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    originalBarrier(list, 2, b);
    if (submitCommands(*slot)) {
        ++mirrorCalls;
        shownMirrors += shown;
    }
    shownWanted = true;
}
// Present thread (MilesVR 0.1.80): the game's HUD texture copied on its queue, after the frame that drew
// it, in the state the game's submitted barriers left it and back to that state.
bool copyHud() {
    std::lock_guard submitLock(submission);
    std::lock_guard recordLock(recording);
    if (!enabled || fault || GetTickCount64() >= deadline || !hudResource || !hudKnown)
        return false;
    const auto desc = hudResource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || !validEyeSize(desc.Width) || !validEyeSize(desc.Height))
        return false;
    bool linear{};
    const auto view = screenView(desc.Format, linear);
    if (view == DXGI_FORMAT_UNKNOWN)
        return false;
    const auto now = GetTickCount64();
    std::erase_if(retiredHuds, [&](const Retired& old) { return now >= old.releaseMs; });
    if (!hud.texture || hud.width != desc.Width || hud.height != desc.Height || hud.view != view) {
        if (hud.texture)
            retiredHuds.push_back({hud.texture, now + 3000});
        hud = {};
        auto copy = desc;
        copy.Format = typelessFamily(view);
        copy.Flags = D3D12_RESOURCE_FLAG_NONE;
        copy.Alignment = 0;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&hud.texture))))
            return false;
        hud.view = view;
        hud.linear = linear;
        hud.width = static_cast<uint32_t>(desc.Width);
        hud.height = desc.Height;
    }
    auto* slot = readyCommands(hudCommands);
    if (!slot)
        return false;
    D3D12_RESOURCE_BARRIER b[2]{};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {hudResource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, hudState,
                       D3D12_RESOURCE_STATE_COPY_SOURCE};
    b[1].Transition = {hud.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
    // Already readable for a copy: only the copy's own texture moves.
    const bool move = hudState != D3D12_RESOURCE_STATE_COPY_SOURCE;
    originalBarrier(slot->list.Get(), move ? 2 : 1, move ? b : b + 1);
    slot->list->CopyResource(hud.texture.Get(), hudResource.Get());
    for (auto& barrier : b)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    originalBarrier(slot->list.Get(), move ? 2 : 1, move ? b : b + 1);
    if (!submitCommands(*slot))
        return false;
    ++hud.frame;
    hudMs = now;
    return true;
}
void captureScreen(IDXGISwapChain* swapchain) {
    ++screenPresents;
    screenSkipped = copyScreen(swapchain);
    publishScreen(0, nullptr, 0);
}
// Present reaches Present1 in some DXGI builds: one of them handles a frame.
thread_local bool presenting{};
void presentFrame(IDXGISwapChain* swapchain, UINT flags) {
    if (!enabled || (flags & DXGI_PRESENT_TEST))
        return;
    if (hudWanted)
        ++(copyHud() ? hudCopied : hudSkipped);
    if (screenWanted)
        captureScreen(swapchain);
    else if (mirrorWanted)
        mirrorEye(swapchain);
}
// The game presents once a frame whether or not it rendered a view of its
// own. While it does not, the window only mirrors the left eye, and a present
// costs GPU time the eyes could have: hold back those beyond the monitor's rate.
// The game goes on without them (it paces itself by its own fences), and a
// held present draws no mirror either.
bool holdPresent(UINT flags) {
    const auto rate = mirrorRate.load();
    if (!rate || !enabled || !mirrorWanted || screenWanted || (flags & DXGI_PRESENT_TEST))
        return false;
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    auto due = mirrorDue.load();
    if (spidy::monitorPresentDue(now.QuadPart, due, frequency.QuadPart, rate)) {
        mirrorDue = due;
        return false;
    }
    ++presentsHeld;
    return true;
}
HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* swapchain, UINT interval, UINT flags) {
    ++presentCalls;
    const bool outer = !presenting;
    if (outer && holdPresent(flags))
        return S_OK;
    if (outer) {
        presenting = true;
        presentFrame(swapchain, flags);
    }
    const auto result = originalPresent(swapchain, interval, flags);
    if (outer)
        presenting = false;
    return result;
}
HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* swapchain, UINT interval, UINT flags,
                                   const DXGI_PRESENT_PARAMETERS* parameters) {
    ++presentCalls;
    const bool outer = !presenting;
    if (outer && holdPresent(flags))
        return S_OK;
    if (outer) {
        presenting = true;
        presentFrame(swapchain, flags);
    }
    const auto result = originalPresent1(swapchain, interval, flags, parameters);
    if (outer)
        presenting = false;
    return result;
}
// Every swapchain in the process shares DXGI's Present methods. A throwaway
// 64-pixel swapchain on a hidden window, on a queue of its own, gives their
// addresses; the hooks act only on swapchains presenting on the game's queue.
bool presentEntries(void*& presentAddress, void*& present1Address) {
    using CreateFactory = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto dxgi = GetModuleHandleW(L"dxgi.dll");
    const auto create = dxgi ? reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
    if (!create)
        return false;
    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    type.lpszClassName = L"SpidyPresentEntries";
    RegisterClassExW(&type);
    const auto window = CreateWindowExW(0, type.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr,
                                        nullptr, type.hInstance, nullptr);
    if (!window)
        return false;
    bool found{};
    {
        ComPtr<IDXGIFactory2> factory;
        ComPtr<ID3D12CommandQueue> own;
        ComPtr<IDXGISwapChain1> chain;
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = d.Height = 64;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (SUCCEEDED(create(0, IID_PPV_ARGS(&factory))) && SUCCEEDED(device->CreateCommandQueue(&q, IID_PPV_ARGS(&own))) &&
            SUCCEEDED(factory->CreateSwapChainForHwnd(own.Get(), window, &d, nullptr, nullptr, &chain))) {
            const auto table = *reinterpret_cast<void***>(chain.Get());
            presentAddress = table[8];   // IDXGISwapChain::Present
            present1Address = table[22]; // IDXGISwapChain1::Present1
            found = true;
        }
    }
    DestroyWindow(window);
    return found;
}
} // namespace
DXGI_FORMAT screenView(DXGI_FORMAT backBuffer, bool& linear) {
    linear = false;
    switch (backBuffer) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        linear = true;
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
uint32_t presents() {
    return presentCalls.load();
}
void mirror(bool wanted) {
    mirrorWanted = wanted;
}
void monitorRate(uint32_t rate) {
    mirrorRate = rate;
}
uint32_t heldPresents() {
    return presentsHeld.load();
}
uint32_t mirrored() {
    return mirrorCalls.load();
}
uint32_t mirroredShown() {
    return shownMirrors.load();
}
bool videoMemory(uint64_t& budget, uint64_t& usage) {
    // The adapter behind the game's device, found once (the main thread asks).
    static ComPtr<IDXGIAdapter3> adapter;
    if (!adapter) {
        using CreateFactory = HRESULT(WINAPI*)(UINT, REFIID, void**);
        const auto dxgi = GetModuleHandleW(L"dxgi.dll");
        const auto create =
            dxgi ? reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
        ComPtr<IDXGIFactory4> factory;
        if (!create || !device || FAILED(create(0, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
            return false;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return false;
    budget = info.Budget;
    usage = info.CurrentUsage;
    return true;
}
void wantScreen(bool wanted) {
    screenWanted = wanted;
}
bool screenRequested() {
    return screenWantedFlag();
}
void registerHud(ID3D12Resource* resource) {
    if (!enabled || hudWatched.load() == resource)
        return;
    std::lock_guard lock(recording);
    if (resource) {
        ComPtr<ID3D12Device> owner;
        ComPtr<IUnknown> a, b;
        if (FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) || FAILED(owner.As(&a)) || FAILED(device.As(&b)) ||
            a.Get() != b.Get())
            resource = nullptr;
    }
    hudResource = resource;
    hudWatched = resource;
    hudKnown = false;
}
void wantHud(bool wanted) {
    hudWanted = wanted;
}
bool latestHud(Screen& out, uint64_t maxAgeMs) {
    std::lock_guard submitLock(submission);
    const auto now = GetTickCount64();
    if (!hud.texture || !hud.frame || now < hudMs || now - hudMs > maxAgeMs)
        return false;
    out = hud;
    return true;
}
uint32_t hudCopies() {
    return hudCopied.load();
}
uint32_t hudSkips() {
    return hudSkipped.load();
}
bool latestScreen(Screen& out, uint64_t maxAgeMs) {
    std::lock_guard submitLock(submission);
    const auto now = GetTickCount64();
    if (!screen.texture || !screen.frame || now < screenMs || now - screenMs > maxAgeMs)
        return false;
    out = screen;
    return true;
}
// Probes only: reads the newest copy back to memory, waiting for the GPU.
DWORD readScreen() {
    std::lock_guard submitLock(submission);
    std::lock_guard recordLock(recording);
    if (!enabled || fault || !screen.texture || !screen.frame)
        return 3951;
    const auto desc = screen.texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 bytes{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&readback))))
        return 3952;
    auto* slot = readyCommands(screenCommands);
    if (!slot)
        return 3953;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {screen.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE};
    originalBarrier(slot->list.Get(), 1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = screen.texture.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = layout;
    slot->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    originalBarrier(slot->list.Get(), 1, &b);
    if (!submitCommands(*slot))
        return 3954;
    if (fence->GetCompletedValue() < fenceValue &&
        (FAILED(fence->SetEventOnCompletion(fenceValue, fenceEvent)) ||
         WaitForSingleObject(fenceEvent, 2000) != WAIT_OBJECT_0))
        return 3955;
    void* data{};
    if (FAILED(readback->Map(0, nullptr, &data)))
        return 3956;
    const auto rowBytes = static_cast<uint32_t>(desc.Width * (screen.view == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4));
    screenPixels.resize(static_cast<size_t>(rowBytes) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(screenPixels.data() + static_cast<size_t>(y) * rowBytes,
                    static_cast<uint8_t*>(data) + layout.Offset + static_cast<size_t>(y) * layout.Footprint.RowPitch,
                    rowBytes);
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    publishScreen(1, screenPixels.data(), rowBytes);
    return 0;
}
void enterMarker(unsigned eye, uint64_t serial, uint64_t generation, bool ended) {
    currentMarker = {eye, {serial, generation}, ended, eye < 2};
}
void leaveMarker() {
    currentMarker = {};
}
void copyTiming(TimingMetric& gpu, TimingMetric& pair) {
    std::lock_guard lock(copyTimingLock);
    gpu = copyGpu;
    pair = copyPair;
}
uint32_t error() {
    return fault.load();
}
std::atomic<uint32_t> supersamplePercent{100};
bool setSupersample(uint32_t percent) {
    if (percent < minimumSupersample || percent > maximumSupersample || enabled)
        return false;
    supersamplePercent = percent;
    return true;
}
uint32_t supersample() {
    return supersamplePercent.load();
}
void setEyeOutput(uint32_t outputWidth, uint32_t outputHeight) {
    wantedOutWidth = outputWidth;
    wantedOutHeight = outputHeight;
}
bool setEyeAA(uint32_t mode) {
    if (mode > 1)
        return false;
    aaMode = staged ? mode : 0;
    return true;
}
// Update 15: the wrist hologram's GRAPHICS page reads what is in force.
uint32_t eyeAA() {
    return aaMode.load();
}
uint32_t eyeHistory() {
    return historyPercent.load();
}
bool setEyeSharpen(uint32_t percent) {
    if (percent > eye_sharpen::maximumPercent || (percent && sharpenFault))
        return false;
    sharpenPercent = staged ? percent : 0;
    if (!sharpenPercent)
        sharpening = false;
    return true;
}
bool setEyeDebug(uint32_t mode) {
    if (!eye_debug::validEyeDebug(mode))
        return false;
    eyeDebugMode = mode;
    return true;
}
uint32_t eyeDebug() {
    return eyeDebugMode.load();
}
uint32_t eyeSharpen() {
    return sharpenPercent.load();
}
uint32_t eyeSharpenFault() {
    return sharpenFault.load();
}
void eyeSharpenTiming(TimingMetric& gpu, uint32_t& percent, uint32_t& code, uint64_t& sharpened) {
    {
        std::lock_guard lock(copyTimingLock);
        gpu = sharpenGpu;
    }
    percent = sharpenPercent.load();
    code = sharpenFault.load();
    sharpened = sharpenPairs.load();
}
float eyePassesGpuMs() {
    const uint64_t now = GetTickCount64();
    float total = 0;
    bool known = true, any = false;
    const auto add = [&](bool on, const PassClock& clock) {
        if (!on)
            return;
        any = true;
        const uint64_t tick = clock.tick.load();
        if (!tick || now - tick > 1000)
            known = false;
        else
            total += clock.ms.load();
    };
    add(staged && !aaFault.load() && (downsampling() || aaMode.load() == 1), aaClock);
    add(staged && !historyFault.load() && historyPercent.load(), historyClock);
    add(sharpening.load(), sharpenClock);
    return !any ? 0.f : known ? total : std::numeric_limits<float>::quiet_NaN();
}
uint32_t monitorRateNow() {
    return mirrorRate.load();
}
bool setEyeHistory(uint32_t percent) {
    if (percent > eye_history::maximumPercent)
        return false;
    historyPercent = percent;
    std::lock_guard lock(copyTimingLock);
    SpidyEyeHistoryData.percent = percent;
    return true;
}
void noteEyeViews(uint64_t serial, const std::array<eye_history::EyeView, 2>& views) {
    std::lock_guard lock(viewLock);
    notedViews[nextNoted] = {serial, views};
    nextNoted = (nextNoted + 1) % notedViews.size();
}
void eyeAATiming(TimingMetric& gpu, uint32_t& mode, uint32_t& code, uint64_t& smoothed) {
    {
        std::lock_guard lock(copyTimingLock);
        gpu = aaGpu;
    }
    // 2: the supersampled eyes' downsample filter, whatever FXAA is set to.
    mode = width && downsampling() ? 2 : aaMode.load();
    code = aaFault.load();
    smoothed = aaPairs.load();
}
void registerEye(unsigned eye, ID3D12Resource* resource) {
    if (!enabled || eye >= 2 || !resource)
        return;
    // Submission first, as arm() takes them: a new eye image waits for Spidy's own copies.
    std::lock_guard submitLock(submission);
    std::lock_guard lock(recording);
    if (resources[eye].Get() == resource)
        return;
    ComPtr<ID3D12Device> owner;
    ComPtr<IUnknown> a, b;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) || FAILED(owner.As(&a)) || FAILED(device.As(&b)) ||
        a.Get() != b.Get()) {
        fault = 3502;
        return;
    }
    if (resources[eye]) {
        // SM2VR 0.0.57: the game made new eye images - a change on its graphics menu (the user, October
        // 23: "changing graphics settings" pushed them out of VR; 0.0.54's reports: fault 3501 right
        // after the game's "Finished enforcing display settings"). Of the same size and format the
        // bridge goes on with them: its own GPU work on the old one done, the AA view rewritten, the
        // old image's transitions still waiting in recorded lists dropped, its state learned afresh.
        // Another size still stops (the staged eyes, readbacks and passes are made for one).
        const auto was = resources[eye]->GetDesc(), now = resource->GetDesc();
        if (was.Width != now.Width || was.Height != now.Height || was.Format != now.Format ||
            was.Dimension != now.Dimension || was.MipLevels != now.MipLevels ||
            was.DepthOrArraySize != now.DepthOrArraySize || was.SampleDesc.Count != now.SampleDesc.Count ||
            (now.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) {
            fault = 3501;
            return;
        }
        if (fence && fence->GetCompletedValue() < fenceValue &&
            (FAILED(fence->SetEventOnCompletion(fenceValue, fenceEvent)) ||
             WaitForSingleObject(fenceEvent, 1000) != WAIT_OBJECT_0)) {
            fault = 3503;
            return;
        }
        for (auto& [list, events] : lists)
            events.erase(std::remove_if(events.begin(), events.end(),
                                        [&](const Event& e) { return !e.marker && e.eye == eye; }),
                         events.end());
        resources[eye] = resource;
        watched[eye] = resource;
        known[eye] = false;
        if (aa.ready)
            writeEyeView(eye);
        ++eyeReplacements;
        return;
    }
    resources[eye] = resource;
    watched[eye] = resource;
}
bool arm(uint64_t serial, ID3D12Resource* left, ID3D12Resource* right) {
    std::lock_guard submitLock(submission);
    std::lock_guard recordLock(recording);
    if (!enabled || !copyToXr || staged || fault || !serial || requestedSerial ||
        GetTickCount64() >= deadline)
        return false;
    ID3D12Resource* incoming[] = {left, right};
    for (unsigned i = 0; i < 2; ++i) {
        if (!incoming[i]) {
            fault = 3701;
            return false;
        }
        ComPtr<ID3D12Device> owner;
        ComPtr<IUnknown> a, b;
        if (FAILED(incoming[i]->GetDevice(IID_PPV_ARGS(&owner))) || FAILED(owner.As(&a)) ||
            FAILED(device.As(&b)) || a.Get() != b.Get()) {
            fault = 3702;
            return false;
        }
        const auto desc = incoming[i]->GetDesc();
        if (!rgba8CopyTarget(desc)) {
            fault = 3703;
            return false;
        }
        if (resources[i]) {
            const auto source = resources[i]->GetDesc();
            if (source.Width != desc.Width || source.Height != desc.Height ||
                resources[i].Get() == incoming[i]) {
                fault = 3704;
                return false;
            }
        }
    }
    targets = {left, right};
    requestedSerial = serial;
    requestFence = 0;
    return true;
}
bool copyLatest(ID3D12Resource* left, ID3D12Resource* right, uint64_t& serial, uint64_t& generation) {
    serial = generation = 0;
    std::lock_guard submitLock(submission);
    std::lock_guard recordLock(recording);
    if (!enabled || !staged || fault || !capturedStamp.serial || GetTickCount64() >= deadline ||
        GetTickCount64() - capturedMs > imageHoldMs)
        return false;
    ID3D12Resource* incoming[] = {left, right};
    for (unsigned i = 0; i < 2; ++i) {
        if (!incoming[i]) {
            fault = 3701;
            return false;
        }
        const auto desc = incoming[i]->GetDesc();
        if (!rgba8CopyTarget(desc) || desc.Width != outWidth || desc.Height != outHeight) {
            fault = 3703;
            return false;
        }
        ComPtr<ID3D12Device> owner;
        ComPtr<IUnknown> a, b;
        if (FAILED(incoming[i]->GetDevice(IID_PPV_ARGS(&owner))) || FAILED(owner.As(&a)) ||
            FAILED(device.As(&b)) || a.Get() != b.Get()) {
            fault = 3702;
            return false;
        }
    }
    auto* slot = readyCommands(presentCommands);
    if (!slot)
        return false;
    const auto slotIndex = static_cast<size_t>(slot - presentCommands.data());
    noteSharpenTime(slotIndex);
    // Update 16: the headset's images sharpened (in place of the copy), or
    // copied as before when sharpening is off or cannot run.
    const uint32_t percent = sharpenPercent.load();
    // Update 24: read once, so both images of this pair follow the same mode.
    const uint32_t debug = eyeDebugMode.load();
    const bool sharpened =
        percent && prepareSharpen() && drawSharpen(slot->list.Get(), slotIndex, incoming, percent, debug);
    for (unsigned i = 0; i < 2; ++i) {
        if (!sharpened)
            copyEyeTexture(slot->list.Get(), stagedEyes[eye_debug::eyeDebugSource(debug, i)].Get(),
                           D3D12_RESOURCE_STATE_COPY_SOURCE, incoming[i], originalBarrier);
        if (captureImages)
            captureImage(slot->list.Get(), incoming[i], i);
    }
    if (!submitCommands(*slot)) {
        if (sharpened)
            sharp.timed[slotIndex] = false;
        return false;
    }
    sharpening = sharpened;
    sharpenPairs += sharpened;
    if (pendingCount == pendingCopies.size()) { // not seen finishing: drop the oldest
        std::move(pendingCopies.begin() + 1, pendingCopies.end(), pendingCopies.begin());
        --pendingCount;
    }
    pendingCopies[pendingCount++] = {slot->fence, qpcNow(), capturedQpc};
    reusedPairs += capturedStamp.generation == lastGeneration;
    lastSerial = serial = capturedStamp.serial;
    lastGeneration = generation = capturedStamp.generation;
    ++pairs;
    publish();
    return true;
}
void keepShownEye(ID3D12Resource* left) {
    // Update 16: while the headset's images are sharpened, the window shows
    // stagedEyes[0] (before sharpening, without Spidy's drawings): no copy.
    if (!left || !mirrorWanted || screenWanted || !shownWanted || sharpening)
        return;
    std::lock_guard submitLock(submission);
    std::lock_guard recordLock(recording);
    if (!enabled || !staged || fault || shownFailed || !stagedEyes[0] || GetTickCount64() >= deadline)
        return;
    const auto desc = left->GetDesc();
    if (!rgba8CopyTarget(desc) || desc.Width != outWidth || desc.Height != outHeight)
        return;
    if (!shownEye) {
        const auto own = stagedEyes[0]->GetDesc();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        // Without it the window goes on with the game's own image of the eye.
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &own,
                                                   D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                                   IID_PPV_ARGS(&shownEye)))) {
            shownFailed = true; // not tried again every frame
            return;
        }
    }
    auto* slot = readyCommands(shownCommands);
    if (!slot)
        return; // earlier copies still in flight: the next frame's image instead
    copyEyeTexture(slot->list.Get(), left, D3D12_RESOURCE_STATE_RENDER_TARGET, shownEye.Get(),
                   originalBarrier, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!submitCommands(*slot))
        return;
    shownMs = GetTickCount64();
    shownWanted = false;
}
bool waitCopy(uint64_t serial, uint32_t timeoutMs) {
    std::unique_lock lock(submission);
    copyReady.wait_for(lock, std::chrono::milliseconds(std::min(timeoutMs, 500u)),
                       [&] { return requestedSerial != serial || requestFence || fault || !enabled; });
    if (requestedSerial != serial)
        return false;
    const bool submitted = requestFence != 0;
    // Copies and overlay are submitted to the exact direct queue bound to
    // OpenXR. Queue order supplies the dependency at image release. Allocator
    // reuse still checks the GPU fence; the CPU need not drain the game queue.
    requestedSerial = requestFence = 0;
    targets = {};
    return submitted;
}
DWORD start(void* input) {
    std::lock_guard life(lifecycle);
    Config c{};
    if (enabled || hooked)
        return 1000;
    if (!read(reinterpret_cast<uintptr_t>(input), &c, sizeof(c)) || c.magic != 0x53475043 || c.version != 3 ||
        c.bytes != sizeof(c) || c.pid != GetCurrentProcessId() ||
        c.base != reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) ||
        !GetModuleHandleW(L"MilesMorales.exe") || (c.durationMs && c.durationMs < 500) ||
        c.durationMs > 30000 || c.copyToXr > 2 || c.captureImages > 1 || c.eyeAA > 1 ||
        (!c.copyToXr && !c.captureImages) || !c.queue)
        return 1001;
    copyToXr = c.copyToXr != 0;
    staged = c.copyToXr == 2;
    captureImages = c.captureImages != 0;
    // Only the staged images (copyToXr 2) pass through the AA pass.
    aaMode = staged ? c.eyeAA : 0;
    pendingCount = 0; // a new fence starts at 0
    queue = reinterpret_cast<ID3D12CommandQueue*>(c.queue);
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return 1002;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&command))) ||
        FAILED(command->Close()) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return 1003;
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent)
        return 1004;
    auto vt = *reinterpret_cast<void***>(command.Get());
    hooks = {vt[26], vt[10], (*reinterpret_cast<void***>(queue.Get()))[10], vt[53]}; // EndQuery
    auto s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
        return 1100 + s;
    s = MH_CreateHook(hooks[0], reinterpret_cast<void*>(barrier), reinterpret_cast<void**>(&originalBarrier));
    if (s == MH_OK)
        s = MH_CreateHook(hooks[1], reinterpret_cast<void*>(reset), reinterpret_cast<void**>(&originalReset));
    if (s == MH_OK)
        s = MH_CreateHook(hooks[2], reinterpret_cast<void*>(execute),
                          reinterpret_cast<void**>(&originalExecute));
    if (s == MH_OK)
        s = MH_CreateHook(hooks[3], reinterpret_cast<void*>(endQuery),
                          reinterpret_cast<void**>(&originalEndQuery));
    if (s != MH_OK) {
        for (auto h : hooks)
            MH_RemoveHook(h);
        return 1200 + s;
    }
    hooked = true;
    deadline = c.durationMs ? GetTickCount64() + c.durationMs : UINT64_MAX;
    enabled = true;
    // Under one freeze of the game's threads (stereo_probe.cpp says why).
    for (auto h : hooks)
        if (s == MH_OK)
            s = MH_QueueEnableHook(h);
    if (s == MH_OK)
        s = MH_ApplyQueued();
    if (s != MH_OK) {
        enabled = false;
        for (auto t : hooks)
            MH_QueueDisableHook(t);
        MH_ApplyQueued();
        return 1300 + s;
    }
    // The game's presented frame is optional: without it the headset shows
    // nothing while gameplay is unavailable, as before.
    void* presentAddress{};
    void* present1Address{};
    if (presentEntries(presentAddress, present1Address) &&
        MH_CreateHook(presentAddress, reinterpret_cast<void*>(present), reinterpret_cast<void**>(&originalPresent)) ==
            MH_OK) {
        presentHooks[0] = presentAddress;
        if (MH_CreateHook(present1Address, reinterpret_cast<void*>(present1),
                          reinterpret_cast<void**>(&originalPresent1)) == MH_OK)
            presentHooks[1] = present1Address;
        bool hookedPresent = true;
        for (auto h : presentHooks)
            if (h)
                hookedPresent = MH_QueueEnableHook(h) == MH_OK && hookedPresent;
        hookedPresent = MH_ApplyQueued() == MH_OK && hookedPresent;
        publishScreen(hookedPresent ? 1 : 2, nullptr, 0);
    }
    std::lock_guard lock(telemetry);
    InterlockedIncrement64(&SpidyGpuData.sequence);
    SpidyGpuData.status = 1;
    InterlockedIncrement64(&SpidyGpuData.sequence);
    return 0;
}
bool readPixels() { // submission and recording held by caller; GPU work complete
    const UINT width = imageWidth(), height = imageHeight();
    for (unsigned i = 0; i < 2; ++i) {
        void* data{};
        const auto& layout = layouts[i];
        // The last row has no pitch padding: rows whose bytes are not a
        // multiple of 256 make the buffer shorter than RowPitch * height.
        D3D12_RANGE range{0, static_cast<SIZE_T>(layout.Offset + static_cast<UINT64>(layout.Footprint.RowPitch) *
                                                                       (height - 1) +
                                                 width * 4)};
        if (FAILED(readbacks[i]->Map(0, &range, &data)))
            return false;
        for (unsigned y = 0; y < height; ++y)
            std::memcpy(pixels[i].data() + static_cast<size_t>(y) * width * 4,
                        static_cast<uint8_t*>(data) + layout.Offset +
                            static_cast<size_t>(y) * layout.Footprint.RowPitch,
                        width * 4);
        D3D12_RANGE written{0, 0};
        readbacks[i]->Unmap(0, &written);
    }
    return true;
}
DWORD freeze() {
    std::lock_guard life(lifecycle);
    std::lock_guard submitLock(submission);
    std::lock_guard recordLock(recording);
    if (!enabled || !captureImages || staged || copyToXr)
        return 3901;
    if (!pairs || !readbacks[0] || !readbacks[1])
        return 3902;
    if (fence->GetCompletedValue() < fenceValue &&
        (FAILED(fence->SetEventOnCompletion(fenceValue, fenceEvent)) ||
         WaitForSingleObject(fenceEvent, 1000) != WAIT_OBJECT_0))
        return 3903;
    if (!readPixels())
        return 3904;
    publish();
    return 0;
}
DWORD stop() {
    std::lock_guard life(lifecycle);
    enabled = false;
    screenWanted = false;
    copyReady.notify_all();
    if (hooked) {
        for (auto h : hooks)
            MH_DisableHook(h);
        for (auto h : presentHooks)
            if (h)
                MH_DisableHook(h);
    }
    std::lock_guard submitLock(submission);
    std::lock_guard recordLock(recording);
    if (fence && fence->GetCompletedValue() < fenceValue) {
        if (FAILED(fence->SetEventOnCompletion(fenceValue, fenceEvent)) ||
            WaitForSingleObject(fenceEvent, 5000) != WAIT_OBJECT_0)
            fault = 3601;
    }
    if (fence && (fence->GetCompletedValue() == UINT64_MAX || fence->GetCompletedValue() < fenceValue))
        fault = 3601;
    if (pairs && captureImages && !fault && !readPixels())
        fault = 3602;
    publish();
    std::lock_guard lock(telemetry);
    InterlockedIncrement64(&SpidyGpuData.sequence);
    SpidyGpuData.status = fault ? 3 : 2;
    InterlockedIncrement64(&SpidyGpuData.sequence);
    return fault.load();
}
} // namespace spidy::native_gpu
extern "C" __declspec(dllexport) DWORD WINAPI SpidyGpuStart(void* input) {
    return native_gpu::start(input);
}
extern "C" __declspec(dllexport) DWORD WINAPI SpidyGpuStop(void*) {
    return native_gpu::stop();
}
// Diagnostic: how many times the game presented since the GPU bridge started
// (Present and Present1 each count); with 1 how many presents got the left
// eye on the game's window, and with 3 how many of them got it as the headset
// showed it; with 2 how many presents were held back from the window.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPresentCount(void* which) {
    switch (reinterpret_cast<uintptr_t>(which)) {
    case 0:
        return native_gpu::presents();
    case 2:
        return native_gpu::heldPresents();
    case 3:
        return native_gpu::mirroredShown();
    default:
        return native_gpu::mirrored();
    }
}
// While the window shows the left eye, at most this many of the game's
// presents a second reach it (1-240; 0: every one). Takes effect at once.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyMonitorRate(void* rate) {
    const auto value = reinterpret_cast<uintptr_t>(rate);
    if (!spidy::validMonitorRate(static_cast<uint32_t>(std::min<uintptr_t>(value, UINT32_MAX))))
        return 3911;
    native_gpu::monitorRate(static_cast<uint32_t>(value));
    return 0;
}
// Eye anti-aliasing now (live.txt "aa"): 0 off, 1 FXAA. Takes effect with the
// next eye pair; refused (3960) for another value.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyEyeAA(void* mode) {
    return native_gpu::setEyeAA(static_cast<uint32_t>(std::min<uintptr_t>(reinterpret_cast<uintptr_t>(mode), 2)))
               ? 0
               : 3960;
}
// Eye history (live.txt history): this percent of each pair is the pair
// before it, turned to the new view (0 off, at most 95 since Update 21, the
// clamp widened from 50 on); refused (3962) above.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyEyeHistory(void* percent) {
    return native_gpu::setEyeHistory(
               static_cast<uint32_t>(std::min<uintptr_t>(reinterpret_cast<uintptr_t>(percent), 1000)))
               ? 0
               : 3962;
}
// Update 16: eye sharpening (live.txt sharpen): 0 off (no pass), 1-100 the
// headset's images sharpened (CAS), 101-200 (Update 21) the strong mode past
// CAS's top (eye_sharpen.hpp boost); refused (3963) above 200 or once the pass
// could not be built (its code: SpidyXrTimingData's eyeSharpenFault).
extern "C" __declspec(dllexport) DWORD WINAPI SpidyEyeSharpen(void* percent) {
    return native_gpu::setEyeSharpen(
               static_cast<uint32_t>(std::min<uintptr_t>(reinterpret_cast<uintptr_t>(percent), 1000)))
               ? 0
               : 3963;
}
// Update 24: live.txt eye_debug (eye_debug.hpp): 0 off, 1 both headset images
// from the left eye's picture (a flicker test; the world looks flat). Only the
// copy to the headset changes; refused (3991) for anything else.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyEyeDebug(void* mode) {
    return native_gpu::setEyeDebug(
               static_cast<uint32_t>(std::min<uintptr_t>(reinterpret_cast<uintptr_t>(mode), 1000)))
               ? 0
               : 3991;
}
// Eye supersampling (launcher -Supersample): the eyes are rendered at this
// percent of the headset's eye size, per side (100-200; 100 none), and filtered
// down to it. Only before SpidyXrStart; refused (3961) otherwise.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyEyeSupersample(void* percent) {
    return native_gpu::setSupersample(
               static_cast<uint32_t>(std::min<uintptr_t>(reinterpret_cast<uintptr_t>(percent), 1000)))
               ? 0
               : 3961;
}
extern "C" __declspec(dllexport) DWORD WINAPI SpidyGpuFreeze(void*) {
    return native_gpu::freeze();
}
// Probes: 0 stops and 1 starts copying the game's presented frame; 2 reads
// the newest copy back into SpidyScreenData.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyGpuScreen(void* mode) {
    const auto value = reinterpret_cast<uintptr_t>(mode);
    if (value > 2)
        return 3950;
    if (value < 2) {
        native_gpu::wantScreen(value == 1);
        return 0;
    }
    return native_gpu::readScreen();
}
