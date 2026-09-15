// EncryptedResultBatch serialization.

#include "proto2/result.h"

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/unordered_map.hpp>

#include <sstream>

namespace cereal {

template <class Archive>
void serialize(Archive& ar, proto2::EncryptedColumnResult& c) {
    ar(c.columnName, c.ciphertextBytes, c.logicalSize);
}

template <class Archive>
void serialize(Archive& ar, proto2::EncryptedResultBatch& b) {
    ar(b.batchId, b.logicalRowCount, b.columns,
       b.selectionMaskBytes, b.hasMask);
}

} // namespace cereal

namespace proto2 {

std::vector<uint8_t> EncryptedResultBatch::serialize() const {
    std::ostringstream oss(std::ios::binary);
    {
        cereal::BinaryOutputArchive ar(oss);
        ar(*this);
    }
    auto s = oss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

EncryptedResultBatch EncryptedResultBatch::deserialize(const std::vector<uint8_t>& bytes) {
    std::string s(bytes.begin(), bytes.end());
    std::istringstream iss(s, std::ios::binary);
    EncryptedResultBatch b;
    {
        cereal::BinaryInputArchive ar(iss);
        ar(b);
    }
    return b;
}

} // namespace proto2
