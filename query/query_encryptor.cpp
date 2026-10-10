// QueryEncryptor — walks AST, encrypts literals, builds QueryPackage.

#include "proto2/query_encryptor.h"
#include <stdexcept>

namespace proto2 {

QueryEncryptor::QueryEncryptor(std::shared_ptr<ClientCryptoContext> ctx)
    : ctx_(std::move(ctx)) {}

uint32_t QueryEncryptor::addEncryptedString(
    const std::string& value,
    size_t maxLength,
    std::vector<EncryptedLiteralEntry>& literals,
    uint32_t& nextLiteralId) {
    const uint32_t firstId = nextLiteralId;
    for (size_t position = 0; position < maxLength; ++position) {
        const int64_t code = position < value.size()
            ? static_cast<unsigned char>(value[position])
            : 0;
        auto ct = ctx_->encryptScalar(code);

        EncryptedLiteralEntry entry;
        entry.id = nextLiteralId++;
        entry.logicalType = "STRING_CHAR";
        entry.ciphertextBytes = ct->serialize();
        entry.logicalSize = ct->logicalSize();
        literals.push_back(std::move(entry));
    }
    return firstId;
}

QueryPackage QueryEncryptor::encrypt(const SelectStatement& stmt,
                                      const PhysicalPlan& plan,
                                      const TableSchema& schema,
                                      const FHEParamsMeta& fheMeta) {
    QueryPackage pkg;
    pkg.tableName      = plan.tableName;
    pkg.tableAlias     = plan.tableAlias;
    pkg.hasJoin        = plan.hasJoin;
    pkg.joinedTableName = plan.joinedTableName;
    pkg.joinedTableAlias = plan.joinedTableAlias;
    pkg.joinLeftColumn = plan.joinLeftColumn;
    pkg.joinRightColumn = plan.joinRightColumn;
    pkg.scanColumns    = plan.scanColumns;
    pkg.projectColumns = plan.projectColumns;
    pkg.fheParams      = fheMeta;
    pkg.schemaVersion  = 1;

    // Build result schema
    for (auto& col : plan.projectColumns) {
        const auto dot = col.find('.');
        const auto bare = dot == std::string::npos ? col : col.substr(dot + 1);
        auto& cs = schema.column(bare);
        pkg.resultSchema.push_back({cs.name, cs.type, cs.maxLength});
    }

    // Build predicate node tree (encrypts literals)
    if (stmt.whereClause) {
        uint32_t nextLiteralId = 0;
        buildPredNode(*stmt.whereClause,
                      pkg.predicateNodes,
                      pkg.encryptedLiterals,
                      nextLiteralId,
                      schema);
    }

    return pkg;
}

QueryPackage QueryEncryptor::encrypt(const SelectStatement& stmt,
                                      const PhysicalPlan& plan,
                                      const Catalog& catalog,
                                      const FHEParamsMeta& fheMeta) {
    QueryPackage pkg;
    pkg.tableName = plan.tableName;
    pkg.tableAlias = plan.tableAlias;
    pkg.hasJoin = plan.hasJoin;
    pkg.joinedTableName = plan.joinedTableName;
    pkg.joinedTableAlias = plan.joinedTableAlias;
    pkg.joinLeftColumn = plan.joinLeftColumn;
    pkg.joinRightColumn = plan.joinRightColumn;
    pkg.scanColumns = plan.scanColumns;
    pkg.projectColumns = plan.projectColumns;
    pkg.fheParams = fheMeta;
    for (const auto& name : plan.projectColumns) {
        const auto dot = name.find('.');
        const auto alias = dot == std::string::npos ? plan.tableAlias : name.substr(0, dot);
        const auto bare = dot == std::string::npos ? name : name.substr(dot + 1);
        const auto table = alias == plan.tableAlias ? plan.tableName : plan.joinedTableName;
        const auto& cs = catalog.getTable(table).column(bare);
        pkg.resultSchema.push_back({name, cs.type, cs.maxLength});
    }
    if (stmt.whereClause) {
        uint32_t nextLiteralId = 0;
        buildPredNode(*stmt.whereClause, pkg.predicateNodes,
                      pkg.encryptedLiterals, nextLiteralId,
                      catalog.getTable(plan.tableName));
    }
    return pkg;
}

uint32_t QueryEncryptor::buildPredNode(const ASTExpr& expr,
                                        std::vector<PredNode>& nodes,
                                        std::vector<EncryptedLiteralEntry>& literals,
                                        uint32_t& nextLiteralId,
                                        const TableSchema& schema) {
    PredNode node;

    switch (expr.kind) {
        case ASTExprKind::ColumnRef:
            node.kind       = PredNodeKind::ColumnRef;
            node.columnName = expr.columnName;
            nodes.push_back(node);
            return static_cast<uint32_t>(nodes.size() - 1);

        case ASTExprKind::IntLiteral: {
            // Encrypt the literal — server never sees the plaintext value.
            auto ct = ctx_->encryptScalar(expr.intValue);
            auto bytes = ct->serialize();

            EncryptedLiteralEntry entry;
            entry.id             = nextLiteralId++;
            entry.logicalType    = "INT64";
            entry.ciphertextBytes = std::move(bytes);
            entry.logicalSize    = ct->logicalSize();
            literals.push_back(std::move(entry));

            node.kind      = PredNodeKind::LiteralRef;
            node.literalId = entry.id;
            // Fix: entry was moved, use the id we saved
            node.literalId = nextLiteralId - 1;
            nodes.push_back(node);
            return static_cast<uint32_t>(nodes.size() - 1);
        }

        case ASTExprKind::StringLiteral:
            throw std::runtime_error(
                "QueryEncryptor: string literal must be used in a comparison");

        case ASTExprKind::Equal:
        case ASTExprKind::NotEqual:
        case ASTExprKind::GreaterThan:
        case ASTExprKind::LessThan:
        case ASTExprKind::GreaterEqual:
        case ASTExprKind::LessEqual: {
            const ASTExpr& left = *expr.left;
            const ASTExpr& right = *expr.right;
            const bool stringComparison =
                (left.kind == ASTExprKind::ColumnRef &&
                 right.kind == ASTExprKind::StringLiteral) ||
                (left.kind == ASTExprKind::StringLiteral &&
                 right.kind == ASTExprKind::ColumnRef);

            if (stringComparison) {
                const ASTExpr& column =
                    left.kind == ASTExprKind::ColumnRef ? left : right;
                const ASTExpr& literal =
                    left.kind == ASTExprKind::StringLiteral ? left : right;
                const auto dot = column.columnName.find('.');
                const auto bareName = dot == std::string::npos
                    ? column.columnName : column.columnName.substr(dot + 1);
                const auto& columnSchema = schema.column(bareName);
                const uint32_t firstId = addEncryptedString(
                    literal.stringValue,
                    columnSchema.maxLength,
                    literals,
                    nextLiteralId);

                node.kind = expr.kind == ASTExprKind::Equal
                    ? PredNodeKind::StringEqual
                    : PredNodeKind::StringNotEqual;
                node.columnName = column.columnName;
                node.literalId = firstId;
                node.stringWidth =
                    static_cast<uint32_t>(columnSchema.maxLength);
                nodes.push_back(node);
                return static_cast<uint32_t>(nodes.size() - 1);
            }

            uint32_t l = buildPredNode(*expr.left, nodes, literals,
                                        nextLiteralId, schema);
            uint32_t r = buildPredNode(*expr.right, nodes, literals,
                                        nextLiteralId, schema);
            node.left  = l;
            node.right = r;
            switch (expr.kind) {
                case ASTExprKind::Equal:       node.kind = PredNodeKind::Equal;       break;
                case ASTExprKind::NotEqual:    node.kind = PredNodeKind::NotEqual;    break;
                case ASTExprKind::GreaterThan: node.kind = PredNodeKind::GreaterThan; break;
                case ASTExprKind::LessThan:    node.kind = PredNodeKind::LessThan;    break;
                case ASTExprKind::GreaterEqual:node.kind = PredNodeKind::GreaterEqual;break;
                case ASTExprKind::LessEqual:   node.kind = PredNodeKind::LessEqual;   break;
                default: break;
            }
            nodes.push_back(node);
            return static_cast<uint32_t>(nodes.size() - 1);
        }

        case ASTExprKind::And:
        case ASTExprKind::Or: {
            uint32_t l = buildPredNode(*expr.left, nodes, literals,
                                        nextLiteralId, schema);
            uint32_t r = buildPredNode(*expr.right, nodes, literals,
                                        nextLiteralId, schema);
            node.left  = l;
            node.right = r;
            node.kind  = (expr.kind == ASTExprKind::And) ? PredNodeKind::And : PredNodeKind::Or;
            nodes.push_back(node);
            return static_cast<uint32_t>(nodes.size() - 1);
        }

        case ASTExprKind::Not: {
            uint32_t l = buildPredNode(*expr.left, nodes, literals,
                                        nextLiteralId, schema);
            node.left  = l;
            node.kind  = PredNodeKind::Not;
            nodes.push_back(node);
            return static_cast<uint32_t>(nodes.size() - 1);
        }

        default:
            throw std::runtime_error("QueryEncryptor: unsupported expression kind");
    }
}

} // namespace proto2
