#pragma once
// The winmm half of the stand-in (see windows.h): a pretend audio device whose behaviour the test sets - how many
// voices it will open, what it refuses, whether a write ever finishes - and which counts what the pool asks of it.
#include "windows.h"
#include <algorithm>
#include <map>
#include <vector>

typedef UINT MMRESULT;
struct HWAVEOUT__;
typedef HWAVEOUT__* HWAVEOUT;
typedef HWAVEOUT* LPHWAVEOUT;
typedef struct wavehdr_tag {
    LPSTR lpData;
    DWORD dwBufferLength;
    DWORD dwBytesRecorded;
    DWORD_PTR dwUser;
    DWORD dwFlags;
    DWORD dwLoops;
    struct wavehdr_tag* lpNext;
    DWORD_PTR reserved;
} WAVEHDR, *PWAVEHDR, *LPWAVEHDR;
typedef struct tWAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
} WAVEFORMATEX, *LPWAVEFORMATEX;
typedef const WAVEFORMATEX* LPCWAVEFORMATEX;
#define WAVE_FORMAT_PCM 1
#define WAVE_MAPPER ((UINT)-1)
#define CALLBACK_NULL 0
#define MMSYSERR_NOERROR 0
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_ALLOCATED 4
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_NODRIVER 6
#define MMSYSERR_NOMEM 7
#define WAVERR_BADFORMAT 32
#define WAVERR_STILLPLAYING 33
#define WAVERR_UNPREPARED 34
#define WHDR_DONE 0x1
#define WHDR_PREPARED 0x2
#define WHDR_INQUEUE 0x10
#define SND_ASYNC 0x1
#define SND_NODEFAULT 0x2
#define SND_MEMORY 0x4

