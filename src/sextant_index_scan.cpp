#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "sextant/sextant_c.h"
#include "sextant_index.hpp"
#include "sextant_index_scan.hpp"

namespace duckdb {

//-------------------------------------------------------------------------
// Internal index scan: emitted rows come back in ANN distance order.
//-------------------------------------------------------------------------

struct SextantIndexScanGlobalState : public GlobalTableFunctionState {
	ColumnFetchState fetch_state;
	vector<uint64_t> row_ids;
	idx_t offset = 0;
	vector<StorageIndex> column_ids;

	// k rows total: parallel fetching would interleave chunks and break
	// the distance order the rewritten plan (TopN removed) relies on.
	idx_t MaxThreads() const override {
		return 1;
	}
};

static BindInfo SextantIndexScanBindInfo(const optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<SextantIndexScanBindData>();
	return BindInfo(*bind_data.table);
}

static unique_ptr<GlobalTableFunctionState> SextantIndexScanInitGlobal(ClientContext &context,
                                                                       TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<SextantIndexScanBindData>();
	auto result = make_uniq<SextantIndexScanGlobalState>();

	auto handle = bind_data.index->GetEngineHandle();
	if (!handle) {
		bind_data.index->AttachAndVerify();
		handle = bind_data.index->GetEngineHandle();
		if (!handle) {
			throw InvalidInputException("Sextant index '%s': index handle unavailable",
			                            bind_data.index->GetIndexName());
		}
	}

	// Same serving options as the explicit sextant_query() function:
	// session within-query threads, adaptive-W tau at CLI AUTO parity.
	sextant_search_opts opts = sextant_default_search_opts();
	opts.k = static_cast<uint32_t>(bind_data.k);
	Value st;
	if (context.TryGetCurrentSetting("sextant_search_threads", st)) {
		const int64_t n = st.GetValue<int64_t>();
		if (n < 0 || n > 1024) {
			throw InvalidInputException("sextant_search_threads must be in [0, 1024]");
		}
		opts.search_threads = static_cast<uint32_t>(n);
	}
	const uint32_t code_size = sextant_index_code_size(handle);
	opts.adaptive_w_gap = code_size >= 288 ? 2.5f : 5.0f;

	result->row_ids.resize(bind_data.k);
	vector<float> dists(bind_data.k);
	char err[512] = {0};
	const int32_t n = sextant_search(handle, bind_data.query.data(), &opts, result->row_ids.data(),
	                                 dists.data(), static_cast<uint32_t>(bind_data.k), err, sizeof(err));
	if (n < 0) {
		throw InvalidInputException("Sextant index '%s': search failed: %s", bind_data.index->GetIndexName(),
		                            err);
	}
	result->row_ids.resize(n);

	result->column_ids.reserve(input.column_indexes.size());
	for (const auto &column_index : input.column_indexes) {
		result->column_ids.push_back(bind_data.table->GetStorageIndex(column_index));
	}
	return std::move(result);
}

static void SextantIndexScanFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<SextantIndexScanBindData>();
	auto &state = data.global_state->Cast<SextantIndexScanGlobalState>();

	const idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.row_ids.size() - state.offset);
	if (count == 0) {
		output.SetCardinality(0);
		return;
	}

	// The index was built by streaming the table in row order: engine
	// row ids are DuckDB row ids.
	Vector row_ids(LogicalType::ROW_TYPE);
	auto ids = FlatVector::GetData<row_t>(row_ids);
	for (idx_t i = 0; i < count; i++) {
		ids[i] = static_cast<row_t>(state.row_ids[state.offset + i]);
	}
	auto &transaction = DuckTransaction::Get(context, bind_data.table->catalog);
	bind_data.table->GetStorage().Fetch(transaction, output, state.column_ids, row_ids, count, state.fetch_state);
	output.SetCardinality(count);
	state.offset += count;
}

static unique_ptr<NodeStatistics> SextantIndexScanCardinality(ClientContext &context,
                                                              const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<SextantIndexScanBindData>();
	return make_uniq<NodeStatistics>(bind_data.k, bind_data.k);
}

TableFunction GetSextantIndexScanFunction() {
	TableFunction func("sextant_index_scan", {}, SextantIndexScanFunction, nullptr, SextantIndexScanInitGlobal);
	func.cardinality = SextantIndexScanCardinality;
	func.get_bind_info = SextantIndexScanBindInfo;
	// Let the planner keep rowid/virtual-column handling (plan_get throws
	// "Virtual columns require projection pushdown" otherwise).
	func.projection_pushdown = true;
	return func;
}

} // namespace duckdb
