#include "sextant_index.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/storage/table_io_manager.hpp"

#include "sextant/sextant_c.h"

#include <atomic>
#include <filesystem>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Metadata blob (linked blocks, vss pattern)
//===--------------------------------------------------------------------===//
namespace {

constexpr uint32_t kSextantMetaMagic = 0x53585431; // "SXT1"

// Fixed-size blocks in a FixedSizeAllocator, chained through IndexPointers.
// Our blob is tiny (path + uuid + n_build), so one block virtually always
// suffices.
class MetaBlock {
public:
	static constexpr idx_t BLOCK_SIZE = Storage::DEFAULT_BLOCK_SIZE - sizeof(validity_t);
	static constexpr idx_t BLOCK_DATA_SIZE = BLOCK_SIZE - sizeof(IndexPointer);

	IndexPointer next_block;
	char data[BLOCK_DATA_SIZE] = {0};
};

class MetaReader {
public:
	MetaReader(FixedSizeAllocator &allocator, IndexPointer root)
	    : allocator(allocator), current_pointer(root) {
	}

	void ReadData(data_ptr_t buffer, idx_t length) {
		while (length > 0) {
			if (current_pointer.Get() == 0) {
				throw InternalException("SextantIndex: metadata blob truncated");
			}
			auto block = allocator.Get<MetaBlock>(current_pointer);
			idx_t to_copy = MinValue(length, MetaBlock::BLOCK_DATA_SIZE - position_in_block);
			memcpy(buffer, block->data + position_in_block, to_copy);
			buffer += to_copy;
			length -= to_copy;
			position_in_block += to_copy;
			if (position_in_block == MetaBlock::BLOCK_DATA_SIZE) {
				current_pointer = block->next_block;
				position_in_block = 0;
			}
		}
	}

private:
	FixedSizeAllocator &allocator;
	IndexPointer current_pointer;
	idx_t position_in_block = 0;
};

class MetaWriter {
public:
	MetaWriter(FixedSizeAllocator &allocator, IndexPointer root)
	    : allocator(allocator), current_pointer(root) {
	}

	void ClearCurrentBlock() {
		auto block = allocator.Get<MetaBlock>(current_pointer);
		block->next_block.Clear();
		memset(block->data, 0, MetaBlock::BLOCK_DATA_SIZE);
	}

	void WriteData(const_data_ptr_t buffer, idx_t length) {
		while (length > 0) {
			auto block = allocator.Get<MetaBlock>(current_pointer);
			idx_t to_copy = MinValue(length, MetaBlock::BLOCK_DATA_SIZE - position_in_block);
			memcpy(block->data + position_in_block, buffer, to_copy);
			buffer += to_copy;
			length -= to_copy;
			position_in_block += to_copy;
			if (position_in_block == MetaBlock::BLOCK_DATA_SIZE) {
				if (block->next_block.Get() == 0) {
					block->next_block = allocator.New();
				}
				current_pointer = block->next_block;
				position_in_block = 0;
				ClearCurrentBlock();
			}
		}
	}

private:
	FixedSizeAllocator &allocator;
	IndexPointer current_pointer;
	idx_t position_in_block = 0;
};

string ImmutableMessage(const string &index_name, const char *what) {
	return "Immutable sextant index: " + string(what) +
	       " is not supported. Drop and re-create the index "
	       "(WITH (delta_scan = true) enables append-only serving). "
	       "(index: " + index_name + ")";
}

} // namespace

//===--------------------------------------------------------------------===//
// Construction / loading
//===--------------------------------------------------------------------===//

