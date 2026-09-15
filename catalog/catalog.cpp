// Catalog implementation.

#include "proto2/catalog.h"

namespace proto2 {

Catalog makeEmployeesCatalog() {
    Catalog cat;
    TableSchema employees;
    employees.tableId = "employees";
    employees.columns = {
        {"id",            DataType::INT64},
        {"age",           DataType::INT64},
        {"salary",        DataType::INT64},
        {"department_id", DataType::INT64},
    };
    cat.registerTable(std::move(employees));
    return cat;
}

} // namespace proto2
