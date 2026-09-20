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

#include <cmath>
#include <cstdlib>
#include <limits>

namespace duckdb {

//-------------------------------------------------------------------------
// Internal index scan: emitted rows come back in ANN distance order.
//-------------------------------------------------------------------------

namespace {

/// Evaluate one translated predicate against a delta row with exact SQL
/// semantics (NULL fails everything except IS NULL). `numeric`/`str` hold
/// the row's value; `is_null` its NULL flag; `set_vals` the row's VARCHAR[]
/// elements (set columns; empty when is_null).
bool EvalDeltaPredicate(const SextantScanPredicate &p, bool is_null, double numeric, const string &str,
                         const vector<string> &set_vals) {
	if (is_null) {
		return p.op == SEXTANT_PRED_IS_NULL;
	}
	switch (p.op) {
		case SEXTANT_PRED_CONTAINS:
			for (const auto &v : set_vals) {
				if (v == p.str_value) return true;
			}
			return false;
		case SEXTANT_PRED_CONTAINS_ANY:
			for (const auto &v : set_vals) {
				for (const auto &c : p.values) {
					if (v == c) return true;
				}
			}
			return false;
		case SEXTANT_PRED_CONTAINS_ALL:
			for (const auto &c : p.values) {
				bool found = false;
				for (const auto &v : set_vals) {
					if (v == c) {
						found = true;
						break;
					}
				}
				if (!found) return false;
			}
			return true;
		case SEXTANT_PRED_IS_NULL:    return false;
		case SEXTANT_PRED_IS_NOT_NULL: return true;
		case SEXTANT_PRED_EQ:         return numeric == p.value;
		case SEXTANT_PRED_NEQ:        return numeric != p.value;
		case SEXTANT_PRED_LT:         return numeric <  p.value;
		case SEXTANT_PRED_LE:         return numeric <= p.value;
		case SEXTANT_PRED_GT:         return numeric >  p.value;
		case SEXTANT_PRED_GE:         return numeric >= p.value;
		case SEXTANT_PRED_IN:
			for (const auto &v : p.values) {
				if (std::strtod(v.c_str(), nullptr) == numeric) return true;
			}
			return false;
		case SEXTANT_PRED_NOT_IN:
			for (const auto &v : p.values) {
				if (std::strtod(v.c_str(), nullptr) == numeric) return false;
			}
			return true;
		default:
			// String predicates on the string comparand.
			break;
	}
	switch (p.op) {
		case SEXTANT_PRED_EQ:  return str == p.str_value;
		case SEXTANT_PRED_NEQ: return str != p.str_value;
		case SEXTANT_PRED_IN:
			for (const auto &v : p.values) {
				if (str == v) return true;
			}
			return false;
		case SEXTANT_PRED_NOT_IN:
			for (const auto &v : p.values) {
				if (str == v) return false;
			}
			return true;
		default:
			return true; // ops the translator never produces
	}
}

} // namespace

/// Engine-owned W-TinyLFU cache budgets (MiB) from the session settings.
/// Validated here so an invalid value fails at attach with a clear
/// message. Binds at FIRST attach per index lifetime; later session
/// changes do not re-open an attached handle.
std::pair<uint64_t, uint64_t> SextantCacheBudget(ClientContext &context) {
	uint64_t leaf = 0, plane = 0;
	Value v;
	constexpr uint64_t kMaxMiB = 1ull << 20; // 1 PiB: sanity, not policy
	if (context.TryGetCurrentSetting("sextant_leaf_cache_mb", v)) {
		const int64_t mb = v.GetValue<int64_t>();
		if (mb < 0 || mb > static_cast<int64_t>(kMaxMiB)) {
			throw InvalidInputException("sextant_leaf_cache_mb must be in [0, 2^20] MiB");
		}
		leaf = static_cast<uint64_t>(mb) << 20;
	}
	if (context.TryGetCurrentSetting("sextant_plane_cache_mb", v)) {
		const int64_t mb = v.GetValue<int64_t>();
		if (mb < 0 || mb > static_cast<int64_t>(kMaxMiB)) {
			throw InvalidInputException("sextant_plane_cache_mb must be in [0, 2^20] MiB");
		}
		plane = static_cast<uint64_t>(mb) << 20;
	}
	return {leaf, plane};
}

void ApplySextantSearchSettings(ClientContext &context, void *engine_handle, sextant_search_opts &opts) {
	Value v;
	// Per-query within-query parallelism (default 1: DuckDB supplies
	// cross-query parallelism via its own threads).
	if (context.TryGetCurrentSetting("sextant_search_threads", v)) {
		const int64_t n = v.GetValue<int64_t>();
		if (n < 0 || n > 1024) {
			throw InvalidInputException("sextant_search_threads must be in [0, 1024]");
		}
		opts.search_threads = static_cast<uint32_t>(n);
	}
	// Corpus-fraction probe budget; 0 keeps the index default (new trees
	// persist 0.5 — measured ~0.99 recall@10 across corpora).
	if (context.TryGetCurrentSetting("sextant_probe_fraction", v)) {
		const double f = v.GetValue<double>();
		if (f < 0.0 || f > 1.0) {
			throw InvalidInputException("sextant_probe_fraction must be in [0, 1] (0 = index default)");
		}
		opts.probe_fraction = static_cast<float>(f);
	}
	// Shortlist width; 0 keeps the engine default (max(k, 1000)).
	if (context.TryGetCurrentSetting("sextant_fastscan_w", v)) {
		const int64_t w = v.GetValue<int64_t>();
		if (w < 0 || w > std::numeric_limits<uint32_t>::max()) {
			throw InvalidInputException("sextant_fastscan_w must be in [0, 2^32) (0 = engine default)");
		}
		opts.fastscan_W = static_cast<uint32_t>(w);
	}
	if (context.TryGetCurrentSetting("sextant_rerank", v)) {
		opts.rerank = v.GetValue<bool>() ? 1 : 0;
	}
	if (context.TryGetCurrentSetting("sextant_exhaustive", v)) {
		opts.exhaustive = v.GetValue<bool>() ? 1 : 0;
	}
	// Adaptive-W tau, CLI AUTO parity (code_size >= 288B -> 2.5 else 5.0):
	// without it the shortlist is fixed at W~k and recall collapses (a
	// clustered 2000x32 fixture scored 10/20 vs 20/20 with the wide cut).
	// Requires rerank — skip the cut when rerank is disabled.
	if (engine_handle && opts.rerank) {
		const uint32_t code_size = sextant_index_code_size(engine_handle);
		opts.adaptive_w_gap = code_size >= 288 ? 2.5f : 5.0f;
	}
}

/// Append-only delta serving: brute-force rows past n_build (DELETE is
/// fenced, so those row ids are dense and stable), compute squared-L2
/// distances to the query, apply the translated predicates, and merge the
/// best candidates into the engine's (rowid, dist) result. Shared by the
/// top-k rewrite scan and the debug sextant_query() function.
void MergeDeltaRows(ClientContext &context, DuckTableEntry &table, SextantIndex &index,
                    const vector<float> &query, idx_t k, const vector<SextantScanPredicate> &predicates,
                    vector<uint64_t> &row_ids, vector<float> &dists) {
	auto &duck_table = table;
	const idx_t n_build = index.GetNBuild();
	const idx_t total = duck_table.GetStorage().GetTotalRows();

	// Defensive: the recorded build boundary must match the tree's actual
	// row count (guarded at CREATE INDEX for prebuilt attaches; this
	// catches a metadata blob desynced from the sidecar).
	const uint64_t tree_rows = sextant_index_count(index.GetEngineHandle());
	if (tree_rows != n_build) {
		throw InvalidInputException("Sextant index '%s': tree holds %llu rows but n_build is %llu — drop "
		                            "and re-create the index",
		                            index.GetIndexName(), (unsigned long long)tree_rows,
		                            (unsigned long long)n_build);
	}

	if (total <= n_build) {
		return; // nothing appended since the build
	}

	// Resolve fetch columns: [vector column, predicate columns...].
	const idx_t vec_col = index.GetColumnIds()[0];
	vector<StorageIndex> fetch_cols = {StorageIndex(vec_col)};
	vector<LogicalType> fetch_types;
	vector<idx_t> pred_col_ids; // index into fetch_cols (1..)
	{
		const auto &columns = duck_table.GetColumns();
		case_insensitive_map_t<idx_t> by_name;
		for (idx_t c = 0; c < columns.PhysicalColumnCount(); c++) {
			by_name[columns.GetColumn(PhysicalIndex(c)).Name()] = c;
		}
		fetch_types.push_back(columns.GetColumn(PhysicalIndex(vec_col)).Type());
		for (const auto &pred : predicates) {
			const auto it = by_name.find(pred.column);
			if (it == by_name.end() || it->second == vec_col) {
				return; // predicate column vanished: serve tree rows only
			}
			pred_col_ids.push_back(fetch_cols.size());
			fetch_cols.emplace_back(it->second);
			fetch_types.push_back(columns.GetColumn(PhysicalIndex(it->second)).Type());
		}
	}

	auto &transaction = DuckTransaction::Get(context, duck_table.catalog);
	ColumnFetchState fetch_state;
	const idx_t dim = query.size();
	// Merge score: squared-L2 ascending for L2 trees; negated inner
	// product ascending (= q·x descending) for IP trees — matching the
	// engine's best-first ordering.
	const bool ip_metric = index.GetEngineHandle() != nullptr &&
	                       sextant_index_metric(index.GetEngineHandle()) == SEXTANT_METRIC_IP;
	const auto score_of = [&](const float *v) {
		if (ip_metric) {
			float dot = 0.0f;
			for (idx_t j = 0; j < dim; j++) {
				dot += v[j] * query[j];
			}
			return -dot;
		}
		float d = 0.0f;
		for (idx_t j = 0; j < dim; j++) {
			const float diff = v[j] - query[j];
			d += diff * diff;
		}
		return d;
	};
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), fetch_types);

	// Max-heap-style candidate tracking: keep the best `keep` delta rows.
	const size_t keep = k;
	vector<std::pair<float, uint64_t>> cand;
	const size_t n_pred = predicates.size();

	for (idx_t start = n_build; start < total; start += STANDARD_VECTOR_SIZE) {
		const idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, total - start);
		Vector row_ids_vec(LogicalType::ROW_TYPE);
		auto ids = FlatVector::GetData<row_t>(row_ids_vec);
		for (idx_t i = 0; i < count; i++) {
			ids[i] = static_cast<row_t>(start + i);
		}
		duck_table.GetStorage().Fetch(transaction, chunk, fetch_cols, row_ids_vec, count, fetch_state);

		UnifiedVectorFormat vfmt;
		chunk.data[0].ToUnifiedFormat(count, vfmt);
		auto &vec_child = ArrayVector::GetEntry(chunk.data[0]);
		auto *vecs = FlatVector::GetData<float>(vec_child);

		// Predicate column value caches for this chunk.
		vector<UnifiedVectorFormat> pfmt(n_pred);
		for (size_t pi = 0; pi < n_pred; pi++) {
			chunk.data[pred_col_ids[pi]].ToUnifiedFormat(count, pfmt[pi]);
		}

		for (idx_t r = 0; r < count; r++) {
			const auto vi = vfmt.sel->get_index(r);
			if (!vfmt.validity.RowIsValid(vi)) {
				continue; // NULL vector: never indexed
			}
			const float d = score_of(vecs + static_cast<size_t>(vi) * dim);

			bool pass = true;
			for (size_t pi = 0; pi < n_pred && pass; pi++) {
				const auto &fmt = pfmt[pi];
				const auto &ptype = fetch_types[pred_col_ids[pi]];
				const auto i = fmt.sel->get_index(r);
				const bool is_null = !fmt.validity.RowIsValid(i);
				double numeric = 0.0;
				string str;
				vector<string> set_vals;
				if (!is_null) {
					switch (ptype.id()) {
					case LogicalTypeId::INTEGER:  numeric = UnifiedVectorFormat::GetData<int32_t>(fmt)[i]; break;
					case LogicalTypeId::BIGINT:   numeric = UnifiedVectorFormat::GetData<int64_t>(fmt)[i]; break;
					case LogicalTypeId::FLOAT:    numeric = UnifiedVectorFormat::GetData<float>(fmt)[i]; break;
					case LogicalTypeId::DOUBLE:   numeric = UnifiedVectorFormat::GetData<double>(fmt)[i]; break;
					case LogicalTypeId::BOOLEAN:  numeric = UnifiedVectorFormat::GetData<bool>(fmt)[i] ? 1 : 0; break;
					case LogicalTypeId::LIST: {
						// VARCHAR[] predicate column: expand the row's
						// elements (NULL elements never match).
						auto &le = UnifiedVectorFormat::GetData<list_entry_t>(fmt)[i];
						auto &child = ListVector::GetEntry(chunk.data[pred_col_ids[pi]]);
						UnifiedVectorFormat cfmt;
						child.ToUnifiedFormat(ListVector::GetListSize(chunk.data[pred_col_ids[pi]]), cfmt);
						auto *cd = UnifiedVectorFormat::GetData<string_t>(cfmt);
						for (idx_t e = le.offset; e < le.offset + le.length; e++) {
							const auto ci = cfmt.sel->get_index(e);
							if (cfmt.validity.RowIsValid(ci)) {
								set_vals.push_back(cd[ci].GetString());
							}
						}
						break;
					}
						// Epoch encodings matching the tree-side push and the
						// predicate comparands (see SextantIndex::EngineColType).
						case LogicalTypeId::DATE:
							numeric = UnifiedVectorFormat::GetData<int32_t>(fmt)[i]; // days
							break;
						case LogicalTypeId::TIMESTAMP:
							numeric = static_cast<double>(UnifiedVectorFormat::GetData<int64_t>(fmt)[i]);
							break;
						case LogicalTypeId::TIMESTAMP_SEC:
							numeric = static_cast<double>(UnifiedVectorFormat::GetData<int64_t>(fmt)[i]) * 1e6;
							break;
						case LogicalTypeId::TIMESTAMP_MS:
							numeric = static_cast<double>(UnifiedVectorFormat::GetData<int64_t>(fmt)[i]) * 1e3;
							break;
						default: {
							const auto s = UnifiedVectorFormat::GetData<string_t>(fmt)[i];
							str = s.GetString();
							break;
						}
					}
				}
				pass = EvalDeltaPredicate(predicates[pi], is_null, numeric, str, set_vals);
			}
			if (!pass) {
				continue;
			}
			cand.emplace_back(d, static_cast<uint64_t>(start + r));
		}
	}
	// Recompute EXACT squared-L2 distances for the engine's rows too:
	// engine distances are family-specific scores (offset/surrogate
	// scales), not comparable with the brute-forced values. Fetching the
	// k <= 144 vectors is cheap.
	{
		Vector ids_vec(LogicalType::ROW_TYPE);
		auto ids = FlatVector::GetData<row_t>(ids_vec);
		for (size_t i = 0; i < row_ids.size(); i++) {
			ids[i] = static_cast<row_t>(row_ids[i]);
		}
		DataChunk echunk;
		echunk.Initialize(Allocator::DefaultAllocator(), fetch_types);
		duck_table.GetStorage().Fetch(transaction, echunk, fetch_cols, ids_vec,
		                              row_ids.size(), fetch_state);
		UnifiedVectorFormat efmt;
		echunk.data[0].ToUnifiedFormat(row_ids.size(), efmt);
		auto &echild = ArrayVector::GetEntry(echunk.data[0]);
		auto *evecs = FlatVector::GetData<float>(echild);
		for (size_t i = 0; i < row_ids.size(); i++) {
			const auto ei = efmt.sel->get_index(i);
			if (!efmt.validity.RowIsValid(ei)) {
				dists[i] = std::numeric_limits<float>::max();
				continue;
			}
			dists[i] = score_of(evecs + static_cast<size_t>(ei) * dim);
		}
	}

	std::sort(cand.begin(), cand.end());
	if (cand.size() > keep) {
		cand.resize(keep);
	}
	// Merge with the engine's results (both ascending distance) and keep
	// the best `keep` overall.
	vector<std::pair<float, uint64_t>> merged;
	for (size_t i = 0; i < row_ids.size(); i++) {
		merged.emplace_back(dists[i], row_ids[i]);
	}
	merged.insert(merged.end(), cand.begin(), cand.end());
	std::sort(merged.begin(), merged.end());
	if (merged.size() > keep) {
		merged.resize(keep);
	}
	row_ids.clear();
	dists.clear();
	for (const auto &m : merged) {
		row_ids.push_back(m.second);
		dists.push_back(m.first);
	}
}

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
		auto [leaf_mb, plane_mb] = SextantCacheBudget(context);
		bind_data.index->AttachAndVerify(leaf_mb, plane_mb);
		handle = bind_data.index->GetEngineHandle();
		if (!handle) {
			throw InvalidInputException("Sextant index '%s': index handle unavailable",
			                            bind_data.index->GetIndexName());
		}
	}

	// Same serving options as the explicit sextant_query() function:
	// session tuning (threads, probe fraction, W, rerank, exhaustive)
	// plus adaptive-W tau at CLI AUTO parity.
	sextant_search_opts opts = sextant_default_search_opts();
	opts.k = static_cast<uint32_t>(bind_data.k);
	ApplySextantSearchSettings(context, handle, opts);

	result->row_ids.resize(bind_data.k);
	vector<float> dists(bind_data.k);
	char err[512] = {0};
	int32_t n;
	if (bind_data.predicates.empty()) {
		n = sextant_search(handle, bind_data.query.data(), &opts, result->row_ids.data(),
		                   dists.data(), static_cast<uint32_t>(bind_data.k), err, sizeof(err));
	} else {
		// Engine predicate view: strings stay owned by the bind data
		// (alive for the whole query); the IN-list pointer/length
		// arrays are locals alive across the call.
		vector<sextant_predicate> preds(bind_data.predicates.size());
		vector<vector<const char *>> in_ptrs(bind_data.predicates.size());
		vector<vector<uint32_t>> in_lens(bind_data.predicates.size());
		for (idx_t p = 0; p < bind_data.predicates.size(); p++) {
			const auto &src = bind_data.predicates[p];
			auto &dst = preds[p];
			dst.column = src.column.c_str();
			dst.op = src.op;
			dst.value = src.value;
			dst.str_value = src.str_value.empty() ? nullptr : src.str_value.c_str();
			if (!src.values.empty()) {
				auto &ptrs = in_ptrs[p];
				auto &lens = in_lens[p];
				ptrs.reserve(src.values.size());
				lens.reserve(src.values.size());
				for (const auto &v : src.values) {
					ptrs.push_back(v.c_str());
					lens.push_back(static_cast<uint32_t>(v.size()));
				}
				dst.values = ptrs.data();
				dst.value_lengths = lens.data();
				dst.n_values = static_cast<uint32_t>(src.values.size());
			}
		}
		n = sextant_search_filtered(handle, bind_data.query.data(), &opts, preds.data(),
		                            static_cast<uint32_t>(preds.size()), result->row_ids.data(),
		                            dists.data(), static_cast<uint32_t>(bind_data.k), err, sizeof(err));
	}
	if (n < 0) {
		throw InvalidInputException("Sextant index '%s': search failed: %s", bind_data.index->GetIndexName(),
		                            err);
	}
	result->row_ids.resize(n);
	dists.resize(n);

	// Append-only delta serving: merge brute-forced rows past n_build.
	if (bind_data.index->GetDeltaScan()) {
		MergeDeltaRows(context, *bind_data.table, *bind_data.index, bind_data.query, bind_data.k,
		               bind_data.predicates, result->row_ids, dists);
	}

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
