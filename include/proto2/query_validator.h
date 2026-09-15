#pragma once
// QueryValidator — server-side validation of a received QueryPackage.

#include "proto2/catalog.h"
#include "proto2/crypto_context.h"
#include "proto2/query_package.h"
#include <stdexcept>
#include <string>

namespace proto2 {

struct ValidationError : std::runtime_error {
    explicit ValidationError(const std::string& msg) : std::runtime_error(msg) {}
};

class QueryValidator {
public:
    QueryValidator(const Catalog& catalog,
                   const ServerCryptoContext& serverCtx);

    // Validate the package. Throws ValidationError on failure.
    void validate(const QueryPackage& pkg) const;

private:
    const Catalog&             catalog_;
    const ServerCryptoContext& serverCtx_;

    void validatePredTree(const QueryPackage& pkg,
                           uint32_t nodeIdx,
                           const TableSchema& schema) const;
};

} // namespace proto2
