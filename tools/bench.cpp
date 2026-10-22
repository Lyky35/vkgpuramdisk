#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "legacy_proxy_protocol.h"
#include "legacy_proxy_server.h"
#include "gpu/vulkan_drive.h"
#include "gpu/vulkan_runtime.h"

namespace {

using Clock = std::chrono::steady_clock;
using grd::ProxyServer;
using grd::VulkanDrive;
using grd::VulkanRuntime;

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kSector = 4096;

struct Options {
    uint32_t gpu = 0;
    uint64_t sizeMiB = 256;
    uint64_t randomOps = 32768;
    uint64_t chunkMiB = 4;
    bool raw = true;
    bool proxy = true;
};

struct Result {
    double wallSeconds = 0.0;
    double cpuSeconds = 0.0;
    uint64_t bytes = 0;
    uint64_t ops = 0;
};

uint64_t nextRandom(uint64_t& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1Dull;
}

double processCpuSeconds() {
    FILETIME creation = {};
    FILETIME exit = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0.0;
    }

    ULARGE_INTEGER k = {};
    ULARGE_INTEGER u = {};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 10'000'000.0;
}

template <typename Fn>
Result measure(uint64_t bytes, uint64_t ops, Fn&& fn) {
    const double cpuStart = processCpuSeconds();
    const auto wallStart = Clock::now();
    fn();
    const auto wallEnd = Clock::now();
    const double cpuEnd = processCpuSeconds();

    Result result;
    result.wallSeconds =
        std::chrono::duration<double>(wallEnd - wallStart).count();
    result.cpuSeconds = std::max(0.0, cpuEnd - cpuStart);
    result.bytes = bytes;
    result.ops = ops;
    return result;
}

void printResult(const char* label, const Result& result) {
    const double seconds = std::max(result.wallSeconds, 0.000001);
    const double mib = static_cast<double>(result.bytes) / static_cast<double>(kMiB);
    const double iops = static_cast<double>(result.ops) / seconds;
    const double cpuPercent = 100.0 * result.cpuSeconds / seconds;
    const double usecPerOp =
        result.ops == 0 ? 0.0 : 1'000'000.0 * result.cpuSeconds /
                                    static_cast<double>(result.ops);
    std::printf("%-26s %10.1f MiB/s %10.0f IOPS %8.1f ms CPU %6.1f%% %8.1f us/op\n",
                label, mib / seconds, iops, result.cpuSeconds * 1000.0,
                cpuPercent, usecPerOp);
}

bool parseBytes(const char* text, uint64_t& value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

void printUsage() {
    std::printf(
        "Usage: gpuvramdisk-bench [options]\n"
        "  --gpu N       Vulkan GPU index (default 0)\n"
        "  --size MiB    drive size in MiB (default 256)\n"
        "  --ops N       random-I/O operation count (default 32768)\n"
        "  --chunk MiB   sequential transfer size (default 4)\n"
        "  --raw-only    benchmark Vulkan storage without the proxy protocol\n"
        "  --proxy-only  benchmark only the shared-memory proxy path\n"
        "  --help        show this help\n"
        "\n"
        "The proxy benchmark is a legacy in-process measurement, not the\n"
        "WinSpd path. It requires the same privileges as a mounted drive.\n"
        "Set GPURAMDRIVE_FORCE_STAGING=1 to compare the staging path.\n");
}

bool parseOptions(int argc, char** argv, Options& options, bool& showHelp) {
    showHelp = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            showHelp = true;
            return true;
        }
        if (arg == "--raw-only") {
            options.proxy = false;
            continue;
        }
        if (arg == "--proxy-only") {
            options.raw = false;
            continue;
        }

        auto nextValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("Missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--gpu") {
            const char* value = nextValue("--gpu");
            uint64_t parsed = 0;
            if (value == nullptr || !parseBytes(value, parsed) ||
                parsed > UINT32_MAX) {
                std::printf("Invalid --gpu value\n");
                return false;
            }
            options.gpu = static_cast<uint32_t>(parsed);
            continue;
        }
        if (arg == "--size") {
            const char* value = nextValue("--size");
            if (value == nullptr || !parseBytes(value, options.sizeMiB) ||
                options.sizeMiB < 8) {
                std::printf("Invalid --size value (minimum 8 MiB)\n");
                return false;
            }
            continue;
        }
        if (arg == "--ops") {
            const char* value = nextValue("--ops");
            if (value == nullptr || !parseBytes(value, options.randomOps) ||
                options.randomOps == 0) {
                std::printf("Invalid --ops value\n");
                return false;
            }
            continue;
        }
        if (arg == "--chunk") {
            const char* value = nextValue("--chunk");
            if (value == nullptr || !parseBytes(value, options.chunkMiB) ||
                options.chunkMiB == 0 || options.chunkMiB > 256) {
                std::printf("Invalid --chunk value (1-256 MiB)\n");
                return false;
            }
            continue;
        }

        std::printf("Unknown option: %s\n", arg.c_str());
        return false;
    }
    return true;
}

