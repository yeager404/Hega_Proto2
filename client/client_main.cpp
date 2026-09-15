// prototype2_client — FHE SQL terminal client.
//
// Usage:
//   ./prototype2_client [--host HOST] [--port PORT]
//                       [--keys-dir DIR] [--data-dir DIR]
//                       [--setup]
//
// --setup: generate FHE keys, encrypt the employees dataset, save to disk.
//          Must be run before starting the server.
//
// The secret key NEVER leaves the client.

#include "proto2/ast.h"
#include "proto2/catalog.h"
#include "proto2/crypto_context.h"
#include "proto2/metrics.h"
#include "proto2/parser.h"
#include "proto2/planner.h"
#include "proto2/query_encryptor.h"
#include "proto2/query_package.h"
#include "proto2/result_decoder.h"
#include "proto2/semantic.h"
#include "proto2/transport.h"
#include "proto1/fhe_runtime.h"
#include "proto1/ingestion.h"
#include "proto1/storage.h"
#include "proto1/catalog.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace proto2;
using Clock = std::chrono::high_resolution_clock;

// ── Dataset ───────────────────────────────────────────────────────────────
static const char* EMPLOYEES_CSV =
    "id,age,salary,department_id\n"
    "1,2,5,1\n"
    "2,3,6,1\n"
    "3,4,7,2\n"
    "4,5,8,2\n"
    "5,6,9,3\n"
    "6,7,10,3\n"
    "7,8,11,4\n"
    "8,9,12,4\n"
    "9,10,13,5\n"
    "10,11,14,5\n";

// ── Setup ─────────────────────────────────────────────────────────────────
static void runSetup(const std::string& keysDir, const std::string& dataDir) {
    std::cout << "╔══════════════════════════════════════════════════════╗\n"
              << "║  Prototype 2 — Setup                                ║\n"
              << "╚══════════════════════════════════════════════════════╝\n\n";

    proto1::FHEParameterSet params;
    params.id                  = "bfv-257-depth9";
    params.plaintextModulus    = 257;
    params.multiplicativeDepth = 9;
    params.ringDimension       = 128;

    std::cout << "[Setup] Generating FHE keys (BFV p=257 depth=9 N=128)...\n";
    auto t0 = Clock::now();
    auto clientCtx = ClientCryptoContext::create(params);
    auto t1 = Clock::now();
    std::cout << "[Setup] Key generation: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count()
              << " ms\n";
    std::cout << "[Setup] Slot count: " << clientCtx->slotCount() << "\n";

    fs::create_directories(keysDir);
    clientCtx->saveKeys(keysDir);
    std::cout << "[Setup] Server keys saved to: " << keysDir << "\n";

    auto tmpCsv = fs::temp_directory_path() / "proto2_employees.csv";
    { std::ofstream f(tmpCsv); f << EMPLOYEES_CSV; }

    proto1::TableSchema schema{"employees",
        {{"id",            proto1::DataType::INT64},
         {"age",           proto1::DataType::INT64},
         {"salary",        proto1::DataType::INT64},
         {"department_id", proto1::DataType::INT64}}};

    fs::create_directories(dataDir);
    auto storage = proto1::makeFilesystemStorage(dataDir, clientCtx->keyCtxPtr());

    std::cout << "[Setup] Encrypting employees dataset...\n";
    auto t2 = Clock::now();
    proto1::ingestCSV(tmpCsv.string(), schema, 10, clientCtx->keyCtx(), *storage);
    auto t3 = Clock::now();
    std::cout << "[Setup] Encryption + storage: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t3-t2).count()
              << " ms\n";

    fs::remove(tmpCsv);

    std::cout << "\n[Setup] Complete.\n"
              << "  Keys: " << keysDir << "\n"
              << "  Data: " << dataDir << "\n\n"
              << "  Start server:  ./prototype2_server --keys-dir " << keysDir
              << " --data-dir " << dataDir << "\n"
              << "  Start client:  ./prototype2_client --keys-dir " << keysDir << "\n\n";
}

// ── Query ID ──────────────────────────────────────────────────────────────
static std::string makeQueryId() {
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(8) << dist(rng) << std::setw(8) << dist(rng);
    return oss.str();
}

// ── Result printer ────────────────────────────────────────────────────────
static void printResult(const std::vector<std::string>& cols,
                         const std::vector<ResultRow>& rows) {
    if (rows.empty()) { std::cout << "(0 rows)\n"; return; }

    std::vector<size_t> widths;
    for (auto& c : cols) widths.push_back(c.size());
    for (auto& row : rows)
        for (size_t i = 0; i < cols.size(); ++i) {
            auto it = row.values.find(cols[i]);
            if (it != row.values.end())
                widths[i] = std::max(widths[i], std::to_string(it->second).size());
        }

    for (size_t i = 0; i < cols.size(); ++i) {
        if (i) std::cout << " | ";
        std::cout << std::setw((int)widths[i]) << std::left << cols[i];
    }
    std::cout << "\n";
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i) std::cout << "-+-";
        std::cout << std::string(widths[i], '-');
    }
    std::cout << "\n";
    for (auto& row : rows) {
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) std::cout << " | ";
            auto it = row.values.find(cols[i]);
            std::string val = (it != row.values.end()) ? std::to_string(it->second) : "NULL";
            std::cout << std::setw((int)widths[i]) << std::left << val;
        }
        std::cout << "\n";
    }
    std::cout << "(" << rows.size() << " row" << (rows.size()==1?"":"s") << ")\n";
}

