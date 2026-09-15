// prototype2_server — FHE SQL query server.
//
// Usage: ./prototype2_server [--port PORT] [--data-dir DIR] [--keys-dir DIR]
//
// The server:
//   1. Loads server-side FHE keys (public + eval keys, NO secret key).
//   2. Loads encrypted database from storage.
//   3. Listens for query packages.
//   4. Validates each package.
//   5. Executes encrypted queries using OpenFHE.
//   6. Streams encrypted result batches back to the client.
//
// The server NEVER decrypts data. The secret key is NOT present.

#include "proto2/catalog.h"
#include "proto2/crypto_context.h"
#include "proto2/query_executor.h"
#include "proto2/query_validator.h"
#include "proto2/transport.h"
#include "proto1/storage.h"
#include "proto1/fhe_runtime.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace proto2;

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " [--port PORT] [--data-dir DIR] [--keys-dir DIR]\n"
              << "  --port     TCP port to listen on (default: 7777)\n"
              << "  --data-dir Encrypted database directory (default: ./server_data)\n"
              << "  --keys-dir Server FHE keys directory (default: ./server_keys)\n";
}

int main(int argc, char* argv[]) {
    uint16_t    port     = 7777;
    std::string dataDir  = "./server_data";
    std::string keysDir  = "./server_keys";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port"     && i+1 < argc) { port    = (uint16_t)std::stoi(argv[++i]); }
        else if (arg == "--data-dir" && i+1 < argc) { dataDir = argv[++i]; }
        else if (arg == "--keys-dir" && i+1 < argc) { keysDir = argv[++i]; }
        else if (arg == "--help") { printUsage(argv[0]); return 0; }
        else { std::cerr << "Unknown argument: " << arg << "\n"; printUsage(argv[0]); return 1; }
    }

    std::cout << "╔══════════════════════════════════════════════════════╗\n"
              << "║  Prototype 2 — FHE SQL Server                       ║\n"
              << "╚══════════════════════════════════════════════════════╝\n\n";

    // ── Load server FHE context (NO secret key) ───────────────────────────
    std::cout << "[Server] Loading FHE keys from: " << keysDir << "\n";
    if (!fs::exists(keysDir)) {
        std::cerr << "[Server] ERROR: Keys directory not found: " << keysDir << "\n"
                  << "         Run the client with --setup first to generate keys.\n";
        return 1;
    }

    std::shared_ptr<ServerCryptoContext> serverCtx;
    try {
        serverCtx = ServerCryptoContext::load(keysDir);
        std::cout << "[Server] FHE context loaded: paramSetId=" << serverCtx->paramSetId()
                  << " slotCount=" << serverCtx->slotCount() << "\n";
    } catch (std::exception& e) {
        std::cerr << "[Server] ERROR loading FHE context: " << e.what() << "\n";
        return 1;
    }

    // ── Load encrypted storage ────────────────────────────────────────────
    std::cout << "[Server] Loading encrypted storage from: " << dataDir << "\n";
    if (!fs::exists(dataDir)) {
        std::cerr << "[Server] ERROR: Data directory not found: " << dataDir << "\n"
                  << "         Run the client with --setup first to ingest data.\n";
        return 1;
    }

    auto storage = proto1::makeFilesystemStorage(dataDir, serverCtx->keyCtxPtr());
    std::cout << "[Server] Storage loaded\n";

    // ── Build catalog ─────────────────────────────────────────────────────
    Catalog catalog = makeEmployeesCatalog();
    std::cout << "[Server] Catalog ready\n\n";

    // ── Build query handler ───────────────────────────────────────────────
    QueryValidator validator(catalog, *serverCtx);
    QueryExecutor  executor(serverCtx, storage, catalog);

    auto handler = [&](const QueryPackage& pkg) -> std::vector<EncryptedResultBatch> {
        auto t0 = std::chrono::high_resolution_clock::now();

        // Validate
        std::cout << "[Server] Validating query: " << pkg.queryId << "\n" << std::flush;
        validator.validate(pkg);
        std::cout << "[Server] Validation OK\n" << std::flush;

        // Execute (all FHE, no decryption)
        std::cout << "[Server] Executing encrypted query...\n" << std::flush;
        auto batches = executor.execute(pkg);

        auto t1 = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        size_t totalBytes = 0;
        for (auto& b : batches) {
            for (auto& c : b.columns) totalBytes += c.ciphertextBytes.size();
            totalBytes += b.selectionMaskBytes.size();
        }

        std::cout << "[Server] Execution complete: " << batches.size()
                  << " batch(es), " << totalBytes << " ciphertext bytes, "
                  << ms << " ms\n" << std::flush;

        return batches;
    };

    // ── Start server ──────────────────────────────────────────────────────
    TcpServerTransport server(port, handler);
    std::cout << "[Server] Started\n";
    server.serve(); // blocks

    return 0;
}