SextantIndex::SextantIndex(const string &name, IndexConstraintType index_constraint_type,
                           const vector<column_t> &column_ids, TableIOManager &table_io_manager,
                           const vector<unique_ptr<Expression>> &unbound_expressions, AttachedDatabase &db,
                           const case_insensitive_map_t<Value> &options, const IndexStorageInfo &info,
                           idx_t estimated_cardinality)
    : BoundIndex(name, TYPE_NAME, index_constraint_type, column_ids, table_io_manager, unbound_expressions, db) {

	if (index_constraint_type != IndexConstraintType::NONE) {
		throw NotImplementedException("Sextant indexes do not support unique or primary key constraints");
	}

	// One FLOAT[d] column (fixed-size ARRAY).
	if (logical_types.size() != 1 || logical_types[0].id() != LogicalTypeId::ARRAY) {
		throw BinderException("Sextant indexes require exactly one fixed-size FLOAT[d] ARRAY column");
	}

	auto &block_manager = table_io_manager.GetIndexBlockManager();
	linked_block_allocator = make_uniq<FixedSizeAllocator>(sizeof(MetaBlock), block_manager);

	if (info.IsValid()) {
		// Existing index: load the metadata blob from DuckDB storage.
		root_block_ptr.Set(info.root);
		D_ASSERT(info.allocator_infos.size() == 1);
		linked_block_allocator->Init(info.allocator_infos[0]);
		LoadFromStorage();
	} else {
		// Fresh creation: options must carry the sidecar path.
		auto path_opt = options.find("path");
		if (path_opt == options.end()) {
			throw BinderException("Sextant indexes require WITH (path = '<tree file>') pointing at a "
			                      "built sextant .tree sidecar");
		}
		sidecar_path = ResolveSidecarPath(db, path_opt->second.GetValue<string>());
		is_dirty = true;
	}
}

SextantIndex::~SextantIndex() {
	// Sidecar deletion happens in ResetStorage (committed DROP vs checkpoint
	// rebuild) — NOT here: destructors also run on database close. The
	// engine handle, however, is ours to release on any teardown.
	CloseHandle();
}

void SextantIndex::CloseHandle() {
	if (engine_handle) {
		sextant_close_index(engine_handle);
		engine_handle = nullptr;
	}
}

string SextantIndex::ResolveSidecarPath(AttachedDatabase &db, const string &path) {
	if (path.empty()) {
		throw BinderException("Sextant index path must not be empty");
	}
	std::error_code ec;
	if (std::filesystem::path(path).is_absolute()) {
		return path;
	}
	// Relative paths resolve against the database file's directory (not
	// cwd): whole-directory moves keep working and WAL replay after a
	// `cd` elsewhere cannot break resolution.
	const string db_path = db.StoredPath();
	if (db_path.empty() || db_path == ":memory:") {
		throw BinderException("Sextant indexes in in-memory databases require an absolute WITH (path = ...)");
	}
	return (std::filesystem::path(db_path).parent_path() / path).lexically_normal().string();
}

void SextantIndex::AttachAndVerify() {
	// Open the sidecar read-only, verify its UUID against the one we
	// recorded (stale/wrong-file detection), and CACHE the handle for the
	// index lifetime: one open handle per index entry, shared by all DuckDB
	// scan threads (sextant_search is thread-safe on a handle — verified
	// under TSAN by the engine's ConcurrentSearchStress test).
	CloseHandle();
	char err[512] = {0};
	void *handle = sextant_open_index(sidecar_path.c_str(), err, sizeof(err));
	if (!handle) {
		throw InvalidInputException("Sextant index '%s': cannot open sidecar '%s': %s. Drop and re-create the "
		                            "index pointing at a valid .tree file",
		                            GetIndexName(), sidecar_path, err);
	}
	char uuid[40] = {0};
	int32_t uuid_rc = sextant_index_uuid(handle, uuid, sizeof(uuid));
	if (uuid_rc < 0) {
		sextant_close_index(handle);
		throw InvalidInputException("Sextant index '%s': cannot read tree UUID from '%s'", GetIndexName(),
		                            sidecar_path);
	}
	const string sidecar_uuid(uuid);
	engine_handle = handle;  // kept open until CloseHandle()

	if (tree_uuid.empty()) {
		// First attach: adopt the sidecar's identity.
		tree_uuid = sidecar_uuid;
		is_dirty = true;
	} else if (!sidecar_uuid.empty() && sidecar_uuid != tree_uuid) {
		throw InvalidInputException("Sextant index '%s': sidecar '%s' is tree %s, but the index was created on "
		                            "tree %s — stale or wrong file. Drop and re-create the index",
		                            GetIndexName(), sidecar_path, sidecar_uuid, tree_uuid);
	}
}

//===--------------------------------------------------------------------===//
// Build callbacks (generic CREATE INDEX plan, ART pattern)
//===--------------------------------------------------------------------===//

