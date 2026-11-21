// SPDX-License-Identifier: GPL-2.0-only
// FEAT-GT7-184: Audit GT7 shader data and warm the unified renderer's compute recipes.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

using SteadyClock = std::chrono::steady_clock;

std::atomic<bool> g_cancel{false};
std::mutex g_outMutex;

void onSignal(int) {
    g_cancel.store(true, std::memory_order_release);
}

void emitLine(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_outMutex);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

struct WarmRecipe {
    std::string filename;
    std::uint64_t fnvHash = 0;
    bool pushDescriptors = false;
    std::uint32_t subgroupCtl = 0;
    std::uint32_t subgroupSize = 0;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<std::uint32_t> spirv;
};

std::uint64_t fnv1a64(const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

bool parseWarmRecipe(const fs::path& path, WarmRecipe& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto size = f.tellg();
    if (size < 36 || size > (16 << 20) || size % 4 != 0) return false;
    const auto sz = static_cast<std::size_t>(size);
    std::vector<std::uint8_t> buf(sz);
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz))) return false;

    auto rd32 = [&](std::size_t off) -> std::uint32_t {
        std::uint32_t v = 0;
        std::memcpy(&v, buf.data() + off, 4);
        return v;
    };

    if (rd32(0) != 0x43574752u || rd32(4) != 2u || rd32(16) > 1u || rd32(20) > 1u) return false;
    const auto policy = (static_cast<std::uint64_t>(rd32(12)) << 32) | rd32(8);
    std::ostringstream suffix;
    suffix << "-0x" << std::hex << std::setw(16) << std::setfill('0') << policy;
    if (!path.stem().string().ends_with(suffix.str())) return false;
    out.pushDescriptors = rd32(16) != 0u;
    out.subgroupCtl = rd32(20);
    out.subgroupSize = rd32(24);
    if (out.subgroupCtl && out.subgroupSize != 32u && out.subgroupSize != 64u) return false;
    const auto count = rd32(28);
    std::size_t off = 32u + static_cast<std::size_t>(count) * 12u;
    if (count > 256u || off + 4u > sz) return false;
    out.bindings.resize(count);
    std::uint32_t nextBinding = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto at = 32u + i * 12u;
        const auto type = rd32(at + 4);
        const auto descriptors = rd32(at + 8);
        if (rd32(at) != nextBinding || descriptors == 0 || descriptors > 256u - nextBinding ||
            (type != 0 && type != 2 && type != 3 && type != 7)) return false;
        out.bindings[i] = {nextBinding, static_cast<VkDescriptorType>(type), descriptors,
                           VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        nextBinding += descriptors;
    }
    const auto spirvWords = rd32(off);
    off += 4u;
    if (spirvWords < 5 || (sz - off) / 4u != spirvWords) return false;
    out.spirv.resize(spirvWords);
    std::memcpy(out.spirv.data(), buf.data() + off, static_cast<std::size_t>(spirvWords) * 4u);
    if (out.spirv[0] != 0x07230203u || out.spirv[1] > 0x00010600u) return false;
    for (std::size_t i = 5; i < spirvWords;) {
        const auto words = out.spirv[i] >> 16;
        if (!words || words > spirvWords - i) return false;
        i += words;
    }
    out.filename = path.filename().string();
    out.fnvHash = fnv1a64(buf.data(), sz);
    return true;
}

std::string formatUuidHex(const std::uint8_t uuid[16]) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) oss << std::setw(2) << static_cast<unsigned>(uuid[i]);
    return oss.str();
}

struct CacheHeaderCheck {
    bool exists = false;
    bool validHeader = false;
    bool uuidMatches = false;
    std::uint64_t sizeBytes = 0;
    fs::file_time_type lastWrite{};
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::string uuidHex;
};

CacheHeaderCheck inspectPipelineCacheHeader(const fs::path& path, const VkPhysicalDeviceProperties& gpu) {
    CacheHeaderCheck res{};
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) return res;
    res.exists = true;
    res.sizeBytes = fs::file_size(path, ec);
    if (ec || res.sizeBytes < 32) return res;
    res.lastWrite = fs::last_write_time(path, ec);
    if (ec) return res;
    std::uint8_t hdr[32]{};
    std::ifstream f(path, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(hdr), 32)) return res;
    std::uint32_t hSize = 0, hVer = 0;
    std::memcpy(&hSize, hdr + 0, 4);
    std::memcpy(&hVer, hdr + 4, 4);
    std::memcpy(&res.vendorId, hdr + 8, 4);
    std::memcpy(&res.deviceId, hdr + 12, 4);
    res.uuidHex = formatUuidHex(hdr + 16);
    res.validHeader = (hSize >= 32 && hSize <= res.sizeBytes && hVer == 1);
    res.uuidMatches = res.validHeader &&
                      res.vendorId == gpu.vendorID &&
                      res.deviceId == gpu.deviceID &&
                      std::memcmp(hdr + 16, gpu.pipelineCacheUUID, 16) == 0;
    return res;
}

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

struct StoreAudit {
    std::uint64_t totalBytes = 0;
    std::uint64_t storeSig = 0;
    std::uint64_t maxWriteTime = 0;
    std::uint32_t spvCount = 0;
    std::uint32_t metaCount = 0;
    std::uint32_t spvInvalid = 0;
    std::uint32_t stVertex = 0;
    std::uint32_t stTessCtrl = 0;
    std::uint32_t stTessEval = 0;
    std::uint32_t stGeom = 0;
    std::uint32_t stFrag = 0;
    std::uint32_t stCompute = 0;
    bool verifiedComplete = false;
    std::string rendererBuild;
};

struct PrewarmState {
    std::string fileKey;
    std::uint64_t fileCacheSz = 0;
    std::unordered_map<std::string, std::uint64_t> done;
    std::uint64_t storeSig = 0;
    bool hasStoreAudit = false;
    StoreAudit cachedAudit{};
};

std::string readRendererBuild(const fs::path& directSetsDir) {
    std::string build;
    std::error_code ec;
    const fs::path rbPath = directSetsDir / "renderer_build.txt";
    if (fs::exists(rbPath, ec)) {
        std::ifstream rf(rbPath);
        std::getline(rf, build);
        while (!build.empty() && (build.back() == '\r' || build.back() == '\n')) {
            build.pop_back();
        }
    }
    return build;
}

