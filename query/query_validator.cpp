// QueryValidator — server-side validation of QueryPackage.

#include "proto2/query_validator.h"

namespace proto2 {

QueryValidator::QueryValidator(const Catalog& catalog,
                                const ServerCryptoContext& serverCtx)
    : catalog_(catalog), serverCtx_(serverCtx) {}

void QueryValidator::validate(const QueryPackage& pkg) const {
    if (pkg.protocolVersion != 2)
        throw ValidationError("Unsupported protocol version: " +
                              std::to_string(pkg.protocolVersion));

    if (!catalog_.hasTable(pkg.tableName))
        throw ValidationError("Unknown table: " + pkg.tableName);

    const TableSchema& schema = catalog_.getTable(pkg.tableName);

    // Validate scan columns
    for (auto& col : pkg.scanColumns) {
        if (!schema.hasColumn(col))
            throw ValidationError("Unknown scan column '" + col +
                                  "' in table '" + pkg.tableName + "'");
    }

    // Validate project columns
    for (auto& col : pkg.projectColumns) {
        if (!schema.hasColumn(col))
            throw ValidationError("Unknown project column '" + col +
                                  "' in table '" + pkg.tableName + "'");
    }

    // Validate FHE params match server context
    if (pkg.fheParams.paramSetId != serverCtx_.paramSetId())
        throw ValidationError("FHE parameter set mismatch: client='" +
                              pkg.fheParams.paramSetId + "' server='" +
                              serverCtx_.paramSetId() + "'");

    // Validate encrypted literals are non-empty
    for (auto& lit : pkg.encryptedLiterals) {
        if (lit.ciphertextBytes.empty())
            throw ValidationError("Encrypted literal " +
                                  std::to_string(lit.id) + " has empty ciphertext");
    }

    // Validate predicate tree
    if (!pkg.predicateNodes.empty())
        validatePredTree(pkg, static_cast<uint32_t>(pkg.predicateNodes.size() - 1), schema);
}

void QueryValidator::validatePredTree(const QueryPackage& pkg,
                                       uint32_t nodeIdx,
                                       const TableSchema& schema) const {
    if (nodeIdx >= pkg.predicateNodes.size())
        throw ValidationError("Predicate node index out of range: " +
                              std::to_string(nodeIdx));

    const PredNode& node = pkg.predicateNodes[nodeIdx];

    switch (node.kind) {
        case PredNodeKind::ColumnRef:
            if (!schema.hasColumn(node.columnName))
                throw ValidationError("Unknown column in predicate: " + node.columnName);
            return;

        case PredNodeKind::LiteralRef: {
            bool found = false;
            for (auto& lit : pkg.encryptedLiterals)
                if (lit.id == node.literalId) { found = true; break; }
            if (!found)
                throw ValidationError("Predicate references unknown literal id: " +
                                      std::to_string(node.literalId));
            return;
        }

        case PredNodeKind::Equal:
        case PredNodeKind::GreaterThan:
        case PredNodeKind::LessThan:
            if (node.left == UINT32_MAX || node.right == UINT32_MAX)
                throw ValidationError("Binary predicate node missing children");
            validatePredTree(pkg, node.left, schema);
            validatePredTree(pkg, node.right, schema);
            return;

        case PredNodeKind::And:
        case PredNodeKind::Or:
            if (node.left == UINT32_MAX || node.right == UINT32_MAX)
                throw ValidationError("Boolean predicate node missing children");
            validatePredTree(pkg, node.left, schema);
            validatePredTree(pkg, node.right, schema);
            return;

        case PredNodeKind::Not:
            if (node.left == UINT32_MAX)
                throw ValidationError("NOT predicate node missing child");
            validatePredTree(pkg, node.left, schema);
            return;
    }
}

} // namespace proto2
