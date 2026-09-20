#include "sextant_index.hpp"
#include "sextant_index_scan.hpp"

#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/table_index_list.hpp"

#include "sextant/sextant_c.h"

namespace duckdb {

struct SextantQueryBindData : public TableFunctionData {
	DuckTableEntry *table = nullptr;
	SextantIndex *index = nullptr;
	vector<float> query;
	uint32_t k = 10;
	bool exhaustive = false;
};

struct SextantQueryBatchBindData : public TableFunctionData {
	DuckTableEntry *table = nullptr;
	SextantIndex *index = nullptr;
	vector<float> queries; // n_queries * dim, row-major
	uint32_t n_queries = 0;
	uint32_t k = 10;
};

struct SextantQueryBatchGlobalState : public GlobalTableFunctionState {
	// Packed per-query results (query-major, n_per_query[i] rows each).
	vector<uint64_t> row_ids;
	vector<float> dists;
	vector<uint32_t> n_per_query;
	idx_t offset = 0;
};

/// Resolve '<table>, <index_name>' to the table + its sextant index
/// (shared by sextant_query and sextant_query_batch binds).
static std::pair<DuckTableEntry *, SextantIndex *> ResolveSextantTableIndex(ClientContext &context,
                                                                            const string &table_name,
                                                                            const string &index_name,
                                                                            const string &fn_name) {
	auto &entry = Catalog::GetEntry(context, INVALID_CATALOG, INVALID_SCHEMA,
	                                 EntryLookupInfo(CatalogType::TABLE_ENTRY, table_name));
	if (entry.type != CatalogType::TABLE_ENTRY) {
		throw BinderException("%s: '%s' is not a table", fn_name, table_name);
	}
	auto *table = &entry.Cast<DuckTableEntry>();
	if (!table->IsDuckTable()) {
		throw BinderException("%s: '%s' is not a DuckDB table", fn_name, table_name);
	}
	auto &storage = table->GetStorage();
	storage.BindIndexes(context);
	auto bound_index = storage.GetDataTableInfo()->GetIndexes().Find(index_name);
	if (!bound_index || bound_index->GetIndexType() != SextantIndex::TYPE_NAME) {
		throw BinderException("%s: no sextant index named '%s' on table '%s'", fn_name, index_name, table_name);
	}
	return {table, &bound_index->Cast<SextantIndex>()};
}

struct SextantQueryGlobalState : public GlobalTableFunctionState {
	vector<uint64_t> row_ids;
	vector<float> dists;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> SextantQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 4) {
		throw BinderException("sextant_query(table, index_name, query_vector, k) requires four arguments");
	}

	const auto table_name = input.inputs[0].ToString();
	const auto index_name = input.inputs[1].ToString();

	auto bind_data = make_uniq<SextantQueryBindData>();

	// Resolve table + sextant index.
	std::tie(bind_data->table, bind_data->index) =
	    ResolveSextantTableIndex(context, table_name, index_name, "sextant_query");

	// Query vector: numeric array/list literal, cast per element to FLOAT.
	const auto &query_val = input.inputs[2];
	const auto tclass = query_val.type().id();
	if (tclass != LogicalTypeId::ARRAY && tclass != LogicalTypeId::LIST) {
		throw BinderException("sextant_query: query_vector must be a numeric array literal, got %s",
		                      query_val.type().ToString());
	}
	const auto &children = tclass == LogicalTypeId::ARRAY ? ArrayValue::GetChildren(query_val)
	                                                      : ListValue::GetChildren(query_val);
	if (children.empty()) {
		throw BinderException("sextant_query: query_vector must not be empty");
	}
	for (const auto &v : children) {
		Value fv;
		string cast_err;
		if (!v.DefaultTryCastAs(LogicalType::FLOAT, fv, &cast_err)) {
			throw BinderException("sextant_query: query_vector elements must be numeric, got %s",
			                      v.type().ToString());
		}
		bind_data->query.push_back(fv.GetValue<float>());
	}
	const auto idx_dim = ArrayType::GetSize(bind_data->index->logical_types[0]);
	if (bind_data->query.size() != idx_dim) {
		throw BinderException("sextant_query: index dimension is %llu but query has %llu elements", idx_dim,
		                      bind_data->query.size());
	}

