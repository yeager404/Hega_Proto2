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

    return project;
}

// ── PhysicalPlanner ───────────────────────────────────────────────────────

// Collect all column references from an expression.
static void collectColumns(const ASTExpr& expr, std::vector<std::string>& cols) {
    if (expr.kind == ASTExprKind::ColumnRef) {
        if (std::find(cols.begin(), cols.end(), expr.columnName) == cols.end())
            cols.push_back(expr.columnName);
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

    // Scan must include all projected columns + predicate columns
    for (auto& c : pp.projectColumns) {
        if (std::find(pp.scanColumns.begin(), pp.scanColumns.end(), c) == pp.scanColumns.end())
            pp.scanColumns.push_back(c);
    }

    return pp;
}

} // namespace proto2
