#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace grd {

// The ImDisk side of a shared-memory proxy drive. The driver writes a request
// into shared memory, signals _Request, and waits for _Response. This class
// owns those named objects and the worker that answers them.
class ProxyServer {
public:
    using ReadHandler =
        std::function<bool(uint64_t offset, void* buffer, size_t length)>;
    using WriteHandler =
        std::function<bool(uint64_t offset, const void* buffer, size_t length)>;

    ProxyServer() = default;
    ~ProxyServer();

    ProxyServer(const ProxyServer&) = delete;
    ProxyServer& operator=(const ProxyServer&) = delete;

    bool start(const std::wstring& serviceName, uint64_t driveSize,
               ReadHandler readHandler, WriteHandler writeHandler,
               std::wstring& error);

    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

private:
    void run();
    void signalResponse();
    void cleanup();

    std::wstring serviceName_;
    uint64_t driveSize_ = 0;
    ReadHandler readHandler_;
    WriteHandler writeHandler_;

    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    uint8_t* data_ = nullptr;
    HANDLE serverMutex_ = nullptr;
    HANDLE requestEvent_ = nullptr;
    HANDLE responseEvent_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    bool ownsMutex_ = false;

    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace grd
