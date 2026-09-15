// Prototype 2 — comprehensive test suite.

#include "proto2/ast.h"
#include "proto2/catalog.h"
#include "proto2/crypto_context.h"
#include "proto2/parser.h"
#include "proto2/planner.h"
#include "proto2/query_encryptor.h"
#include "proto2/query_executor.h"
#include "proto2/query_package.h"
#include "proto2/query_validator.h"
#include "proto2/result_decoder.h"
#include "proto2/semantic.h"
#include "proto1/fhe_runtime.h"
#include "proto1/ingestion.h"
#include "proto1/storage.h"
#include "proto1/catalog.h"

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace proto2;

// ── Shared lightweight FHE fixture (for non-execution tests) ──────────────

class FHEFixture : public ::testing::Test {
protected:
    static std::shared_ptr<ClientCryptoContext> clientCtx_;
    static std::string keysDir_;

    static void SetUpTestSuite() {
        proto1::FHEParameterSet params;
        params.id                  = "bfv-257-depth9";
        params.plaintextModulus    = 257;
        params.multiplicativeDepth = 9;
        params.ringDimension       = 128;

        clientCtx_ = ClientCryptoContext::create(params);
        keysDir_ = (fs::temp_directory_path() / "proto2_fhe_keys").string();
        fs::remove_all(keysDir_);
        clientCtx_->saveKeys(keysDir_);
    }

    static void TearDownTestSuite() {
        fs::remove_all(keysDir_);
    }
};

std::shared_ptr<ClientCryptoContext> FHEFixture::clientCtx_;
std::string FHEFixture::keysDir_;

// ── E2E context ───────────────────────────────────────────────────────────
// Each test gets its own isolated FHE context + storage.
// Key insight: we create the client context once, ingest data, then wrap
// the same underlying FHEKeyContext as both client and server contexts.
// This avoids the ReleaseAllContexts problem entirely.

struct E2ECtx {
    std::string tmpDir;
    std::shared_ptr<ClientCryptoContext>      clientCtx;
    std::shared_ptr<ServerCryptoContext>      serverCtx;
    std::shared_ptr<proto1::EncryptedStorage> storage;
    Catalog                                   catalog;

    E2ECtx() = default;
    E2ECtx(const E2ECtx&) = delete;
    E2ECtx& operator=(const E2ECtx&) = delete;
    E2ECtx(E2ECtx&& o) noexcept
        : tmpDir(std::move(o.tmpDir)), clientCtx(std::move(o.clientCtx))
        , serverCtx(std::move(o.serverCtx)), storage(std::move(o.storage))
        , catalog(std::move(o.catalog)) { o.tmpDir.clear(); }
    E2ECtx& operator=(E2ECtx&& o) noexcept {
        if (this != &o) {
            cleanup();
            tmpDir    = std::move(o.tmpDir);    o.tmpDir.clear();
            clientCtx = std::move(o.clientCtx);
            serverCtx = std::move(o.serverCtx);
            storage   = std::move(o.storage);
            catalog   = std::move(o.catalog);
        }
        return *this;
    }
    ~E2ECtx() { cleanup(); }
    void cleanup() { if (!tmpDir.empty()) { fs::remove_all(tmpDir); tmpDir.clear(); } }

