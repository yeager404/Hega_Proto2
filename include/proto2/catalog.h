#pragma once
// Catalog: table/column schema registry.
// Reuses proto1::DataType and ColumnSchema concepts but lives in proto2 namespace.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace proto2 {

enum class DataType { INT64, BOOL, STRING };

inline std::string dataTypeName(DataType t) {
    switch (t) {
        case DataType::INT64: return "INT64";
        case DataType::BOOL:  return "BOOL";
        case DataType::STRING: return "STRING";
    }
    return "UNKNOWN";
}

struct ColumnSchema {
    std::string name;
    DataType    type;
    size_t      maxLength{0};
};

struct TableSchema {
    std::string               tableId;
    std::vector<ColumnSchema> columns;

    bool hasColumn(const std::string& name) const {
        for (auto& c : columns) if (c.name == name) return true;
        return false;
    }

    const ColumnSchema& column(const std::string& name) const {
        for (auto& c : columns) if (c.name == name) return c;
        throw std::runtime_error("Column not found: " + name);
    }
};

class Catalog {
public:
    void registerTable(TableSchema schema) {
        tables_[schema.tableId] = std::move(schema);
    }

    bool hasTable(const std::string& id) const {
        return tables_.count(id) > 0;
    }

    const TableSchema& getTable(const std::string& id) const {
        auto it = tables_.find(id);
        if (it == tables_.end())
            throw std::runtime_error("Table not found: " + id);
        return it->second;
    }

    bool hasColumn(const std::string& table, const std::string& column) const {
        return hasTable(table) && getTable(table).hasColumn(column);
    }

    // Schema version — increment when tables change.
    uint32_t version() const { return version_; }
    void bumpVersion()       { ++version_; }

private:
    std::unordered_map<std::string, TableSchema> tables_;
    uint32_t version_{1};
};

// Build the default employees catalog used by both client and server.
Catalog makeEmployeesCatalog();

} // namespace proto2
