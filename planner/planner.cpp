// Logical and Physical planner implementations.

#include "proto2/planner.h"
#include <algorithm>
#include <stdexcept>

namespace proto2 {

// ── LogicalPlanner ────────────────────────────────────────────────────────

std::shared_ptr<LogicalNode>
LogicalPlanner::plan(const SelectStatement& stmt, const TableSchema& schema) {
    // Scan
    auto scan = std::make_shared<LogicalNode>();
    scan->kind      = LogicalNodeKind::Scan;
    scan->tableName = stmt.tableName;
    scan->tableAlias = stmt.tableAlias;
    if (!stmt.joins.empty()) {
        const auto& join = stmt.joins.front();
        if (!join.condition || join.condition->kind != ASTExprKind::Equal)
            throw std::runtime_error("Only equality JOINs are supported");
        scan->hasJoin = true;
        scan->joinedTableName = join.rightTable.tableName;
        scan->joinedTableAlias = join.rightTable.alias;
        scan->joinLeftColumn = join.condition->left->columnName;
        scan->joinRightColumn = join.condition->right->columnName;
    }

    // Filter (if WHERE clause present)
    std::shared_ptr<LogicalNode> filterOrScan = scan;
    if (stmt.whereClause) {
        auto filter = std::make_shared<LogicalNode>();
        filter->kind      = LogicalNodeKind::Filter;
        filter->predicate = stmt.whereClause;
        filter->child     = scan;
        filterOrScan = filter;
    }

    // Project
    auto project = std::make_shared<LogicalNode>();
    project->kind  = LogicalNodeKind::Project;
    project->child = filterOrScan;

    if (stmt.projections.empty()) {
        // SELECT * — expand to all columns
        for (auto& c : schema.columns)
            project->columns.push_back(c.name);
    } else {
        project->columns = stmt.projections;
    }

    // Ensure ORDER BY columns are included in projection (needed for client-side sorting)
    for (auto& orderCol : stmt.orderByColumns) {
        if (std::find(project->columns.begin(), project->columns.end(), 
                     orderCol.columnName) == project->columns.end()) {
            project->columns.push_back(orderCol.columnName);
        }
    }

    return project;
}

// ── PhysicalPlanner ───────────────────────────────────────────────────────

// Collect all column references from an expression.
static void collectColumns(const ASTExpr& expr, std::vector<std::string>& cols) {
    if (expr.kind == ASTExprKind::ColumnRef) {
        const auto dot = expr.columnName.find('.');
        const auto bare = dot == std::string::npos
            ? expr.columnName : expr.columnName.substr(dot + 1);
        if (std::find(cols.begin(), cols.end(), bare) == cols.end())
            cols.push_back(bare);
        return;
    }
    if (expr.left)  collectColumns(*expr.left, cols);
    if (expr.right) collectColumns(*expr.right, cols);
}

PhysicalPlan PhysicalPlanner::plan(const LogicalNode& logical) {
    // Expect: Project -> [Filter ->] Scan
    if (logical.kind != LogicalNodeKind::Project)
        throw std::runtime_error("PhysicalPlanner: expected Project at root");

    PhysicalPlan pp;
    pp.projectColumns = logical.columns;
    pp.tableAlias = logical.child->kind == LogicalNodeKind::Scan
        ? logical.child->tableAlias : logical.child->child->tableAlias;

    auto* node = logical.child.get();
    if (!node) throw std::runtime_error("PhysicalPlanner: no child under Project");

    if (node->kind == LogicalNodeKind::Filter) {
        pp.hasFilter = true;
        // Collect columns needed by the predicate
        if (node->predicate)
            collectColumns(*node->predicate, pp.scanColumns);
        node = node->child.get();
    }

    if (!node || node->kind != LogicalNodeKind::Scan)
        throw std::runtime_error("PhysicalPlanner: expected Scan");

    pp.tableName = node->tableName;
    pp.hasJoin = node->hasJoin;
    pp.joinedTableName = node->joinedTableName;
    pp.joinedTableAlias = node->joinedTableAlias;
    pp.joinLeftColumn = node->joinLeftColumn;
    pp.joinRightColumn = node->joinRightColumn;

    // Scan must include all projected columns + predicate columns
    for (auto& c : pp.projectColumns) {
        const auto dot = c.find('.');
        if (pp.hasJoin && dot != std::string::npos &&
            c.substr(0, dot) == pp.joinedTableAlias)
            continue;
        const auto bare = dot == std::string::npos ? c : c.substr(dot + 1);
        if (std::find(pp.scanColumns.begin(), pp.scanColumns.end(), bare) == pp.scanColumns.end())
            pp.scanColumns.push_back(bare);
    }

    return pp;
}

} // namespace proto2