// ── Plaintext reference ───────────────────────────────────────────────────
struct PlainRow { int64_t id, age, salary, department_id; };
static const PlainRow PLAIN_DATA[10] = {
    {1,2,5,1},{2,3,6,1},{3,4,7,2},{4,5,8,2},{5,6,9,3},
    {6,7,10,3},{7,8,11,4},{8,9,12,4},{9,10,13,5},{10,11,14,5}
};

static int64_t getField(const PlainRow& r, const std::string& col) {
    if (col=="id")            return r.id;
    if (col=="age")           return r.age;
    if (col=="salary")        return r.salary;
    if (col=="department_id") return r.department_id;
    throw std::runtime_error("Unknown column: "+col);
}

static bool evalPlain(const ASTExpr& e, const PlainRow& r) {
    switch (e.kind) {
        case ASTExprKind::Equal:        return getField(r,e.left->columnName)==e.right->intValue;
        case ASTExprKind::NotEqual:     return getField(r,e.left->columnName)!=e.right->intValue;
        case ASTExprKind::GreaterThan:  return getField(r,e.left->columnName)> e.right->intValue;
        case ASTExprKind::LessThan:     return getField(r,e.left->columnName)< e.right->intValue;
        case ASTExprKind::GreaterEqual: return getField(r,e.left->columnName)>=e.right->intValue;
        case ASTExprKind::LessEqual:    return getField(r,e.left->columnName)<=e.right->intValue;
        case ASTExprKind::And:  return evalPlain(*e.left,r) && evalPlain(*e.right,r);
        case ASTExprKind::Or:   return evalPlain(*e.left,r) || evalPlain(*e.right,r);
        case ASTExprKind::Not:  return !evalPlain(*e.left,r);
        default: throw std::runtime_error("evalPlain: unsupported kind");
    }
}

static std::vector<ResultRow> executePlaintext(const SelectStatement& stmt) {
    std::vector<ResultRow> result;
    for (auto& row : PLAIN_DATA) {
        if (stmt.whereClause && !evalPlain(*stmt.whereClause, row)) continue;
        ResultRow r;
        if (stmt.projections.empty()) {
            r.values["id"]=row.id; r.values["age"]=row.age;
            r.values["salary"]=row.salary; r.values["department_id"]=row.department_id;
        } else {
            for (auto& c : stmt.projections) r.values[c]=getField(row,c);
        }
        result.push_back(std::move(r));
    }
    return result;
}

// ── Main query pipeline ───────────────────────────────────────────────────
static void executeQuery(const std::string& sql,
                          ClientCryptoContext& clientCtx,
                          QueryTransport& transport,
                          const Catalog& catalog) {
    ClientMetrics metrics;
    auto tTotal = Clock::now();

    // Parse
    auto t0 = Clock::now();
    SelectStatement stmt;
    try { SQLParser p; stmt = p.parse(sql); }
    catch (ParseError& e) { std::cout << "Error: " << e.what() << "\n"; return; }
    metrics.parseMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-t0).count();

    // Semantic analysis
    t0 = Clock::now();
    const TableSchema* schema = nullptr;
    try { SemanticAnalyzer a(catalog); schema = &a.analyze(stmt); }
    catch (SemanticError& e) { std::cout << "Error: " << e.what() << "\n"; return; }
    metrics.semanticMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-t0).count();

    // Plan
    t0 = Clock::now();
    auto logPlan  = LogicalPlanner().plan(stmt, *schema);
    auto physPlan = PhysicalPlanner().plan(*logPlan);
    metrics.planMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-t0).count();

    // Encrypt literals
    t0 = Clock::now();
    FHEParamsMeta fheMeta;
    fheMeta.paramSetId = clientCtx.paramSetId();
    fheMeta.slotCount  = clientCtx.slotCount();
    fheMeta.keyId      = "key-0";

    auto ctxPtr = std::make_shared<ClientCryptoContext>(clientCtx.keyCtxPtr());
    QueryEncryptor enc(ctxPtr);
    auto pkg = enc.encrypt(stmt, physPlan, *schema, fheMeta);
    pkg.queryId = makeQueryId();
    metrics.encryptMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-t0).count();

    // Serialize
    t0 = Clock::now();
    auto pkgBytes = pkg.serialize();
    metrics.serializeMs     = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-t0).count();
    metrics.queryPackageBytes = pkgBytes.size();

    std::cout << "\n[Query ID] " << pkg.queryId << "\n"
              << "[Plan]     table=" << physPlan.tableName
              << " scan=" << physPlan.scanColumns.size()
              << " project=" << physPlan.projectColumns.size()
              << " filter=" << (physPlan.hasFilter?"yes":"no") << "\n"
              << "[Literals] " << pkg.encryptedLiterals.size() << " encrypted\n"
              << "[Package]  " << pkgBytes.size() << " bytes\n";

    // Send to server
    std::vector<EncryptedResultBatch> batches;
    t0 = Clock::now();
    QueryResponse resp;
    try { resp = transport.sendQuery(pkg, batches); }
    catch (std::exception& e) { std::cout << "Error: Network: " << e.what() << "\n"; return; }
    auto netMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-t0).count();
    metrics.networkUpMs = metrics.networkDownMs = netMs / 2;

    if (!resp.success) { std::cout << "Error: Server: " << resp.errorMessage << "\n"; return; }

    metrics.batchCount = resp.batchCount;
    for (auto& b : batches) {
        for (auto& c : b.columns) metrics.resultBytesReceived += c.ciphertextBytes.size();
        metrics.resultBytesReceived += b.selectionMaskBytes.size();
    }

    // Decrypt
    t0 = Clock::now();
    ResultDecoder decoder(ctxPtr);
    std::vector<ResultRow> allRows;
    for (auto& batch : batches) {
        auto rows = decoder.decode(batch);
        allRows.insert(allRows.end(), rows.begin(), rows.end());
    }
    metrics.decryptMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-t0).count();

    metrics.totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-tTotal).count();

    std::cout << "\n";
    printResult(physPlan.projectColumns, allRows);

    // Plaintext verification
    auto plainRows = executePlaintext(stmt);
    bool match = (allRows.size() == plainRows.size());
    if (match) {
        for (size_t i = 0; i < allRows.size() && match; ++i)
            for (auto& col : physPlan.projectColumns) {
                auto it1 = allRows[i].values.find(col);
                auto it2 = plainRows[i].values.find(col);
                if (it1==allRows[i].values.end() || it2==plainRows[i].values.end() ||
                    it1->second != it2->second) { match=false; break; }
            }
    }
    std::cout << "\n[Verification] " << (match?"✓ PASS":"✗ FAIL")
              << " — FHE result " << (match?"matches":"does NOT match")
              << " plaintext reference\n";

    metrics.print();
}