namespace {

struct SextantBindData : public IndexBuildBindData {
	string sidecar_path; // as-given WITH (path = ...) value
	bool attach = false; // WITH (prebuilt): sidecar exists, do not build
	vector<string> filter_cols; // WITH (filter_cols = [...]): table columns
	string payload_col;         // WITH (payload_col = '...'): blob column
	int64_t build_threads = 0;  // WITH (build_threads = N): engine build threads
};

struct SextantGlobalState : public IndexBuildGlobalState {
	unique_ptr<BoundIndex> global_index;
	std::atomic<idx_t> n_rows {0};
	// Build-from-table state (attach == false).
	void *builder = nullptr;       // sextant builder handle
	uint32_t dim = 0;              // vector dimension (ARRAY size)
	ClientContext *context = nullptr; // for the finalize scan
	string scan_sql;               // SELECT vec, filters..., payload
	vector<int> filter_types;      // SEXTANT_COL_* per declared filter col
	bool has_payload = false;
};

struct SextantLocalState : public IndexBuildLocalState {
	idx_t n_rows = 0;
};

} // namespace

unique_ptr<IndexBuildBindData> SextantIndex::BuildBind(IndexBuildBindInput &input) {
	auto &info = input.info;

	// Validate options strictly.
		static const char *const kKnown[] = {"path", "delta_scan", "prebuilt", "filter_cols", "payload_col",
		                                "build_threads"};
	for (const auto &opt : info.options) {
		bool known = false;
		for (auto *k : kKnown) {
			if (StringUtil::CIEquals(opt.first, k)) {
				known = true;
				break;
			}
		}
		if (!known) {
			throw BinderException("Unknown option '%s' for sextant index", opt.first);
		}
	}
	if (info.options.find("path") == info.options.end()) {
		throw BinderException("Sextant indexes require WITH (path = '<tree file>'): by default the tree is "
		                      "BUILT from the table into that file; WITH (prebuilt = true) attaches an "
		                      "existing prebuilt tree");
	}

	auto bind = make_uniq<SextantBindData>();
	bind->sidecar_path = info.options.at("path").GetValue<string>();
	if (auto prebuilt = info.options.find("prebuilt"); prebuilt != info.options.end()) {
		bind->attach = prebuilt->second.DefaultCastAs(LogicalType::BOOLEAN).GetValue<bool>();
	}
	if (auto fc = info.options.find("filter_cols"); fc != info.options.end()) {
		// Comma-separated column names: 1.5.5's option parser rejects
		// non-constant def-elems, and list literals do not fold to
		// constants there.
		const string spec = fc->second.ToString();
		string cur;
		for (const char ch : spec) {
			if (ch == ',') {
				bind->filter_cols.push_back(StringUtil::Replace(StringUtil::Replace(cur, " ", ""), "\t", ""));
				cur.clear();
			} else {
				cur.push_back(ch);
			}
		}
		if (cur.find_first_not_of(" \t") != std::string::npos) {
			bind->filter_cols.push_back(StringUtil::Replace(StringUtil::Replace(cur, " ", ""), "\t", ""));
		}
		if (bind->filter_cols.empty()) {
			throw BinderException("sextant filter_cols must be a comma-separated list of column names");
		}
	}
	if (auto pc = info.options.find("payload_col"); pc != info.options.end()) {
		bind->payload_col = pc->second.ToString();
	}
	if (auto bt = info.options.find("build_threads"); bt != info.options.end()) {
		bind->build_threads = bt->second.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		if (bind->build_threads < 0 || bind->build_threads > 1024) {
			throw BinderException("sextant build_threads must be in [0, 1024]");
		}
	}
	return std::move(bind);
}

