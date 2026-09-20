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
	auto &entry = Catalog::GetEntry(context, INVALID_CATALOG, INVALID_SCHEMA,
	                                 EntryLookupInfo(CatalogType::TABLE_ENTRY, table_name));
	if (entry.type != CatalogType::TABLE_ENTRY) {
		throw BinderException("sextant_query: '%s' is not a table", table_name);
	}
	bind_data->table = &entry.Cast<DuckTableEntry>();
	if (!bind_data->table->IsDuckTable()) {
		throw BinderException("sextant_query: '%s' is not a DuckDB table", table_name);
	}
	auto &storage = bind_data->table->GetStorage();
	storage.BindIndexes(context);
	auto bound_index = storage.GetDataTableInfo()->GetIndexes().Find(index_name);
	if (!bound_index || bound_index->GetIndexType() != SextantIndex::TYPE_NAME) {
		throw BinderException("sextant_query: no sextant index named '%s' on table '%s'", index_name, table_name);
	}
	bind_data->index = &bound_index->Cast<SextantIndex>();

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
		bind_data.index->AttachAndVerify();
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
}

} // namespace duckdb
