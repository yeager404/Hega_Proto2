// ResultDecoder — client-side decryption and row reconstruction.

#include "proto2/result_decoder.h"
#include "proto2/ast.h"
#include <algorithm>
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
    std::unordered_map<std::string,
                       std::vector<std::vector<int64_t>>> stringCols;
    for (auto& cr : batch.columns) {
        auto ct = ctx_->keyCtx().deserializeWithSize(cr.ciphertextBytes, rowCount);
        auto values = ctx_->decrypt(*ct, rowCount);
        if (cr.logicalType == "STRING") {
            auto& positions = stringCols[cr.columnName];
            if (positions.size() <= cr.stringPosition)
                positions.resize(cr.stringPosition + 1);
            positions[cr.stringPosition] = std::move(values);
        } else {
            cols[cr.columnName] = std::move(values);
        }
    }

    // Reconstruct rows where mask[i] == 1
    std::vector<ResultRow> rows;
    for (size_t i = 0; i < rowCount; ++i) {
        if (i < mask.size() && mask[i] == 1) {
            ResultRow row;
            for (auto& [colName, vals] : cols)
                row.values[colName] = vals[i];
            for (auto& [colName, positions] : stringCols) {
                std::string value;
                for (const auto& chars : positions) {
                    if (i >= chars.size() || chars[i] == 0)
                        break;
                    if (chars[i] < 0 || chars[i] > 127)
                        throw std::runtime_error("Decoded string contains invalid ASCII");
                    value.push_back(static_cast<char>(chars[i]));
                }
                row.stringValues[colName] = std::move(value);
            }
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

void ResultDecoder::applySortAndLimit(std::vector<ResultRow>& rows,
                                       const std::vector<OrderByColumn>& orderBy,
                                       int64_t limit) {
    // Apply ORDER BY
    if (!orderBy.empty()) {
        std::stable_sort(rows.begin(), rows.end(),
            [&orderBy](const ResultRow& a, const ResultRow& b) -> bool {
                for (auto& orderCol : orderBy) {
                    auto itA = a.values.find(orderCol.columnName);
                    auto itB = b.values.find(orderCol.columnName);
                    
                    // Handle missing columns (should not happen with valid queries)
                    if (itA == a.values.end() || itB == b.values.end())
                        continue;
                    
                    int64_t valA = itA->second;
                    int64_t valB = itB->second;
                    
                    if (valA != valB) {
                        if (orderCol.direction == SortDirection::Ascending)
                            return valA < valB;
                        else
                            return valA > valB;
                    }
                    // If equal, continue to next sort column
                }
                return false; // all sort columns equal
            });
    }
    
    // Apply LIMIT
    if (limit >= 0 && static_cast<size_t>(limit) < rows.size()) {
        rows.resize(static_cast<size_t>(limit));
    }
}

} // namespace proto2
