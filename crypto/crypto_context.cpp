// ClientCryptoContext and ServerCryptoContext implementations.

#include "proto2/crypto_context.h"
#include "proto1/fhe_runtime.h"

namespace proto2 {

// ── ClientCryptoContext ───────────────────────────────────────────────────

std::shared_ptr<proto1::EncryptedVector>
ClientCryptoContext::encryptScalar(int64_t v) const {
    proto1::PlaintextVector pv;
    pv.values.assign(ctx_->slotCount(), v);
    return ctx_->encrypt(pv);
}

std::vector<int64_t>
ClientCryptoContext::decrypt(const proto1::EncryptedVector& v,
                              size_t logicalLen) const {
    auto pv = ctx_->decrypt(v, logicalLen);
    return pv.values;
}

void ClientCryptoContext::saveKeys(const std::string& dir) const {
    ctx_->saveServerKeys(dir);
    ctx_->savePrivateKey(dir + "/client");
}

std::shared_ptr<ClientCryptoContext>
ClientCryptoContext::load(const std::string& dir) {
    auto ctx = proto1::loadClientContext(dir);
    return std::make_shared<ClientCryptoContext>(ctx);
}

std::shared_ptr<ClientCryptoContext>
ClientCryptoContext::create(const proto1::FHEParameterSet& params) {
    auto ctx = proto1::createBFVContext(params);
    return std::make_shared<ClientCryptoContext>(ctx);
}

// ── ServerCryptoContext ───────────────────────────────────────────────────

std::shared_ptr<ServerCryptoContext>
ServerCryptoContext::load(const std::string& dir) {
    auto ctx = proto1::loadServerContext(dir);
    return std::make_shared<ServerCryptoContext>(ctx);
}

} // namespace proto2
