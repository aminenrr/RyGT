#ifndef CORE_LIBS_PRX_LIBSCESYSTEMSERVICE_SYSPARAMLOG_HPP
#define CORE_LIBS_PRX_LIBSCESYSTEMSERVICE_SYSPARAMLOG_HPP

#include <chrono>
#include <cstdarg>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include "GameSerial.hpp"

namespace SysParamLog {

inline bool On() {
    static const bool on = [] {
        // Off since Round 7 (Opening Movie parked); RYGT_SYSPARAM_LOG=1 turns it on in GT7.
        const char* env = std::getenv("RYGT_SYSPARAM_LOG");
        if (!(env && env[0] == '1')) return false;
        return GameSerial::RunningGt7();
    }();
    return on;
}

inline std::string Format(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (written < 0) return std::string();
    return std::string(text);
}

inline std::string Clock() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    const long long ms =
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03lld", local.tm_hour, local.tm_min, local.tm_sec, ms);
    return std::string(text);
}

inline const char* ParamName(int id) {
    switch (id) {
        case 1: return "LANG";
        case 2: return "DATE_FORMAT";
        case 3: return "TIME_FORMAT";
        case 4: return "TIME_ZONE";
        case 5: return "SUMMERTIME";
        case 6: return "SYSTEM_NAME";
        case 7: return "GAME_PARENTAL_LEVEL";
        case 1000: return "ENTER_BUTTON_ASSIGN";
        default: return "unknown";
    }
}

inline void Note(const char* call, const std::string& args, const std::string& answer) {
    constexpr unsigned kMaxPerCall = 64;
    static std::mutex mutex;
    static std::map<std::string, std::string> answers;
    static std::map<std::string, unsigned> lines;
    std::string key(call);
    key += '(';
    key += args;
    key += ')';
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = answers.find(key);
    if (it != answers.end() && it->second == answer) return;
    unsigned& count = lines[call];
    if (count >= kMaxPerCall) {
        if (count == kMaxPerCall) {
            std::fprintf(stderr, "[SYSPARAM] PRT-SYSPARAM: %s: %u lines, later answers not logged\n", call, kMaxPerCall);
            count++;
        }
        return;
    }
    count++;
    if (it == answers.end()) {
        std::fprintf(stderr, "[SYSPARAM] PRT-SYSPARAM: [%s] %s -> %s\n", Clock().c_str(), key.c_str(), answer.c_str());
        answers.emplace(key, answer);
    } else {
        std::fprintf(stderr, "[SYSPARAM] PRT-SYSPARAM: [%s] %s -> %s (was %s)\n", Clock().c_str(), key.c_str(), answer.c_str(),
                     it->second.c_str());
        it->second = answer;
    }
}

}

#define PRT_SYSPARAM(call, args, answer)                              \
    do {                                                              \
        if (SysParamLog::On()) SysParamLog::Note((call), (args), (answer)); \
    } while (0)

#endif
