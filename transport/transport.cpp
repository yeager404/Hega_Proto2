// TCP transport implementation.
// Wire format: [4-byte type][4-byte length][payload bytes]
// All integers are big-endian.

#include "proto2/transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <iostream>

namespace proto2 {

// ── Frame I/O helpers ─────────────────────────────────────────────────────

static void writeU32BE(int fd, uint32_t v) {
    uint32_t n = htonl(v);
    const char* p = reinterpret_cast<const char*>(&n);
    size_t sent = 0;
    while (sent < 4) {
        ssize_t r = ::write(fd, p + sent, 4 - sent);
        if (r <= 0) throw std::runtime_error("Transport: write failed");
        sent += r;
    }
}

static uint32_t readU32BE(int fd) {
    uint32_t n = 0;
    char* p = reinterpret_cast<char*>(&n);
    size_t got = 0;
    while (got < 4) {
        ssize_t r = ::read(fd, p + got, 4 - got);
        if (r <= 0) throw std::runtime_error("Transport: connection closed");
        got += r;
    }
    return ntohl(n);
}

static void writeBytes(int fd, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = ::write(fd, data + sent, len - sent);
        if (r <= 0) throw std::runtime_error("Transport: write failed");
        sent += r;
    }
}

static void readBytes(int fd, uint8_t* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = ::read(fd, data + got, len - got);
        if (r <= 0) throw std::runtime_error("Transport: connection closed");
        got += r;
    }
}

void TcpClientTransport::sendFrame(int fd, MsgType type,
                                    const std::vector<uint8_t>& payload) {
    writeU32BE(fd, static_cast<uint32_t>(type));
    writeU32BE(fd, static_cast<uint32_t>(payload.size()));
    if (!payload.empty())
        writeBytes(fd, payload.data(), payload.size());
}

std::pair<MsgType, std::vector<uint8_t>>
TcpClientTransport::recvFrame(int fd) {
    uint32_t type = readU32BE(fd);
    uint32_t len  = readU32BE(fd);
    std::vector<uint8_t> payload(len);
    if (len > 0) readBytes(fd, payload.data(), len);
    return {static_cast<MsgType>(type), std::move(payload)};
}

void TcpServerTransport::sendFrame(int fd, MsgType type,
                                    const std::vector<uint8_t>& payload) {
    writeU32BE(fd, static_cast<uint32_t>(type));
    writeU32BE(fd, static_cast<uint32_t>(payload.size()));
    if (!payload.empty())
        writeBytes(fd, payload.data(), payload.size());
}

std::pair<MsgType, std::vector<uint8_t>>
TcpServerTransport::recvFrame(int fd) {
    uint32_t type = readU32BE(fd);
    uint32_t len  = readU32BE(fd);
    std::vector<uint8_t> payload(len);
    if (len > 0) readBytes(fd, payload.data(), len);
    return {static_cast<MsgType>(type), std::move(payload)};
}

// ── TcpClientTransport ────────────────────────────────────────────────────

TcpClientTransport::TcpClientTransport(const std::string& host, uint16_t port)
    : host_(host), port_(port) {}

TcpClientTransport::~TcpClientTransport() = default;

int TcpClientTransport::connectSocket() const {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port_);
    if (::getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res) != 0)
        throw std::runtime_error("Transport: cannot resolve host: " + host_);

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { ::freeaddrinfo(res); throw std::runtime_error("Transport: socket failed"); }

    if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ::close(fd);
        ::freeaddrinfo(res);
        throw std::runtime_error("Transport: connect failed to " + host_ + ":" + portStr);
    }
    ::freeaddrinfo(res);
    return fd;
}

