// QueryExecutor — server-side encrypted query execution.
// Builds proto1 physical operators from a QueryPackage and executes them.
// The server NEVER decrypts data. The secret key is NOT present here.

#include "proto2/query_executor.h"
#include "proto1/operators.h"
#include "proto1/result_stream.h"
#include "proto1/expression.h"
#include "proto1/ingestion.h"

#include <functional>
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
    std::vector<std::string> physicalScanColumns;
    for (const auto& column : pkg.scanColumns) {
        const auto& schema = catalog_.getTable(pkg.tableName).column(column);
        if (schema.type == DataType::STRING) {
            for (size_t position = 0; position < schema.maxLength; ++position)
                physicalScanColumns.push_back(
                    proto1::stringPhysicalColumn(column, position));
        } else {
            physicalScanColumns.push_back(column);
        }
    }
    auto scan = std::make_shared<proto1::TableScanExec>(
        pkg.tableName, physicalScanColumns, storage_);

    std::shared_ptr<proto1::PhysicalOperator> root = scan;

    if (!pkg.predicateNodes.empty()) {
        uint32_t rootIdx = static_cast<uint32_t>(pkg.predicateNodes.size() - 1);
        auto predExpr = buildExpression(pkg, rootIdx);
        root = std::make_shared<proto1::FilterExec>(scan, predExpr, fheCtx);
    }

    std::vector<std::string> physicalProjectColumns;
    for (const auto& column : pkg.projectColumns) {
        const auto& schema = catalog_.getTable(pkg.tableName).column(column);
        if (schema.type == DataType::STRING) {
            for (size_t position = 0; position < schema.maxLength; ++position)
                physicalProjectColumns.push_back(
                    proto1::stringPhysicalColumn(column, position));
        } else {
            physicalProjectColumns.push_back(column);
        }
    }
    root = std::make_shared<proto1::ProjectExec>(
        root, std::move(physicalProjectColumns));

    // Stream results
    proto1::EncryptedResultStream stream(root);
    std::vector<EncryptedResultBatch> results;
    uint32_t batchId = 0;

    while (auto batch = stream.next()) {
        EncryptedResultBatch rb;
        rb.batchId        = batchId++;
        rb.logicalRowCount = batch->logicalRowCount;

        // Serialize each projected column
        for (const auto& colName : pkg.projectColumns) {
            const auto& schema = catalog_.getTable(pkg.tableName).column(colName);
            const size_t width = schema.type == DataType::STRING
                ? schema.maxLength : 1;
            for (size_t position = 0; position < width; ++position) {
                const std::string physicalName =
                    schema.type == DataType::STRING
                        ? proto1::stringPhysicalColumn(colName, position)
                        : colName;
                auto it = batch->columns.find(physicalName);
                if (it == batch->columns.end())
                    throw std::runtime_error("QueryExecutor: missing column: " +
                                             physicalName);

                auto& chunk = it->second;
                if (chunk.ciphertexts.empty())
                    throw std::runtime_error("QueryExecutor: empty ciphertext for: " +
                                             physicalName);

                EncryptedColumnResult cr;
                cr.columnName = colName;
                cr.logicalType = schema.type == DataType::STRING
                    ? "STRING" : "INT64";
                cr.stringPosition = static_cast<uint32_t>(position);
                cr.ciphertextBytes = chunk.ciphertexts[0]->serialize();
                cr.logicalSize = chunk.ciphertexts[0]->logicalSize();
                rb.columns.push_back(std::move(cr));
            }
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

        case PredNodeKind::StringEqual:
        case PredNodeKind::StringNotEqual: {
            std::vector<std::shared_ptr<proto1::Expression>> comparisons;
            for (uint32_t position = 0; position < node.stringWidth; ++position) {
                const uint32_t literalId = node.literalId + position;
                const EncryptedLiteralEntry* entry = nullptr;
                for (const auto& literal : pkg.encryptedLiterals) {
                    if (literal.id == literalId) {
                        entry = &literal;
                        break;
                    }
                }
                if (!entry)
                    throw std::runtime_error("buildExpression: string literal not found");

                auto literal = fheCtx->deserializeWithSize(
                    entry->ciphertextBytes, entry->logicalSize);
                auto equal = proto1::Expression::equal(
                    proto1::Expression::columnRef(
                        proto1::stringPhysicalColumn(node.columnName, position)),
                    proto1::Expression::encLiteral(literal));
                comparisons.push_back(std::move(equal));
            }
            if (comparisons.empty())
                throw std::runtime_error("buildExpression: empty string width");
            std::function<std::shared_ptr<proto1::Expression>(size_t, size_t)>
                combine;
            combine = [&](size_t begin, size_t end) {
                if (end - begin == 1)
                    return comparisons[begin];
                const size_t middle = begin + (end - begin) / 2;
                return proto1::Expression::andExpr(
                    combine(begin, middle), combine(middle, end));
            };
            auto result = combine(0, comparisons.size());
            return node.kind == PredNodeKind::StringNotEqual
                ? proto1::Expression::notExpr(result)
                : result;
        }

        case PredNodeKind::Equal: {
            auto l = buildExpression(pkg, node.left);
            auto r = buildExpression(pkg, node.right);
            return proto1::Expression::equal(l, r);
        }

        case PredNodeKind::NotEqual: {
            auto l = buildExpression(pkg, node.left);
            auto r = buildExpression(pkg, node.right);
            auto eq = proto1::Expression::equal(l, r);
            return proto1::Expression::notExpr(eq);
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

        case PredNodeKind::GreaterEqual: {
            auto l = buildExpression(pkg, node.left);
            auto r = buildExpression(pkg, node.right);
            auto gt = proto1::Expression::gt(l, r);
            auto eq = proto1::Expression::equal(l, r);
            return proto1::Expression::orExpr(gt, eq);
        }

        case PredNodeKind::LessEqual: {
            auto l = buildExpression(pkg, node.left);
            auto r = buildExpression(pkg, node.right);
            auto lt = proto1::Expression::lt(l, r);
            auto eq = proto1::Expression::equal(l, r);
            return proto1::Expression::orExpr(lt, eq);
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
