#pragma once
// Semantic analyzer: validates AST against catalog before any encryption.

#include "proto2/ast.h"
#include "proto2/catalog.h"
#include <stdexcept>
#include <string>

namespace proto2 {

struct SemanticError : std::runtime_error {
    explicit SemanticError(const std::string& msg) : std::runtime_error(msg) {}
};

class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(const Catalog& catalog);

    // Validate a SELECT statement. Throws SemanticError on failure.
    // Returns the resolved table schema for downstream use.
    const TableSchema& analyze(const SelectStatement& stmt);

private:
    const Catalog& catalog_;

    void validateExpr(const ASTExpr& expr, const TableSchema& schema);
    void validateComparison(const ASTExpr& expr, const TableSchema& schema);
};

} // namespace proto2
