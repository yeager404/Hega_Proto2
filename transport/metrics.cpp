// Metrics printing.

#include "proto2/metrics.h"
#include <iostream>
#include <iomanip>

namespace proto2 {

void ClientMetrics::print() const {
    std::cout << "\n[Client Metrics]\n";
    std::cout << "  Parse:              " << std::setw(6) << parseMs      << " ms\n";
    std::cout << "  Semantic analysis:  " << std::setw(6) << semanticMs   << " ms\n";
    std::cout << "  Planning:           " << std::setw(6) << planMs       << " ms\n";
    std::cout << "  Literal encryption: " << std::setw(6) << encryptMs    << " ms\n";
    std::cout << "  Serialization:      " << std::setw(6) << serializeMs  << " ms\n";
    std::cout << "  Network upload:     " << std::setw(6) << networkUpMs  << " ms\n";
    std::cout << "  Network download:   " << std::setw(6) << networkDownMs<< " ms\n";
    std::cout << "  Decryption:         " << std::setw(6) << decryptMs    << " ms\n";
    std::cout << "  Reconstruction:     " << std::setw(6) << reconstructMs<< " ms\n";
    std::cout << "  Total:              " << std::setw(6) << totalMs      << " ms\n";
    std::cout << "  Query package:      " << queryPackageBytes << " bytes\n";
    std::cout << "  Result received:    " << resultBytesReceived << " bytes\n";
    std::cout << "  Result batches:     " << batchCount << "\n";
}

void ServerMetrics::print() const {
    std::cout << "\n[Server Metrics]\n";
    std::cout << "  Validation:         " << std::setw(6) << validationMs  << " ms\n";
    std::cout << "  Storage read+exec:  " << std::setw(6) << fheExecMs     << " ms\n";
    std::cout << "  Serialization:      " << std::setw(6) << serializeMs   << " ms\n";
    std::cout << "  Result produced:    " << resultBytesProduced << " bytes\n";
}

} // namespace proto2