	// k.
	const idx_t k = input.inputs[3].GetValue<idx_t>();
	if (k == 0 || k > 100000) {
		throw BinderException("sextant_query: k must be in [1, 100000]");
	}
	bind_data->k = static_cast<uint32_t>(k);

	return_types = {LogicalType::BIGINT, LogicalType::FLOAT};
	names = {"row_id", "distance"};
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> SextantQueryInitGlobal(ClientContext &context,
                                                                   TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<SextantQueryBindData>();
	auto result = make_uniq<SextantQueryGlobalState>();

	auto handle = bind_data.index->GetEngineHandle();
	if (!handle) {
		// Lazily-bound index was force-bound above; attach happened at
		// create/load. If the handle is somehow gone, re-attach.
		auto [leaf_mb, plane_mb] = SextantCacheBudget(context);
		bind_data.index->AttachAndVerify(leaf_mb, plane_mb);
		handle = bind_data.index->GetEngineHandle();
		if (!handle) {
			throw InvalidInputException("sextant_query: index handle unavailable");
		}
	}

	sextant_search_opts opts = sextant_default_search_opts();
	opts.k = bind_data.k;
	// Session tuning (threads, probe fraction, W, rerank, exhaustive),
	// identical to the top-k rewrite path.
	ApplySextantSearchSettings(context, handle, opts);
	result->row_ids.resize(bind_data.k);
	result->dists.resize(bind_data.k);
	char err[512] = {0};
	const int32_t n = sextant_search(handle, bind_data.query.data(), &opts, result->row_ids.data(),
	                                 result->dists.data(), bind_data.k, err, sizeof(err));
	if (n < 0) {
		throw InvalidInputException("sextant_query: search failed: %s", err);
	}
	result->row_ids.resize(n);
	result->dists.resize(n);

	// Parity with the top-k rewrite: delta_scan indexes serve appended
	// rows (past n_build) by brute force. Note distances are recomputed
	// exactly on this path (squared-L2 / negated dot), unlike the pure
	// engine scores above.
	if (bind_data.index->GetDeltaScan()) {
		MergeDeltaRows(context, *bind_data.table, *bind_data.index, bind_data.query, bind_data.k, {},
		               result->row_ids, result->dists);
	}

	return std::move(result);
}

static void SextantQueryFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<SextantQueryBindData>();
	auto &state = data.global_state->Cast<SextantQueryGlobalState>();

	const idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.row_ids.size() - state.offset);
	if (count == 0) {
		output.SetCardinality(0);
		return;
	}
	output.SetCardinality(count);
	auto row_ids = FlatVector::GetData<int64_t>(output.data[0]);
	auto dists = FlatVector::GetData<float>(output.data[1]);
	for (idx_t i = 0; i < count; i++) {
		row_ids[i] = static_cast<int64_t>(state.row_ids[state.offset + i]);
		dists[i] = state.dists[state.offset + i];
	}
	state.offset += count;
}

static unique_ptr<NodeStatistics> SextantQueryCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<SextantQueryBindData>();
	return make_uniq<NodeStatistics>(bind_data.k, bind_data.k);
}

// -------------------------------------------------------------------------
// sextant_query_batch: many query vectors, one engine call (union probing,
// batched kernels, shared cache locality). Same serving contract as
// sextant_query — same tuning SET vars, delta_scan parity — pluralized.
// -------------------------------------------------------------------------

