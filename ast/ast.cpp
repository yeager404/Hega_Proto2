// AST pretty-printing.

#include "proto2/ast.h"
#include <stdexcept>

namespace proto2 {

static const char* opStr(ASTExprKind k) {
    switch (k) {
        case ASTExprKind::Equal:        return " = ";
        case ASTExprKind::NotEqual:     return " != ";
        case ASTExprKind::GreaterThan:  return " > ";
        case ASTExprKind::LessThan:     return " < ";
        case ASTExprKind::GreaterEqual: return " >= ";
        case ASTExprKind::LessEqual:    return " <= ";
        case ASTExprKind::And:          return " AND ";
        case ASTExprKind::Or:           return " OR ";
        default: return "?";
    }
}

std::string ASTExpr::toSQL() const {
    switch (kind) {
        case ASTExprKind::ColumnRef:   return columnName;
        case ASTExprKind::IntLiteral:  return std::to_string(intValue);
        case ASTExprKind::Not:         return "NOT(" + left->toSQL() + ")";
        case ASTExprKind::Equal:
        case ASTExprKind::NotEqual:
        case ASTExprKind::GreaterThan:
        case ASTExprKind::LessThan:
        case ASTExprKind::GreaterEqual:
        case ASTExprKind::LessEqual:
            return left->toSQL() + opStr(kind) + right->toSQL();
        case ASTExprKind::And:
        case ASTExprKind::Or:
            return "(" + left->toSQL() + opStr(kind) + right->toSQL() + ")";
    }
    return "?";
}

std::string SelectStatement::toSQL() const {
    std::string s = "SELECT ";
    if (projections.empty()) {
        s += "*";
    } else {
        for (size_t i = 0; i < projections.size(); ++i) {
            if (i) s += ", ";
            s += projections[i];
        }
    }
    s += " FROM " + tableName;
    if (whereClause)
        s += " WHERE " + whereClause->toSQL();
    return s;
}

} // namespace proto2