namespace winshim {
struct Device {
    std::mutex m;
    struct Voice {
        bool open = false;
        DWORD rate = 0;
        unsigned generation = 0;
        std::map<WAVEHDR*, ULONGLONG> due; // queued buffers and when each ends
    };
    std::vector<Voice> voices;
    // The test's knobs.
    int refuseOpens = 0;                  // the next this-many opens are refused
    UINT refuseCode = MMSYSERR_ALLOCATED; // ... with this code
    unsigned openLimit = 1000;            // voices open at once
    bool stall = false;                   // a write is accepted and never ends
    unsigned generation = 0;              // counts the voices opened
    unsigned deadBefore = 0;              // a voice opened before this generation is dead: its calls fail
    UINT deadCode = MMSYSERR_NODRIVER;
    DWORD openDelayMs = 0;                // a waveOutOpen takes this long (virtual time)
    // What it was asked.
    unsigned opens = 0, closes = 0, writes = 0, resets = 0, soundCalls = 0, refused = 0, maxPlaying = 0;
    const void* lastSound = nullptr;
    unsigned openNow() {
        unsigned n = 0;
        for (const auto& v : voices)
            n += v.open ? 1u : 0u;
        return n;
    }
    unsigned playingNow() {
        unsigned n = 0;
        for (const auto& v : voices)
            n += v.due.empty() ? 0u : 1u;
        return n;
    }
    // The pretend driver: buffers whose time has come are done.
    void tick() {
        std::lock_guard g(m);
        const ULONGLONG now = virtualNow();
        for (auto& v : voices)
            for (auto it = v.due.begin(); it != v.due.end();) {
                if (it->second <= now) {
                    it->first->dwFlags = (it->first->dwFlags | WHDR_DONE) & ~static_cast<DWORD>(WHDR_INQUEUE);
                    it = v.due.erase(it);
                } else {
                    ++it;
                }
            }
    }
    Voice* of(HWAVEOUT h) {
        const auto id = reinterpret_cast<std::uintptr_t>(h);
        return id >= 1 && id <= voices.size() && voices[id - 1].open ? &voices[id - 1] : nullptr;
    }
};
inline Device device;
// A thread that plays the driver while the test runs.
struct Driver {
    std::atomic<bool> stop{false};
    std::thread body;
    void start() {
        stop = false;
        body = std::thread([this] {
            while (!stop.load()) {
                device.tick();
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
    }
    void end() {
        stop = true;
        if (body.joinable())
            body.join();
    }
};
} // namespace winshim

inline MMRESULT waveOutOpen(LPHWAVEOUT out, UINT, LPCWAVEFORMATEX format, DWORD_PTR, DWORD_PTR, DWORD) {
    auto& d = winshim::device;
    DWORD delay;
    {
        std::lock_guard g(d.m);
        delay = d.openDelayMs;
        ++d.opens; // counted when it is asked for, before it takes its time
    }
    if (delay)
        std::this_thread::sleep_for(winshim::realFor(delay));
    std::lock_guard g(d.m);
    if (d.refuseOpens > 0) {
        --d.refuseOpens;
        ++d.refused;
        return d.refuseCode;
    }
    if (format->wFormatTag != WAVE_FORMAT_PCM || format->nChannels != 1 || format->wBitsPerSample != 16)
        return WAVERR_BADFORMAT;
    if (d.openNow() >= d.openLimit) {
        ++d.refused;
        return MMSYSERR_ALLOCATED;
    }
    std::size_t slot = 0;
    while (slot < d.voices.size() && d.voices[slot].open)
        ++slot;
    if (slot == d.voices.size())
        d.voices.emplace_back();
    d.voices[slot] = winshim::Device::Voice{};
    d.voices[slot].open = true;
    d.voices[slot].rate = format->nSamplesPerSec;
    d.voices[slot].generation = ++d.generation;
    *out = reinterpret_cast<HWAVEOUT>(slot + 1);
    return MMSYSERR_NOERROR;
}
inline MMRESULT waveOutClose(HWAVEOUT h) {
    auto& d = winshim::device;
    std::lock_guard g(d.m);
    auto* v = d.of(h);
    if (!v)
        return MMSYSERR_INVALHANDLE;
    if (!v->due.empty())
        return WAVERR_STILLPLAYING;
    v->open = false;
    ++d.closes;
    return MMSYSERR_NOERROR;
}
inline MMRESULT waveOutReset(HWAVEOUT h) {
    auto& d = winshim::device;
    std::lock_guard g(d.m);
    auto* v = d.of(h);
    if (!v)
        return MMSYSERR_INVALHANDLE;
    for (auto& [header, due] : v->due) {
        (void)due;
        header->dwFlags = (header->dwFlags | WHDR_DONE) & ~static_cast<DWORD>(WHDR_INQUEUE);
    }
    v->due.clear();
    ++d.resets;
    return MMSYSERR_NOERROR;
}
inline MMRESULT waveOutPrepareHeader(HWAVEOUT h, LPWAVEHDR header, UINT) {
    auto& d = winshim::device;
    std::lock_guard g(d.m);
    auto* v = d.of(h);
    if (!v)
        return MMSYSERR_INVALHANDLE;
    if (v->generation < d.deadBefore)
        return d.deadCode;
    header->dwFlags = WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}
inline MMRESULT waveOutUnprepareHeader(HWAVEOUT h, LPWAVEHDR header, UINT) {
    auto& d = winshim::device;
    std::lock_guard g(d.m);
    auto* v = d.of(h);
    if (!v)
        return MMSYSERR_INVALHANDLE;
    if (header->dwFlags & WHDR_INQUEUE)
        return WAVERR_STILLPLAYING;
    header->dwFlags &= ~static_cast<DWORD>(WHDR_PREPARED);
    return MMSYSERR_NOERROR;
}
inline MMRESULT waveOutWrite(HWAVEOUT h, LPWAVEHDR header, UINT) {
    auto& d = winshim::device;
    std::lock_guard g(d.m);
    auto* v = d.of(h);
    if (!v)
        return MMSYSERR_INVALHANDLE;
    if (v->generation < d.deadBefore)
        return d.deadCode;
    if (!(header->dwFlags & WHDR_PREPARED))
        return WAVERR_UNPREPARED;
    ++d.writes;
    header->dwFlags = (header->dwFlags | WHDR_INQUEUE) & ~static_cast<DWORD>(WHDR_DONE);
    // A stalled device takes the buffer and never gives it back.
    const ULONGLONG length = static_cast<ULONGLONG>(header->dwBufferLength) * 1000 / (static_cast<ULONGLONG>(v->rate) * 2);
    v->due[header] = d.stall ? ~0ull : winshim::virtualNow() + length;
    d.maxPlaying = std::max(d.maxPlaying, d.playingNow());
    return MMSYSERR_NOERROR;
}
inline BOOL PlaySoundW(LPCWSTR sound, HMODULE, DWORD) {
    auto& d = winshim::device;
    std::lock_guard g(d.m);
    ++d.soundCalls;
    d.lastSound = sound;
    return TRUE;
}