static unique_ptr<FunctionData> SextantQueryBatchBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 4) {
		throw BinderException(
		    "sextant_query_batch(table, index_name, query_vectors, k) requires four arguments");
	}

	auto bind_data = make_uniq<SextantQueryBatchBindData>();
	std::tie(bind_data->table, bind_data->index) = ResolveSextantTableIndex(
	    context, input.inputs[0].ToString(), input.inputs[1].ToString(), "sextant_query_batch");

	const auto idx_dim = ArrayType::GetSize(bind_data->index->logical_types[0]);

	// query_vectors: a list of numeric array/list literals.
	const auto &queries_val = input.inputs[2];
	if (queries_val.type().id() != LogicalTypeId::LIST) {
		throw BinderException("sextant_query_batch: query_vectors must be a list of numeric arrays, got %s",
		                      queries_val.type().ToString());
	}
	for (const auto &q : ListValue::GetChildren(queries_val)) {
		const auto tclass = q.type().id();
		if (tclass != LogicalTypeId::ARRAY && tclass != LogicalTypeId::LIST) {
			throw BinderException("sextant_query_batch: each query vector must be a numeric array literal, got %s",
			                      q.type().ToString());
		}
		const auto &children =
		    tclass == LogicalTypeId::ARRAY ? ArrayValue::GetChildren(q) : ListValue::GetChildren(q);
		if (children.size() != idx_dim) {
			throw BinderException("sextant_query_batch: index dimension is %llu but query %llu has %llu elements",
			                      idx_dim, bind_data->n_queries + 1, children.size());
		}
		for (const auto &v : children) {
			Value fv;
			string cast_err;
			if (!v.DefaultTryCastAs(LogicalType::FLOAT, fv, &cast_err)) {
				throw BinderException("sextant_query_batch: query vector elements must be numeric, got %s",
				                      v.type().ToString());
			}
			bind_data->queries.push_back(fv.GetValue<float>());
		}
		bind_data->n_queries++;
	}
	if (bind_data->n_queries == 0) {
		throw BinderException("sextant_query_batch: query_vectors must not be empty");
	}
	if (bind_data->n_queries > 65536) {
		throw BinderException("sextant_query_batch: at most 65536 queries per batch, got %llu",
		                      bind_data->n_queries);
	}

	const idx_t k = input.inputs[3].GetValue<idx_t>();
	if (k == 0 || k > 100000) {
		throw BinderException("sextant_query_batch: k must be in [1, 100000]");
	}
	bind_data->k = static_cast<uint32_t>(k);

	// One row per (query, result); query_index is 1-based, matching
	// DuckDB list positions in the input.
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::FLOAT};
	names = {"query_index", "row_id", "distance"};
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> SextantQueryBatchInitGlobal(ClientContext &context,
                                                                        TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<SextantQueryBatchBindData>();
	auto result = make_uniq<SextantQueryBatchGlobalState>();

	auto handle = bind_data.index->GetEngineHandle();
	if (!handle) {
		auto [leaf_mb, plane_mb] = SextantCacheBudget(context);
		bind_data.index->AttachAndVerify(leaf_mb, plane_mb);
		handle = bind_data.index->GetEngineHandle();
		if (!handle) {
			throw InvalidInputException("sextant_query_batch: index handle unavailable");
		}
	}

	const uint32_t nq = bind_data.n_queries;
	const uint32_t k = bind_data.k;
	const bool delta_scan = bind_data.index->GetDeltaScan();
	const idx_t dim = ArrayType::GetSize(bind_data.index->logical_types[0]);
	sextant_search_opts opts = sextant_default_search_opts();
	opts.k = k;
	// Session tuning, identical to sextant_query and the top-k rewrite.
	ApplySextantSearchSettings(context, handle, opts);

	// Raw packed output: nq * k entries (engine writes up to k per query).
	vector<uint64_t> raw_ids(static_cast<size_t>(nq) * k);
	vector<float> raw_dists(static_cast<size_t>(nq) * k);
	vector<uint32_t> raw_n(nq);
	char err[512] = {0};
	if (sextant_search_batch(handle, bind_data.queries.data(), nq, &opts,
	                         /*preds=*/nullptr, /*n_preds=*/0, raw_ids.data(), raw_dists.data(), k,
	                         raw_n.data(), err, sizeof(err)) != 0) {
		throw InvalidInputException("sextant_query_batch: search failed: %s", err);
	}

	// Compact to the true per-query counts; delta_scan indexes merge
	// brute-forced appended rows per query here (same as sextant_query).
	result->row_ids.reserve(static_cast<size_t>(nq) * k);
	result->dists.reserve(static_cast<size_t>(nq) * k);
	result->n_per_query.reserve(nq);
	for (uint32_t q = 0; q < nq; q++) {
		const uint32_t n = raw_n[q] > k ? k : raw_n[q];
		vector<uint64_t> q_ids(raw_ids.begin() + static_cast<size_t>(q) * k,
		                       raw_ids.begin() + static_cast<size_t>(q) * k + n);
		vector<float> q_dists(raw_dists.begin() + static_cast<size_t>(q) * k,
		                      raw_dists.begin() + static_cast<size_t>(q) * k + n);
		if (delta_scan) {
			MergeDeltaRows(context, *bind_data.table, *bind_data.index,
			                {bind_data.queries.begin() + static_cast<size_t>(q) * dim,
			                 bind_data.queries.begin() + static_cast<size_t>(q + 1) * dim},
			                k, {}, q_ids, q_dists);
		}
		result->row_ids.insert(result->row_ids.end(), q_ids.begin(), q_ids.end());
		result->dists.insert(result->dists.end(), q_dists.begin(), q_dists.end());
		result->n_per_query.push_back(static_cast<uint32_t>(q_ids.size()));
	}

	return std::move(result);
}