unique_ptr<IndexBuildGlobalState> SextantIndex::BuildGlobalInit(IndexBuildInitGlobalStateInput &input) {
	auto state = make_uniq<SextantGlobalState>();

	auto &storage = input.table.GetStorage();
	state->global_index = make_uniq<SextantIndex>(input.info.index_name, input.info.constraint_type,
	                                              input.storage_ids, TableIOManager::Get(storage), input.expressions,
	                                              storage.db, input.info.options, IndexStorageInfo(), 0);

	if (input.bind_data && !input.bind_data->Cast<SextantBindData>().attach) {
		// Build-from-table: the generic plan's sink only receives the
		// indexed column + rowid, so the vector+filter+payload stream is
		// produced by OUR scan at finalize. Here: validate columns, declare
		// the builder, and prepare the scan SQL.
		auto &bind = input.bind_data->Cast<SextantBindData>();
		auto &index = state->global_index->Cast<SextantIndex>();
		auto &vec_type = index.logical_types[0];
		state->dim = ArrayType::GetSize(vec_type);
		auto &child_type = ArrayType::GetChildType(vec_type);
		if (child_type != LogicalType::FLOAT) {
			throw BinderException("Sextant indexes require a FLOAT[d] ARRAY column, got %s",
			                      child_type.ToString());
		}

		auto &duck_table = input.table;
		auto quoted = [&](const string &s) { return "\"" + StringUtil::Replace(s, "\"", "\"\"") + "\""; };

		// Vector column name (from the table's indexed storage column).
		const auto &column_ids = index.GetColumnIds();
		string vec_name = duck_table.GetColumns().GetColumn(PhysicalIndex(column_ids[0])).Name();

		// Filter columns: name -> type mapping (Bool unsupported by the v1
		// push contract; Set later).
		vector<sextant_filter_col_def> defs;
		string filter_sql;
		for (const auto &name : bind.filter_cols) {
			auto &col = duck_table.GetColumn(name);
			int t;
			switch (col.Type().id()) {
				case LogicalTypeId::INTEGER:  t = SEXTANT_COL_INT32; break;
				case LogicalTypeId::BIGINT:   t = SEXTANT_COL_INT64; break;
				case LogicalTypeId::FLOAT:    t = SEXTANT_COL_FLOAT; break;
				case LogicalTypeId::VARCHAR:  t = SEXTANT_COL_STRING; break;
				default:
					throw BinderException("sextant filter column '%s' has unsupported type %s "
					                      "(supported: INTEGER, BIGINT, FLOAT, VARCHAR)",
					                      name, col.Type().ToString());
			}
			defs.push_back({name.c_str(), t});
			state->filter_types.push_back(t);
			filter_sql += ", " + quoted(name);
		}
		// Payload column.
		string payload_sql;
		if (!bind.payload_col.empty()) {
			auto &col = duck_table.GetColumn(bind.payload_col);
			if (col.Type().id() != LogicalTypeId::VARCHAR) {
				throw BinderException("sextant payload column '%s' must be VARCHAR, got %s", bind.payload_col,
				                      col.Type().ToString());
			}
			state->has_payload = true;
			payload_sql = ", " + quoted(bind.payload_col);
		}

		sextant_build_opts opts = sextant_default_build_opts();
		if (bind.build_threads > 0) {
			opts.num_threads = static_cast<uint32_t>(bind.build_threads);
		}
		char err[512] = {0};
		state->builder = sextant_build_begin(
		    &opts, state->dim, defs.empty() ? nullptr : defs.data(),
		    static_cast<uint32_t>(defs.size()), state->has_payload ? 1 : 0, err, sizeof(err));
		if (!state->builder) {
			throw InvalidInputException("Sextant index '%s': cannot start build: %s", index.GetIndexName(), err);
		}

		state->context = &input.context;
		state->scan_sql = "SELECT " + quoted(vec_name) + filter_sql + payload_sql + " FROM " +
		                  quoted(duck_table.schema.name) + "." + quoted(duck_table.name) + " WHERE " +
		                  quoted(vec_name) + " IS NOT NULL";
	}

	return std::move(state);
}

unique_ptr<IndexBuildLocalState> SextantIndex::BuildLocalInit(IndexBuildInitLocalStateInput &input) {
	return make_uniq<SextantLocalState>();
}

void SextantIndex::BuildSink(IndexBuildSinkInput &input, DataChunk &key_chunk, DataChunk &row_chunk) {
	// Count only: the vector+filter+payload stream comes from our own scan
	// at finalize (the generic plan's sink cannot see non-indexed columns).
	auto &lstate = input.local_state.Cast<SextantLocalState>();
	lstate.n_rows += row_chunk.size();
}