    static E2ECtx create() {
        E2ECtx c;
        auto ts = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        c.tmpDir = (fs::temp_directory_path() / ("p2e2e_" + std::to_string(ts))).string();
        fs::remove_all(c.tmpDir);
        std::string dataDir = c.tmpDir + "/data";

        proto1::FHEParameterSet params;
        params.id = "bfv-257-depth9";
        params.plaintextModulus = 257;
        params.multiplicativeDepth = 9;
        params.ringDimension = 128;

        // Create client context
        c.clientCtx = ClientCryptoContext::create(params);

        // Ingest dataset
        static const char* CSV =
            "id,age,salary,department_id\n"
            "1,2,5,1\n2,3,6,1\n3,4,7,2\n4,5,8,2\n5,6,9,3\n"
            "6,7,10,3\n7,8,11,4\n8,9,12,4\n9,10,13,5\n10,11,14,5\n";
        std::string csvPath = c.tmpDir + "/emp.csv";
        fs::create_directories(c.tmpDir);
        { std::ofstream f(csvPath); f << CSV; }

        proto1::TableSchema schema{"employees",
            {{"id",proto1::DataType::INT64},{"age",proto1::DataType::INT64},
             {"salary",proto1::DataType::INT64},{"department_id",proto1::DataType::INT64}}};

        fs::create_directories(dataDir);
        auto stor = proto1::makeFilesystemStorage(dataDir, c.clientCtx->keyCtxPtr());
        proto1::ingestCSV(csvPath, schema, 10, c.clientCtx->keyCtx(), *stor);
        fs::remove(csvPath);

        // Server context wraps the SAME underlying FHEKeyContext as client.
        // This avoids ReleaseAllContexts invalidating the ciphertexts.
        // In production the server would load from disk; here we share the
        // context pointer directly since client and server are in the same process.
        c.serverCtx = std::make_shared<ServerCryptoContext>(c.clientCtx->keyCtxPtr());
        c.storage   = proto1::makeFilesystemStorage(dataDir, c.serverCtx->keyCtxPtr());
        c.catalog   = makeEmployeesCatalog();

        return c;
    }

    std::vector<ResultRow> runQuery(const std::string& sql) {
        SQLParser p;
        auto stmt = p.parse(sql);
        SemanticAnalyzer a(catalog);
        auto& schema  = a.analyze(stmt);
        auto logPlan  = LogicalPlanner().plan(stmt, schema);
        auto physPlan = PhysicalPlanner().plan(*logPlan);

        FHEParamsMeta meta;
        meta.paramSetId = clientCtx->paramSetId();
        meta.slotCount  = clientCtx->slotCount();
        meta.keyId      = "key-0";

        auto ctxPtr = std::make_shared<ClientCryptoContext>(clientCtx->keyCtxPtr());
        auto pkg = QueryEncryptor(ctxPtr).encrypt(stmt, physPlan, schema, meta);
        pkg.queryId = "e2e-test";

        QueryValidator(catalog, *serverCtx).validate(pkg);

        auto batches = QueryExecutor(serverCtx, storage, catalog).execute(pkg);

        ResultDecoder decoder(ctxPtr);
        std::vector<ResultRow> rows;
        for (auto& b : batches) {
            auto r = decoder.decode(b);
            rows.insert(rows.end(), r.begin(), r.end());
        }
        return rows;
    }
};

class E2ETest : public ::testing::Test {
protected:
    E2ECtx ctx_;
    void SetUp() override { ctx_ = E2ECtx::create(); }
};

// ═══════════════════════════════════════════════════════════════════════════
// PARSER TESTS
// ═══════════════════════════════════════════════════════════════════════════

TEST(ParserTest, SelectStar) {
    SQLParser p;
    auto stmt = p.parse("SELECT * FROM employees");
    EXPECT_EQ(stmt.tableName, "employees");
    EXPECT_TRUE(stmt.projections.empty());
    EXPECT_EQ(stmt.whereClause, nullptr);
}

TEST(ParserTest, SelectColumns) {
    SQLParser p;
    auto stmt = p.parse("SELECT id, salary FROM employees");
    ASSERT_EQ(stmt.projections.size(), 2u);
    EXPECT_EQ(stmt.projections[0], "id");
    EXPECT_EQ(stmt.projections[1], "salary");
}

TEST(ParserTest, WhereEqual) {
    SQLParser p;
    auto stmt = p.parse("SELECT id FROM employees WHERE id = 10");
    ASSERT_NE(stmt.whereClause, nullptr);
    EXPECT_EQ(stmt.whereClause->kind, ASTExprKind::Equal);
    EXPECT_EQ(stmt.whereClause->left->columnName, "id");
    EXPECT_EQ(stmt.whereClause->right->intValue, 10);
}

