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

    if (pkg.hasJoin) {
        const auto leftPrefix = pkg.tableAlias + ".";
        const auto rightPrefix = pkg.joinedTableAlias + ".";
        if (pkg.joinLeftColumn.compare(0, leftPrefix.size(), leftPrefix) != 0 ||
            pkg.joinRightColumn.compare(0, rightPrefix.size(), rightPrefix) != 0)
            throw std::runtime_error("JOIN columns must be qualified by their aliases");
        const auto leftKey = pkg.joinLeftColumn.substr(pkg.tableAlias.size() + 1);
        const auto rightKey = pkg.joinRightColumn.substr(pkg.joinedTableAlias.size() + 1);
        const auto leftChunks = storage_->listChunks(pkg.tableName, leftKey);
        const auto rightChunks = storage_->listChunks(pkg.joinedTableName, rightKey);
        std::vector<EncryptedResultBatch> results;
        uint32_t batchId = 0;
        proto1::ExpressionExecutor exprExec(fheCtx);
        auto addTableColumns = [&](proto1::EncryptedBatch& batch,
                                    const std::string& table,
                                    const std::string& alias,
                                    uint32_t chunkId) {
            for (const auto& schemaColumn : catalog_.getTable(table).columns) {
                if (schemaColumn.type == DataType::STRING) {
                    for (size_t position = 0; position < schemaColumn.maxLength; ++position) {
                        const auto physical = proto1::stringPhysicalColumn(
                            schemaColumn.name, position);
                        auto chunk = storage_->getChunk(table, physical, 0, chunkId);
                        batch.columns[alias + "." + physical] = std::move(chunk);
                    }
                } else {
                    auto chunk = storage_->getChunk(table, schemaColumn.name, 0, chunkId);
                    batch.columns[alias + "." + schemaColumn.name] = std::move(chunk);
                }
            }
        };
        std::shared_ptr<proto1::Expression> where;
        if (!pkg.predicateNodes.empty())
            where = buildExpression(pkg,
                static_cast<uint32_t>(pkg.predicateNodes.size() - 1));
        const auto whereUsesJoinedTable = [&]() {
            for (const auto& node : pkg.predicateNodes) {
                if (node.kind == PredNodeKind::ColumnRef &&
                    node.columnName.compare(0, rightPrefix.size(), rightPrefix) == 0)
                    return true;
            }
            return false;
        }();
        for (const auto& lm : leftChunks) {
            auto leftJoin = storage_->getChunk(pkg.tableName, leftKey, lm.partitionId, lm.chunkId);
            if (leftJoin.meta.logicalRowCount != 1)
                throw std::runtime_error("JOIN requires setup data ingested with chunk size 1");
            std::shared_ptr<proto1::EncryptedVector> leftMask;
            if (where && !whereUsesJoinedTable) {
                proto1::EncryptedBatch leftOnly;
                leftOnly.logicalRowCount = 1;
                addTableColumns(leftOnly, pkg.tableName, pkg.tableAlias, lm.chunkId);
                leftMask = exprExec.evaluate(*where, leftOnly);
            }
            for (const auto& rm : rightChunks) {
                auto rightJoin = storage_->getChunk(pkg.joinedTableName, rightKey,
                                                     rm.partitionId, rm.chunkId);
                if (rightJoin.meta.logicalRowCount != 1)
                    throw std::runtime_error("JOIN requires setup data ingested with chunk size 1");
                proto1::EncryptedBatch pair;
                pair.logicalRowCount = 1;
                addTableColumns(pair, pkg.tableName, pkg.tableAlias, lm.chunkId);
                addTableColumns(pair, pkg.joinedTableName, pkg.joinedTableAlias, rm.chunkId);
                auto joinExpr = proto1::Expression::equal(
                    proto1::Expression::columnRef(pkg.joinLeftColumn),
                    proto1::Expression::columnRef(pkg.joinRightColumn));
                auto mask = exprExec.evaluate(*joinExpr, pair);
                if (whereUsesJoinedTable) {
                    auto pairWhereMask = exprExec.evaluate(*where, pair);
                    mask = fheCtx->evalMult(*mask, *pairWhereMask);
                } else if (leftMask) {
                    mask = fheCtx->evalMult(*mask, *leftMask);
                }
                EncryptedResultBatch rb;
                rb.batchId = batchId++;
                rb.logicalRowCount = 1;
                for (const auto& name : pkg.projectColumns) {
                    const auto dot = name.find('.');
                    const auto alias = dot == std::string::npos
                        ? pkg.tableAlias : name.substr(0, dot);
                    const auto bare = dot == std::string::npos
                        ? name : name.substr(dot + 1);
                    const auto table = alias == pkg.tableAlias
                        ? pkg.tableName : pkg.joinedTableName;
                    const auto chunkId = alias == pkg.tableAlias ? lm.chunkId : rm.chunkId;
                    const auto& cs = catalog_.getTable(table).column(bare);
                    const size_t width = cs.type == DataType::STRING ? cs.maxLength : 1;
                    for (size_t position = 0; position < width; ++position) {
                        const auto physical = cs.type == DataType::STRING
                            ? proto1::stringPhysicalColumn(bare, position) : bare;
                        auto chunk = storage_->getChunk(table, physical, 0, chunkId);
                        if (chunk.ciphertexts.empty())
                            throw std::runtime_error("JOIN result column has no ciphertext");
                        EncryptedColumnResult cr;
                        cr.columnName = name;
                        cr.logicalType = cs.type == DataType::STRING ? "STRING" : "INT64";
                        cr.stringPosition = static_cast<uint32_t>(position);
                        cr.ciphertextBytes = chunk.ciphertexts[0]->serialize();
                        cr.logicalSize = chunk.ciphertexts[0]->logicalSize();
                        rb.columns.push_back(std::move(cr));
                    }
                }
                rb.selectionMaskBytes = mask->serialize();
                rb.hasMask = true;
                results.push_back(std::move(rb));
            }
        }
        return results;
    }

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
