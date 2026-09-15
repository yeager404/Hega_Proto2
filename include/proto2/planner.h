#pragma once
// Logical and Physical query plans.
//
// Logical plan: close to the AST, literals still plaintext.
// Physical plan: ready for execution, literals replaced by EncryptedLiteral nodes.
//
// Physical expressions reuse proto1::Expression / ExprKind directly
// (they are already the right abstraction). We add a thin wrapper here
// so proto2 code doesn't need to include proto1 headers everywhere.

#include "proto2/ast.h"
#include "proto2/catalog.h"
#include <memory>
#include <string>
#include <vector>

namespace proto2 {

// ── Logical Plan ──────────────────────────────────────────────────────────

enum class LogicalNodeKind { Scan, Filter, Project };

struct LogicalNode {
    LogicalNodeKind kind;

    // Scan
    std::string tableName;

    // Filter
    std::shared_ptr<ASTExpr> predicate; // still plaintext literals

    // Project
    std::vector<std::string> columns;

    // Tree structure
    std::shared_ptr<LogicalNode> child;
};

// ── Physical Plan ─────────────────────────────────────────────────────────
// Physical plan uses proto1::Expression nodes (with EncryptedLiteral).
// We forward-declare here; the actual proto1 types are used in planner.cpp.

struct PhysicalPlan {
    std::string              tableName;
    std::vector<std::string> scanColumns;    // all columns needed by scan
    std::vector<std::string> projectColumns; // final output columns
    bool                     hasFilter{false};
    // The filter predicate is stored as a serialized form in QueryPackage.
    // The planner also returns the proto1::Expression for direct local use.
};

// ── Planner ───────────────────────────────────────────────────────────────

class LogicalPlanner {
public:
    // Convert a validated SELECT statement into a logical plan.
    std::shared_ptr<LogicalNode> plan(const SelectStatement& stmt,
                                      const TableSchema& schema);
};

class PhysicalPlanner {
public:
    // Convert a logical plan into a physical plan descriptor.
    PhysicalPlan plan(const LogicalNode& logical);
};

} // namespace proto2
