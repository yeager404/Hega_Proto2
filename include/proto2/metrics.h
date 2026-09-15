#pragma once
// Metrics collection for the client and server pipelines.

#include <chrono>
#include <cstdint>
#include <string>

namespace proto2 {

using Clock = std::chrono::high_resolution_clock;
using Ms    = std::chrono::milliseconds;

struct ClientMetrics {
    long parseMs{0};
    long semanticMs{0};
    long planMs{0};
    long encryptMs{0};
    long serializeMs{0};
    long networkUpMs{0};
    long networkDownMs{0};
    long decryptMs{0};
    long reconstructMs{0};
    long totalMs{0};

    size_t queryPackageBytes{0};
    size_t resultBytesReceived{0};
    uint32_t batchCount{0};

    void print() const;
};

struct ServerMetrics {
    long validationMs{0};
    long storageReadMs{0};
    long fheExecMs{0};
    long serializeMs{0};

    size_t resultBytesProduced{0};

    void print() const;
};

} // namespace proto2