PrewarmState loadCompletedState(
    const fs::path& statePath, const std::string& expectedKey, std::uint64_t expectedCacheSize) {
    PrewarmState state{};
    std::ifstream f(statePath);
    if (!f) return state;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.rfind("KEY=", 0) == 0) {
            state.fileKey = line.substr(4);
        } else if (line.rfind("CACHE_SIZE=", 0) == 0) {
            state.fileCacheSz = std::strtoull(line.substr(11).c_str(), nullptr, 10);
        } else if (line.rfind("STORE_AUDIT=", 0) == 0) {
            unsigned long long sig = 0, maxWt = 0, bytes = 0;
            unsigned int spv = 0, meta = 0, bad = 0, v = 0, tc = 0, te = 0, g = 0, fl = 0, c = 0;
            bool parsedAudit = false;
            if (std::sscanf(line.c_str() + 12,
                            "%llx|maxwt=%llx|spv=%u|meta=%u|bytes=%llu|bad=%u|V:%u,TC:%u,TE:%u,G:%u,F:%u,C:%u",
                            &sig, &maxWt, &spv, &meta, &bytes, &bad, &v, &tc, &te, &g, &fl, &c) == 12) {
                parsedAudit = true;
            } else if (std::sscanf(line.c_str() + 12,
                            "%llx|spv=%u|meta=%u|bytes=%llu|bad=%u|V:%u,TC:%u,TE:%u,G:%u,F:%u,C:%u",
                            &sig, &spv, &meta, &bytes, &bad, &v, &tc, &te, &g, &fl, &c) == 11) {
                maxWt = 0;
                parsedAudit = true;
            }
            if (parsedAudit) {
                state.storeSig = static_cast<std::uint64_t>(sig);
                state.cachedAudit.storeSig = state.storeSig;
                state.cachedAudit.maxWriteTime = static_cast<std::uint64_t>(maxWt);
                state.cachedAudit.spvCount = spv;
                state.cachedAudit.metaCount = meta;
                state.cachedAudit.totalBytes = static_cast<std::uint64_t>(bytes);
                state.cachedAudit.spvInvalid = bad;
                state.cachedAudit.stVertex = v;
                state.cachedAudit.stTessCtrl = tc;
                state.cachedAudit.stTessEval = te;
                state.cachedAudit.stGeom = g;
                state.cachedAudit.stFrag = fl;
                state.cachedAudit.stCompute = c;
                state.cachedAudit.verifiedComplete = true;
                state.hasStoreAudit = (state.storeSig != 0);
            }
        } else if (line.rfind("RECIPE=", 0) == 0) {
            const auto sep = line.find('|', 7);
            if (sep != std::string::npos) {
                const std::string name = line.substr(7, sep - 7);
                const std::uint64_t h = std::strtoull(line.substr(sep + 1).c_str(), nullptr, 16);
                state.done[name] = h;
            }
        }
    }
    if (!expectedKey.empty() && state.fileKey != expectedKey) {
        state.hasStoreAudit = false;
    }
    if (state.fileKey != expectedKey || state.fileCacheSz < 32 ||
        expectedCacheSize < 32 || expectedCacheSize < state.fileCacheSz) {
        state.done.clear();
    }
    return state;
}

