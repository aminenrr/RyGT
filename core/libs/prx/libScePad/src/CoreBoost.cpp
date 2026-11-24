// P-cores for GT7's busy threads. On a hybrid CPU (Intel P/E cores) Windows may run the title's
// main job thread ("Job#0", at full load on one core) or the renderer's recorder on an efficiency
// core, and may throttle them (EcoQoS) when it sees fit. A watcher pins those threads to the
// performance cores, turns power throttling off for them and the process, and raises Job#0 a step.
// Other threads stay free to use every core. RYGT_PCORES=0 turns it off; non-hybrid CPUs are left
// alone.
// PRT-AUDIOCORE (pr-tests, Round 16b, RYGT_AUDIO_CORE=1, default off): R15 had GT7's audio mixer
// (SceSndzAudioOutMain) at 80-98% of a core while it shared the P-cores with the renderer threads,
// so it may run next to a busy hyperthread sibling. With the switch the last P-core goes to it
// alone: the audio thread is pinned to one logical CPU of that core at highest priority, and the
// other busy threads plus every other game thread are kept off both logical CPUs of the core.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <thread>
#include <unordered_set>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include "prx/libScePad/include/PadState.hpp"
#include "prx/libc/include/CpuTopology.hpp"

namespace {

// The threads that set the frame rate (by the renderer's perf monitor): the game's main job thread,
// the renderer's command threads, presentation and the audio mixer.
constexpr const wchar_t* BusyThreads[] = {
    L"Job#0",
    L"shadGT:GpuRecorder",
    L"shadGT:GpuCommandProcessor",
    L"shadGT:VkCommands",
    L"shadGT:PresentThread",
    L"SceSndzAudioOutMain",
};

// The power throttling switch (THREAD_POWER_THROTTLING_STATE / PROCESS_POWER_THROTTLING_STATE).
struct PowerThrottling {
    ULONG version;
    ULONG control_mask;
    ULONG state_mask;
};
constexpr ULONG ThrottleExecutionSpeed = 0x1;
constexpr int ThreadPowerThrottlingClass = 3;   // THREAD_INFORMATION_CLASS ThreadPowerThrottling
constexpr int ProcessPowerThrottlingClass = 4;  // PROCESS_INFORMATION_CLASS ProcessPowerThrottling

using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
using SetThreadInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);

bool IsBusy(const wchar_t* name) {
    for (const wchar_t* busy : BusyThreads)
        if (std::wcscmp(name, busy) == 0) return true;
    return false;
}

// The logical CPUs of the last physical core inside `mask` (0 when there is none).
std::uint64_t LastCoreIn(std::uint64_t mask) {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) return 0;
    std::vector<unsigned char> buffer(length);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &length)) return 0;
    std::uint64_t last = 0;
    for (DWORD offset = 0; offset + 8 <= length;) {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (entry->Size == 0 || offset + entry->Size > length) break;
        if (entry->Relationship == RelationProcessorCore && entry->Processor.GroupCount >= 1 &&
            entry->Processor.GroupMask[0].Group == 0) {
            const auto core = static_cast<std::uint64_t>(entry->Processor.GroupMask[0].Mask);
            if (core != 0 && (core & mask) == core && core > last) last = core;
        }
        offset += entry->Size;
    }
    return last;
}

std::uint64_t LowestBit(std::uint64_t mask) { return mask & (~mask + 1); }

