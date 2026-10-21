#include "core/proxy_server.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <format>
#include <utility>

#include "core/log.h"
#include "core/proxy_protocol.h"
#include "core/util.h"

namespace grd {
namespace {

std::wstring globalName(const std::wstring& serviceName,
                        const wchar_t* suffix = L"") {
    return L"Global\\" + serviceName + suffix;
}

}  // namespace

ProxyServer::~ProxyServer() {
    stop();
}

bool ProxyServer::start(const std::wstring& serviceName, uint64_t driveSize,
                        ReadHandler readHandler, WriteHandler writeHandler,
                        std::wstring& error) {
    error.clear();
    if (running_) {
        error = L"The proxy is already running.";
        return false;
    }

    serviceName_ = serviceName;
    driveSize_ = driveSize;
    readHandler_ = std::move(readHandler);
    writeHandler_ = std::move(writeHandler);

    const std::wstring mappingName = globalName(serviceName_);
    const uint64_t mappingSize = kProxyHeaderSize + kProxyBufferSize;

    SetLastError(0);
    mapping_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
        static_cast<DWORD>(mappingSize >> 32),
        static_cast<DWORD>(mappingSize & 0xFFFFFFFF), mappingName.c_str());
    if (mapping_ == nullptr) {
        error = L"Unable to create the proxy shared memory: " + lastError();
        cleanup();
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        error = L"A drive with the same proxy name is already running: " +
                serviceName_;
        cleanup();
        return false;
    }

    view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (view_ == nullptr) {
        error = L"Unable to map the proxy shared memory: " + lastError();
        cleanup();
        return false;
    }
    data_ = static_cast<uint8_t*>(view_) + kProxyHeaderSize;

    serverMutex_ = CreateMutexW(nullptr, FALSE,
                                globalName(serviceName_, L"_Server").c_str());
    if (serverMutex_ == nullptr) {
        error = L"Unable to create the proxy mutex: " + lastError();
        cleanup();
        return false;
    }

    const DWORD wait = WaitForSingleObject(serverMutex_, 0);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        ownsMutex_ = true;
    } else if (wait == WAIT_TIMEOUT) {
        error = L"Another process is already serving this drive.";
        cleanup();
        return false;
    } else {
        error = L"Unable to acquire the proxy mutex: " + lastError();
        cleanup();
        return false;
    }

    requestEvent_ = CreateEventW(nullptr, FALSE, FALSE,
                                 globalName(serviceName_, L"_Request").c_str());
    if (requestEvent_ == nullptr) {
        error = L"Unable to create the proxy request event: " + lastError();
        cleanup();
        return false;
    }

    responseEvent_ = CreateEventW(
        nullptr, FALSE, FALSE, globalName(serviceName_, L"_Response").c_str());
    if (responseEvent_ == nullptr) {
        error = L"Unable to create the proxy response event: " + lastError();
        cleanup();
        return false;
    }

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent_ == nullptr) {
        error = L"Unable to create the proxy stop event: " + lastError();
        cleanup();
        return false;
    }

    running_.store(true, std::memory_order_release);
    try {
        thread_ = std::thread(&ProxyServer::run, this);
    } catch (const std::exception& ex) {
        running_.store(false, std::memory_order_release);
        cleanup();
        error = L"Unable to start the proxy thread: " + widen(ex.what());
        return false;
    } catch (...) {
        running_.store(false, std::memory_order_release);
        cleanup();
        error = L"Unable to start the proxy thread.";
        return false;
    }
    return true;
}