StoreAudit auditDirectSets(
    const fs::path& directSetsDir,
    bool verifyHeaders,
    const PrewarmState* cachedState = nullptr,
    bool forceRewarm = false,
    std::vector<std::wstring>* outMetaNames = nullptr) {
    StoreAudit audit{};
    audit.rendererBuild = readRendererBuild(directSetsDir);

    std::wstring dirPrefix = directSetsDir.wstring();
    if (!dirPrefix.empty() && dirPrefix.back() != L'\\' && dirPrefix.back() != L'/') {
        dirPrefix.push_back(L'\\');
    }
    const std::wstring pattern = dirPrefix + L"*";

    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileExW(
        pattern.c_str(),
        FindExInfoBasic,
        &ffd,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE) {
        return audit;
    }

    struct SpvEntry {
        std::wstring name;
        std::uint64_t size = 0;
    };
    std::vector<SpvEntry> spvEntries;
    std::vector<SpvEntry> deltaSpvEntries;
    if (verifyHeaders) {
        spvEntries.reserve(20000);
    }
    if (outMetaNames) {
        outMetaNames->reserve(20000);
    }

    auto endsWithCI = [](const wchar_t* s, std::size_t len, const wchar_t* suf, std::size_t sufLen) -> bool {
        if (len < sufLen) return false;
        const wchar_t* p = s + (len - sufLen);
        for (std::size_t i = 0; i < sufLen; ++i) {
            wchar_t c = p[i];
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
            if (c != suf[i]) return false;
        }
        return true;
    };

    const std::uint64_t prevMaxWt =
        (cachedState && cachedState->hasStoreAudit && cachedState->cachedAudit.spvInvalid == 0 && !forceRewarm)
            ? cachedState->cachedAudit.maxWriteTime
            : 0;

    std::uint64_t sigXor = 0;
    std::uint64_t sigSum = 0;
    std::uint64_t oldSigXor = 0;
    std::uint64_t oldSigSum = 0;
    std::uint64_t oldBytes = 0;
    std::uint32_t oldSpvCount = 0;
    std::uint32_t oldMetaCount = 0;

    do {
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::uint64_t fileSize = (static_cast<std::uint64_t>(ffd.nFileSizeHigh) << 32) |
                                       static_cast<std::uint64_t>(ffd.nFileSizeLow);
        const std::uint64_t writeTime = (static_cast<std::uint64_t>(ffd.ftLastWriteTime.dwHighDateTime) << 32) |
                                        static_cast<std::uint64_t>(ffd.ftLastWriteTime.dwLowDateTime);
        const std::size_t nameLen = std::wcslen(ffd.cFileName);

        audit.totalBytes += fileSize;
        if (writeTime > audit.maxWriteTime) {
            audit.maxWriteTime = writeTime;
        }

        std::uint64_t fh = fnv1a64(ffd.cFileName, nameLen * sizeof(wchar_t));
        fh = splitmix64(fh ^ splitmix64(fileSize) ^ splitmix64(writeTime));
        sigXor ^= fh;
        sigSum += fh;

        const bool inOldSubset = (prevMaxWt > 0 && writeTime <= prevMaxWt);
        if (inOldSubset) {
            oldSigXor ^= fh;
            oldSigSum += fh;
            oldBytes += fileSize;
        }

        if (endsWithCI(ffd.cFileName, nameLen, L".spv", 4)) {
            ++audit.spvCount;
            if (inOldSubset) {
                ++oldSpvCount;
            }
            if (verifyHeaders) {
                spvEntries.push_back(SpvEntry{std::wstring(ffd.cFileName, nameLen), fileSize});
                if (prevMaxWt > 0 && !inOldSubset) {
                    deltaSpvEntries.push_back(spvEntries.back());
                }
            }
        } else if (endsWithCI(ffd.cFileName, nameLen, L".meta", 5)) {
            ++audit.metaCount;
            if (inOldSubset) {
                ++oldMetaCount;
            }
            if (outMetaNames) {
                outMetaNames->emplace_back(ffd.cFileName, nameLen);
            }
        }
    } while (FindNextFileW(hFind, &ffd));

    FindClose(hFind);

    auto finalizeStoreSig = [](std::uint64_t x, std::uint64_t s, std::uint32_t spv, std::uint32_t meta, std::uint64_t bytes) {
        std::uint64_t fs = splitmix64(x) ^ splitmix64(s);
        fs = splitmix64(fs ^ ((static_cast<std::uint64_t>(spv) << 32) | meta));
        fs = splitmix64(fs ^ bytes);
        return fs == 0 ? std::uint64_t{1} : fs;
    };

    audit.storeSig = finalizeStoreSig(sigXor, sigSum, audit.spvCount, audit.metaCount, audit.totalBytes);

    if (verifyHeaders) {
        if (cachedState && cachedState->hasStoreAudit &&
            cachedState->storeSig == audit.storeSig &&
            cachedState->cachedAudit.spvCount == audit.spvCount &&
            cachedState->cachedAudit.metaCount == audit.metaCount &&
            cachedState->cachedAudit.totalBytes == audit.totalBytes &&
            cachedState->cachedAudit.spvInvalid == 0 && !forceRewarm) {
            audit.spvInvalid = 0;
            audit.stVertex = cachedState->cachedAudit.stVertex;
            audit.stTessCtrl = cachedState->cachedAudit.stTessCtrl;
            audit.stTessEval = cachedState->cachedAudit.stTessEval;
            audit.stGeom = cachedState->cachedAudit.stGeom;
            audit.stFrag = cachedState->cachedAudit.stFrag;
            audit.stCompute = cachedState->cachedAudit.stCompute;
            audit.verifiedComplete = true;
            return audit;
        }

        std::uint32_t initVertex = 0, initTessCtrl = 0, initTessEval = 0, initGeom = 0, initFrag = 0, initCompute = 0;
        if (prevMaxWt > 0 &&
            oldSpvCount == cachedState->cachedAudit.spvCount &&
            oldMetaCount == cachedState->cachedAudit.metaCount &&
            oldBytes == cachedState->cachedAudit.totalBytes &&
            finalizeStoreSig(oldSigXor, oldSigSum, oldSpvCount, oldMetaCount, oldBytes) == cachedState->storeSig) {
            initVertex = cachedState->cachedAudit.stVertex;
            initTessCtrl = cachedState->cachedAudit.stTessCtrl;
            initTessEval = cachedState->cachedAudit.stTessEval;
            initGeom = cachedState->cachedAudit.stGeom;
            initFrag = cachedState->cachedAudit.stFrag;
            initCompute = cachedState->cachedAudit.stCompute;
            spvEntries.swap(deltaSpvEntries);
        }

        if (spvEntries.empty()) {
            audit.stVertex = initVertex;
            audit.stTessCtrl = initTessCtrl;
            audit.stTessEval = initTessEval;
            audit.stGeom = initGeom;
            audit.stFrag = initFrag;
            audit.stCompute = initCompute;
            audit.verifiedComplete = true;
            return audit;
        }

        const std::uint32_t hwThreads = std::clamp(std::thread::hardware_concurrency(), 2u, 8u);
        const std::uint32_t verifyThreads = std::min<std::uint32_t>(
            hwThreads, static_cast<std::uint32_t>(spvEntries.size()));

        std::atomic<std::size_t> nextSpv{0};
        std::atomic<std::uint32_t> aInvalid{0};
        std::atomic<std::uint32_t> aVertex{initVertex};
        std::atomic<std::uint32_t> aTessCtrl{initTessCtrl};
        std::atomic<std::uint32_t> aTessEval{initTessEval};
        std::atomic<std::uint32_t> aGeom{initGeom};
        std::atomic<std::uint32_t> aFrag{initFrag};
        std::atomic<std::uint32_t> aCompute{initCompute};

        auto verifyWorker = [&]() {
            std::uint8_t headerBuf[1024];
            std::vector<std::uint8_t> full;
            std::wstring fullPath;
            fullPath.reserve(dirPrefix.size() + 64);

            while (!g_cancel.load(std::memory_order_acquire)) {
                const std::size_t idx = nextSpv.fetch_add(1, std::memory_order_relaxed);
                if (idx >= spvEntries.size()) break;

                const auto& entry = spvEntries[idx];
                const std::uint64_t sz = entry.size;
                if (sz < 20 || (sz & 3u) != 0) {
                    aInvalid.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                fullPath.assign(dirPrefix);
                fullPath.append(entry.name);

                HANDLE hFile = CreateFileW(
                    fullPath.c_str(),
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_SEQUENTIAL_SCAN,
                    nullptr);
                if (hFile == INVALID_HANDLE_VALUE) {
                    aInvalid.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                const DWORD toRead = static_cast<DWORD>(std::min<std::uint64_t>(sz, sizeof(headerBuf)));
                DWORD bytesRead = 0;
                const BOOL readOk = ReadFile(hFile, headerBuf, toRead, &bytesRead, nullptr);
                if (!readOk || bytesRead != toRead) {
                    CloseHandle(hFile);
                    aInvalid.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                std::uint32_t magic = 0;
                std::memcpy(&magic, headerBuf, 4);
                if (magic != 0x07230203u) {
                    CloseHandle(hFile);
                    aInvalid.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                // Scan execution model from OpEntryPoint (read further if header exceeded 1 KB):
                std::size_t off = 20;
                bool parsed = false;
                full.clear();
                const std::uint8_t* ptr = headerBuf;
                std::size_t avail = bytesRead;

                while (!parsed) {
                    if (off + 8 <= avail) {
                        std::uint32_t w0 = 0;
                        std::memcpy(&w0, ptr + off, 4);
                        const std::uint32_t wc = w0 >> 16;
                        const std::uint32_t op = w0 & 0xFFFFu;
                        if (wc == 0) {
                            aInvalid.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                        if (op == 15u) {
                            std::uint32_t em = 999;
                            std::memcpy(&em, ptr + off + 4, 4);
                            switch (em) {
                                case 0: aVertex.fetch_add(1, std::memory_order_relaxed); break;
                                case 1: aTessCtrl.fetch_add(1, std::memory_order_relaxed); break;
                                case 2: aTessEval.fetch_add(1, std::memory_order_relaxed); break;
                                case 3: aGeom.fetch_add(1, std::memory_order_relaxed); break;
                                case 4: aFrag.fetch_add(1, std::memory_order_relaxed); break;
                                case 5: aCompute.fetch_add(1, std::memory_order_relaxed); break;
                                default: break;
                            }
                            parsed = true;
                            break;
                        }
                        off += static_cast<std::size_t>(wc) * 4u;
                    } else if (full.empty() && sz > avail) {
                        full.resize(static_cast<std::size_t>(sz));
                        std::memcpy(full.data(), headerBuf, avail);
                        const DWORD rem = static_cast<DWORD>(sz - avail);
                        DWORD remRead = 0;
                        if (!ReadFile(hFile, full.data() + avail, rem, &remRead, nullptr) || remRead != rem) {
                            aInvalid.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                        ptr = full.data();
                        avail = full.size();
                    } else {
                        break;
                    }
                }

                CloseHandle(hFile);
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(verifyThreads);
        for (std::uint32_t t = 0; t < verifyThreads; ++t) {
            workers.emplace_back(verifyWorker);
        }
        for (auto& w : workers) {
            w.join();
        }

        audit.spvInvalid = aInvalid.load(std::memory_order_relaxed);
        audit.stVertex = aVertex.load(std::memory_order_relaxed);
        audit.stTessCtrl = aTessCtrl.load(std::memory_order_relaxed);
        audit.stTessEval = aTessEval.load(std::memory_order_relaxed);
        audit.stGeom = aGeom.load(std::memory_order_relaxed);
        audit.stFrag = aFrag.load(std::memory_order_relaxed);
        audit.stCompute = aCompute.load(std::memory_order_relaxed);
        audit.verifiedComplete = !g_cancel.load(std::memory_order_acquire);
    } else if (cachedState && cachedState->hasStoreAudit &&
               cachedState->storeSig == audit.storeSig && !forceRewarm) {
        audit.spvInvalid = cachedState->cachedAudit.spvInvalid;
        audit.stVertex = cachedState->cachedAudit.stVertex;
        audit.stTessCtrl = cachedState->cachedAudit.stTessCtrl;
        audit.stTessEval = cachedState->cachedAudit.stTessEval;
        audit.stGeom = cachedState->cachedAudit.stGeom;
        audit.stFrag = cachedState->cachedAudit.stFrag;
        audit.stCompute = cachedState->cachedAudit.stCompute;
        audit.verifiedComplete = true;
    }

    return audit;
}

void warmMetaFilesToOsCache(
    const fs::path& directSetsDir,
    const std::vector<std::wstring>& metaNames,
    std::uint32_t threads) {
    if (metaNames.empty() || g_cancel.load(std::memory_order_acquire)) {
        return;
    }

    std::wstring dirPrefix = directSetsDir.wstring();
    if (!dirPrefix.empty() && dirPrefix.back() != L'\\' && dirPrefix.back() != L'/') {
        dirPrefix.push_back(L'\\');
    }

    const std::uint32_t workerCount = std::min<std::uint32_t>(
        std::clamp(threads, 2u, 8u),
        static_cast<std::uint32_t>(metaNames.size()));

    std::atomic<std::size_t> nextIdx{0};
    auto worker = [&]() {
        std::uint8_t buf[32768];
        std::wstring fullPath;
        fullPath.reserve(dirPrefix.size() + 64);

        while (!g_cancel.load(std::memory_order_acquire)) {
            const std::size_t i = nextIdx.fetch_add(1, std::memory_order_relaxed);
            if (i >= metaNames.size()) break;

            fullPath.assign(dirPrefix);
            fullPath.append(metaNames[i]);

            HANDLE hFile = CreateFileW(
                fullPath.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD bytesRead = 0;
                while (ReadFile(hFile, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
                    if (bytesRead < sizeof(buf)) break;
                }
                CloseHandle(hFile);
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(workerCount);
    for (std::uint32_t t = 0; t < workerCount; ++t) {
        pool.emplace_back(worker);
    }
    for (auto& t : pool) {
        t.join();
    }
}

std::string jsonEscape(const std::string& s) {
    std::ostringstream oss;
    for (unsigned char c : s) {
        if (c == '\\') oss << "\\\\";
        else if (c == '"') oss << "\\\"";
        else if (c >= 32) oss << c;
    }
    return oss.str();
}

bool atomicWriteText(const fs::path& path, const std::string& content) {
    fs::path tmp = path;
    tmp += L".tmp-" + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        f.flush();
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool atomicWriteBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    fs::path tmp = path;
    tmp += L".tmp-" + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        f.flush();
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool writePrewarmState(
    const fs::path& statePath,
    const std::string& stateKey,
    std::uint64_t finalCacheBytes,
    const StoreAudit& store,
    const std::vector<WarmRecipe>& recipes,
    const std::vector<std::uint8_t>& recipeSucceeded) {
    std::ostringstream stateOss;
    stateOss << "KEY=" << stateKey << "\nCACHE_SIZE=" << finalCacheBytes << "\n";
    if (store.verifiedComplete && store.storeSig != 0 && store.spvInvalid == 0) {
        stateOss << "STORE_AUDIT=" << std::hex << store.storeSig
                 << "|maxwt=" << store.maxWriteTime << std::dec
                 << "|spv=" << store.spvCount
                 << "|meta=" << store.metaCount
                 << "|bytes=" << store.totalBytes
                 << "|bad=" << store.spvInvalid
                 << "|V:" << store.stVertex
                 << ",TC:" << store.stTessCtrl
                 << ",TE:" << store.stTessEval
                 << ",G:" << store.stGeom
                 << ",F:" << store.stFrag
                 << ",C:" << store.stCompute << "\n";
    }
    for (std::size_t i = 0; i < recipes.size(); ++i) {
        if (recipeSucceeded[i]) {
            stateOss << "RECIPE=" << recipes[i].filename << "|" << std::hex << recipes[i].fnvHash << std::dec << "\n";
        }
    }
    return atomicWriteText(statePath, stateOss.str());
}

struct VulkanCtx {
    HMODULE dll = nullptr;
    VkInstance instance = nullptr;
    VkPhysicalDevice physicalDevice = nullptr;
    VkPhysicalDeviceProperties props{};
    VkDevice device = nullptr;

    PFN_vkCreatePipelineCache createPipelineCache = nullptr;
    PFN_vkDestroyPipelineCache destroyPipelineCache = nullptr;
    PFN_vkGetPipelineCacheData getPipelineCacheData = nullptr;
    PFN_vkCreateShaderModule createShaderModule = nullptr;
    PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines createComputePipelines = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;

    ~VulkanCtx() {
        if (device && destroyDevice) destroyDevice(device, nullptr);
        if (instance && destroyInstance) destroyInstance(instance, nullptr);
        if (dll) FreeLibrary(dll);
    }

    std::uint32_t maxPushDescriptors = 0;
    VkPhysicalDeviceSubgroupSizeControlProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    bool subgroupControl = false;

    bool init(std::string& err, int deviceIndex) {
        dll = LoadLibraryA("vulkan-1.dll");
        if (!dll) {
            err = "vulkan-1.dll could not be loaded";
            return false;
        }
        auto getInstProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dll, "vkGetInstanceProcAddr"));
        if (!getInstProc) {
            err = "vkGetInstanceProcAddr missing";
            return false;
        }
        auto pCreateInst = reinterpret_cast<PFN_vkCreateInstance>(getInstProc(nullptr, "vkCreateInstance"));
        if (!pCreateInst) {
            err = "vkCreateInstance missing";
            return false;
        }

        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "ryty_prewarm";
        app.applicationVersion = 1;
        app.pEngineName = "RyGT7";
        app.engineVersion = 1;
        app.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        if (pCreateInst(&ici, nullptr, &instance) != VK_SUCCESS) {
            err = "Vulkan 1.3 instance creation failed";
            return false;
        }

        auto loadI = [&](const char* n) { return getInstProc(instance, n); };
        destroyInstance = reinterpret_cast<decltype(destroyInstance)>(loadI("vkDestroyInstance"));

        auto pEnumPD = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(loadI("vkEnumeratePhysicalDevices"));
        auto pGetProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(loadI("vkGetPhysicalDeviceProperties"));
        auto pGetQF = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(loadI("vkGetPhysicalDeviceQueueFamilyProperties"));
        auto pCreateDev = reinterpret_cast<PFN_vkCreateDevice>(loadI("vkCreateDevice"));
        auto pGetDevProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(loadI("vkGetDeviceProcAddr"));

        const auto getMemory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            loadI("vkGetPhysicalDeviceMemoryProperties"));
        const auto getFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            loadI("vkGetPhysicalDeviceFeatures2"));
        const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            loadI("vkGetPhysicalDeviceProperties2"));
        const auto enumerateExtensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            loadI("vkEnumerateDeviceExtensionProperties"));
        std::uint32_t count = 0;
        if (pEnumPD(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
            err = "No Vulkan physical devices found";
            return false;
        }
        std::vector<VkPhysicalDevice> pds(count);
        if (pEnumPD(instance, &count, pds.data()) != VK_SUCCESS || !count ||
            (deviceIndex >= 0 && static_cast<std::uint32_t>(deviceIndex) >= count)) {
            err = "Could not select the requested Vulkan device";
            return false;
        }
        auto rank = [&](VkPhysicalDevice pd, const VkPhysicalDeviceProperties& properties) {
            VkPhysicalDeviceMemoryProperties memory{};
            getMemory(pd, &memory);
            VkDeviceSize largest = 0;
            for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i)
                if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    largest = std::max(largest, memory.memoryHeaps[i].size);
            return std::tuple{properties.apiVersion >= VK_API_VERSION_1_3,
                              properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                              properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU, largest};
        };
        physicalDevice = pds[deviceIndex >= 0 ? static_cast<std::size_t>(deviceIndex) : 0];
        pGetProps(physicalDevice, &props);
        if (deviceIndex < 0) {
            for (std::uint32_t i = 1; i < count; ++i) {
                VkPhysicalDeviceProperties candidate{};
                pGetProps(pds[i], &candidate);
                if (rank(pds[i], candidate) > rank(physicalDevice, props)) {
                    physicalDevice = pds[i];
                    props = candidate;
                }
            }
        }
        if (props.apiVersion < VK_API_VERSION_1_3) {
            err = "Selected GPU does not support Vulkan 1.3";
            return false;
        }

        std::uint32_t extensionCount = 0;
        if (enumerateExtensions(physicalDevice, nullptr, &extensionCount, nullptr) != VK_SUCCESS) {
            err = "Could not query device extensions";
            return false;
        }
        std::vector<VkExtensionProperties> extensions(extensionCount);
        if (enumerateExtensions(physicalDevice, nullptr, &extensionCount, extensions.data()) != VK_SUCCESS) {
            err = "Could not read device extensions";
            return false;
        }
        const char* pushExtension = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        const bool push = std::any_of(extensions.begin(), extensions.end(), [&](const auto& extension) {
            return std::strcmp(extension.extensionName, pushExtension) == 0;
        });
        VkPhysicalDevicePushDescriptorPropertiesKHR pushProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
        subgroup.pNext = push ? &pushProps : nullptr;
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup};
        getProperties(physicalDevice, &properties);
        subgroup.pNext = nullptr;
        maxPushDescriptors = push ? pushProps.maxPushDescriptors : 0;
        VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &features11};
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &features12};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features13};
        getFeatures(physicalDevice, &features);
        subgroupControl = features13.subgroupSizeControl;
        std::uint32_t qfCount = 0;
        pGetQF(physicalDevice, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        pGetQF(physicalDevice, &qfCount, qfs.data());
        std::uint32_t computeQ = qfCount;
        for (std::uint32_t i = 0; i < qfCount; ++i) {
            if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { computeQ = i; break; }
        }

        if (computeQ == qfCount) {
            err = "Selected GPU has no compute queue";
            return false;
        }

        float prio = 1.0f;
        VkDeviceQueueCreateInfo dq{};
        dq.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        dq.queueFamilyIndex = computeQ;
        dq.queueCount = 1;
        dq.pQueuePriorities = &prio;

        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &dq;
        dci.pNext = &features;
        if (push) {
            dci.enabledExtensionCount = 1;
            dci.ppEnabledExtensionNames = &pushExtension;
        }
        if (pCreateDev(physicalDevice, &dci, nullptr, &device) != VK_SUCCESS) {
            err = "vkCreateDevice failed";
            return false;
        }

        auto loadD = [&](const char* n) { return pGetDevProc(device, n); };
        destroyDevice = reinterpret_cast<decltype(destroyDevice)>(loadD("vkDestroyDevice"));
        createPipelineCache = reinterpret_cast<decltype(createPipelineCache)>(loadD("vkCreatePipelineCache"));
        destroyPipelineCache = reinterpret_cast<decltype(destroyPipelineCache)>(loadD("vkDestroyPipelineCache"));
        getPipelineCacheData = reinterpret_cast<decltype(getPipelineCacheData)>(loadD("vkGetPipelineCacheData"));
        createShaderModule = reinterpret_cast<decltype(createShaderModule)>(loadD("vkCreateShaderModule"));
        destroyShaderModule = reinterpret_cast<decltype(destroyShaderModule)>(loadD("vkDestroyShaderModule"));
        createDescriptorSetLayout = reinterpret_cast<decltype(createDescriptorSetLayout)>(loadD("vkCreateDescriptorSetLayout"));
        destroyDescriptorSetLayout = reinterpret_cast<decltype(destroyDescriptorSetLayout)>(loadD("vkDestroyDescriptorSetLayout"));
        createPipelineLayout = reinterpret_cast<decltype(createPipelineLayout)>(loadD("vkCreatePipelineLayout"));
        destroyPipelineLayout = reinterpret_cast<decltype(destroyPipelineLayout)>(loadD("vkDestroyPipelineLayout"));
        createComputePipelines = reinterpret_cast<decltype(createComputePipelines)>(loadD("vkCreateComputePipelines"));
        destroyPipeline = reinterpret_cast<decltype(destroyPipeline)>(loadD("vkDestroyPipeline"));
        return true;
    }
};

void startStdinCancelWatcher() {
    std::thread([]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.find("CANCEL") != std::string::npos || line.find("cancel") != std::string::npos) {
                g_cancel.store(true, std::memory_order_release);
                break;
            }
        }
    }).detach();
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (argc < 2) {
        std::fprintf(stderr, "Usage: ryty_prewarm <inspect|prewarm> --root <dir> [--device-index N] [--max-threads N] [--cache-max-mb M] [--verify-store] [--no-warm-meta] [--force]\n");
        return 2;
    }

    const std::string mode = argv[1];
    fs::path root;
    std::uint32_t maxThreads = 0;
    int deviceIndex = -1;
    std::uint64_t cacheMaxMb = 3072;
    bool verifyStore = (mode == "prewarm");
    bool warmMeta = (mode == "prewarm");
    bool forceRewarm = false;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--root" && i + 1 < argc) root = fs::path(argv[++i]);
        else if (arg == "--device-index" && i + 1 < argc) deviceIndex = std::atoi(argv[++i]);
        else if (arg == "--max-threads" && i + 1 < argc) maxThreads = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (arg == "--cache-max-mb" && i + 1 < argc) cacheMaxMb = std::strtoull(argv[++i], nullptr, 10);
        else if (arg == "--verify-store") verifyStore = true;
        else if (arg == "--no-verify-store") verifyStore = false;
        else if (arg == "--warm-meta") warmMeta = true;
        else if (arg == "--no-warm-meta") warmMeta = false;
        else if (arg == "--force") forceRewarm = true;
    }

    if (root.empty() || (mode != "inspect" && mode != "prewarm")) {
        std::fprintf(stderr, "ERROR|invalid mode or missing --root path\n");
        return 2;
    }

    const auto startTs = SteadyClock::now();
    startStdinCancelWatcher();

    VulkanCtx vk;
    std::string vkErr;
    if (!vk.init(vkErr, deviceIndex)) {
        emitLine("ERROR|vulkan|" + vkErr);
        return 1;
    }

    const std::string gpuUuid = formatUuidHex(vk.props.pipelineCacheUUID);
    std::ostringstream cacheIdentity;
    cacheIdentity << "CUSA24769-" << std::hex << std::setw(4) << std::setfill('0') << vk.props.vendorID << "-"
                  << std::setw(4) << vk.props.deviceID << "-" << std::setw(8) << vk.props.driverVersion << "-"
                  << gpuUuid;

    {
        std::ostringstream oss;
        oss << "INFO|gpu=" << jsonEscape(vk.props.deviceName)
            << "|vendor=" << std::hex << std::setw(4) << std::setfill('0') << vk.props.vendorID
            << "|device=" << std::setw(4) << std::setfill('0') << vk.props.deviceID
            << "|driver=" << std::dec << vk.props.driverVersion
            << "|uuid=" << gpuUuid;
        emitLine(oss.str());
    }

    const fs::path shadersDir = root / "shaders";
    const fs::path directSetsDir = shadersDir / "CUSA24769-direct-sets";
    const fs::path wcDir = shadersDir / "CUSA24769.warm_compute";
    const fs::path cachePath = shadersDir / (cacheIdentity.str() + ".vkpipelinecache");
    const fs::path statePath = shadersDir / (cacheIdentity.str() + ".prewarm_state.txt");

    emitLine("STAGE|AUDIT|0|1|Auditing shader store and pipeline cache...");
    const CacheHeaderCheck cacheInfo = inspectPipelineCacheHeader(cachePath, vk.props);
    const std::string rendererBuild = readRendererBuild(directSetsDir);
    const std::string stateKey = cacheIdentity.str() + ":" + rendererBuild;
    const std::uint64_t expectedCacheSize =
        (cacheInfo.exists && cacheInfo.validHeader && cacheInfo.uuidMatches) ? cacheInfo.sizeBytes : 0;
    PrewarmState state = loadCompletedState(statePath, stateKey, expectedCacheSize);
    if (forceRewarm) {
        state.done.clear();
    }

    std::vector<std::wstring> metaNames;
    const StoreAudit store = auditDirectSets(
        directSetsDir,
        verifyStore,
        &state,
        forceRewarm,
        warmMeta ? &metaNames : nullptr);

    // Discover WarmCompute recipes
    std::vector<WarmRecipe> recipes;
    std::error_code ec;
    if (fs::exists(wcDir, ec)) {
        std::size_t recipeBytes = 0;
        for (fs::directory_iterator it(wcDir, ec), end; !ec && it != end && recipes.size() < 1024;
             it.increment(ec)) {
            const auto& e = *it;
            if (e.is_regular_file(ec) && e.path().extension() == ".bin") {
                WarmRecipe r;
                if (!parseWarmRecipe(e.path(), r)) continue;
                const auto descriptors = r.bindings.empty() ? 0 :
                    r.bindings.back().binding + r.bindings.back().descriptorCount;
                if (r.pushDescriptors && descriptors >= vk.maxPushDescriptors) continue;
                if (r.subgroupCtl && (!vk.subgroupControl ||
                    !(vk.subgroup.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
                    r.subgroupSize < vk.subgroup.minSubgroupSize || r.subgroupSize > vk.subgroup.maxSubgroupSize))
                    continue;
                recipeBytes += r.spirv.size() * sizeof(std::uint32_t);
                if (recipeBytes > (256u << 20)) break;
                recipes.push_back(std::move(r));
            }
        }
        std::sort(recipes.begin(), recipes.end(), [](const WarmRecipe& a, const WarmRecipe& b) {
            return a.filename < b.filename;
        });
    }

    {
        std::ostringstream oss;
        oss << "AUDIT|direct_sets_spv=" << store.spvCount
            << "|direct_sets_meta=" << store.metaCount
            << "|direct_sets_mb=" << std::fixed << std::setprecision(2) << (static_cast<double>(store.totalBytes) / (1024.0 * 1024.0))
            << "|spv_invalid=" << store.spvInvalid
            << "|stages=V:" << store.stVertex << ",TC:" << store.stTessCtrl << ",TE:" << store.stTessEval
            << ",G:" << store.stGeom << ",F:" << store.stFrag << ",C:" << store.stCompute
            << "|warm_compute_recipes=" << recipes.size()
            << "|cache_exists=" << (cacheInfo.exists ? 1 : 0)
            << "|cache_valid=" << (cacheInfo.validHeader ? 1 : 0)
            << "|cache_uuid_match=" << (cacheInfo.uuidMatches ? 1 : 0)
            << "|cache_mb=" << std::fixed << std::setprecision(2) << (static_cast<double>(cacheInfo.sizeBytes) / (1024.0 * 1024.0))
            << "|renderer_build=" << store.rendererBuild;
        emitLine(oss.str());
    }

    if (mode == "inspect") {
        emitLine("DONE|inspect|0|0|0");
        return 0;
    }

    const std::uint32_t hwThreads = std::max(1u, std::thread::hardware_concurrency());
    const std::uint32_t metaWarmThreads = std::clamp(maxThreads > 0 ? maxThreads : hwThreads, 2u, 8u);

    std::vector<std::size_t> pendingIndices;
    std::uint32_t alreadyCached = 0;
    for (std::size_t i = 0; i < recipes.size(); ++i) {
        auto it = state.done.find(recipes[i].filename);
        if (it != state.done.end() && it->second == recipes[i].fnvHash) {
            ++alreadyCached;
        } else {
            pendingIndices.push_back(i);
        }
    }

    if (pendingIndices.empty() && !recipes.empty() &&
        cacheInfo.exists && cacheInfo.validHeader && cacheInfo.uuidMatches) {
        if (cacheInfo.sizeBytes != state.fileCacheSz ||
            !state.hasStoreAudit || state.storeSig != store.storeSig ||
            state.cachedAudit.maxWriteTime != store.maxWriteTime) {
            std::vector<std::uint8_t> allSucceeded(recipes.size(), 1u);
            writePrewarmState(statePath, stateKey, cacheInfo.sizeBytes, store, recipes, allSucceeded);
        }
        if (warmMeta && !metaNames.empty()) {
            warmMetaFilesToOsCache(directSetsDir, metaNames, metaWarmThreads);
        }
        const double elapsedMs = std::chrono::duration<double, std::milli>(SteadyClock::now() - startTs).count();
        std::ostringstream oss;
        oss << "DONE|up_to_date|compiled=0|skipped=" << alreadyCached
            << "|failed=0|cacheMb=" << std::fixed << std::setprecision(2)
            << (static_cast<double>(cacheInfo.sizeBytes) / (1024.0 * 1024.0))
            << "|elapsedMs=" << std::fixed << std::setprecision(1) << elapsedMs;
        emitLine(oss.str());
        return 0;
    }

    if (recipes.empty()) {
        if (warmMeta && !metaNames.empty()) warmMetaFilesToOsCache(directSetsDir, metaNames, metaWarmThreads);
        emitLine("DONE|no_recipes|compiled=0|skipped=0|failed=0");
        return g_cancel.load() ? 3 : 0;
    }

    const auto maxBytes = std::min(cacheMaxMb, std::numeric_limits<std::uint64_t>::max() >> 20) << 20;
    std::vector<std::uint8_t> initCacheBytes;
    if (cacheInfo.exists) {
        if (!cacheInfo.validHeader || !cacheInfo.uuidMatches || (maxBytes && cacheInfo.sizeBytes > maxBytes)) {
            emitLine("WARN|existing_cache|Driver cache left untouched");
            return 1;
        }
        std::ifstream file(cachePath, std::ios::binary);
        initCacheBytes.resize(static_cast<std::size_t>(cacheInfo.sizeBytes));
        if (!file.read(reinterpret_cast<char*>(initCacheBytes.data()), static_cast<std::streamsize>(initCacheBytes.size()))) {
            emitLine("WARN|cache_read_failed|Driver cache left untouched");
            return 1;
        }
    }

    emitLine("STAGE|LOAD_CACHE|0|1|Loading Vulkan pipeline cache...");
    VkPipelineCacheCreateInfo pcci{};
    pcci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    pcci.initialDataSize = initCacheBytes.size();
    pcci.pInitialData = initCacheBytes.empty() ? nullptr : initCacheBytes.data();
    VkPipelineCache vkCache = nullptr;
    if (vk.createPipelineCache(vk.device, &pcci, nullptr, &vkCache) != VK_SUCCESS) {
        emitLine("WARN|cache_create_failed|Driver cache left untouched");
        return 1;
    }
    initCacheBytes.clear();
    initCacheBytes.shrink_to_fit();

    const std::uint32_t workerCount = maxThreads > 0
        ? std::min<std::uint32_t>(maxThreads, std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(pendingIndices.size())))
        : std::clamp(hwThreads / 2u, 1u, 4u);

    std::atomic<std::size_t> nextWork{0};
    std::atomic<std::uint32_t> doneCount{alreadyCached};
    std::atomic<std::uint32_t> compiledCount{0};
    std::atomic<std::uint32_t> failCount{0};
    std::vector<std::uint8_t> recipeSucceeded(recipes.size(), 0u);
    for (std::size_t i = 0; i < recipes.size(); ++i) {
        auto it = state.done.find(recipes[i].filename);
        if (it != state.done.end() && it->second == recipes[i].fnvHash) {
            recipeSucceeded[i] = 1u;
        }
    }

    {
        std::ostringstream oss;
        oss << "STAGE|PREWARM_COMPUTE|" << alreadyCached << "|" << recipes.size()
            << "|Compiling " << pendingIndices.size() << " compute pipelines on " << workerCount << " thread(s)...";
        emitLine(oss.str());
    }

    auto workerFn = [&]() {
        while (!g_cancel.load(std::memory_order_acquire)) {
            const std::size_t pos = nextWork.fetch_add(1, std::memory_order_relaxed);
            if (pos >= pendingIndices.size()) break;
            const std::size_t rIdx = pendingIndices[pos];
            const WarmRecipe& r = recipes[rIdx];

            VkShaderModuleCreateInfo smci{};
            smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            smci.codeSize = r.spirv.size() * 4u;
            smci.pCode = r.spirv.data();
            VkShaderModule mod = nullptr;
            if (vk.createShaderModule(vk.device, &smci, nullptr, &mod) != VK_SUCCESS) {
                failCount.fetch_add(1, std::memory_order_relaxed);
                doneCount.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            VkDescriptorSetLayoutCreateInfo dslci{};
            dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            if (r.pushDescriptors) dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dslci.bindingCount = static_cast<std::uint32_t>(r.bindings.size());
            dslci.pBindings = r.bindings.data();
            VkDescriptorSetLayout dsl = nullptr;
            if (vk.createDescriptorSetLayout(vk.device, &dslci, nullptr, &dsl) != VK_SUCCESS) {
                vk.destroyShaderModule(vk.device, mod, nullptr);
                failCount.fetch_add(1, std::memory_order_relaxed);
                doneCount.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // FEAT-GT7-184: the version 2 recipe ABI uses Shader::PushData (120 bytes).
            VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 120};
            VkPipelineLayoutCreateInfo plci{};
            plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            plci.setLayoutCount = 1;
            plci.pSetLayouts = &dsl;
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &pcr;
            VkPipelineLayout pl = nullptr;
            if (vk.createPipelineLayout(vk.device, &plci, nullptr, &pl) != VK_SUCCESS) {
                vk.destroyDescriptorSetLayout(vk.device, dsl, nullptr);
                vk.destroyShaderModule(vk.device, mod, nullptr);
                failCount.fetch_add(1, std::memory_order_relaxed);
                doneCount.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subInfo{};
            subInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
            subInfo.requiredSubgroupSize = r.subgroupSize ? r.subgroupSize : 32u;

            VkComputePipelineCreateInfo cpci{};
            cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpci.stage.module = mod;
            cpci.stage.pName = "main";
            if (r.subgroupCtl != 0) {
                cpci.stage.pNext = &subInfo;
            }
            cpci.layout = pl;

            VkPipeline pipe = nullptr;
            if (vk.createComputePipelines(vk.device, vkCache, 1, &cpci, nullptr, &pipe) == VK_SUCCESS) {
                compiledCount.fetch_add(1, std::memory_order_relaxed);
                recipeSucceeded[rIdx] = 1u;
                vk.destroyPipeline(vk.device, pipe, nullptr);
            } else {
                failCount.fetch_add(1, std::memory_order_relaxed);
            }

            vk.destroyPipelineLayout(vk.device, pl, nullptr);
            vk.destroyDescriptorSetLayout(vk.device, dsl, nullptr);
            vk.destroyShaderModule(vk.device, mod, nullptr);

            const std::uint32_t dNow = doneCount.fetch_add(1, std::memory_order_relaxed) + 1u;
            if ((dNow % 8u) == 0u || dNow == recipes.size()) {
                const double elapsedMs = std::chrono::duration<double, std::milli>(SteadyClock::now() - startTs).count();
                std::ostringstream oss;
                oss << "PROGRESS|COMPUTE|" << dNow << "|" << recipes.size()
                    << "|compiled=" << compiledCount.load(std::memory_order_relaxed)
                    << "|skipped=" << alreadyCached
                    << "|elapsedMs=" << std::fixed << std::setprecision(0) << elapsedMs;
                emitLine(oss.str());
            }
        }
    };

    std::vector<std::thread> workers;
    for (std::uint32_t w = 0; w < workerCount; ++w) {
        workers.emplace_back(workerFn);
    }
    for (auto& w : workers) w.join();

    const bool cancelled = g_cancel.load(std::memory_order_acquire);

    // Always serialize any completed work safely so cancel supports resume on next run!
    std::uint64_t finalCacheBytes = cacheInfo.sizeBytes;
    bool cacheSaved = false;
    if (compiledCount.load(std::memory_order_relaxed) > 0) {
        emitLine("STAGE|SAVE_CACHE|0|1|Writing serialized Vulkan pipeline cache atomically...");
        std::size_t outSz = 0;
        if (vk.getPipelineCacheData(vk.device, vkCache, &outSz, nullptr) == VK_SUCCESS && outSz >= 32) {
            if (!maxBytes || outSz <= maxBytes) {
                std::vector<std::uint8_t> outBytes(outSz);
                if (vk.getPipelineCacheData(vk.device, vkCache, &outSz, outBytes.data()) == VK_SUCCESS) {
                    outBytes.resize(outSz);
                    const auto current = inspectPipelineCacheHeader(cachePath, vk.props);
                    const bool unchanged = current.exists == cacheInfo.exists &&
                        (!current.exists || (current.validHeader && current.uuidMatches &&
                         current.sizeBytes == cacheInfo.sizeBytes && current.lastWrite == cacheInfo.lastWrite));
                    if (unchanged && atomicWriteBytes(cachePath, outBytes)) {
                        finalCacheBytes = outSz;
                        cacheSaved = true;
                    } else {
                        emitLine("WARN|save_failed|Could not atomically replace the driver cache");
                    }
                }
            } else {
                emitLine("WARN|cache_over_budget|Serialized cache exceeds cache-max-mb budget; skipping overwrite");
            }
        }
    }

    // Write resume/completion state tied strictly to finalCacheBytes
    if (cacheSaved) writePrewarmState(statePath, stateKey, finalCacheBytes, store, recipes, recipeSucceeded);

    vk.destroyPipelineCache(vk.device, vkCache, nullptr);

    if (warmMeta && !metaNames.empty() && !cancelled) {
        warmMetaFilesToOsCache(directSetsDir, metaNames, metaWarmThreads);
    }

    const double totalMs = std::chrono::duration<double, std::milli>(SteadyClock::now() - startTs).count();
    std::ostringstream doneOss;
    doneOss << "DONE|" << (cancelled ? "cancelled" : "ok")
            << "|compiled=" << compiledCount.load()
            << "|skipped=" << alreadyCached
            << "|failed=" << failCount.load()
            << "|cacheMb=" << std::fixed << std::setprecision(2) << (static_cast<double>(finalCacheBytes) / (1024.0 * 1024.0))
            << "|elapsedMs=" << std::fixed << std::setprecision(1) << totalMs;
    emitLine(doneOss.str());
    return cancelled ? 3 : 0;
}