// ── main ──────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::string host    = "127.0.0.1";
    uint16_t    port    = 7777;
    std::string keysDir = "./server_keys";
    std::string dataDir = "./server_data";
    bool        setup   = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg=="--host"     && i+1<argc) host    = argv[++i];
        else if (arg=="--port"     && i+1<argc) port    = (uint16_t)std::stoi(argv[++i]);
        else if (arg=="--keys-dir" && i+1<argc) keysDir = argv[++i];
        else if (arg=="--data-dir" && i+1<argc) dataDir = argv[++i];
        else if (arg=="--setup")                setup   = true;
        else if (arg=="--help") {
            std::cout << "Usage: " << argv[0]
                      << " [--host H] [--port P] [--keys-dir D] [--data-dir D] [--setup]\n";
            return 0;
        }
    }

    if (setup) { runSetup(keysDir, dataDir); return 0; }

    std::cout << "╔══════════════════════════════════════════════════════╗\n"
              << "║  Prototype 2 — FHE SQL Client                       ║\n"
              << "╚══════════════════════════════════════════════════════╝\n\n";

    if (!fs::exists(keysDir)) {
        std::cerr << "Error: Keys directory not found: " << keysDir << "\n"
                  << "       Run with --setup first.\n";
        return 1;
    }

    std::shared_ptr<ClientCryptoContext> clientCtx;
    try {
        clientCtx = ClientCryptoContext::load(keysDir);
        std::cout << "[Client] FHE context loaded: paramSetId=" << clientCtx->paramSetId()
                  << " slotCount=" << clientCtx->slotCount() << "\n";
    } catch (std::exception& e) {
        std::cerr << "Error loading FHE context: " << e.what() << "\n";
        return 1;
    }

    Catalog catalog = makeEmployeesCatalog();
    TcpClientTransport transport(host, port);

    std::cout << "[Client] Server: " << host << ":" << port << "\n"
              << "[Client] Type SQL queries. 'exit' or Ctrl-D to quit.\n\n";

    std::string line, sql;
    while (true) {
        std::cout << (sql.empty() ? "SQL> " : "  -> ") << std::flush;
        if (!std::getline(std::cin, line)) break;

        while (!line.empty() && std::isspace((unsigned char)line.back())) line.pop_back();
        if (line=="exit"||line=="quit") break;
        if (line.empty()) continue;

        sql += (sql.empty() ? "" : " ") + line;
        bool hasSemi = (sql.find(';') != std::string::npos);

        try {
            SQLParser testParser;
            testParser.parse(sql);
            executeQuery(sql, *clientCtx, transport, catalog);
            sql.clear();
        } catch (ParseError& e) {
            std::string msg = e.what();
            if (msg.find("Unexpected end") != std::string::npos && !hasSemi) {
                // incomplete — keep reading
            } else {
                std::cout << "Error: " << msg << "\n";
                sql.clear();
            }
        }
    }

    std::cout << "\nGoodbye.\n";
    return 0;
}