void fillPattern(std::vector<uint8_t>& buffer, uint64_t seed) {
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<uint8_t>((seed + i * 37u + 11u) & 0xFFu);
    }
}

uint64_t randomOffset(uint64_t& state, uint64_t driveSize, uint64_t length) {
    const uint64_t slots = driveSize / length;
    return (nextRandom(state) % slots) * length;
}

void runRawBenchmarks(VulkanDrive& drive, const Options& options) {
    const uint64_t chunkBytes =
        std::min<uint64_t>(options.chunkMiB * kMiB, drive.size());
    std::vector<uint8_t> buffer(static_cast<size_t>(chunkBytes));
    fillPattern(buffer, 0x1234);

    std::printf("\nRaw Vulkan storage (%s, %llu MiB transfers)\n",
                drive.directMapped() ? "direct mapped" : "staged",
                static_cast<unsigned long long>(chunkBytes / kMiB));
    const uint64_t sequentialBytes = drive.size();
    const uint64_t sequentialOps =
        (sequentialBytes + chunkBytes - 1) / chunkBytes;

    Result write = measure(sequentialBytes, sequentialOps, [&] {
        for (uint64_t offset = 0; offset < sequentialBytes;
             offset += chunkBytes) {
            const uint64_t length =
                std::min<uint64_t>(chunkBytes, sequentialBytes - offset);
            drive.write(offset, buffer.data(), static_cast<size_t>(length));
        }
    });
    printResult("sequential write", write);

    Result read = measure(sequentialBytes, sequentialOps, [&] {
        for (uint64_t offset = 0; offset < sequentialBytes;
             offset += chunkBytes) {
            const uint64_t length =
                std::min<uint64_t>(chunkBytes, sequentialBytes - offset);
            drive.read(offset, buffer.data(), static_cast<size_t>(length));
        }
    });
    printResult("sequential read", read);

    std::vector<uint8_t> small(static_cast<size_t>(kSector));
    fillPattern(small, 0x5678);
    uint64_t state = 0x9E3779B97F4A7C15ull;

    Result randomWrite =
        measure(options.randomOps * kSector, options.randomOps, [&] {
            for (uint64_t i = 0; i < options.randomOps; ++i) {
                const uint64_t offset =
                    randomOffset(state, drive.size(), kSector);
                drive.write(offset, small.data(), small.size());
            }
        });
    printResult("random 4K write", randomWrite);

    state = 0x9E3779B97F4A7C15ull;
    Result randomRead =
        measure(options.randomOps * kSector, options.randomOps, [&] {
            for (uint64_t i = 0; i < options.randomOps; ++i) {
                const uint64_t offset =
                    randomOffset(state, drive.size(), kSector);
                drive.read(offset, small.data(), small.size());
            }
        });
    printResult("random 4K read", randomRead);
}

class ProxyClient {
public:
    ~ProxyClient() { closeHandles(); }