void ProxyServer::stop() {
    if (stopEvent_ != nullptr) {
        SetEvent(stopEvent_);
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    running_.store(false, std::memory_order_release);
    cleanup();
}

void ProxyServer::cleanup() {
    if (view_ != nullptr) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
        data_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    if (requestEvent_ != nullptr) {
        CloseHandle(requestEvent_);
        requestEvent_ = nullptr;
    }
    if (responseEvent_ != nullptr) {
        CloseHandle(responseEvent_);
        responseEvent_ = nullptr;
    }
    if (stopEvent_ != nullptr) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    if (serverMutex_ != nullptr) {
        if (ownsMutex_) {
            ReleaseMutex(serverMutex_);
            ownsMutex_ = false;
        }
        CloseHandle(serverMutex_);
        serverMutex_ = nullptr;
    }
}

void ProxyServer::signalResponse() {
    if (responseEvent_ != nullptr) {
        SetEvent(responseEvent_);
    }
}

void ProxyServer::run() {
    while (running_.load(std::memory_order_acquire)) {
        const HANDLE handles[2] = {requestEvent_, stopEvent_};
        const DWORD wait =
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);

        if (wait == WAIT_OBJECT_0 + 1) {
            return;
        }
        if (wait != WAIT_OBJECT_0) {
            Log::error(L"Proxy wait failed: " + lastError());
            return;
        }

        auto* request = static_cast<ProxyReadRequest*>(view_);
        const auto code = static_cast<ProxyRequestCode>(request->requestCode);

        switch (code) {
            case ProxyRequestCode::Info: {
                auto* info = static_cast<ProxyInfoResponse*>(view_);
                info->fileSize = driveSize_;
                info->requestAlignment = 512;
                info->flags = 0;
                signalResponse();
                break;
            }

            case ProxyRequestCode::Read: {
                const uint64_t offset = request->offset;
                const uint64_t length = request->length;
                auto* response = static_cast<ProxyReadResponse*>(view_);
                if (length > kProxyBufferSize || (offset % 512) != 0 ||
                    (length % 512) != 0 || offset > driveSize_ ||
                    length > driveSize_ - offset) {
                    response->error = EINVAL;
                    response->length = 0;
                    signalResponse();
                    break;
                }
                bool ok = false;
                try {
                    ok = readHandler_ && readHandler_(offset, data_, length);
                } catch (const std::exception& ex) {
                    Log::error(L"Proxy read exception: " + widen(ex.what()));
                } catch (...) {
                    Log::error(L"Proxy read failed with an unknown exception.");
                }
                response->error = ok ? 0 : EIO;
                response->length = ok ? length : 0;
                if (!ok) {
                    Log::warn(std::format(
                        L"Proxy read failed at offset {} ({} bytes).", offset,
                        length));
                }
                signalResponse();
                break;
            }

            case ProxyRequestCode::Write: {
                const uint64_t offset = request->offset;
                const uint64_t length = request->length;
                auto* response = static_cast<ProxyWriteResponse*>(view_);
                if (length > kProxyBufferSize || (offset % 512) != 0 ||
                    (length % 512) != 0 || offset > driveSize_ ||
                    length > driveSize_ - offset) {
                    response->error = EINVAL;
                    response->length = 0;
                    signalResponse();
                    break;
                }
                bool ok = false;
                try {
                    ok = writeHandler_ && writeHandler_(offset, data_, length);
                } catch (const std::exception& ex) {
                    Log::error(L"Proxy write exception: " + widen(ex.what()));
                } catch (...) {
                    Log::error(L"Proxy write failed with an unknown exception.");
                }
                response->error = ok ? 0 : EIO;
                response->length = ok ? length : 0;
                if (!ok) {
                    Log::warn(std::format(
                        L"Proxy write failed at offset {} ({} bytes).", offset,
                        length));
                }
                signalResponse();
                break;
            }

            case ProxyRequestCode::Close:
                Log::debug(L"Proxy received a close request.");
                return;

            case ProxyRequestCode::Unmap:
            case ProxyRequestCode::Zero: {
                auto* response = static_cast<ProxyReadResponse*>(view_);
                response->error = ENOSYS;
                response->length = 0;
                signalResponse();
                break;
            }

            default: {
                auto* response = static_cast<ProxyReadResponse*>(view_);
                response->error = ENODEV;
                response->length = 0;
                signalResponse();
                break;
            }
        }
    }
}

}  // namespace grd