TEST(ParserTest, WhereGreaterThan) {
    SQLParser p;
    auto stmt = p.parse("SELECT id FROM employees WHERE age > 30");
    ASSERT_NE(stmt.whereClause, nullptr);
    EXPECT_EQ(stmt.whereClause->kind, ASTExprKind::GreaterThan);
    EXPECT_EQ(stmt.whereClause->right->intValue, 30);
}

TEST(ParserTest, WhereLessThan) {
    SQLParser p;
    auto stmt = p.parse("SELECT id FROM employees WHERE age < 50");
    ASSERT_NE(stmt.whereClause, nullptr);
    EXPECT_EQ(stmt.whereClause->kind, ASTExprKind::LessThan);
}

TEST(ParserTest, WhereAnd) {
    SQLParser p;
    auto stmt = p.parse("SELECT id, salary FROM employees WHERE id = 10 AND age > 30");
    ASSERT_NE(stmt.whereClause, nullptr);
    EXPECT_EQ(stmt.whereClause->kind, ASTExprKind::And);
    EXPECT_EQ(stmt.whereClause->left->kind, ASTExprKind::Equal);
    EXPECT_EQ(stmt.whereClause->right->kind, ASTExprKind::GreaterThan);
}

TEST(ParserTest, WhereOr) {
    SQLParser p;
    auto stmt = p.parse("SELECT id FROM employees WHERE id = 10 OR age > 30");
    ASSERT_NE(stmt.whereClause, nullptr);
    EXPECT_EQ(stmt.whereClause->kind, ASTExprKind::Or);
}

TEST(ParserTest, WhereNot) {
    SQLParser p;
    auto stmt = p.parse("SELECT id FROM employees WHERE NOT age > 30");
    ASSERT_NE(stmt.whereClause, nullptr);
    EXPECT_EQ(stmt.whereClause->kind, ASTExprKind::Not);
    EXPECT_EQ(stmt.whereClause->left->kind, ASTExprKind::GreaterThan);
}

TEST(ParserTest, TrailingSemicolon) {
    SQLParser p;
    auto stmt = p.parse("SELECT id FROM employees WHERE id = 5;");
    EXPECT_EQ(stmt.tableName, "employees");
}

TEST(ParserTest, EmptyQueryThrows) {
    SQLParser p;
    EXPECT_THROW(p.parse(""), ParseError);
}

TEST(ParserTest, InvalidSQLThrows) {
    SQLParser p;
    EXPECT_THROW(p.parse("INSERT INTO foo VALUES (1)"), ParseError);
}

TEST(ParserTest, MultilineQuery) {
    SQLParser p;
    auto stmt = p.parse("SELECT id, salary FROM employees WHERE id = 10 AND age > 3");
    EXPECT_EQ(stmt.tableName, "employees");
    ASSERT_EQ(stmt.projections.size(), 2u);
    ASSERT_NE(stmt.whereClause, nullptr);
    EXPECT_EQ(stmt.whereClause->kind, ASTExprKind::And);
}

// ═══════════════════════════════════════════════════════════════════════════
// SEMANTIC ANALYSIS TESTS
// ═══════════════════════════════════════════════════════════════════════════

TEST(SemanticTest, ValidQuery) {
    Catalog cat = makeEmployeesCatalog();
    SemanticAnalyzer a(cat);
    SQLParser p;
    auto stmt = p.parse("SELECT id, salary FROM employees WHERE id = 10");
    EXPECT_NO_THROW(a.analyze(stmt));
}

TEST(SemanticTest, UnknownTable) {
    Catalog cat = makeEmployeesCatalog();
    SemanticAnalyzer a(cat);
    SQLParser p;
    auto stmt = p.parse("SELECT id FROM nonexistent");
    EXPECT_THROW(a.analyze(stmt), SemanticError);
}

