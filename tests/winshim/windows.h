#pragma once
// A stand-in for the few Windows calls src/voice_pool.cpp makes, built on the C++ standard library, so that
// tests/voice_pool_test.cpp can run the pool's thread on Linux (tests/run_voice_pool_test.sh). Not part of the
// Windows build: CMake never sees this folder.
//
// Time is the test's to speed up: GetTickCount64() counts `timeScale` milliseconds for every real one, and the waits
// are shortened the same way, so a pool that closes an idle voice after 12 seconds does it in under a second here.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <thread>

typedef unsigned int DWORD;
typedef unsigned int UINT;
typedef unsigned short WORD;
typedef int BOOL;
typedef void* HANDLE;
typedef void* LPVOID;
typedef void* HMODULE;
typedef char* LPSTR;
typedef const wchar_t* LPCWSTR;
typedef unsigned long long ULONGLONG;
typedef std::uintptr_t DWORD_PTR;
#define WINAPI
#define FALSE 0
#define TRUE 1
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u

namespace winshim {
// How many milliseconds GetTickCount64() counts for each real one.
inline std::atomic<unsigned> timeScale{1};
inline const std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
inline ULONGLONG virtualNow() {
    const auto real = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - epoch).count();
    return 100000ull + static_cast<ULONGLONG>(real) * timeScale.load() / 1000;
}
// Real milliseconds (at least 1, for a finite wait) that `virtualMs` take at the current speed.
inline std::chrono::microseconds realFor(DWORD virtualMs) {
    const unsigned scale = timeScale.load();
    const auto us = static_cast<long long>(virtualMs) * 1000 / scale;
    return std::chrono::microseconds(us < 1000 ? 1000 : us);
}
struct Object {
    enum class Kind { event, thread } kind;
    std::mutex m;
    std::condition_variable cv;
    // An event: signalled, and whether a wait clears it. A thread: ended.
    bool flag = false, autoReset = true;
    std::thread body;
};
} // namespace winshim

struct SRWLOCK {
    std::shared_mutex m;
};
#define SRWLOCK_INIT {}
inline void AcquireSRWLockExclusive(SRWLOCK* l) { l->m.lock(); }
inline void ReleaseSRWLockExclusive(SRWLOCK* l) { l->m.unlock(); }
inline void AcquireSRWLockShared(SRWLOCK* l) { l->m.lock_shared(); }
inline void ReleaseSRWLockShared(SRWLOCK* l) { l->m.unlock_shared(); }

inline ULONGLONG GetTickCount64() { return winshim::virtualNow(); }

inline HANDLE CreateEventW(void*, BOOL manualReset, BOOL initial, LPCWSTR) {
    auto* e = new winshim::Object;
    e->kind = winshim::Object::Kind::event;
    e->autoReset = !manualReset;
    e->flag = initial != 0;
    return e;
}
inline BOOL SetEvent(HANDLE h) {
    auto* e = static_cast<winshim::Object*>(h);
    {
        std::lock_guard g(e->m);
        e->flag = true;
    }
    e->cv.notify_all();
    return TRUE;
}
typedef DWORD (*LPTHREAD_START_ROUTINE)(void*);
inline HANDLE CreateThread(void*, std::size_t, LPTHREAD_START_ROUTINE fn, void* param, DWORD, DWORD*) {
    auto* t = new winshim::Object;
    t->kind = winshim::Object::Kind::thread;
    t->body = std::thread([t, fn, param] {
        fn(param);
        {
            std::lock_guard g(t->m);
            t->flag = true;
        }
        t->cv.notify_all();
    });
    return t;
}
inline DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    auto* o = static_cast<winshim::Object*>(h);
    std::unique_lock lk(o->m);
    const auto ready = [&] { return o->flag; };
    if (ms == INFINITE)
        o->cv.wait(lk, ready);
    else if (!o->cv.wait_for(lk, winshim::realFor(ms), ready))
        return WAIT_TIMEOUT;
    if (o->kind == winshim::Object::Kind::event && o->autoReset)
        o->flag = false;
    return WAIT_OBJECT_0;
}
inline BOOL CloseHandle(HANDLE h) {
    auto* o = static_cast<winshim::Object*>(h);
    if (o->kind == winshim::Object::Kind::thread && o->body.joinable())
        o->body.join();
    delete o;
    return TRUE;
}
