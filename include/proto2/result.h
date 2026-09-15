#pragma once
// EncryptedResultBatch — what the server sends back to the client.
// All data remains encrypted; the client decrypts.

#include "proto2/catalog.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace proto2 {

struct EncryptedColumnResult {
    std::string          columnName;
    std::vector<uint8_t> ciphertextBytes;
    size_t               logicalSize{0};
};

struct EncryptedResultBatch {
    uint32_t batchId{0};
    int64_t  logicalRowCount{0};

    std::vector<EncryptedColumnResult> columns;

    // Selection mask — 0/1 per slot, encrypted.
    std::vector<uint8_t> selectionMaskBytes;
    bool                 hasMask{false};

    // Serialize/deserialize for transport.
    std::vector<uint8_t> serialize() const;
    static EncryptedResultBatch deserialize(const std::vector<uint8_t>& bytes);
};

// Reconstructed plaintext row after client decryption.
struct ResultRow {
    std::unordered_map<std::string, int64_t> values;
};

} // namespace proto2
