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

    std::unordered_map<std::string, const TableSchema*> scopes;
    scopes[stmt.tableAlias.empty() ? stmt.tableName : stmt.tableAlias] = &schema;
    for (const auto& join : stmt.joins) {
        if (!catalog_.hasTable(join.rightTable.tableName))
            throw SemanticError("Unknown table '" + join.rightTable.tableName + "'");
        scopes[join.rightTable.alias] = &catalog_.getTable(join.rightTable.tableName);
    }
    auto resolve = [&](const std::string& ref) -> const ColumnSchema& {
        const auto dot = ref.find('.');
        if (dot != std::string::npos) {
            auto it = scopes.find(ref.substr(0, dot));
            if (it == scopes.end() || !it->second->hasColumn(ref.substr(dot + 1)))
                throw SemanticError("Unknown qualified column '" + ref + "'");
            return it->second->column(ref.substr(dot + 1));
        }
        const ColumnSchema* found = nullptr;
        for (const auto& [alias, candidate] : scopes) {
            if (candidate->hasColumn(ref)) {
                if (found) throw SemanticError("Ambiguous column '" + ref + "'");
                found = &candidate->column(ref);
            }
        }
        if (!found) throw SemanticError("Unknown column '" + ref + "'");
        return *found;
    };

    // Validate projection columns
    for (auto& col : stmt.projections) {
        resolve(col);
    }

    // Validate WHERE clause
    if (stmt.whereClause)
        validateExpr(*stmt.whereClause, stmt);

    for (const auto& join : stmt.joins) {
        if (!join.condition || join.condition->kind != ASTExprKind::Equal ||
            join.condition->left->kind != ASTExprKind::ColumnRef ||
            join.condition->right->kind != ASTExprKind::ColumnRef)
            throw SemanticError("JOIN currently requires column equality in ON");
        const auto& left = join.condition->left->columnName;
        const auto& right = join.condition->right->columnName;
        const auto& lcs = resolve(left);
        const auto& rcs = resolve(right);
        if (lcs.type != rcs.type)
            throw SemanticError("JOIN columns must have compatible types");
        if (left.find('.') == std::string::npos || right.find('.') == std::string::npos)
            throw SemanticError("JOIN columns must be qualified");
    }

    // Validate ORDER BY columns
    for (auto& orderCol : stmt.orderByColumns) {
        resolve(orderCol.columnName);
    }

    // Validate LIMIT
    if (stmt.limitCount < -1)
        throw SemanticError("LIMIT value must be non-negative");

    return schema;
}

void SemanticAnalyzer::validateExpr(const ASTExpr& expr,
                                     const SelectStatement& stmt) {
    switch (expr.kind) {
        case ASTExprKind::ColumnRef:
            // Resolve by re-running the statement-level scope checks.
            if (expr.columnName.find('.') == std::string::npos) {
                bool found = false;
                if (catalog_.getTable(stmt.tableName).hasColumn(expr.columnName))
                    found = true;
                for (const auto& join : stmt.joins)
                    if (catalog_.getTable(join.rightTable.tableName).hasColumn(expr.columnName))
                        found = true;
                if (!found) throw SemanticError("Unknown column '" + expr.columnName + "'");
            }
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
            validateComparison(expr, stmt);
            return;

        case ASTExprKind::And:
        case ASTExprKind::Or:
            validateExpr(*expr.left, stmt);
            validateExpr(*expr.right, stmt);
            return;

        case ASTExprKind::Not:
            validateExpr(*expr.left, stmt);
            return;
    }
}

void SemanticAnalyzer::validateComparison(const ASTExpr& expr,
                                           const SelectStatement& stmt) {
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
    auto resolve = [&](const std::string& ref) -> const ColumnSchema& {
        auto dot = ref.find('.');
        if (dot != std::string::npos) {
            auto alias = ref.substr(0, dot);
            auto name = ref.substr(dot + 1);
            if (alias == stmt.tableAlias || (stmt.tableAlias.empty() && alias == stmt.tableName))
                return catalog_.getTable(stmt.tableName).column(name);
            for (const auto& join : stmt.joins)
                if (alias == join.rightTable.alias)
                    return catalog_.getTable(join.rightTable.tableName).column(name);
            throw SemanticError("Unknown qualified column '" + ref + "'");
        }
        if (!catalog_.getTable(stmt.tableName).hasColumn(ref))
            throw SemanticError("Unknown column '" + ref + "'");
        return catalog_.getTable(stmt.tableName).column(ref);
    };
    if (lCol) resolve(l.columnName);
    if (rCol) resolve(r.columnName);

    if (lCol && (rLit || rString)) {
        const auto& column = resolve(l.columnName);
        if ((rLit && column.type != DataType::INT64) ||
            (rString && column.type != DataType::STRING))
            throw SemanticError("Literal type does not match column '" +
                                l.columnName + "'");
    }
    if (rCol && (lLit || lString)) {
        const auto& column = resolve(r.columnName);
        if ((lLit && column.type != DataType::INT64) ||
            (lString && column.type != DataType::STRING))
            throw SemanticError("Literal type does not match column '" +
                                r.columnName + "'");
    }

    auto checkNumeric = [&](const std::string& colName) {
        auto& cs = resolve(colName);
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
        const auto& column = resolve(colName);
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