void Watch(std::uint64_t mask) {
    const HMODULE kernel = GetModuleHandleA("kernel32.dll");
    const auto describe = reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(kernel, "GetThreadDescription"));
    const auto set_thread = reinterpret_cast<SetThreadInformationFn>(GetProcAddress(kernel, "SetThreadInformation"));
    const auto set_process = reinterpret_cast<SetProcessInformationFn>(GetProcAddress(kernel, "SetProcessInformation"));
    if (describe == nullptr) return;
    PowerThrottling full_speed{1, ThrottleExecutionSpeed, 0};
    if (set_process != nullptr)
        set_process(GetCurrentProcess(), ProcessPowerThrottlingClass, &full_speed, sizeof(full_speed));

    const DWORD process = GetCurrentProcessId();
    std::unordered_set<DWORD> done;

    // PRT-AUDIOCORE (Round 16b).
    std::uint64_t audio_core = 0;
    std::uint64_t audio_cpu = 0;
    std::uint64_t busy_mask = mask;
    std::uint64_t all = CpuTopology::Get().process;
    if (const char* env = std::getenv("RYGT_AUDIO_CORE"); env != nullptr && env[0] == '1') {
        audio_core = LastCoreIn(mask);
        if (audio_core != 0 && (mask & ~audio_core) != 0) {
            audio_cpu = LowestBit(audio_core);
            busy_mask = mask & ~audio_core;
        } else {
            audio_core = 0;
        }
        std::fprintf(stderr, "[pcores] PRT-AUDIOCORE: %s (audio core 0x%llx, audio CPU 0x%llx, other busy threads 0x%llx)\n",
            audio_core != 0 ? "on" : "no spare P-core, off", static_cast<unsigned long long>(audio_core),
            static_cast<unsigned long long>(audio_cpu), static_cast<unsigned long long>(busy_mask));
        std::fflush(stderr);
    }
    if (all == 0) all = mask | CpuTopology::Get().efficient;
    std::unordered_set<DWORD> moved;
    for (;;) {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
                if (entry.th32OwnerProcessID != process || done.count(entry.th32ThreadID) != 0) continue;
                const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_QUERY_INFORMATION |
                    THREAD_SET_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
                if (thread == nullptr) continue;
                PWSTR name = nullptr;
                if (SUCCEEDED(describe(thread, &name)) && name != nullptr) {
                    if (IsBusy(name)) {
                        done.insert(entry.th32ThreadID);
                        const bool audio = audio_core != 0 && std::wcscmp(name, L"SceSndzAudioOutMain") == 0;
                        const std::uint64_t target = audio ? audio_cpu : busy_mask;
                        const bool pinned = SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(target)) != 0;
                        if (set_thread != nullptr)
                            set_thread(thread, ThreadPowerThrottlingClass, &full_speed, sizeof(full_speed));
                        if (std::wcscmp(name, L"Job#0") == 0) SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
                        if (audio) SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
                        std::fprintf(stderr, "[pcores] %ls (tid %lu) -> %s 0x%llx%s\n", name, entry.th32ThreadID,
                            audio ? "own audio core, CPU" : "P-cores", static_cast<unsigned long long>(target),
                            pinned ? "" : " (not pinned)");
                        std::fflush(stderr);
                    } else if (audio_core != 0 && moved.count(entry.th32ThreadID) == 0) {
                        // PRT-AUDIOCORE: keep the rest of the game off the audio core; a thread the game
                        // pinned elsewhere keeps its own cores, one pinned only there stays as it was.
                        moved.insert(entry.th32ThreadID);
                        const DWORD_PTR before = SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(all & ~audio_core));
                        if (before != 0) {
                            const std::uint64_t kept = static_cast<std::uint64_t>(before) & ~audio_core;
                            if (kept == 0) {
                                SetThreadAffinityMask(thread, before);
                            } else if (kept != (all & ~audio_core)) {
                                SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(kept));
                            }
                        }
                    }
                    LocalFree(name);
                }
                CloseHandle(thread);
            }
            CloseHandle(snapshot);
        }
        Sleep(1000);  // threads come and go between menus and races
    }
}

}

namespace Pad {

void InstallCoreBoost() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* env = std::getenv("RYGT_PCORES"); env != nullptr && env[0] == '0') return;
    const auto& layout = CpuTopology::Get();
    std::uint64_t mask = layout.performant;
    if (layout.process != 0) mask &= layout.process;
    if (!layout.hybrid || mask == 0) return;
    std::fprintf(stderr, "[pcores] hybrid CPU: busy threads go to the P-cores 0x%llx (E-cores 0x%llx)\n",
        static_cast<unsigned long long>(mask), static_cast<unsigned long long>(layout.efficient));
    std::fflush(stderr);
    std::thread(Watch, mask).detach();
}

}
