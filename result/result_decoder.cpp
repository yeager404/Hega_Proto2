// ResultDecoder — client-side decryption and row reconstruction.

#include "proto2/result_decoder.h"
#include <stdexcept>

namespace proto2 {

ResultDecoder::ResultDecoder(std::shared_ptr<ClientCryptoContext> ctx)
    : ctx_(std::move(ctx)) {}

std::vector<int64_t>
ResultDecoder::decryptMask(const EncryptedResultBatch& batch) const {
    if (!batch.hasMask)
        return std::vector<int64_t>(batch.logicalRowCount, 1);

    auto ct = ctx_->keyCtx().deserializeWithSize(
        batch.selectionMaskBytes,
        static_cast<size_t>(batch.logicalRowCount));
    return ctx_->decrypt(*ct, static_cast<size_t>(batch.logicalRowCount));
}

std::vector<ResultRow>
ResultDecoder::decode(const EncryptedResultBatch& batch) const {
    size_t rowCount = static_cast<size_t>(batch.logicalRowCount);

    // Decrypt selection mask
    auto mask = decryptMask(batch);

    // Decrypt each column
    std::unordered_map<std::string, std::vector<int64_t>> cols;
    for (auto& cr : batch.columns) {
        auto ct = ctx_->keyCtx().deserializeWithSize(cr.ciphertextBytes, rowCount);
        cols[cr.columnName] = ctx_->decrypt(*ct, rowCount);
    }

    // Reconstruct rows where mask[i] == 1
    std::vector<ResultRow> rows;
    for (size_t i = 0; i < rowCount; ++i) {
        if (i < mask.size() && mask[i] == 1) {
            ResultRow row;
            for (auto& [colName, vals] : cols)
                row.values[colName] = vals[i];
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

} // namespace proto2
