#pragma once

#include <cstdint>

namespace grd {

// Wire format shared with ImDisk's shared-memory proxy. These definitions
// mirror imdproxy.h from the ImDisk project (MIT licensed).
constexpr uint64_t kProxyHeaderSize = 4096;
constexpr uint64_t kProxyBufferSize = 4ull * 1024ull * 1024ull;

enum class ProxyRequestCode : uint64_t {
    Null = 0,
    Info = 1,
    Read = 2,
    Write = 3,
    Connect = 4,
    Close = 5,
    Unmap = 6,
    Zero = 7,
};

struct ProxyInfoResponse {
    uint64_t fileSize;
    uint64_t requestAlignment;
    uint64_t flags;
};

struct ProxyReadRequest {
    uint64_t requestCode;
    uint64_t offset;
    uint64_t length;
};

struct ProxyReadResponse {
    uint64_t error;
    uint64_t length;
};

struct ProxyWriteRequest {
    uint64_t requestCode;
    uint64_t offset;
    uint64_t length;
};

struct ProxyWriteResponse {
    uint64_t error;
    uint64_t length;
};

static_assert(sizeof(ProxyInfoResponse) == 24);
static_assert(sizeof(ProxyReadRequest) == 24);
static_assert(sizeof(ProxyReadResponse) == 16);
static_assert(sizeof(ProxyWriteRequest) == 24);
static_assert(sizeof(ProxyWriteResponse) == 16);
static_assert(kProxyHeaderSize >= sizeof(ProxyReadRequest));
static_assert(kProxyHeaderSize >= sizeof(ProxyInfoResponse));

}  // namespace grd
