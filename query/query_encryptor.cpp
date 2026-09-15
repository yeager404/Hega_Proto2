// QueryEncryptor — walks AST, encrypts literals, builds QueryPackage.

#include "proto2/query_encryptor.h"
#include <stdexcept>

namespace proto2 {

QueryEncryptor::QueryEncryptor(std::shared_ptr<ClientCryptoContext> ctx)
    : ctx_(std::move(ctx)) {}

QueryPackage QueryEncryptor::encrypt(const SelectStatement& stmt,
                                      const PhysicalPlan& plan,
                                      const TableSchema& schema,
                                      const FHEParamsMeta& fheMeta) {
    QueryPackage pkg;
    pkg.tableName      = plan.tableName;
    pkg.scanColumns    = plan.scanColumns;
    pkg.projectColumns = plan.projectColumns;
    pkg.fheParams      = fheMeta;
    pkg.schemaVersion  = 1;

    // Build result schema
    for (auto& col : plan.projectColumns) {
        auto& cs = schema.column(col);
        pkg.resultSchema.push_back({cs.name, cs.type});
    }

    // Build predicate node tree (encrypts literals)
    if (stmt.whereClause) {
        uint32_t nextLiteralId = 0;
        buildPredNode(*stmt.whereClause,
                      pkg.predicateNodes,
                      pkg.encryptedLiterals,
                      nextLiteralId);
    }

    return pkg;
}

uint32_t QueryEncryptor::buildPredNode(const ASTExpr& expr,
                                        std::vector<PredNode>& nodes,
                                        std::vector<EncryptedLiteralEntry>& literals,
                                        uint32_t& nextLiteralId) {
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

        case ASTExprKind::Equal:
        case ASTExprKind::GreaterThan:
        case ASTExprKind::LessThan: {
            uint32_t l = buildPredNode(*expr.left,  nodes, literals, nextLiteralId);
            uint32_t r = buildPredNode(*expr.right, nodes, literals, nextLiteralId);
            node.left  = l;
            node.right = r;
            switch (expr.kind) {
                case ASTExprKind::Equal:       node.kind = PredNodeKind::Equal;       break;
                case ASTExprKind::GreaterThan: node.kind = PredNodeKind::GreaterThan; break;
                case ASTExprKind::LessThan:    node.kind = PredNodeKind::LessThan;    break;
                default: break;
            }
            nodes.push_back(node);
            return static_cast<uint32_t>(nodes.size() - 1);
        }

        case ASTExprKind::And:
        case ASTExprKind::Or: {
            uint32_t l = buildPredNode(*expr.left,  nodes, literals, nextLiteralId);
            uint32_t r = buildPredNode(*expr.right, nodes, literals, nextLiteralId);
            node.left  = l;
            node.right = r;
            node.kind  = (expr.kind == ASTExprKind::And) ? PredNodeKind::And : PredNodeKind::Or;
            nodes.push_back(node);
            return static_cast<uint32_t>(nodes.size() - 1);
        }

        case ASTExprKind::Not: {
            uint32_t l = buildPredNode(*expr.left, nodes, literals, nextLiteralId);
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
