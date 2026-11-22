#ifndef CORE_LIBS_PRX_LIBSCESYSTEMSERVICE_SYSPARAMLOG_HPP
#define CORE_LIBS_PRX_LIBSCESYSTEMSERVICE_SYSPARAMLOG_HPP

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include "GameSerial.hpp"

namespace SysParamLog {

inline bool On() {
    static const bool on = [] {
        const char* env = std::getenv("RYGT_SYSPARAM_LOG");
        if (env && env[0] == '0') return false;
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
        std::fprintf(stderr, "[SYSPARAM] PRT-SYSPARAM: %s -> %s\n", key.c_str(), answer.c_str());
        answers.emplace(key, answer);
    } else {
        std::fprintf(stderr, "[SYSPARAM] PRT-SYSPARAM: %s -> %s (was %s)\n", key.c_str(), answer.c_str(),
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
