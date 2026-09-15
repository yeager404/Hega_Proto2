#pragma once
// SQL Abstract Syntax Tree — pure data, no FHE types.
// Literals remain plaintext here; encryption happens in QueryEncryptor.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace proto2 {

// ── Expression nodes ──────────────────────────────────────────────────────

enum class ASTExprKind {
    ColumnRef,
    IntLiteral,
    Equal,
    NotEqual,
    GreaterThan,
    LessThan,
    GreaterEqual,
    LessEqual,
    And,
    Or,
    Not,
};

struct ASTExpr {
    ASTExprKind kind;

    // ColumnRef
    std::string columnName;

    // IntLiteral
    int64_t intValue{0};

    // Binary / unary children
    std::shared_ptr<ASTExpr> left;
    std::shared_ptr<ASTExpr> right; // null for Not

    static std::shared_ptr<ASTExpr> columnRef(std::string name) {
        auto e = std::make_shared<ASTExpr>();
        e->kind = ASTExprKind::ColumnRef;
        e->columnName = std::move(name);
        return e;
    }

    static std::shared_ptr<ASTExpr> intLiteral(int64_t v) {
        auto e = std::make_shared<ASTExpr>();
        e->kind = ASTExprKind::IntLiteral;
        e->intValue = v;
        return e;
    }

    static std::shared_ptr<ASTExpr> binary(ASTExprKind k,
                                            std::shared_ptr<ASTExpr> l,
                                            std::shared_ptr<ASTExpr> r) {
        auto e = std::make_shared<ASTExpr>();
        e->kind = k;
        e->left = std::move(l);
        e->right = std::move(r);
        return e;
    }

    static std::shared_ptr<ASTExpr> notExpr(std::shared_ptr<ASTExpr> child) {
        auto e = std::make_shared<ASTExpr>();
        e->kind = ASTExprKind::Not;
        e->left = std::move(child);
        return e;
    }

    std::string toSQL() const;
};

// ── Statement nodes ───────────────────────────────────────────────────────

// SELECT col1, col2, ... FROM table [WHERE expr]
struct SelectStatement {
    std::vector<std::string>  projections; // column names; empty = *
    std::string               tableName;
    std::shared_ptr<ASTExpr>  whereClause; // nullptr if no WHERE

    std::string toSQL() const;
};

} // namespace proto2