void SextantIndex::BuildCombine(IndexBuildCombineInput &input) {
	auto &gstate = input.global_state.Cast<SextantGlobalState>();
	auto &lstate = input.local_state.Cast<SextantLocalState>();
	gstate.n_rows += lstate.n_rows;
	lstate.n_rows = 0;
}

unique_ptr<BoundIndex> SextantIndex::BuildFinalize(IndexBuildFinalizeInput &input) {
	auto &gstate = input.global_state.Cast<SextantGlobalState>();
	auto &index = gstate.global_index->Cast<SextantIndex>();

	// Build-from-table: stream (vector, filters..., payload) rows from our
	// own scan and push them into the engine builder, then run the build.
	if (gstate.builder) {
		D_ASSERT(gstate.context);
		Connection con(*gstate.context->db);
		auto result = con.SendQuery(gstate.scan_sql);
		if (result->HasError()) {
			sextant_build_abort(gstate.builder);
			gstate.builder = nullptr;
			throw InvalidInputException("Sextant index '%s': build scan failed: %s", index.GetIndexName(),
			                            result->GetError());
		}

		const idx_t n_filters = gstate.filter_types.size();
		uint64_t pushed = 0;
		while (true) {
			auto chunk = result->Fetch();
			if (!chunk || chunk->size() == 0) {
				break;
			}
			const idx_t n = chunk->size();
			auto &data = *chunk;
			// Column 0: FLOAT[d] values (contiguous in the flat child).
			auto &vec_vec = data.data[0];
			auto &child = const_cast<Vector &>(ArrayVector::GetEntry(vec_vec));
			auto *vecs = FlatVector::GetData<float>(child);

			// Filter columns.
			vector<const void *> filter_values;
			vector<vector<int32_t>> i32_bufs;
			vector<vector<int64_t>> i64_bufs;
			vector<vector<float>> f32_bufs;
			vector<sextant_str_values> str_bufs;
			vector<vector<const char *>> str_ptrs;
			vector<vector<uint32_t>> str_lens;
			// Reserve: `filter_values` holds pointers INTO these outer
			// vectors' storage — reallocation would dangle them.
			i32_bufs.reserve(n_filters);
			i64_bufs.reserve(n_filters);
			f32_bufs.reserve(n_filters);
			str_bufs.reserve(n_filters);
			str_ptrs.reserve(n_filters);
			str_lens.reserve(n_filters);
			for (idx_t f = 0; f < n_filters; f++) {
				auto &col = data.data[1 + f];
				switch (gstate.filter_types[f]) {
					case SEXTANT_COL_INT32: {
						i32_bufs.emplace_back();
						auto &buf = i32_bufs.back();
						buf.reserve(n);
						UnifiedVectorFormat fmt;
						col.ToUnifiedFormat(n, fmt);
						auto *d = UnifiedVectorFormat::GetData<int32_t>(fmt);
						for (idx_t r = 0; r < n; r++) {
							buf.push_back(fmt.validity.RowIsValid(fmt.sel->get_index(r))
							                  ? d[fmt.sel->get_index(r)] : 0);
						}
						filter_values.push_back(buf.data());
						break;
					}
					case SEXTANT_COL_INT64: {
						i64_bufs.emplace_back();
						auto &buf = i64_bufs.back();
						buf.reserve(n);
						UnifiedVectorFormat fmt;
						col.ToUnifiedFormat(n, fmt);
						auto *d = UnifiedVectorFormat::GetData<int64_t>(fmt);
						for (idx_t r = 0; r < n; r++) {
							buf.push_back(fmt.validity.RowIsValid(fmt.sel->get_index(r))
							                  ? d[fmt.sel->get_index(r)] : 0);
						}
						filter_values.push_back(buf.data());
						break;
					}
					case SEXTANT_COL_FLOAT: {
						f32_bufs.emplace_back();
						auto &buf = f32_bufs.back();
						buf.reserve(n);
						UnifiedVectorFormat fmt;
						col.ToUnifiedFormat(n, fmt);
						auto *d = UnifiedVectorFormat::GetData<float>(fmt);
						for (idx_t r = 0; r < n; r++) {
							buf.push_back(fmt.validity.RowIsValid(fmt.sel->get_index(r))
							                  ? d[fmt.sel->get_index(r)] : 0.0f);
						}
						filter_values.push_back(buf.data());
						break;
					}
					case SEXTANT_COL_STRING: {
						str_bufs.emplace_back();
						str_ptrs.emplace_back();
						str_lens.emplace_back();
						auto &sv = str_bufs.back();
						auto &ptrs = str_ptrs.back();
						auto &lens = str_lens.back();
						ptrs.reserve(n);
						lens.reserve(n);
						UnifiedVectorFormat fmt;
						col.ToUnifiedFormat(n, fmt);
						auto *d = UnifiedVectorFormat::GetData<string_t>(fmt);
						for (idx_t r = 0; r < n; r++) {
							const auto i = fmt.sel->get_index(r);
							if (fmt.validity.RowIsValid(i)) {
								const auto s = d[i];
								if (s.GetSize() > 65535) {
									throw InvalidInputException("Sextant index: filter string exceeds 65535 "
									                            "bytes at row %llu",
									                            (unsigned long long)(pushed + r));
								}
								ptrs.push_back(s.GetData());
								lens.push_back(s.GetSize());
							} else {
								ptrs.push_back("");
								lens.push_back(0);
							}
						}
						sv.data = ptrs.data();
						sv.lengths = lens.data();
						filter_values.push_back(&sv);
						break;
					}
					default:
						throw InternalException("sextant: unhandled filter type");
				}
			}

			// Payload: blobs appended into one buffer + offsets.
			vector<uint64_t> payload_offsets;
			vector<uint8_t> payload_data;
			const uint8_t *payload_ptr = nullptr;
			const uint64_t *payload_off = nullptr;
			if (gstate.has_payload) {
				auto &col = data.data[1 + n_filters];
				payload_offsets.reserve(n + 1);
				payload_offsets.push_back(0);
				UnifiedVectorFormat fmt;
				col.ToUnifiedFormat(n, fmt);
				auto *d = UnifiedVectorFormat::GetData<string_t>(fmt);
				for (idx_t r = 0; r < n; r++) {
					const auto i = fmt.sel->get_index(r);
					if (fmt.validity.RowIsValid(i)) {
						payload_data.insert(payload_data.end(), d[i].GetData(),
						                    d[i].GetData() + d[i].GetSize());
					}
					payload_offsets.push_back(payload_data.size());
				}
				payload_ptr = payload_data.data();
				payload_off = payload_offsets.data();
			}

			char err[512] = {0};
			if (sextant_build_push(gstate.builder, vecs, static_cast<uint32_t>(n),
			                       filter_values.empty() ? nullptr : filter_values.data(), payload_off,
			                       payload_ptr, err, sizeof(err)) != 0) {
				sextant_build_abort(gstate.builder);
				gstate.builder = nullptr;
				throw InvalidInputException("Sextant index: build push failed at row %llu: %s",
				                            (unsigned long long)pushed, err);
			}
			pushed += n;
		}

		if (pushed != gstate.n_rows.load()) {
			sextant_build_abort(gstate.builder);
			gstate.builder = nullptr;
			throw InvalidInputException(
			    "Sextant index '%s': build scan saw %llu rows but the index plan saw %llu — uncommitted "
			    "table data or concurrent modification. Commit the table data before creating the index.",
			    index.GetIndexName(), (unsigned long long)pushed, (unsigned long long)gstate.n_rows.load());
		}

		char err[512] = {0};
		const string out_path = index.GetSidecarPath();
		if (sextant_build_finish(gstate.builder, out_path.c_str(), err, sizeof(err)) != 0) {
			gstate.builder = nullptr;
			throw InvalidInputException("Sextant index '%s': build failed: %s", index.GetIndexName(), err);
		}
		gstate.builder = nullptr;
		index.n_build = pushed;
	} else {
		index.n_build = gstate.n_rows.load();
	}

	// Attach + verify the sidecar before the index becomes visible.
	index.AttachAndVerify();

	return std::move(gstate.global_index);
}

