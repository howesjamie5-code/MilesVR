#include "video.hpp"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace launcher {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

Clock::duration seconds(double value) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(value));
}

bool seekReader(IMFSourceReader* reader, double seconds) {
    PROPVARIANT at;
    PropVariantInit(&at);
    at.vt = VT_I8;
    at.hVal.QuadPart = static_cast<LONGLONG>(seconds * 1e7);
    const HRESULT result = reader->SetCurrentPosition(GUID_NULL, at);
    PropVariantClear(&at);
    return SUCCEEDED(result);
}

} // namespace

std::wstring tutorialFolder() {
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::filesystem::path folder = std::filesystem::path(exe).parent_path();
    // Beside "MilesVR Launcher.exe" in a player's copy; at the checkout's top from build\windows-ninja.
    for (int i = 0; i < 5 && !folder.empty(); ++i, folder = folder.parent_path()) {
        std::error_code error;
        if (std::filesystem::is_directory(folder / L"Tutorials", error))
            return (folder / L"Tutorials").wstring();
        if (folder == folder.parent_path())
            break;
    }
    return {};
}

VideoPlayer::VideoPlayer(ID3D11Device* device) : device_(device) {}

VideoPlayer::~VideoPlayer() {
    close();
    if (started_)
        MFShutdown();
}

bool VideoPlayer::open(const std::wstring& path, std::string& error) {
    close();
    if (!started_) {
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
            error = "Windows cannot play videos on this PC (Media Foundation is missing).";
            return false;
        }
        started_ = true;
    }
    {
        std::lock_guard lock(mutex_);
        quit_ = seekWanted_ = false;
        playing_ = true;
        ready_ = -1;
        error_.clear();
        frame_.clear();
        frameId_ = shownId_ = 0;
        position_ = 0;
    }
    worker_ = std::thread(&VideoPlayer::run, this, path);
    std::unique_lock lock(mutex_);
    wake_.wait(lock, [&] { return ready_ != -1; });
    if (ready_ == 1)
        return true;
    error = error_;
    lock.unlock();
    worker_.join();
    return false;
}

void VideoPlayer::close() {
    if (worker_.joinable()) {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        worker_.join();
    }
    if (view_)
        view_->Release();
    if (texture_)
        texture_->Release();
    view_ = nullptr;
    texture_ = nullptr;
    width_ = height_ = 0;
    duration_ = 0;
}

bool VideoPlayer::playing() const {
    std::lock_guard lock(mutex_);
    return playing_;
}

void VideoPlayer::setPlaying(bool playing) {
    {
        std::lock_guard lock(mutex_);
        playing_ = playing;
    }
    wake_.notify_all();
}

void VideoPlayer::seek(double seconds) {
    {
        std::lock_guard lock(mutex_);
        seekWanted_ = true;
        seekTo_ = std::clamp(seconds, 0.0, std::max(0.0, duration_ - 0.05));
        position_ = seekTo_;
    }
    wake_.notify_all();
}

void VideoPlayer::update() {
    std::lock_guard lock(mutex_);
    if (frameId_ == shownId_ || frame_.size() != static_cast<size_t>(width_) * height_)
        return;
    if (!texture_) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width_);
        desc.Height = static_cast<UINT>(height_);
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture_)))
            return;
        if (FAILED(device_->CreateShaderResourceView(texture_, nullptr, &view_))) {
            texture_->Release();
            texture_ = nullptr;
            return;
        }
    }
    ComPtr<ID3D11DeviceContext> context;
    device_->GetImmediateContext(&context);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(texture_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    const size_t row = static_cast<size_t>(width_) * 4;
    for (int y = 0; y < height_; ++y)
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    frame_.data() + static_cast<size_t>(y) * width_, row);
    context->Unmap(texture_, 0);
    shownId_ = frameId_;
}