    bool open(const std::wstring& serviceName) {
        const std::wstring prefix = L"Global\\" + serviceName;
        mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, prefix.c_str());
        if (mapping_ == nullptr) {
            std::printf("OpenFileMapping failed: %lu\n", GetLastError());
            return false;
        }
        view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (view_ == nullptr) {
            std::printf("MapViewOfFile failed: %lu\n", GetLastError());
            return false;
        }
        data_ = static_cast<uint8_t*>(view_) + grd::kProxyHeaderSize;
        request_ = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                              (prefix + L"_Request").c_str());
        response_ = OpenEventW(SYNCHRONIZE, FALSE,
                               (prefix + L"_Response").c_str());
        if (request_ == nullptr || response_ == nullptr) {
            std::printf("OpenEvent failed: %lu\n", GetLastError());
            return false;
        }
        return true;
    }

    bool info(uint64_t& size) {
        grd::ProxyReadRequest request = {};
        request.requestCode =
            static_cast<uint64_t>(grd::ProxyRequestCode::Info);
        if (!roundTrip(&request, sizeof(request))) {
            return false;
        }
        const auto* response =
            static_cast<const grd::ProxyInfoResponse*>(view_);
        size = response->fileSize;
        return true;
    }

    bool write(uint64_t offset, const void* source, size_t length) {
        grd::ProxyWriteRequest request = {};
        request.requestCode =
            static_cast<uint64_t>(grd::ProxyRequestCode::Write);
        request.offset = offset;
        request.length = length;
        std::memcpy(view_, &request, sizeof(request));
        std::memcpy(data_, source, length);
        if (!signalAndWait()) {
            return false;
        }
        const auto* response = static_cast<const grd::ProxyWriteResponse*>(view_);
        return response->error == 0 && response->length == length;
    }

    bool read(uint64_t offset, void* destination, size_t length) {
        grd::ProxyReadRequest request = {};
        request.requestCode =
            static_cast<uint64_t>(grd::ProxyRequestCode::Read);
        request.offset = offset;
        request.length = length;
        if (!roundTrip(&request, sizeof(request))) {
            return false;
        }
        const auto* response = static_cast<const grd::ProxyReadResponse*>(view_);
        if (response->error != 0 || response->length != length) {
            return false;
        }
        std::memcpy(destination, data_, length);
        return true;
    }

    void close() {
        grd::ProxyReadRequest request = {};
        request.requestCode =
            static_cast<uint64_t>(grd::ProxyRequestCode::Close);
        std::memcpy(view_, &request, sizeof(request));
        SetEvent(request_);
        closeHandles();
    }

