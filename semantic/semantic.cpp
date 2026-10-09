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

    // Validate ORDER BY columns
    for (auto& orderCol : stmt.orderByColumns) {
        if (!schema.hasColumn(orderCol.columnName))
            throw SemanticError("Unknown ORDER BY column '" + orderCol.columnName +
                                "' in table '" + stmt.tableName + "'");
    }

    // Validate LIMIT
    if (stmt.limitCount < -1)
        throw SemanticError("LIMIT value must be non-negative");

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
        case ASTExprKind::StringLiteral:
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
    // Supported forms: column op literal, literal op column, or column op column.
    auto& l = *expr.left;
    auto& r = *expr.right;

    bool lCol = (l.kind == ASTExprKind::ColumnRef);
    bool rCol = (r.kind == ASTExprKind::ColumnRef);
    bool lLit = (l.kind == ASTExprKind::IntLiteral);
    bool rLit = (r.kind == ASTExprKind::IntLiteral);
    bool lString = (l.kind == ASTExprKind::StringLiteral);
    bool rString = (r.kind == ASTExprKind::StringLiteral);

    if (!((lCol && (rLit || rString)) ||
          ((lLit || lString) && rCol) ||
          (lCol && rCol)))
        throw SemanticError("Comparison must be between a column and a literal "
                            "or two columns: " + expr.toSQL());

    // Validate column references
    if (lCol && !schema.hasColumn(l.columnName))
        throw SemanticError("Unknown column '" + l.columnName + "'");
    if (rCol && !schema.hasColumn(r.columnName))
        throw SemanticError("Unknown column '" + r.columnName + "'");

    if (lCol && (rLit || rString)) {
        const auto& column = schema.column(l.columnName);
        if ((rLit && column.type != DataType::INT64) ||
            (rString && column.type != DataType::STRING))
            throw SemanticError("Literal type does not match column '" +
                                l.columnName + "'");
    }
    if (rCol && (lLit || lString)) {
        const auto& column = schema.column(r.columnName);
        if ((lLit && column.type != DataType::INT64) ||
            (lString && column.type != DataType::STRING))
            throw SemanticError("Literal type does not match column '" +
                                r.columnName + "'");
    }

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
        if (lString || rString)
            throw SemanticError("Ordering comparisons are only supported for INT64 columns");
    }

    if (lString || rString) {
        if (expr.kind != ASTExprKind::Equal &&
            expr.kind != ASTExprKind::NotEqual)
            throw SemanticError("STRING columns support only = and !=");

        const std::string& colName = lCol ? l.columnName : r.columnName;
        const auto& column = schema.column(colName);
        if (column.type != DataType::STRING)
            throw SemanticError("String literal compared with non-STRING column '" +
                                colName + "'");
        const auto& literal = lString ? l.stringValue : r.stringValue;
        if (column.maxLength == 0 || literal.size() > column.maxLength)
            throw SemanticError("String literal for column '" + colName +
                                "' exceeds maxLength");
    }
}

} // namespace proto2
