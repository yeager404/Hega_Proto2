#pragma once
// QueryPackage — the serializable unit sent from client to server.
//
// Security model:
//   - query structure (table, columns, operators) is visible to server
//   - literal values are encrypted; server sees only ciphertexts
//   - private key is NEVER included
//
// Serialization: cereal binary archive + raw ciphertext bytes.
// Wire format:
//   [cereal binary header]
//   [encrypted_literals: N entries, each: id + logicalType + bytes]
//   [predicate_nodes: serialized predicate tree using literal IDs]
//   [metadata fields]

#include "proto2/catalog.h"
#include <cstdint>
#include <string>
#include <vector>

namespace proto2 {

// FHE parameter metadata — enough for the server to validate compatibility.
struct FHEParamsMeta {
    std::string paramSetId;       // e.g. "bfv-257-depth9"
    uint64_t    plaintextModulus{0};
    uint32_t    multiplicativeDepth{0};
    uint32_t    ringDimension{0};
    size_t      slotCount{0};
    std::string keyId;            // public key fingerprint (non-secret)
};

// A single encrypted literal with metadata.
struct EncryptedLiteralEntry {
    uint32_t             id{0};          // referenced in predicate nodes
    std::string          logicalType;    // "INT64"
    std::vector<uint8_t> ciphertextBytes;
    size_t               logicalSize{0}; // slots used
};

// Predicate node kinds (mirrors ASTExprKind but uses literal IDs).
enum class PredNodeKind : uint8_t {
    ColumnRef    = 0,
    LiteralRef   = 1, // references EncryptedLiteralEntry by id
    Equal        = 2,
    GreaterThan  = 3,
    LessThan     = 4,
    And          = 5,
    Or           = 6,
    Not          = 7,
};

struct PredNode {
    PredNodeKind kind{PredNodeKind::ColumnRef};
    std::string  columnName;   // for ColumnRef
    uint32_t     literalId{0}; // for LiteralRef
    uint32_t     left{UINT32_MAX};  // index into nodes array
    uint32_t     right{UINT32_MAX}; // index into nodes array (UINT32_MAX = none)
};

// The complete query package.
struct QueryPackage {
    // Identity
    std::string queryId;
    uint32_t    protocolVersion{2};
    uint32_t    schemaVersion{1};

    // Target
    std::string              tableName;
    std::vector<std::string> scanColumns;    // columns needed for execution
    std::vector<std::string> projectColumns; // columns in result

    // Result schema
    std::vector<ColumnSchema> resultSchema;

    // Predicate (empty nodes = no WHERE clause)
    std::vector<PredNode>            predicateNodes; // flat array, root = last
    std::vector<EncryptedLiteralEntry> encryptedLiterals;

    // FHE metadata
    FHEParamsMeta fheParams;

    // Sharding metadata (future use)
    uint32_t shardId{0};
    uint32_t partitionId{0};

    // Serialize to bytes using cereal binary archive.
    std::vector<uint8_t> serialize() const;

    // Deserialize from bytes.
    static QueryPackage deserialize(const std::vector<uint8_t>& bytes);
};

} // namespace proto2