private:
    bool roundTrip(const void* request, size_t requestSize) {
        std::memcpy(view_, request, requestSize);
        return signalAndWait();
    }

    bool signalAndWait() {
        SetEvent(request_);
        const DWORD wait = WaitForSingleObject(response_, 30'000);
        if (wait != WAIT_OBJECT_0) {
            std::printf("Proxy response wait failed: %lu\n", wait);
            return false;
        }
        return true;
    }

    void closeHandles() {
        if (view_ != nullptr) {
            UnmapViewOfFile(view_);
            view_ = nullptr;
            data_ = nullptr;
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        if (request_ != nullptr) {
            CloseHandle(request_);
            request_ = nullptr;
        }
        if (response_ != nullptr) {
            CloseHandle(response_);
            response_ = nullptr;
        }
    }

    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    uint8_t* data_ = nullptr;
    HANDLE request_ = nullptr;
    HANDLE response_ = nullptr;
};

void runProxyBenchmarks(VulkanDrive& drive, const Options& options) {
    const std::wstring service =
        L"GpuRamDriveVk_Bench_" + std::to_wstring(GetCurrentProcessId());
    ProxyServer server;
    std::wstring error;
    if (!server.start(
            service, drive.size(),
            [&](uint64_t offset, void* buffer, size_t length) {
                drive.read(offset, buffer, length);
                return true;
            },
            [&](uint64_t offset, const void* buffer, size_t length) {
                drive.write(offset, buffer, length);
                return true;
            },
            error)) {
        std::printf("\nProxy benchmark skipped: %ls\n", error.c_str());
        return;
    }

    ProxyClient client;
    if (!client.open(service)) {
        server.stop();
        return;
    }

    uint64_t reportedSize = 0;
    if (!client.info(reportedSize) || reportedSize != drive.size()) {
        std::printf("\nProxy benchmark skipped: INFO failed\n");
        client.close();
        server.stop();
        return;
    }

    const uint64_t chunk = std::min<uint64_t>(
        grd::kProxyBufferSize, options.chunkMiB * kMiB);
    const uint64_t total = std::min<uint64_t>(drive.size(), 64ull * kMiB);
    const uint64_t sequentialOps = (total + chunk - 1) / chunk;
    std::vector<uint8_t> buffer(static_cast<size_t>(chunk));
    fillPattern(buffer, 0x9ABC);

    std::printf("\nShared-memory proxy path (%llu MiB transfers)\n",
                static_cast<unsigned long long>(chunk / kMiB));
    Result write = measure(total, sequentialOps, [&] {
        for (uint64_t offset = 0; offset < total; offset += chunk) {
            const uint64_t length =
                std::min<uint64_t>(chunk, total - offset);
            if (!client.write(offset, buffer.data(),
                              static_cast<size_t>(length))) {
                std::printf("Proxy write failed\n");
                break;
            }
        }
    });
    printResult("sequential write", write);

    Result read = measure(total, sequentialOps, [&] {
        for (uint64_t offset = 0; offset < total; offset += chunk) {
            const uint64_t length =
                std::min<uint64_t>(chunk, total - offset);
            if (!client.read(offset, buffer.data(),
                             static_cast<size_t>(length))) {
                std::printf("Proxy read failed\n");
                break;
            }
        }
    });
    printResult("sequential read", read);

    std::vector<uint8_t> small(static_cast<size_t>(kSector));
    fillPattern(small, 0xDEF0);
    uint64_t state = 0xA5A5A5A5A5A5A5A5ull;
    const uint64_t proxyRandomOps =
        std::min<uint64_t>(options.randomOps, 4096);

    Result randomWrite =
        measure(proxyRandomOps * kSector, proxyRandomOps, [&] {
            for (uint64_t i = 0; i < proxyRandomOps; ++i) {
                const uint64_t offset =
                    randomOffset(state, drive.size(), kSector);
                if (!client.write(offset, small.data(), small.size())) {
                    std::printf("Proxy random write failed\n");
                    break;
                }
            }
        });
    printResult("random 4K write", randomWrite);

    state = 0xA5A5A5A5A5A5A5A5ull;
    Result randomRead =
        measure(proxyRandomOps * kSector, proxyRandomOps, [&] {
            for (uint64_t i = 0; i < proxyRandomOps; ++i) {
                const uint64_t offset =
                    randomOffset(state, drive.size(), kSector);
                if (!client.read(offset, small.data(), small.size())) {
                    std::printf("Proxy random read failed\n");
                    break;
                }
            }
        });
    printResult("random 4K read", randomRead);

    client.close();
    server.stop();
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    bool showHelp = false;
    if (!parseOptions(argc, argv, options, showHelp)) {
        return 2;
    }
    if (showHelp) {
        printUsage();
        return 0;
    }

    std::wstring error;
    auto& runtime = VulkanRuntime::instance();
    if (!runtime.initialize(error)) {
        std::printf("Vulkan initialization failed: %ls\n", error.c_str());
        return 1;
    }
    if (options.gpu >= runtime.devices().size()) {
        std::printf("GPU index %u is out of range (%zu devices)\n", options.gpu,
                    runtime.devices().size());
        return 1;
    }

    const auto& device = runtime.devices()[options.gpu];
    std::printf("vkgpuramdisk benchmark\n");
    std::printf("GPU %u: %ls (%ls)\n", options.gpu, device.name.c_str(),
                device.typeName().c_str());
    std::printf("Drive size: %llu MiB, random operations: %llu, chunk: %llu MiB\n",
                static_cast<unsigned long long>(options.sizeMiB),
                static_cast<unsigned long long>(options.randomOps),
                static_cast<unsigned long long>(options.chunkMiB));

    auto drive = runtime.createDrive(
        options.gpu, options.sizeMiB * kMiB, error);
    if (!drive) {
        std::printf("Drive creation failed: %ls\n", error.c_str());
        return 1;
    }
    std::printf("Storage: %ls\n", drive->info().c_str());

    if (options.raw) {
        runRawBenchmarks(*drive, options);
    }
    if (options.proxy) {
        runProxyBenchmarks(*drive, options);
    }
    return 0;
}
