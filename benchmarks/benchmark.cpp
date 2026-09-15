// Prototype 2 — end-to-end benchmark.
// Measures each pipeline stage separately.

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

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace fs = std::filesystem;
using namespace proto2;
using Clock = std::chrono::high_resolution_clock;

static long ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(b-a).count();
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════╗\n"
              << "║  Prototype 2 — End-to-End Benchmark                 ║\n"
              << "╚══════════════════════════════════════════════════════╝\n\n";

    auto tmpDir = fs::temp_directory_path() / "proto2_bench";
    fs::remove_all(tmpDir);
    fs::create_directories(tmpDir);
    auto keysDir = (tmpDir / "keys").string();
    auto dataDir = (tmpDir / "data").string();

    // ── Key generation ────────────────────────────────────────────────────
    proto1::FHEParameterSet params;
    params.id                  = "bfv-257-depth9";
    params.plaintextModulus    = 257;
    params.multiplicativeDepth = 9;
    params.ringDimension       = 128;

    std::cout << "Stage 1: Key generation\n";
    auto t0 = Clock::now();
    auto clientCtx = ClientCryptoContext::create(params);
    auto t1 = Clock::now();
    std::cout << "  " << ms(t0,t1) << " ms\n\n";

    fs::create_directories(keysDir);
    clientCtx->saveKeys(keysDir);

    // ── Data ingestion ────────────────────────────────────────────────────
    static const char* CSV =
        "id,age,salary,department_id\n"
        "1,2,5,1\n2,3,6,1\n3,4,7,2\n4,5,8,2\n5,6,9,3\n"
        "6,7,10,3\n7,8,11,4\n8,9,12,4\n9,10,13,5\n10,11,14,5\n";

    auto tmpCsv = tmpDir / "emp.csv";
    { std::ofstream f(tmpCsv); f << CSV; }

    proto1::TableSchema schema{"employees",
        {{"id",proto1::DataType::INT64},{"age",proto1::DataType::INT64},
         {"salary",proto1::DataType::INT64},{"department_id",proto1::DataType::INT64}}};

    std::cout << "Stage 2: Data encryption + storage\n";
    fs::create_directories(dataDir);
    auto storage = proto1::makeFilesystemStorage(dataDir, clientCtx->keyCtxPtr());
    t0 = Clock::now();
    proto1::ingestCSV(tmpCsv.string(), schema, 10, clientCtx->keyCtx(), *storage);
    t1 = Clock::now();
    std::cout << "  " << ms(t0,t1) << " ms\n\n";

    // ── Client pipeline ───────────────────────────────────────────────────
    const std::string SQL = "SELECT id, salary FROM employees WHERE id = 10 AND age > 3";
    std::cout << "Query: " << SQL << "\n\n";

    Catalog catalog = makeEmployeesCatalog();

    std::cout << "Stage 3: Parse\n";
    t0 = Clock::now();
    SQLParser parser;
    auto stmt = parser.parse(SQL);
    t1 = Clock::now();
    std::cout << "  " << ms(t0,t1) << " ms\n\n";

    std::cout << "Stage 4: Semantic analysis\n";
    t0 = Clock::now();
    SemanticAnalyzer analyzer(catalog);
    auto& sch = analyzer.analyze(stmt);
    t1 = Clock::now();
    std::cout << "  " << ms(t0,t1) << " ms\n\n";

    std::cout << "Stage 5: Planning\n";
    t0 = Clock::now();
    auto logPlan  = LogicalPlanner().plan(stmt, sch);
    auto physPlan = PhysicalPlanner().plan(*logPlan);
    t1 = Clock::now();
    std::cout << "  " << ms(t0,t1) << " ms\n\n";

    std::cout << "Stage 6: Literal encryption\n";
    FHEParamsMeta meta;
    meta.paramSetId = clientCtx->paramSetId();
    meta.slotCount  = clientCtx->slotCount();
    meta.keyId      = "key-0";
    auto ctxPtr = std::make_shared<ClientCryptoContext>(clientCtx->keyCtxPtr());
    t0 = Clock::now();
    auto pkg = QueryEncryptor(ctxPtr).encrypt(stmt, physPlan, sch, meta);
    pkg.queryId = "bench-001";
    t1 = Clock::now();
    std::cout << "  " << ms(t0,t1) << " ms  (" << pkg.encryptedLiterals.size() << " literals)\n\n";

    std::cout << "Stage 7: Query package serialization\n";
    t0 = Clock::now();
    auto pkgBytes = pkg.serialize();
    t1 = Clock::now();
    std::cout << "  " << ms(t0,t1) << " ms  (" << pkgBytes.size() << " bytes)\n\n";

    // ── Server pipeline ───────────────────────────────────────────────────
    auto serverCtx = ServerCryptoContext::load(keysDir);
    auto serverStorage = proto1::makeFilesystemStorage(dataDir, serverCtx->keyCtxPtr());

    std::cout << "Stage 8: Query validation (server)\n";
    QueryValidator validator(catalog, *serverCtx);
    t0 = Clock::now();
    validator.validate(pkg);
    t1 = Clock::now();
    std::cout << "  " << ms(t0,t1) << " ms\n\n";

    std::cout << "Stage 9: Encrypted execution (server)\n";
    QueryExecutor executor(serverCtx, serverStorage, catalog);
    t0 = Clock::now();
    auto batches = executor.execute(pkg);
    t1 = Clock::now();
    long execMs = ms(t0,t1);
    std::cout << "  " << execMs << " ms  (" << batches.size() << " batch(es))\n\n";

    std::cout << "Stage 10: Result serialization (server)\n";
    size_t totalResultBytes = 0;
    t0 = Clock::now();
    std::vector<std::vector<uint8_t>> serializedBatches;
    for (auto& b : batches) {
        auto bBytes = b.serialize();
        totalResultBytes += bBytes.size();
        serializedBatches.push_back(std::move(bBytes));
    }
    t1 = Clock::now();
    std::cout << "  " << ms(t0,t1) << " ms  (" << totalResultBytes << " bytes)\n\n";

    // ── Client decryption ─────────────────────────────────────────────────
    std::cout << "Stage 11: Result decryption + reconstruction (client)\n";
    ResultDecoder decoder(ctxPtr);
    t0 = Clock::now();
    std::vector<ResultRow> allRows;
    for (auto& bBytes : serializedBatches) {
        auto b = EncryptedResultBatch::deserialize(bBytes);
        auto rows = decoder.decode(b);
        allRows.insert(allRows.end(), rows.begin(), rows.end());
    }
    t1 = Clock::now();
    std::cout << "  " << ms(t0,t1) << " ms  (" << allRows.size() << " row(s))\n\n";

    // ── Summary ───────────────────────────────────────────────────────────
    std::cout << "Result:\n";
    for (auto& row : allRows) {
        std::cout << "  id=" << row.values.at("id")
                  << " salary=" << row.values.at("salary") << "\n";
    }

    std::cout << "\nStorage metrics (id column):\n";
    auto chunks = serverStorage->listChunks("employees", "id");
    if (!chunks.empty()) {
        auto chunk = serverStorage->getChunk("employees", "id", 0, 0);
        auto ctBytes = chunk.ciphertexts[0]->serialize().size();
        std::cout << "  Plaintext:   " << 10*sizeof(int64_t) << " bytes\n";
        std::cout << "  Ciphertext:  " << ctBytes << " bytes\n";
        std::cout << "  Expansion:   " << std::fixed << std::setprecision(1)
                  << (double)ctBytes / (10*sizeof(int64_t)) << "x\n";
    }

    fs::remove_all(tmpDir);
    return 0;
}
