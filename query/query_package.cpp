// QueryPackage serialization using cereal binary archive.

#include "proto2/query_package.h"

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <sstream>
#include <stdexcept>

namespace cereal {

template <class Archive>
void serialize(Archive& ar, proto2::FHEParamsMeta& m) {
    ar(m.paramSetId, m.plaintextModulus, m.multiplicativeDepth,
       m.ringDimension, m.slotCount, m.keyId);
}

template <class Archive>
void serialize(Archive& ar, proto2::EncryptedLiteralEntry& e) {
    ar(e.id, e.logicalType, e.ciphertextBytes, e.logicalSize);
}

template <class Archive>
void serialize(Archive& ar, proto2::PredNode& n) {
    ar(n.kind, n.columnName, n.literalId, n.left, n.right);
}

template <class Archive>
void serialize(Archive& ar, proto2::ColumnSchema& c) {
    ar(c.name, c.type);
}

template <class Archive>
void serialize(Archive& ar, proto2::QueryPackage& p) {
    ar(p.queryId, p.protocolVersion, p.schemaVersion,
       p.tableName, p.scanColumns, p.projectColumns,
       p.resultSchema, p.predicateNodes, p.encryptedLiterals,
       p.fheParams, p.shardId, p.partitionId);
}

} // namespace cereal

namespace proto2 {

std::vector<uint8_t> QueryPackage::serialize() const {
    std::ostringstream oss(std::ios::binary);
    {
        cereal::BinaryOutputArchive ar(oss);
        ar(*this);
    }
    auto s = oss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

QueryPackage QueryPackage::deserialize(const std::vector<uint8_t>& bytes) {
    std::string s(bytes.begin(), bytes.end());
    std::istringstream iss(s, std::ios::binary);
    QueryPackage pkg;
    {
        cereal::BinaryInputArchive ar(iss);
        ar(pkg);
    }
    return pkg;
}

} // namespace proto2