TEST(SemanticTest, UnknownColumn) {
    Catalog cat = makeEmployeesCatalog();
    SemanticAnalyzer a(cat);
    SQLParser p;
    auto stmt = p.parse("SELECT foo FROM employees");
    EXPECT_THROW(a.analyze(stmt), SemanticError);
}

TEST(SemanticTest, UnknownWhereColumn) {
    Catalog cat = makeEmployeesCatalog();
    SemanticAnalyzer a(cat);
    SQLParser p;
    auto stmt = p.parse("SELECT id FROM employees WHERE bar = 5");
    EXPECT_THROW(a.analyze(stmt), SemanticError);
}

TEST(SemanticTest, ValidAndPredicate) {
    Catalog cat = makeEmployeesCatalog();
    SemanticAnalyzer a(cat);
    SQLParser p;
    auto stmt = p.parse("SELECT id FROM employees WHERE id = 5 AND age > 3");
    EXPECT_NO_THROW(a.analyze(stmt));
}

// ═══════════════════════════════════════════════════════════════════════════
// PLANNER TESTS
// ═══════════════════════════════════════════════════════════════════════════

TEST(PlannerTest, LogicalPlanHasFilter) {
    Catalog cat = makeEmployeesCatalog();
    SemanticAnalyzer a(cat);
    SQLParser p;
    auto stmt = p.parse("SELECT id, salary FROM employees WHERE id = 10");
    auto& schema = a.analyze(stmt);
    auto logPlan = LogicalPlanner().plan(stmt, schema);
    EXPECT_EQ(logPlan->kind, LogicalNodeKind::Project);
    ASSERT_NE(logPlan->child, nullptr);
    EXPECT_EQ(logPlan->child->kind, LogicalNodeKind::Filter);
    ASSERT_NE(logPlan->child->child, nullptr);
    EXPECT_EQ(logPlan->child->child->kind, LogicalNodeKind::Scan);
}

TEST(PlannerTest, LogicalPlanNoFilter) {
    Catalog cat = makeEmployeesCatalog();
    SemanticAnalyzer a(cat);
    SQLParser p;
    auto stmt = p.parse("SELECT id FROM employees");
    auto& schema = a.analyze(stmt);
    auto logPlan = LogicalPlanner().plan(stmt, schema);
    EXPECT_EQ(logPlan->kind, LogicalNodeKind::Project);
    ASSERT_NE(logPlan->child, nullptr);
    EXPECT_EQ(logPlan->child->kind, LogicalNodeKind::Scan);
}

TEST(PlannerTest, PhysicalPlanColumns) {
    Catalog cat = makeEmployeesCatalog();
    SemanticAnalyzer a(cat);
    SQLParser p;
    auto stmt = p.parse("SELECT id, salary FROM employees WHERE age > 3");
    auto& schema = a.analyze(stmt);
    auto logPlan  = LogicalPlanner().plan(stmt, schema);
    auto physPlan = PhysicalPlanner().plan(*logPlan);
    EXPECT_EQ(physPlan.tableName, "employees");
    EXPECT_TRUE(physPlan.hasFilter);
    EXPECT_GE(physPlan.scanColumns.size(), 3u);
    ASSERT_EQ(physPlan.projectColumns.size(), 2u);
    EXPECT_EQ(physPlan.projectColumns[0], "id");
    EXPECT_EQ(physPlan.projectColumns[1], "salary");
}

// ═══════════════════════════════════════════════════════════════════════════
// CRYPTO / SERIALIZATION TESTS
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(FHEFixture, EncryptedLiteralNotEmpty) {
    auto ct = clientCtx_->encryptScalar(42);
    ASSERT_NE(ct, nullptr);
    EXPECT_GT(ct->serialize().size(), 0u);
}

