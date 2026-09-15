#pragma once
// QueryExecutor — server-side execution engine.
// Builds a proto1 physical plan from a QueryPackage and executes it.
// Never decrypts data. Never uses the secret key.

#include "proto2/catalog.h"
#include "proto2/crypto_context.h"
#include "proto2/query_package.h"
#include "proto2/result.h"
#include "proto1/expression.h"
#include "proto1/storage.h"
#include <memory>
#include <vector>

namespace proto2 {

class QueryExecutor {
public:
    QueryExecutor(std::shared_ptr<ServerCryptoContext> serverCtx,
                  std::shared_ptr<proto1::EncryptedStorage> storage,
                  const Catalog& catalog);

    // Execute a validated query package. Returns encrypted result batches.
    // The server never decrypts anything here.
    std::vector<EncryptedResultBatch> execute(const QueryPackage& pkg);

private:
    std::shared_ptr<ServerCryptoContext>      serverCtx_;
    std::shared_ptr<proto1::EncryptedStorage> storage_;
    const Catalog&                            catalog_;

    // Reconstruct a proto1::Expression from the predicate node tree.
    // Encrypted literals are deserialized from the package.
    std::shared_ptr<proto1::Expression>
    buildExpression(const QueryPackage& pkg, uint32_t nodeIdx) const;
};

} // namespace proto2