//===--------------------------------------------------------------------===//
// Maintenance: immutable by construction
//===--------------------------------------------------------------------===//

ErrorData SextantIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
	return ErrorData(ImmutableMessage(GetIndexName(), "INSERT"));
}

ErrorData SextantIndex::Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
	return ErrorData(ImmutableMessage(GetIndexName(), "INSERT"));
}

void SextantIndex::Delete(IndexLock &state, DataChunk &entries, Vector &row_identifiers) {
	// Unreachable in practice: the plan-time immutability optimizer rejects
	// DELETE/UPDATE before execution. DuckDB escalates commit-time index
	// exceptions to FatalException by design, so a commit-time veto is
	// impossible; this is a last-resort guard.
	throw FatalException("%s", ImmutableMessage(GetIndexName(), "DELETE").c_str());
}

bool SextantIndex::MergeIndexes(IndexLock &state, BoundIndex &other_index) {
	throw InvalidInputException("%s", ImmutableMessage(GetIndexName(), "index merging"));
}

//===--------------------------------------------------------------------===//
// Persistence (hybrid: tiny metadata blob, big sidecar)
//===--------------------------------------------------------------------===//

void SextantIndex::PersistToDisk() {
	auto lock = rwlock.GetExclusiveLock();

	// Always rewrite: the blob is tiny, and skipping when clean is wrong —
	// SerializeToWAL (CREATE INDEX commit) clears is_dirty, which would
	// make the following checkpoint's SerializeToDisk skip flushing to
	// actual storage blocks.
	if (root_block_ptr.Get() == 0) {
		root_block_ptr = linked_block_allocator->New();
	}

	// Blob: [magic u32][path_len u32][path bytes][uuid_len u32][uuid bytes]
	//       [n_build u64]
	string blob;
	blob.resize(4 + 4 + sidecar_path.size() + 4 + tree_uuid.size() + 8);
	data_ptr_t p = reinterpret_cast<data_ptr_t>(blob.data());
	Store<uint32_t>(kSextantMetaMagic, p);
	p += 4;
	Store<uint32_t>(static_cast<uint32_t>(sidecar_path.size()), p);
	p += 4;
	memcpy(p, sidecar_path.data(), sidecar_path.size());
	p += sidecar_path.size();
	Store<uint32_t>(static_cast<uint32_t>(tree_uuid.size()), p);
	p += 4;
	memcpy(p, tree_uuid.data(), tree_uuid.size());
	p += tree_uuid.size();
	Store<uint64_t>(static_cast<uint64_t>(n_build), p);

	MetaWriter writer(*linked_block_allocator, root_block_ptr);
	writer.ClearCurrentBlock();
	writer.WriteData(reinterpret_cast<const_data_ptr_t>(blob.data()), blob.size());
}

