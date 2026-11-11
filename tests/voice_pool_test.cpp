// MilesVR 0.1.80, v24: src/voice_pool.cpp run against a pretend audio device (tests/winshim), on Linux:
//   tests/run_voice_pool_test.sh
// It checks what the pool does with the sounds - overlap, the limit of voices, an audio device that refuses, a voice
// that dies, one that never finishes, the end, the queue - not what a real Windows audio device does with them.
#include "spidy/game_fp.hpp"
#include "spidy/voice_pool.hpp"
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>
#include <mmsystem.h>

namespace {
std::mutex logLock;
std::vector<std::string> logLines;
int failures = 0, checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("  FAILED: %s\n", what);
    }
}
bool logged(const char* part) {
    std::lock_guard g(logLock);
    for (const auto& line : logLines)
        if (line.find(part) != std::string::npos)
            return true;
    return false;
}
} // namespace

namespace spidy::game_fp {
void note(const char* text) {
    std::lock_guard g(logLock);
    logLines.emplace_back(text);
}
} // namespace spidy::game_fp

namespace {
using namespace spidy;
constexpr unsigned rate = 44100;

// The WAV impactWave writes: a 44-byte header and `ms` of mono 16-bit samples.
std::vector<unsigned char> makeWav(unsigned sampleRate, unsigned ms) {
    const std::uint32_t count = sampleRate * ms / 1000;
    std::vector<unsigned char> w(44 + count * 2ull);
    const auto put = [&](size_t at, std::uint32_t v, unsigned bytes) {
        for (unsigned b = 0; b < bytes; ++b)
            w[at + b] = static_cast<unsigned char>(v >> (8 * b));
    };
    std::memcpy(w.data(), "RIFF", 4);
    put(4, 36 + count * 2, 4);
    std::memcpy(w.data() + 8, "WAVEfmt ", 8);
    put(16, 16, 4);
    put(20, 1, 2);
    put(22, 1, 2);
    put(24, sampleRate, 4);
    put(28, sampleRate * 2, 4);
    put(32, 2, 2);
    put(34, 16, 2);
    std::memcpy(w.data() + 36, "data", 4);
    put(40, count * 2, 4);
    for (std::uint32_t i = 0; i < count; ++i)
        put(44 + i * 2ull, static_cast<std::uint16_t>(static_cast<std::int16_t>(8000 * std::sin(i * .05))), 2);
    return w;
}
void sleepV(unsigned virtualMs) {
    std::this_thread::sleep_for(winshim::realFor(virtualMs));
}
// Polls (every few real milliseconds) until `ok`, for at most `virtualMs` of the pool's time.
bool until(const std::function<bool()>& ok, unsigned virtualMs) {
    const auto stop = std::chrono::steady_clock::now() + winshim::realFor(virtualMs);
    while (std::chrono::steady_clock::now() < stop) {
        if (ok())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ok();
}
// Everything the pool asked of the device, and the pool, in a known state for a scenario.
winshim::Driver driver;
void fresh(const char* name, unsigned voices) {
    std::printf("%s\n", name);
    voice_pool::close();
    {
        std::lock_guard g(winshim::device.m);
        auto& d = winshim::device;
        d.voices.clear();
        d.refuseOpens = 0;
        d.refuseCode = MMSYSERR_ALLOCATED;
        d.openLimit = 1000;
        d.stall = false;
        d.deadBefore = 0;
        d.openDelayMs = 0;
        d.opens = d.closes = d.writes = d.resets = d.soundCalls = d.refused = d.maxPlaying = 0;
    }
    check(voice_pool::open(rate, voices), "the pool would not open");
}
unsigned openNow() {
    std::lock_guard g(winshim::device.m);
    return winshim::device.openNow();
}
unsigned playingNow() {
    std::lock_guard g(winshim::device.m);
    return winshim::device.playingNow();
}

void basic() {
    fresh("one sound: a voice opens, it plays to its end, it stays open a while and is closed after", 4);
    const auto before = voice_pool::stats();
    const auto wav = makeWav(rate, 1000);
    check(voice_pool::isOpen(), "isOpen");
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] { return voice_pool::stats().started == before.started + 1; }, 600), "the sound did not start");
    check(until([&] { return playingNow() == 1; }, 300), "the device was not playing it");
    // A spare voice is opened for the next blow's sound.
    check(until([&] { return openNow() == 2; }, 600), "no spare voice was opened beside the playing one");
    check(until([&] { return voice_pool::stats().finished == before.finished + 1; }, 2500), "the sound did not finish");
    sleepV(300);
    check(openNow() >= 1, "the voices were closed at once instead of kept for the next sound");
    check(until([&] { return openNow() == 0; }, 20000), "the idle voices were never closed");
    const auto after = voice_pool::stats();
    check(after.idleClosed >= before.idleClosed + 2, "the idle closes were not counted");
    check(after.fallbacks == before.fallbacks && after.failures == before.failures, "a clean run had trouble");
    check(logged("block sound 1: started") && logged("block sound 1: finished"), "the first sound is not in the log");
}
void overlap() {
    fresh("three sounds at once overlap on three voices", 4);
    const auto before = voice_pool::stats();
    const auto wav = makeWav(rate, 1500);
    for (int i = 0; i < 3; ++i) {
        check(voice_pool::play(wav.data(), wav.size()), "play said no");
        sleepV(120);
    }
    check(until([&] { return playingNow() == 3; }, 800), "the three sounds were not playing together");
    check(until([&] { return voice_pool::stats().finished == before.finished + 3; }, 4000), "not all three finished");
    const auto after = voice_pool::stats();
    check(after.overlapped == before.overlapped + 2, "two of them should have overlapped another");
    check(after.started == before.started + 3 && after.fallbacks == before.fallbacks, "all three on voices");
    check(winshim::device.maxPlaying == 3, "the most at once was not three");
}
void spamming() {
    fresh("a punch every 250 ms for 4 seconds, sounds 780 ms long: every one is heard, none cut", 4);
    const auto before = voice_pool::stats();
    const auto wav = makeWav(rate, 780);
    for (int i = 0; i < 16; ++i) {
        check(voice_pool::play(wav.data(), wav.size()), "play said no");
        sleepV(250);
    }
    check(until([&] { return voice_pool::stats().finished == before.finished + 16; }, 4000), "not all sixteen finished");
    const auto after = voice_pool::stats();
    check(after.started == before.started + 16, "every sound should have started");
    check(after.fallbacks == before.fallbacks && after.dropped == before.dropped, "a sound was lost");
    check(winshim::device.resets == 0, "a sound was cut");
    check(winshim::device.maxPlaying >= 3 && winshim::device.maxPlaying <= 4, "the sounds did not overlap as they should");
    check(logged("block sound 12:") && !logged("block sound 13:"), "the first twelve sounds, and only they, are in the log one by one");
}
void limit() {
    fresh("with two voices, a third and fourth sound cut the ones that began first", 2);
    const auto before = voice_pool::stats();
    const auto wav = makeWav(rate, 2000);
    for (int i = 0; i < 4; ++i) {
        check(voice_pool::play(wav.data(), wav.size()), "play said no");
        sleepV(150);
    }
    check(until([&] { return voice_pool::stats().started == before.started + 4; }, 1500), "four sounds should have started");
    check(winshim::device.maxPlaying <= 2, "more than two voices were playing");
    {
        std::lock_guard g(winshim::device.m);
        check(winshim::device.resets >= 2, "the older sounds were not cut");
    }
    check(until([&] { return voice_pool::stats().finished >= before.finished + 2; }, 5000), "the last two did not finish");
    check(openNow() <= 2, "more than two voices were open");
}
void refused() {
    fresh("the device refuses the first open: that sound plays with PlaySound, the next on a voice", 4);
    const auto before = voice_pool::stats();
    const auto wav = makeWav(rate, 500);
    {
        std::lock_guard g(winshim::device.m);
        winshim::device.refuseOpens = 1;
    }
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] { return voice_pool::stats().fallbacks == before.fallbacks + 1; }, 600), "no fallback for the refused sound");
    {
        std::lock_guard g(winshim::device.m);
        check(winshim::device.soundCalls == 1 && winshim::device.lastSound == wav.data(), "PlaySound did not get the sound");
    }
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] { return voice_pool::stats().started == before.started + 1; }, 600), "the next sound did not start on a voice");
    check(voice_pool::stats().failures == before.failures + 1, "the refusal was not counted once");
    check(logged("waveOutOpen was refused by the audio device: the device is in use (code 4)"), "the refusal is not in the log with its code");
}
void deadVoice() {
    fresh("a voice kept open that has died is replaced for the same sound", 4);
    const auto before = voice_pool::stats();
    const auto wav = makeWav(rate, 400);
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] { return voice_pool::stats().finished == before.finished + 1; }, 1500), "the first did not finish");
    {
        // Every voice open now dies; the next one the pool opens is good.
        std::lock_guard g(winshim::device.m);
        winshim::device.deadBefore = winshim::device.generation + 1;
    }
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] { return voice_pool::stats().started == before.started + 2; }, 1500), "the second sound did not start");
    const auto after = voice_pool::stats();
    check(after.fallbacks == before.fallbacks, "the second sound fell back although a new voice would do");
    check(after.failures == before.failures + 1, "the dead voice's refusal was not counted once");
    check(until([&] { return voice_pool::stats().finished == before.finished + 2; }, 1500), "the second did not finish");
}
void stuck() {
    fresh("a voice that never finishes is closed; sounds go to PlaySound for a few seconds, then to voices again", 4);
    const auto before = voice_pool::stats();
    const auto wav = makeWav(rate, 500);
    {
        std::lock_guard g(winshim::device.m);
        winshim::device.stall = true;
    }
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] { return voice_pool::stats().started == before.started + 1; }, 600), "the sound did not start");
    check(until([&] { return voice_pool::stats().stuck == before.stuck + 1; }, 3000), "the stuck voice was not found");
    check(until([&] { return openNow() == 0; }, 600), "the stuck voice and the voices beside it were not closed");
    {
        std::lock_guard g(winshim::device.m);
        winshim::device.stall = false;
    }
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] { return voice_pool::stats().fallbacks == before.fallbacks + 1; }, 600), "no PlaySound in the rest");
    sleepV(3500);
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] { return voice_pool::stats().started == before.started + 2; }, 800), "the voices did not come back after the rest");
    check(until([&] { return voice_pool::stats().finished == before.finished + 1; }, 1500), "the sound after the rest did not finish");
    check(logged("not finished") && logged("voice closed"), "the stuck voice is not in the log");
}
void closing() {
    fresh("close() ends the thread at once, stops the sounds, closes the voices and tells the log; the pool opens again", 4);
    const auto wav = makeWav(rate, 3000);
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] { return playingNow() == 1; }, 800), "the sound was not playing");
    const auto t0 = std::chrono::steady_clock::now();
    voice_pool::close();
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check(took < 1000, "close() took long");
    check(openNow() == 0 && playingNow() == 0, "voices were left open or playing after close()");
    check(!voice_pool::isOpen(), "isOpen after close");
    check(!voice_pool::play(wav.data(), wav.size()), "play said yes on a closed pool");
    check(logged("pool closed - "), "no summary in the log");
    check(voice_pool::open(rate, 4), "the pool would not open again");
    const auto before = voice_pool::stats();
    const auto shortWav = makeWav(rate, 300);
    check(voice_pool::play(shortWav.data(), shortWav.size()), "play said no after reopening");
    check(until([&] { return voice_pool::stats().finished == before.finished + 1; }, 1500), "no sound after reopening");
}
void invalid() {
    fresh("sounds that are not the WAV the pool plays are turned away", 4);
    const auto wav = makeWav(rate, 100);
    check(!voice_pool::play(nullptr, 0), "null");
    check(!voice_pool::play(wav.data(), 44), "only a header");
    check(!voice_pool::play(wav.data(), 20), "a cut header");
    const auto other = makeWav(22050, 100);
    check(!voice_pool::play(other.data(), other.size()), "a sound at another rate");
    auto garbage = wav;
    garbage[0] = 'X';
    check(!voice_pool::play(garbage.data(), garbage.size()), "not RIFF");
    auto stereo = wav;
    stereo[22] = 2;
    check(!voice_pool::play(stereo.data(), stereo.size()), "stereo");
    auto lying = wav;
    lying[40] = 0xff;
    lying[41] = 0xff;
    lying[42] = 0xff;
    check(!voice_pool::play(lying.data(), lying.size()), "a data chunk longer than the file");
    check(!voice_pool::open(100, 4), "an absurd rate");
    check(!voice_pool::open(500000, 4), "an absurd rate (high)");
    check(voice_pool::isOpen(), "the pool should stay as it was");
    const auto before = voice_pool::stats();
    check(voice_pool::play(wav.data(), wav.size()), "a good sound after the bad ones");
    check(until([&] { return voice_pool::stats().finished == before.finished + 1; }, 1500), "the good sound did not finish");
}
void clamped() {
    fresh("a hundred voices asked for is eight", 100);
    const auto before = voice_pool::stats();
    const auto wav = makeWav(rate, 1500);
    for (int i = 0; i < 12; ++i) {
        check(voice_pool::play(wav.data(), wav.size()), "play said no");
        sleepV(40);
    }
    check(until([&] { return voice_pool::stats().started == before.started + 12; }, 2000), "twelve sounds should have started");
    check(winshim::device.maxPlaying == 8, "eight at once, no more, no fewer");
    check(until([&] { return voice_pool::stats().finished >= before.finished + 4; }, 4000), "the sounds did not finish");
}
void closeWhileBusy() {
    fresh("close() while the thread is in a slow call to the device: it waits for the call, then the pool opens again", 4);
    const auto wav = makeWav(rate, 500);
    {
        std::lock_guard g(winshim::device.m);
        winshim::device.openDelayMs = 1500;
    }
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] {
              std::lock_guard g(winshim::device.m);
              return winshim::device.opens >= 1;
          }, 600), "the sound did not reach the device");
    voice_pool::close();
    check(openNow() == 0, "voices left open");
    {
        std::lock_guard g(winshim::device.m);
        winshim::device.openDelayMs = 0;
    }
    check(voice_pool::open(rate, 4), "the pool would not open again");
}
void closeGivesUp() {
    fresh("close() gives up on a thread that is wedged in the device for longer than it waits; the pool opens again once it ends", 4);
    const auto wav = makeWav(rate, 500);
    {
        std::lock_guard g(winshim::device.m);
        winshim::device.openDelayMs = 4000;
    }
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] {
              std::lock_guard g(winshim::device.m);
              return winshim::device.opens >= 1;
          }, 600), "the sound did not reach the device");
    voice_pool::close();
    check(logged("did not end in time"), "the log does not say the thread was left");
    check(!voice_pool::isOpen() && !voice_pool::play(wav.data(), wav.size()), "a pool whose thread is left is closed");
    check(!voice_pool::open(rate, 4), "the pool should not open while the old thread is still in the device");
    {
        std::lock_guard g(winshim::device.m);
        winshim::device.openDelayMs = 0;
    }
    check(until([&] { return voice_pool::open(rate, 4); }, 6000), "the pool did not open again once the old thread ended");
    const auto before = voice_pool::stats();
    check(voice_pool::play(wav.data(), wav.size()), "play said no after the pool opened again");
    check(until([&] { return voice_pool::stats().finished == before.finished + 1; }, 2000), "no sound after the pool opened again");
}
void closeUnderTraffic() {
    fresh("close() while four threads are asking: it ends at once and the threads are turned away after", 4);
    const auto wav = makeWav(rate, 500);
    std::atomic<bool> go{true};
    std::atomic<unsigned> yes{0}, no{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            while (go.load()) {
                if (voice_pool::play(wav.data(), wav.size()))
                    ++yes;
                else
                    ++no;
                sleepV(10);
            }
        });
    sleepV(600);
    const auto t0 = std::chrono::steady_clock::now();
    voice_pool::close();
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    sleepV(200);
    go = false;
    for (auto& t : threads)
        t.join();
    check(took < 1500, "close() under traffic took long");
    check(yes > 0 && no > 0, "the threads should have been taken first and turned away after");
    check(openNow() == 0 && playingNow() == 0, "voices left open or playing after close() under traffic");
    check(!voice_pool::play(wav.data(), wav.size()), "play said yes after close()");
}
void late() {
    fresh("sounds that wait too long for a slow device are dropped, not played in a heap", 4);
    const auto before = voice_pool::stats();
    const auto wav = makeWav(rate, 400);
    {
        std::lock_guard g(winshim::device.m);
        winshim::device.openDelayMs = 1500;
    }
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] {
              std::lock_guard g(winshim::device.m);
              return winshim::device.opens >= 1;
          }, 600), "the first sound did not reach the device");
    for (int i = 0; i < 3; ++i)
        check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] { return voice_pool::stats().dropped == before.dropped + 3; }, 6000), "the late sounds were not dropped");
    check(voice_pool::stats().started == before.started + 1, "only the first should have started");
}
void flood() {
    fresh("more sounds than the queue holds while the thread is busy: the oldest are dropped", 4);
    const auto before = voice_pool::stats();
    const auto wav = makeWav(rate, 400);
    {
        std::lock_guard g(winshim::device.m);
        winshim::device.openDelayMs = 3000;
    }
    check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] {
              std::lock_guard g(winshim::device.m);
              return winshim::device.opens >= 1;
          }, 600), "the first sound did not reach the device");
    for (int i = 0; i < 12; ++i)
        check(voice_pool::play(wav.data(), wav.size()), "play said no");
    check(until([&] { return voice_pool::stats().dropped >= before.dropped + 12; }, 12000), "not all were dropped in the end");
    const auto after = voice_pool::stats();
    check(after.requested == before.requested + 13, "thirteen sounds were asked for");
    check(after.started - before.started + after.fallbacks - before.fallbacks + after.dropped - before.dropped ==
              after.requested - before.requested,
          "each sound asked for should end as exactly one of started, fallback or dropped");
    check(after.started == before.started + 1 && after.dropped == before.dropped + 12, "one started, twelve dropped");
}
void stress() {
    fresh("four threads asking at once: every sound ends up played, in a fallback or dropped, and nothing breaks", 4);
    const auto before = voice_pool::stats();
    const auto wav = makeWav(rate, 300);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 60; ++i) {
                voice_pool::play(wav.data(), wav.size());
                sleepV(15 + static_cast<unsigned>(i % 7));
            }
        });
    for (auto& t : threads)
        t.join();
    check(until([&] {
              const auto s = voice_pool::stats();
              return s.started - before.started + s.fallbacks - before.fallbacks + s.dropped - before.dropped ==
                     s.requested - before.requested;
          }, 6000), "a sound was lost: asked for = started + fallbacks + dropped does not hold");
    check(until([&] { return playingNow() == 0; }, 4000), "sounds still playing at the end");
    check(winshim::device.maxPlaying <= 4, "more than four voices playing");
    check(until([&] { return openNow() == 0; }, 20000), "voices stayed open at the end");
}
} // namespace

int main() {
    winshim::timeScale = 10;
    driver.start();
    basic();
    overlap();
    spamming();
    limit();
    refused();
    deadVoice();
    stuck();
    closing();
    invalid();
    clamped();
    late();
    flood();
    stress();
    closeWhileBusy();
    closeGivesUp();
    closeUnderTraffic();
    voice_pool::close();
    driver.end();
    std::printf("%d checks, %d failed\n", checks, failures);
    if (failures) {
        std::printf("--- the pool's log ---\n");
        for (const auto& line : logLines)
            std::printf("%s\n", line.c_str());
    }
    return failures ? 1 : 0;
}
