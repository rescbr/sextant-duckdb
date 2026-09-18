#pragma once

namespace duckdb {

/// Register the sextant_query(table, index_name, query FLOAT[d], k) table
/// function: unfiltered ANN scan returning (row_id, distance), ascending.
void RegisterSextantScanFunction(DatabaseInstance &db);

} // namespace duckdb