void SextantIndex::LoadFromStorage() {
	if (root_block_ptr.Get() == 0 && linked_block_allocator->GetInfo().buffer_ids.empty()) {
		return; // empty index, nothing to deserialize
	}
	MetaReader reader(*linked_block_allocator, root_block_ptr);

	uint32_t magic = 0;
	reader.ReadData(reinterpret_cast<data_ptr_t>(&magic), 4);
	if (magic != kSextantMetaMagic) {
		throw InternalException("SextantIndex: metadata blob magic mismatch");
	}

	uint32_t path_len = 0;
	reader.ReadData(reinterpret_cast<data_ptr_t>(&path_len), 4);
	sidecar_path.resize(path_len);
	reader.ReadData(reinterpret_cast<data_ptr_t>(sidecar_path.data()), path_len);

	uint32_t uuid_len = 0;
	reader.ReadData(reinterpret_cast<data_ptr_t>(&uuid_len), 4);
	tree_uuid.resize(uuid_len);
	reader.ReadData(reinterpret_cast<data_ptr_t>(tree_uuid.data()), uuid_len);

	uint64_t n = 0;
	reader.ReadData(reinterpret_cast<data_ptr_t>(&n), 8);
	n_build = n;
}

IndexStorageInfo SextantIndex::SerializeToDisk(QueryContext context, const case_insensitive_map_t<Value> &options) {
	PersistToDisk();

	IndexStorageInfo info(GetIndexName());
	info.root = root_block_ptr.Get();

	auto &block_manager = table_io_manager.GetIndexBlockManager();
	PartialBlockManager partial_block_manager(context, block_manager, PartialBlockType::FULL_CHECKPOINT);
	linked_block_allocator->SerializeBuffers(partial_block_manager);
	partial_block_manager.FlushPartialBlocks();
	info.allocator_infos.push_back(linked_block_allocator->GetInfo());

	// Checkpoint rebuild (RebuildIndexes -> ResetStorage) must not delete
	// the sidecar; only a committed DROP may.
	serialized_this_generation = true;

	return info;
}