TEST_F(FHEFixture, QueryPackageRoundTrip) {
    Catalog cat = makeEmployeesCatalog();
    SQLParser p;
    auto stmt = p.parse("SELECT id, salary FROM employees WHERE id = 5");
    SemanticAnalyzer a(cat);
    auto& schema  = a.analyze(stmt);
    auto logPlan  = LogicalPlanner().plan(stmt, schema);
    auto physPlan = PhysicalPlanner().plan(*logPlan);

    FHEParamsMeta meta;
    meta.paramSetId = clientCtx_->paramSetId();
    meta.slotCount  = clientCtx_->slotCount();
    meta.keyId      = "key-0";

    auto ctxPtr = std::make_shared<ClientCryptoContext>(clientCtx_->keyCtxPtr());
    auto pkg = QueryEncryptor(ctxPtr).encrypt(stmt, physPlan, schema, meta);
    pkg.queryId = "test-001";

    auto bytes = pkg.serialize();
    EXPECT_GT(bytes.size(), 0u);

    auto pkg2 = QueryPackage::deserialize(bytes);
    EXPECT_EQ(pkg2.queryId, "test-001");
    EXPECT_EQ(pkg2.tableName, "employees");
    EXPECT_EQ(pkg2.projectColumns.size(), pkg.projectColumns.size());
    ASSERT_EQ(pkg2.encryptedLiterals.size(), 1u);
    EXPECT_FALSE(pkg2.encryptedLiterals[0].ciphertextBytes.empty());
}

TEST_F(FHEFixture, ValidatorAcceptsValidPackage) {
    Catalog cat = makeEmployeesCatalog();
    // Use same context as server (no ReleaseAllContexts issue here)
    auto serverCtx = std::make_shared<ServerCryptoContext>(clientCtx_->keyCtxPtr());
    QueryValidator v(cat, *serverCtx);

    SQLParser p;
    auto stmt = p.parse("SELECT id, salary FROM employees WHERE id = 5");
    SemanticAnalyzer a(cat);
    auto& schema  = a.analyze(stmt);
    auto logPlan  = LogicalPlanner().plan(stmt, schema);
    auto physPlan = PhysicalPlanner().plan(*logPlan);

    FHEParamsMeta meta;
    meta.paramSetId = clientCtx_->paramSetId();
    meta.slotCount  = clientCtx_->slotCount();
    meta.keyId      = "key-0";

    auto ctxPtr = std::make_shared<ClientCryptoContext>(clientCtx_->keyCtxPtr());
    auto pkg = QueryEncryptor(ctxPtr).encrypt(stmt, physPlan, schema, meta);
    pkg.queryId = "test-002";

    EXPECT_NO_THROW(v.validate(pkg));
}

TEST_F(FHEFixture, ValidatorRejectsUnknownTable) {
    Catalog cat = makeEmployeesCatalog();
    auto serverCtx = std::make_shared<ServerCryptoContext>(clientCtx_->keyCtxPtr());
    QueryValidator v(cat, *serverCtx);

    QueryPackage pkg;
    pkg.protocolVersion = 2;
    pkg.tableName       = "nonexistent";
    pkg.fheParams.paramSetId = clientCtx_->paramSetId();

    EXPECT_THROW(v.validate(pkg), ValidationError);
}

TEST_F(FHEFixture, ValidatorRejectsWrongProtocol) {
    Catalog cat = makeEmployeesCatalog();
    auto serverCtx = std::make_shared<ServerCryptoContext>(clientCtx_->keyCtxPtr());
    QueryValidator v(cat, *serverCtx);

    QueryPackage pkg;
    pkg.protocolVersion = 99;
    pkg.tableName       = "employees";

    EXPECT_THROW(v.validate(pkg), ValidationError);
}

// ═══════════════════════════════════════════════════════════════════════════
// END-TO-END TESTS
// Dataset: id=1..10, age=id+1, salary=id+4
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(E2ETest, EqualityFilter) {
    // id = 5 → row 5: id=5, age=6, salary=9
    auto rows = ctx_.runQuery("SELECT id, salary FROM employees WHERE id = 5");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values.at("id"),     5);
    EXPECT_EQ(rows[0].values.at("salary"), 9);
}

