#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "sextant_index.hpp"

namespace duckdb {

struct DuckTableEntry;

/// Bind data for the internal index-scan function installed by the
/// ORDER BY array_distance LIMIT k rewrite (sextant_optimize_topk.cpp).
/// Not user-callable: the function is only ever assigned onto a
/// LogicalGet by the optimizer.
struct SextantIndexScanBindData : public TableFunctionData {
	DuckTableEntry *table = nullptr;
	SextantIndex *index = nullptr;
	vector<float> query;
	idx_t k = 10;
};

/// The index-scan table function: runs the ANN search once at
/// init-global, then streams the matching table rows (fetched by
/// rowid, in distance order) through the pipeline.
TableFunction GetSextantIndexScanFunction();

/// Registers the ORDER BY array_distance(...) LIMIT k -> index-scan
/// rewrite as an optimizer extension.
void RegisterSextantTopKOptimizer(DatabaseInstance &db);

} // namespace duckdb
