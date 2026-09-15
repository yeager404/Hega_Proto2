#pragma once
// Client/Server FHE context separation.
//
// ClientCryptoContext  — owns secret key, can encrypt and decrypt.
// ServerCryptoContext  — owns only public/eval keys, can only compute.
//
// This is a thin wrapper around proto1's ClientKeyContext / FHEKeyContext
// that makes the trust boundary explicit in the type system.

#include "proto1/fhe_runtime.h"
#include <memory>
#include <string>

namespace proto2 {

// Trusted client side — holds secret key.
// Never passed to the server.
class ClientCryptoContext {
public:
    explicit ClientCryptoContext(std::shared_ptr<proto1::ClientKeyContext> ctx)
        : ctx_(std::move(ctx)) {}

    proto1::ClientKeyContext& keyCtx() { return *ctx_; }
    const proto1::ClientKeyContext& keyCtx() const { return *ctx_; }
    std::shared_ptr<proto1::ClientKeyContext> keyCtxPtr() const { return ctx_; }

    // Convenience: encrypt a scalar broadcast to all slots.
    std::shared_ptr<proto1::EncryptedVector> encryptScalar(int64_t v) const;

    // Decrypt a vector.
    std::vector<int64_t> decrypt(const proto1::EncryptedVector& v,
                                  size_t logicalLen) const;

    // Save/load keys.
    void saveKeys(const std::string& dir) const;
    static std::shared_ptr<ClientCryptoContext> load(const std::string& dir);
    static std::shared_ptr<ClientCryptoContext> create(
        const proto1::FHEParameterSet& params);

    const std::string& paramSetId() const { return ctx_->paramSetId(); }
    size_t slotCount() const { return ctx_->slotCount(); }

private:
    std::shared_ptr<proto1::ClientKeyContext> ctx_;
};

// Untrusted server side — no secret key.
class ServerCryptoContext {
public:
    explicit ServerCryptoContext(std::shared_ptr<proto1::FHEKeyContext> ctx)
        : ctx_(std::move(ctx)) {}

    proto1::FHEKeyContext& keyCtx() { return *ctx_; }
    const proto1::FHEKeyContext& keyCtx() const { return *ctx_; }
    std::shared_ptr<proto1::FHEKeyContext> keyCtxPtr() const { return ctx_; }

    static std::shared_ptr<ServerCryptoContext> load(const std::string& dir);

    const std::string& paramSetId() const { return ctx_->paramSetId(); }
    size_t slotCount() const { return ctx_->slotCount(); }

private:
    std::shared_ptr<proto1::FHEKeyContext> ctx_;
};

} // namespace proto2
