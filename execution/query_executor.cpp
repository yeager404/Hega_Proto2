// QueryExecutor — server-side encrypted query execution.
// Builds proto1 physical operators from a QueryPackage and executes them.
// The server NEVER decrypts data. The secret key is NOT present here.

#include "proto2/query_executor.h"
#include "proto1/operators.h"
#include "proto1/result_stream.h"
#include "proto1/expression.h"

#include <stdexcept>

namespace proto2 {

QueryExecutor::QueryExecutor(std::shared_ptr<ServerCryptoContext> serverCtx,
                              std::shared_ptr<proto1::EncryptedStorage> storage,
                              const Catalog& catalog)
    : serverCtx_(std::move(serverCtx))
    , storage_(std::move(storage))
    , catalog_(catalog) {}

std::vector<EncryptedResultBatch>
QueryExecutor::execute(const QueryPackage& pkg) {
    auto fheCtx = serverCtx_->keyCtxPtr();

    // Build physical plan: ProjectExec -> FilterExec -> TableScanExec
    auto scan = std::make_shared<proto1::TableScanExec>(
        pkg.tableName, pkg.scanColumns, storage_);

    std::shared_ptr<proto1::PhysicalOperator> root = scan;

    if (!pkg.predicateNodes.empty()) {
        uint32_t rootIdx = static_cast<uint32_t>(pkg.predicateNodes.size() - 1);
        auto predExpr = buildExpression(pkg, rootIdx);
        root = std::make_shared<proto1::FilterExec>(scan, predExpr, fheCtx);
    }

    root = std::make_shared<proto1::ProjectExec>(root, pkg.projectColumns);

    // Stream results
    proto1::EncryptedResultStream stream(root);
    std::vector<EncryptedResultBatch> results;
    uint32_t batchId = 0;

    while (auto batch = stream.next()) {
        EncryptedResultBatch rb;
        rb.batchId        = batchId++;
        rb.logicalRowCount = batch->logicalRowCount;

        // Serialize each projected column
        for (auto& colName : pkg.projectColumns) {
            auto it = batch->columns.find(colName);
            if (it == batch->columns.end())
                throw std::runtime_error("QueryExecutor: missing column: " + colName);

            auto& chunk = it->second;
            if (chunk.ciphertexts.empty())
                throw std::runtime_error("QueryExecutor: empty ciphertext for: " + colName);

            EncryptedColumnResult cr;
            cr.columnName      = colName;
            cr.ciphertextBytes = chunk.ciphertexts[0]->serialize();
            cr.logicalSize     = chunk.ciphertexts[0]->logicalSize();
            rb.columns.push_back(std::move(cr));
        }

        // Serialize selection mask
        if (batch->selectionMask) {
            rb.selectionMaskBytes = batch->selectionMask->serialize();
            rb.hasMask = true;
        }

        results.push_back(std::move(rb));
    }

    return results;
}

std::shared_ptr<proto1::Expression>
QueryExecutor::buildExpression(const QueryPackage& pkg, uint32_t nodeIdx) const {
    if (nodeIdx >= pkg.predicateNodes.size())
        throw std::runtime_error("buildExpression: node index out of range");

    const PredNode& node = pkg.predicateNodes[nodeIdx];
    auto fheCtx = serverCtx_->keyCtxPtr();

    switch (node.kind) {
        case PredNodeKind::ColumnRef:
            return proto1::Expression::columnRef(node.columnName);

        case PredNodeKind::LiteralRef: {
            // Find the encrypted literal
            const EncryptedLiteralEntry* entry = nullptr;
            for (auto& lit : pkg.encryptedLiterals)
                if (lit.id == node.literalId) { entry = &lit; break; }
            if (!entry)
                throw std::runtime_error("buildExpression: literal not found: " +
                                         std::to_string(node.literalId));
            // Deserialize ciphertext — server never sees the plaintext value
            auto ct = fheCtx->deserializeWithSize(entry->ciphertextBytes,
                                                   entry->logicalSize);
            return proto1::Expression::encLiteral(ct);
        }

        case PredNodeKind::Equal: {
            auto l = buildExpression(pkg, node.left);
            auto r = buildExpression(pkg, node.right);
            return proto1::Expression::equal(l, r);
        }

        case PredNodeKind::GreaterThan: {
            auto l = buildExpression(pkg, node.left);
            auto r = buildExpression(pkg, node.right);
            return proto1::Expression::gt(l, r);
        }

        case PredNodeKind::LessThan: {
            auto l = buildExpression(pkg, node.left);
            auto r = buildExpression(pkg, node.right);
            return proto1::Expression::lt(l, r);
        }

        case PredNodeKind::And: {
            auto l = buildExpression(pkg, node.left);
            auto r = buildExpression(pkg, node.right);
            return proto1::Expression::andExpr(l, r);
        }

        case PredNodeKind::Or: {
            auto l = buildExpression(pkg, node.left);
            auto r = buildExpression(pkg, node.right);
            return proto1::Expression::orExpr(l, r);
        }

        case PredNodeKind::Not: {
            auto l = buildExpression(pkg, node.left);
            return proto1::Expression::notExpr(l);
        }
    }

    throw std::runtime_error("buildExpression: unknown node kind");
}

} // namespace proto2
