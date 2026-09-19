#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "sextant_index.hpp"

namespace duckdb {

struct DuckTableEntry;

/// One engine predicate translated from the plan's pushed-down table
/// filters (WHERE clauses). Storage is self-contained so the engine
/// sextant_predicate view can point into it for the duration of the
/// search call.
struct SextantScanPredicate {
	string column;
	int op = 0;             // SEXTANT_PRED_*
	double value = 0.0;     // numeric comparand
	string str_value;       // string comparand
	vector<string> values;  // IN list entries
};

/// Bind data for the internal index-scan function installed by the
/// ORDER BY array_distance LIMIT k rewrite (sextant_optimize_topk.cpp).
/// Not user-callable: the function is only ever assigned onto a
/// LogicalGet by the optimizer.
struct SextantIndexScanBindData : public TableFunctionData {
	DuckTableEntry *table = nullptr;
	SextantIndex *index = nullptr;
	vector<float> query;
	idx_t k = 10;
	/// Engine-side predicates for the filtered search path. Empty =
	/// plain sextant_search. An exact SQL LogicalFilter is re-injected
	/// above the scan (engine NULL handling differs from SQL), so these
	/// shape recall rather than correctness.
	vector<SextantScanPredicate> predicates;
};

/// Append-only delta serving (shared by the top-k rewrite scan and the
/// debug sextant_query() function): brute-force rows past n_build,
/// apply the predicates, merge the best k with the engine's results.
/// Distances are recomputed exactly (engine scores are family-specific).
void MergeDeltaRows(ClientContext &context, DuckTableEntry &table, SextantIndex &index,
                    const vector<float> &query, idx_t k, const vector<SextantScanPredicate> &predicates,
                    vector<uint64_t> &row_ids, vector<float> &dists);

/// The index-scan table function: runs the ANN search once at
/// init-global, then streams the matching table rows (fetched by
/// rowid, in distance order) through the pipeline.
TableFunction GetSextantIndexScanFunction();

/// Registers the ORDER BY array_distance(...) LIMIT k -> index-scan
/// rewrite as an optimizer extension.
void RegisterSextantTopKOptimizer(DatabaseInstance &db);

} // namespace duckdb
