// Semantic analyzer implementation.

#include "proto2/semantic.h"

namespace proto2 {

SemanticAnalyzer::SemanticAnalyzer(const Catalog& catalog)
    : catalog_(catalog) {}

const TableSchema& SemanticAnalyzer::analyze(const SelectStatement& stmt) {
    if (stmt.tableName.empty())
        throw SemanticError("No table specified");

    if (!catalog_.hasTable(stmt.tableName))
        throw SemanticError("Unknown table '" + stmt.tableName + "'");

    const TableSchema& schema = catalog_.getTable(stmt.tableName);

    // Validate projection columns
    for (auto& col : stmt.projections) {
        if (!schema.hasColumn(col))
            throw SemanticError("Unknown column '" + col + "' in table '" +
                                stmt.tableName + "'");
    }

    // Validate WHERE clause
    if (stmt.whereClause)
        validateExpr(*stmt.whereClause, schema);

    return schema;
}

void SemanticAnalyzer::validateExpr(const ASTExpr& expr,
                                     const TableSchema& schema) {
    switch (expr.kind) {
        case ASTExprKind::ColumnRef:
            if (!schema.hasColumn(expr.columnName))
                throw SemanticError("Unknown column '" + expr.columnName +
                                    "' in table '" + schema.tableId + "'");
            return;

        case ASTExprKind::IntLiteral:
            return;

        case ASTExprKind::Equal:
        case ASTExprKind::NotEqual:
        case ASTExprKind::GreaterThan:
        case ASTExprKind::LessThan:
        case ASTExprKind::GreaterEqual:
        case ASTExprKind::LessEqual:
            validateComparison(expr, schema);
            return;

        case ASTExprKind::And:
        case ASTExprKind::Or:
            validateExpr(*expr.left, schema);
            validateExpr(*expr.right, schema);
            return;

        case ASTExprKind::Not:
            validateExpr(*expr.left, schema);
            return;
    }
}

void SemanticAnalyzer::validateComparison(const ASTExpr& expr,
                                           const TableSchema& schema) {
    // Supported forms: column op literal  OR  literal op column
    auto& l = *expr.left;
    auto& r = *expr.right;

    bool lCol = (l.kind == ASTExprKind::ColumnRef);
    bool rCol = (r.kind == ASTExprKind::ColumnRef);
    bool lLit = (l.kind == ASTExprKind::IntLiteral);
    bool rLit = (r.kind == ASTExprKind::IntLiteral);

    if (!((lCol && rLit) || (lLit && rCol) || (lCol && rCol)))
        throw SemanticError("Comparison must be between a column and a literal "
                            "or two columns: " + expr.toSQL());

    // Validate column references
    if (lCol && !schema.hasColumn(l.columnName))
        throw SemanticError("Unknown column '" + l.columnName + "'");
    if (rCol && !schema.hasColumn(r.columnName))
        throw SemanticError("Unknown column '" + r.columnName + "'");

    // Type check: only INT64 columns support >, <, >=, <=
    auto checkNumeric = [&](const std::string& colName) {
        auto& cs = schema.column(colName);
        if (cs.type != DataType::INT64)
            throw SemanticError("Column '" + colName +
                                "' is not numeric; cannot use comparison operator");
    };

    if (expr.kind == ASTExprKind::GreaterThan ||
        expr.kind == ASTExprKind::LessThan    ||
        expr.kind == ASTExprKind::GreaterEqual ||
        expr.kind == ASTExprKind::LessEqual) {
        if (lCol) checkNumeric(l.columnName);
        if (rCol) checkNumeric(r.columnName);
    }
}

} // namespace proto2
