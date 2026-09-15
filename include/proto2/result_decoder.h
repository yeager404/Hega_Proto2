#pragma once
// ResultDecoder — client-side decryption and row reconstruction.

#include "proto2/crypto_context.h"
#include "proto2/result.h"
#include <memory>
#include <vector>

namespace proto2 {

class ResultDecoder {
public:
    explicit ResultDecoder(std::shared_ptr<ClientCryptoContext> ctx);

    // Decrypt a batch and reconstruct rows where mask == 1.
    std::vector<ResultRow> decode(const EncryptedResultBatch& batch) const;

    // Decrypt the selection mask only.
    std::vector<int64_t> decryptMask(const EncryptedResultBatch& batch) const;

private:
    std::shared_ptr<ClientCryptoContext> ctx_;
};

} // namespace proto2
