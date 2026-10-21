#pragma once
#include <d3d11.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace launcher {

// The folder of the venom tutorial videos (Tutorials beside the launcher, or in the checkout); empty when missing.
std::wstring tutorialFolder();

// A silent video player for the venom tutorials: Media Foundation decodes the picture on its own thread (sound is
// never read), and the window's thread puts the newest frame in a texture ImGui draws. Plays on a loop.
class VideoPlayer {
public:
    explicit VideoPlayer(ID3D11Device* device);
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    bool open(const std::wstring& path, std::string& error);
    void close();
    bool isOpen() const { return worker_.joinable(); }
    // The window's thread, each frame: the newest decoded picture into the texture.
    void update();
    // Null until the first picture is decoded.
    ID3D11ShaderResourceView* texture() const { return view_; }
    int width() const { return width_; }
    int height() const { return height_; }
    double duration() const { return duration_; }
    double position() const { return position_.load(); }
    bool playing() const;
    void setPlaying(bool playing);
    void seek(double seconds);

private:
    void run(std::wstring path);

    ID3D11Device* device_;
    ID3D11Texture2D* texture_{};
    ID3D11ShaderResourceView* view_{};
    int width_{}, height_{};
    double duration_{};
    bool started_{};
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool quit_{}, playing_{true}, seekWanted_{};
    double seekTo_{};
    std::vector<uint32_t> frame_;
    uint64_t frameId_{}, shownId_{};
    std::atomic<double> position_{0};
    // The worker's answer to open(): -1 waiting, 0 failed, 1 ready.
    int ready_ = -1;
    std::string error_;
};

} // namespace launcher
