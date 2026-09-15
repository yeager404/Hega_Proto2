#pragma once
// Transport abstraction — decouples execution from networking.
//
// Wire protocol (TCP, length-prefixed binary):
//   [4 bytes: message type]
//   [4 bytes: payload length]
//   [N bytes: payload]
//
// Message types:
//   QUERY_REQUEST  = 1  — client → server: serialized QueryPackage
//   QUERY_RESPONSE = 2  — server → client: status + batch count
//   RESULT_BATCH   = 3  — server → client: serialized EncryptedResultBatch
//   ERROR          = 4  — server → client: error string

#include "proto2/query_package.h"
#include "proto2/result.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace proto2 {

enum class MsgType : uint32_t {
    QueryRequest  = 1,
    QueryResponse = 2,
    ResultBatch   = 3,
    Error         = 4,
};

struct QueryResponse {
    bool        success{false};
    std::string errorMessage;
    uint32_t    batchCount{0};
    std::string queryId;
};

// ── Abstract transport interface ──────────────────────────────────────────

class QueryTransport {
public:
    virtual ~QueryTransport() = default;

    // Client side: send package, receive all result batches.
    virtual QueryResponse sendQuery(
        const QueryPackage& pkg,
        std::vector<EncryptedResultBatch>& outBatches) = 0;
};

// ── TCP client transport ──────────────────────────────────────────────────

class TcpClientTransport : public QueryTransport {
public:
    TcpClientTransport(const std::string& host, uint16_t port);
    ~TcpClientTransport() override;

    QueryResponse sendQuery(const QueryPackage& pkg,
                             std::vector<EncryptedResultBatch>& outBatches) override;

private:
    std::string host_;
    uint16_t    port_;

    int connectSocket() const;
    static void sendFrame(int fd, MsgType type, const std::vector<uint8_t>& payload);
    static std::pair<MsgType, std::vector<uint8_t>> recvFrame(int fd);
};

// ── TCP server transport ──────────────────────────────────────────────────

// Callback invoked by the server transport for each received query.
using QueryHandler = std::function<
    std::vector<EncryptedResultBatch>(const QueryPackage&)>;

class TcpServerTransport {
public:
    TcpServerTransport(uint16_t port, QueryHandler handler);
    ~TcpServerTransport();

    // Block and serve connections until stop() is called.
    void serve();
    void stop();

private:
    uint16_t     port_;
    QueryHandler handler_;
    int          listenFd_{-1};
    bool         running_{false};

    void handleClient(int clientFd);
    static void sendFrame(int fd, MsgType type, const std::vector<uint8_t>& payload);
    static std::pair<MsgType, std::vector<uint8_t>> recvFrame(int fd);
};

} // namespace proto2