static void SextantQueryBatchFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<SextantQueryBatchGlobalState>();

	const idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.row_ids.size() - state.offset);
	if (count == 0) {
		output.SetCardinality(0);
		return;
	}
	output.SetCardinality(count);
	auto qidx = FlatVector::GetData<int64_t>(output.data[0]);
	auto row_ids = FlatVector::GetData<int64_t>(output.data[1]);
	auto dists = FlatVector::GetData<float>(output.data[2]);
	// Global row offset -> 1-based query index via the per-query counts.
	uint32_t q = 0;
	size_t base = 0;
	while (q + 1 < state.n_per_query.size() && base + state.n_per_query[q] <= state.offset) {
		base += state.n_per_query[q];
		q++;
	}
	for (idx_t i = 0; i < count; i++) {
		while (q + 1 < state.n_per_query.size() && base + state.n_per_query[q] <= state.offset + i) {
			base += state.n_per_query[q];
			q++;
		}
		qidx[i] = static_cast<int64_t>(q) + 1;
		row_ids[i] = static_cast<int64_t>(state.row_ids[state.offset + i]);
		dists[i] = state.dists[state.offset + i];
	}
	state.offset += count;
}

static unique_ptr<NodeStatistics> SextantQueryBatchCardinality(ClientContext &context,
                                                               const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<SextantQueryBatchBindData>();
	const idx_t total = static_cast<idx_t>(bind_data.n_queries) * bind_data.k;
	return make_uniq<NodeStatistics>(total, total);
}

void RegisterSextantScanFunction(DatabaseInstance &db) {
	TableFunction func("sextant_query", {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                                     LogicalType::ANY, LogicalType::BIGINT},
	                   SextantQueryFunction, SextantQueryBind, SextantQueryInitGlobal);
	func.cardinality = SextantQueryCardinality;
	// Debug/introspection surface. Parity with the SQL top-k rewrite:
	// L2 + IP trees, delta_scan appends. NOT supported: WHERE
	// predicates — use ORDER BY array_distance LIMIT k for filtered
	// queries.
	ExtensionLoader loader(db, "sextant");
	loader.RegisterFunction(func);

	TableFunction batch_func("sextant_query_batch", {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                                                LogicalType::ANY, LogicalType::BIGINT},
	                        SextantQueryBatchFunction, SextantQueryBatchBind, SextantQueryBatchInitGlobal);
	batch_func.cardinality = SextantQueryBatchCardinality;
	// Batched serving surface: one engine call for many queries (union
	// leaf probing + batched scan kernels). Same contract as
	// sextant_query — same tuning SET vars, delta_scan appends, no
	// predicates — pluralized: rows are (query_index 1-based, row_id,
	// distance).
	loader.RegisterFunction(batch_func);
}

} // namespace duckdb