IndexStorageInfo SextantIndex::SerializeToWAL(const case_insensitive_map_t<Value> &options) {
	PersistToDisk();

	IndexStorageInfo info(GetIndexName());
	info.root = root_block_ptr.Get();
	info.buffers.push_back(linked_block_allocator->InitSerializationToWAL());
	info.allocator_infos.push_back(linked_block_allocator->GetInfo());

	serialized_this_generation = true;

	return info;
}

void SextantIndex::ResetStorage(IndexLock &index_lock) {
	if (serialized_this_generation) {
		// Called from RebuildIndexes during checkpoint: keep the sidecar,
		// just drop in-memory state (nothing to do for an external file).
		serialized_this_generation = false;
	} else {
		// Called from TableIndexList::RemoveIndex on a committed DROP:
		// release the handle, then delete the sidecar (best-effort;
		// crash-window orphans are documented).
		CloseHandle();
		std::error_code ec;
		if (!sidecar_path.empty() && std::filesystem::exists(sidecar_path, ec)) {
			std::filesystem::remove(sidecar_path, ec);
		}
	}
	// NOTE: unlike ART we keep the in-memory metadata (allocator + root)
	// alive: our storage is external, and RebuildIndexes reuses the index
	// instance after ResetStorage.
}

//===--------------------------------------------------------------------===//
// Misc
//===--------------------------------------------------------------------===//

idx_t SextantIndex::GetInMemorySize(IndexLock &state) {
	return sizeof(SextantIndex) + sidecar_path.size() + tree_uuid.size();
}

void SextantIndex::Vacuum(IndexLock &state) {
	// Nothing in memory to vacuum (metadata only).
}

void SextantIndex::Verify(IndexLock &state) {
	// The generic create plan calls Verify after finalize; nothing to check.
}

void SextantIndex::VerifyAllocations(IndexLock &state) {
	// No buffer-managed allocations beyond the metadata blob.
}

string SextantIndex::ToString(IndexLock &state, bool display_ascii) {
	return "SEXTANT INDEX (" + sidecar_path + ", n_build=" + to_string(n_build) + ", uuid=" + tree_uuid + ")";
}

string SextantIndex::GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index, DataChunk &input) {
	throw NotImplementedException("Sextant indexes do not enforce constraints");
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterSextantIndexType(DatabaseInstance &db) {
	IndexType index_type;
	index_type.name = SextantIndex::TYPE_NAME;
	index_type.create_instance = [](CreateIndexInput &input) -> unique_ptr<BoundIndex> {
		// Load path: existing index deserialized from DuckDB storage
		// (checkpoint/WAL replay). Re-verify the sidecar identity.
		auto index = make_uniq<SextantIndex>(input.name, input.constraint_type, input.column_ids,
		                                     input.table_io_manager, input.unbound_expressions, input.db,
		                                     input.options, input.storage_info, 0);
		if (input.storage_info.IsValid() && !index->GetSidecarPath().empty()) {
			index->AttachAndVerify();
		}
		return std::move(index);
	};
	index_type.build_bind = SextantIndex::BuildBind;
	index_type.build_global_init = SextantIndex::BuildGlobalInit;
	index_type.build_local_init = SextantIndex::BuildLocalInit;
	index_type.build_sink = SextantIndex::BuildSink;
	index_type.build_combine = SextantIndex::BuildCombine;
	index_type.build_finalize = SextantIndex::BuildFinalize;

	// No create_plan override: the generic plan (SCAN -> PROJECTION ->
	// FILTER -> CREATE INDEX) drives our callbacks.

	db.config.GetIndexTypes().RegisterIndexType(index_type);
}

} // namespace duckdb