QueryResponse TcpClientTransport::sendQuery(
    const QueryPackage& pkg,
    std::vector<EncryptedResultBatch>& outBatches) {

    int fd = connectSocket();

    try {
        // Send query request
        auto pkgBytes = pkg.serialize();
        sendFrame(fd, MsgType::QueryRequest, pkgBytes);

        // Receive response header
        auto [type1, payload1] = recvFrame(fd);
        if (type1 == MsgType::Error) {
            std::string err(payload1.begin(), payload1.end());
            ::close(fd);
            return {false, err, 0, pkg.queryId};
        }
        if (type1 != MsgType::QueryResponse)
            throw std::runtime_error("Transport: expected QueryResponse");

        // Deserialize response
        QueryResponse resp;
        {
            std::string s(payload1.begin(), payload1.end());
            std::istringstream iss(s, std::ios::binary);
            cereal::BinaryInputArchive ar(iss);
            ar(resp.success, resp.errorMessage, resp.batchCount, resp.queryId);
        }

        if (!resp.success) {
            ::close(fd);
            return resp;
        }

        // Receive result batches
        for (uint32_t i = 0; i < resp.batchCount; ++i) {
            auto [type2, payload2] = recvFrame(fd);
            if (type2 != MsgType::ResultBatch)
                throw std::runtime_error("Transport: expected ResultBatch");
            outBatches.push_back(EncryptedResultBatch::deserialize(payload2));
        }

        ::close(fd);
        return resp;

    } catch (...) {
        ::close(fd);
        throw;
    }
}

// ── TcpServerTransport ────────────────────────────────────────────────────

TcpServerTransport::TcpServerTransport(uint16_t port, QueryHandler handler)
    : port_(port), handler_(std::move(handler)) {}

TcpServerTransport::~TcpServerTransport() {
    stop();
}

void TcpServerTransport::stop() {
    running_ = false;
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

void TcpServerTransport::serve() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0)
        throw std::runtime_error("Server: socket failed");

    int opt = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);

    if (::bind(listenFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("Server: bind failed on port " + std::to_string(port_));

    if (::listen(listenFd_, 8) < 0)
        throw std::runtime_error("Server: listen failed");

    running_ = true;
    std::cout << "[Server] Listening on port " << port_ << "\n" << std::flush;

    while (running_) {
        struct sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = ::accept(listenFd_, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientFd < 0) {
            if (!running_) break;
            continue;
        }
        std::cout << "[Server] Client connected\n" << std::flush;
        handleClient(clientFd);
        ::close(clientFd);
    }
}

void TcpServerTransport::handleClient(int clientFd) {
    try {
        auto [type, payload] = recvFrame(clientFd);
        if (type != MsgType::QueryRequest) {
            std::string err = "Expected QueryRequest";
            std::vector<uint8_t> errBytes(err.begin(), err.end());
            sendFrame(clientFd, MsgType::Error, errBytes);
            return;
        }

        // Deserialize query package
        QueryPackage pkg = QueryPackage::deserialize(payload);
        std::cout << "[Server] Query received: " << pkg.queryId << "\n" << std::flush;

        // Execute
        std::vector<EncryptedResultBatch> batches;
        try {
            batches = handler_(pkg);
        } catch (std::exception& e) {
            std::string err = e.what();
            std::vector<uint8_t> errBytes(err.begin(), err.end());
            sendFrame(clientFd, MsgType::Error, errBytes);
            return;
        }

        // Send response header
        QueryResponse resp;
        resp.success    = true;
        resp.batchCount = static_cast<uint32_t>(batches.size());
        resp.queryId    = pkg.queryId;

        std::vector<uint8_t> respBytes;
        {
            std::ostringstream oss(std::ios::binary);
            cereal::BinaryOutputArchive ar(oss);
            ar(resp.success, resp.errorMessage, resp.batchCount, resp.queryId);
            auto s = oss.str();
            respBytes.assign(s.begin(), s.end());
        }
        sendFrame(clientFd, MsgType::QueryResponse, respBytes);

        // Stream result batches
        for (auto& batch : batches) {
            auto batchBytes = batch.serialize();
            sendFrame(clientFd, MsgType::ResultBatch, batchBytes);
        }

        std::cout << "[Server] Query complete: " << batches.size()
                  << " batch(es) sent\n" << std::flush;

    } catch (std::exception& e) {
        std::cerr << "[Server] Error handling client: " << e.what() << "\n";
    }
}

} // namespace proto2