TEST_F(E2ETest, GreaterThanFilter) {
    // age > 9 → age=10(id=9), age=11(id=10) → 2 rows
    auto rows = ctx_.runQuery("SELECT id FROM employees WHERE age > 9");
    ASSERT_EQ(rows.size(), 2u);
}

TEST_F(E2ETest, LessThanFilter) {
    // age < 4 → age=2(id=1), age=3(id=2) → 2 rows
    auto rows = ctx_.runQuery("SELECT id FROM employees WHERE age < 4");
    ASSERT_EQ(rows.size(), 2u);
}

TEST_F(E2ETest, AndFilter) {
    // id = 5 AND age > 3 → row 5: age=6 > 3 ✓ → 1 row
    auto rows = ctx_.runQuery("SELECT id, salary FROM employees WHERE id = 5 AND age > 3");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values.at("id"), 5);
}

TEST_F(E2ETest, AndFilterNoMatch) {
    // id = 5 AND age > 9 → row 5 has age=6, not > 9 → 0 rows
    auto rows = ctx_.runQuery("SELECT id FROM employees WHERE id = 5 AND age > 9");
    EXPECT_EQ(rows.size(), 0u);
}

TEST_F(E2ETest, OrFilter) {
    // id = 1 OR id = 2 → 2 rows
    auto rows = ctx_.runQuery("SELECT id FROM employees WHERE id = 1 OR id = 2");
    ASSERT_EQ(rows.size(), 2u);
}

TEST_F(E2ETest, NotFilter) {
    // NOT age > 9 → age <= 9: id=1..8 → 8 rows
    auto rows = ctx_.runQuery("SELECT id FROM employees WHERE NOT age > 9");
    ASSERT_EQ(rows.size(), 8u);
}

TEST_F(E2ETest, NoWhereClause) {
    auto rows = ctx_.runQuery("SELECT id FROM employees");
    ASSERT_EQ(rows.size(), 10u);
}

TEST_F(E2ETest, PrimaryQuery) {
    // SELECT id, salary FROM employees WHERE id = 10 AND age > 3
    // Row 10: id=10, age=11, salary=14. age=11 > 3 ✓
    auto rows = ctx_.runQuery("SELECT id, salary FROM employees WHERE id = 10 AND age > 3");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values.at("id"),     10);
    EXPECT_EQ(rows[0].values.at("salary"), 14);
}

TEST_F(E2ETest, ResultBatchSerialization) {
    // Verify EncryptedResultBatch round-trips through serialization
    Catalog cat = makeEmployeesCatalog();
    SQLParser p;
    auto stmt = p.parse("SELECT id FROM employees WHERE id = 3");
    SemanticAnalyzer a(cat);
    auto& schema  = a.analyze(stmt);
    auto logPlan  = LogicalPlanner().plan(stmt, schema);
    auto physPlan = PhysicalPlanner().plan(*logPlan);

    FHEParamsMeta meta;
    meta.paramSetId = ctx_.clientCtx->paramSetId();
    meta.slotCount  = ctx_.clientCtx->slotCount();
    meta.keyId      = "key-0";

    auto ctxPtr = std::make_shared<ClientCryptoContext>(ctx_.clientCtx->keyCtxPtr());
    auto pkg = QueryEncryptor(ctxPtr).encrypt(stmt, physPlan, schema, meta);
    pkg.queryId = "ser-test";

    auto batches = QueryExecutor(ctx_.serverCtx, ctx_.storage, cat).execute(pkg);
    ASSERT_FALSE(batches.empty());

    auto bytes = batches[0].serialize();
    EXPECT_GT(bytes.size(), 0u);
    auto batch2 = EncryptedResultBatch::deserialize(bytes);
    EXPECT_EQ(batch2.logicalRowCount, batches[0].logicalRowCount);
    EXPECT_EQ(batch2.columns.size(), batches[0].columns.size());

    ResultDecoder decoder(ctxPtr);
    auto rows = decoder.decode(batch2);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values.at("id"), 3);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