void VideoPlayer::run(std::wstring path) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMFSourceReader> reader;
    std::string error;
    UINT32 frameWidth = 0, frameHeight = 0;
    int width = 0, height = 0, cropX = 0, cropY = 0;
    LONG defaultStride = 0;
    double duration = 0;
    {
        // The picture only: the sound stream is never selected, so nothing is heard.
        ComPtr<IMFAttributes> attributes;
        ComPtr<IMFMediaType> wanted, current;
        if (FAILED(MFCreateAttributes(&attributes, 1)) ||
            FAILED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
            FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader))) {
            error = "The video could not be opened. Is the Tutorials folder beside the launcher complete?";
        } else if (FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE)) ||
                   FAILED(reader->SetStreamSelection(kVideo, TRUE)) || FAILED(MFCreateMediaType(&wanted)) ||
                   FAILED(wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
                   FAILED(wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
                   FAILED(reader->SetCurrentMediaType(kVideo, nullptr, wanted.Get())) ||
                   FAILED(reader->GetCurrentMediaType(kVideo, &current)) ||
                   FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &frameWidth, &frameHeight))) {
            error = "Windows cannot decode this video (H.264).";
        } else {
            width = static_cast<int>(frameWidth);
            height = static_cast<int>(frameHeight);
            // H.264 pictures are often 1088 rows with 1080 shown.
            MFVideoArea area{};
            UINT32 size = 0;
            if (SUCCEEDED(current->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area),
                                           sizeof(area), &size)) &&
                area.Area.cx > 0 && area.Area.cy > 0) {
                cropX = area.OffsetX.value;
                cropY = area.OffsetY.value;
                width = std::min<int>(area.Area.cx, width - cropX);
                height = std::min<int>(area.Area.cy, height - cropY);
            }
            UINT32 stride = 0;
            if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)))
                defaultStride = static_cast<LONG>(stride);
            else if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, frameWidth, &defaultStride)))
                defaultStride = static_cast<LONG>(frameWidth * 4);
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                           MF_PD_DURATION, &value)))
                duration = static_cast<double>(value.uhVal.QuadPart) / 1e7;
            PropVariantClear(&value);
        }
    }
    {
        std::lock_guard lock(mutex_);
        ready_ = error.empty() ? 1 : 0;
        error_ = error;
        width_ = width;
        height_ = height;
        duration_ = duration;
    }
    wake_.notify_all();
    if (!error.empty()) {
        reader.Reset();
        if (SUCCEEDED(com))
            CoUninitialize();
        return;
    }

    std::vector<uint32_t> picture;
    // One decoded sample into the frame the window's thread shows next.
    const auto publish = [&](IMFSample* sample) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
            return;
        ComPtr<IMF2DBuffer> flat;
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        BYTE* data = nullptr;
        bool locked2d = false;
        if (SUCCEEDED(buffer.As(&flat)) && SUCCEEDED(flat->Lock2D(&scan0, &pitch))) {
            locked2d = true;
        } else {
            DWORD length = 0;
            if (FAILED(buffer->Lock(&data, nullptr, &length)))
                return;
            pitch = defaultStride;
            // A negative stride: the picture is stored bottom row first.
            scan0 = pitch < 0 ? data + static_cast<size_t>(frameHeight - 1) * static_cast<size_t>(-pitch) : data;
        }
        picture.resize(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; ++y) {
            const auto* source = reinterpret_cast<const uint32_t*>(scan0 + static_cast<ptrdiff_t>(y + cropY) * pitch) + cropX;
            uint32_t* target = picture.data() + static_cast<size_t>(y) * width;
            // RGB32 leaves the alpha byte unset; ImGui would draw it see-through.
            for (int x = 0; x < width; ++x)
                target[x] = source[x] | 0xff000000u;
        }
        if (locked2d)
            flat->Unlock2D();
        else
            buffer->Unlock();
        std::lock_guard lock(mutex_);
        frame_.swap(picture);
        ++frameId_;
    };

    ComPtr<IMFSample> pending;
    double pendingAt = 0, shownAt = 0;
    // A seek: decode up to this time without waiting, then show it (-1: none).
    double target = 0;
    Clock::time_point origin = Clock::now();
    for (;;) {
        bool seekNow = false;
        double seekTo = 0;
        {
            std::unique_lock lock(mutex_);
            bool waited = false;
            while (!quit_ && !seekWanted_ && !playing_ && target < 0) {
                wake_.wait(lock);
                waited = true;
            }
            if (quit_)
                break;
            if (seekWanted_) {
                seekWanted_ = false;
                seekNow = true;
                seekTo = seekTo_;
            }
            // Paused: play on from the picture it stopped at.
            if (waited)
                origin = Clock::now() - seconds(pending ? pendingAt : shownAt);
        }
        if (seekNow) {
            pending.Reset();
            if (seekReader(reader.Get(), seekTo))
                target = seekTo;
        }
        if (!pending) {
            DWORD flags = 0;
            LONGLONG time = 0;
            ComPtr<IMFSample> sample;
            if (FAILED(reader->ReadSample(kVideo, 0, nullptr, &flags, &time, &sample)))
                break;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                // The tutorials play on a loop.
                if (!seekReader(reader.Get(), 0))
                    break;
                target = 0;
                continue;
            }
            if (!sample)
                continue;
            const double at = static_cast<double>(time) / 1e7;
            if (target >= 0) {
                if (at + 0.02 < target)
                    continue; // a picture before the seek point
                publish(sample.Get());
                shownAt = at;
                position_ = at;
                target = -1;
                origin = Clock::now() - seconds(at);
                continue;
            }
            pending = sample;
            pendingAt = at;
        }
        {
            std::unique_lock lock(mutex_);
            if (wake_.wait_until(lock, origin + seconds(pendingAt), [&] { return quit_ || seekWanted_ || !playing_; }))
                continue;
        }
        publish(pending.Get());
        shownAt = pendingAt;
        position_ = pendingAt;
        pending.Reset();
    }
    pending.Reset();
    reader.Reset();
    if (SUCCEEDED(com))
        CoUninitialize();
}

} // namespace launcher
