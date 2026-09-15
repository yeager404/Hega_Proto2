#pragma once
// QueryEncryptor — walks the AST and produces a QueryPackage.
// Encrypts all integer literals using the client's crypto context.
// The server never sees plaintext literal values.

#include "proto2/ast.h"
#include "proto2/catalog.h"
#include "proto2/crypto_context.h"
#include "proto2/planner.h"
#include "proto2/query_package.h"
#include <memory>

namespace proto2 {

class QueryEncryptor {
public:
    explicit QueryEncryptor(std::shared_ptr<ClientCryptoContext> ctx);

    // Build a QueryPackage from a validated SELECT statement + physical plan.
    QueryPackage encrypt(const SelectStatement& stmt,
                         const PhysicalPlan& plan,
                         const TableSchema& schema,
                         const FHEParamsMeta& fheMeta);

private:
    std::shared_ptr<ClientCryptoContext> ctx_;

    // Recursively walk the AST expression, encrypt literals, build PredNodes.
    // Returns the index of the root node in `nodes`.
    uint32_t buildPredNode(const ASTExpr& expr,
                            std::vector<PredNode>& nodes,
                            std::vector<EncryptedLiteralEntry>& literals,
                            uint32_t& nextLiteralId);
};

} // namespace proto2
