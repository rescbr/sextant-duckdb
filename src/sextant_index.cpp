#include "sextant_index.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/exception.hpp"
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
	// rebuild) — NOT here: destructors also run on database close.
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
	// Open the sidecar read-only and verify its UUID against the one we
	// recorded (stale/wrong-file detection). For the lifecycle spike the
	// handle is not retained (scan support lands in step 2).
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
	sextant_close_index(handle);

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
};

struct SextantGlobalState : public IndexBuildGlobalState {
	unique_ptr<BoundIndex> global_index;
	std::atomic<idx_t> n_rows {0};
};

struct SextantLocalState : public IndexBuildLocalState {
	idx_t n_rows = 0;
};

} // namespace

unique_ptr<IndexBuildBindData> SextantIndex::BuildBind(IndexBuildBindInput &input) {
	auto &info = input.info;

	// Validate options strictly.
	for (const auto &opt : info.options) {
		if (opt.first != "path" && opt.first != "delta_scan") {
			throw BinderException("Unknown option '%s' for sextant index", opt.first);
		}
	}
	if (info.options.find("path") == info.options.end()) {
		throw BinderException("Sextant indexes require WITH (path = '<tree file>') pointing at a built "
		                      "sextant .tree sidecar");
	}

	auto bind = make_uniq<SextantBindData>();
	bind->sidecar_path = info.options.at("path").GetValue<string>();
	return std::move(bind);
}

unique_ptr<IndexBuildGlobalState> SextantIndex::BuildGlobalInit(IndexBuildInitGlobalStateInput &input) {
	auto state = make_uniq<SextantGlobalState>();

	auto &storage = input.table.GetStorage();
	state->global_index = make_uniq<SextantIndex>(input.info.index_name, input.info.constraint_type,
	                                              input.storage_ids, TableIOManager::Get(storage), input.expressions,
	                                              storage.db, input.info.options, IndexStorageInfo(), 0);

	return std::move(state);
}

unique_ptr<IndexBuildLocalState> SextantIndex::BuildLocalInit(IndexBuildInitLocalStateInput &input) {
	return make_uniq<SextantLocalState>();
}

void SextantIndex::BuildSink(IndexBuildSinkInput &input, DataChunk &key_chunk, DataChunk &row_chunk) {
	// The sidecar is authoritative for vector data; we only record the
	// table row count at index creation (n_build, the delta_scan split
	// point later).
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

	// Record n_build and attach + verify the sidecar before the index
	// becomes visible: a missing or wrong tree fails CREATE INDEX itself.
	index.n_build = gstate.n_rows.load();
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
		// delete the sidecar (best-effort; crash-window orphans are
		// documented).
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
