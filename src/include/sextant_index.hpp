#pragma once

#include "duckdb.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/storage/partial_block_manager.hpp"
#include "sextant/sextant_c.h"

#include <atomic>
#include <mutex>

namespace duckdb {

/// Sextant IVF-tree index (lifecycle spike).
///
/// Storage model (hybrid): the DuckDB-side index blob holds only metadata
/// (sidecar path, tree UUID, n_build) in linked blocks; all vector data
/// lives in the sidecar .tree file owned by the sextant engine.
///
/// Mutation semantics: immutable by construction. Append/Insert/Delete
/// throw — any modification of a table with a sextant index aborts the
/// transaction with a "drop/rebuild" hint.
class SextantIndex : public BoundIndex {
public:
	static constexpr const char *TYPE_NAME = "sextant";

	SextantIndex(const string &name, IndexConstraintType index_constraint_type,
	             const vector<column_t> &column_ids, TableIOManager &table_io_manager,
	             const vector<unique_ptr<Expression>> &unbound_expressions, AttachedDatabase &db,
	             const case_insensitive_map_t<Value> &options, const IndexStorageInfo &info,
	             idx_t estimated_cardinality);
	~SextantIndex() override;

	// --- persistence (hybrid metadata blob) ---
	IndexStorageInfo SerializeToDisk(QueryContext context, const case_insensitive_map_t<Value> &options) override;
	IndexStorageInfo SerializeToWAL(const case_insensitive_map_t<Value> &options) override;
	void ResetStorage(IndexLock &index_lock) override;

	/// Open the sidecar read-only, verify its UUID (adopt on first attach;
	/// hard error on mismatch) and cache the handle for the index lifetime.
	void AttachAndVerify();

	/// Release the cached engine handle (idempotent).
	void CloseHandle();

	/// Cached sextant engine handle (one per index entry; searches on it
	/// are thread-safe). Null until first successful attach. Atomic so
	/// the optimizer / scan threads can read it without synchronization
	/// while a lazy attach is in flight (a torn read there only skips the
	/// rewrite for that one plan — exact fallback).
	std::atomic<void *> engine_handle {nullptr};

	// --- maintenance: refuse everything ---
	ErrorData Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) override;
	void Delete(IndexLock &state, DataChunk &entries, Vector &row_identifiers) override;
	ErrorData Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids) override;

	// --- build_* callbacks wired into the generic CREATE INDEX plan ---
	static unique_ptr<IndexBuildBindData> BuildBind(IndexBuildBindInput &input);
	static unique_ptr<IndexBuildGlobalState> BuildGlobalInit(IndexBuildInitGlobalStateInput &input);
	static unique_ptr<IndexBuildLocalState> BuildLocalInit(IndexBuildInitLocalStateInput &input);
	static void BuildSink(IndexBuildSinkInput &input, DataChunk &key_chunk, DataChunk &row_chunk);
	static void BuildCombine(IndexBuildCombineInput &input);
	static unique_ptr<BoundIndex> BuildFinalize(IndexBuildFinalizeInput &input);

	// --- misc ---
	idx_t GetInMemorySize(IndexLock &state) override;
	bool MergeIndexes(IndexLock &state, BoundIndex &other_index) override;
	void Vacuum(IndexLock &state) override;
	void Verify(IndexLock &state) override;
	void VerifyAllocations(IndexLock &state) override;
	string ToString(IndexLock &state, bool display_ascii = false) override;
	string GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index, DataChunk &input) override;

	/// Resolve `path` against the database file's directory when relative
	/// (cwd-independent; WAL-replay-safe). Returns "" for in-memory DBs.
	static string ResolveSidecarPath(AttachedDatabase &db, const string &path);

	const string &GetTreeUuid() const {
		return tree_uuid;
	}
	idx_t GetNBuild() const {
		return n_build;
	}
	/// WITH (delta_scan = true): INSERTs are accepted (append-only) and
	/// served by brute-forcing rowid >= n_build at query time, merged with
	/// the tree's results. DELETE/UPDATE stay fenced either way.
	bool GetDeltaScan() const {
		return delta_scan;
	}
const string &GetSidecarPath() const {
		return sidecar_path;
	}

	/// DuckDB filter-column type -> engine SEXTANT_COL_*. -1 = unsupported.
	/// VARCHAR[] (LIST of VARCHAR) maps to the engine set column; the
	/// CONTAINS predicate family evaluates against it. Date/time map to
	/// engine INT64 epochs (DATE = days since epoch,
	/// TIMESTAMP/_S/_MS normalized to µs; TIMESTAMP_NS rejected — ns since
	/// epoch overflows the double-safe comparison range). DOUBLE maps to
	/// engine FLOAT (binary32): filter shape stays consistent because the
	/// engine snaps comparands through the column domain, and the extension
	/// forces an exact SQL re-filter for DOUBLE columns.
	static int EngineColType(const LogicalType &t) {
		switch (t.id()) {
			case LogicalTypeId::INTEGER:     return SEXTANT_COL_INT32;
			case LogicalTypeId::BIGINT:      return SEXTANT_COL_INT64;
			case LogicalTypeId::FLOAT:       return SEXTANT_COL_FLOAT;
			case LogicalTypeId::VARCHAR:     return SEXTANT_COL_STRING;
			case LogicalTypeId::BOOLEAN:     return SEXTANT_COL_BOOL;
			case LogicalTypeId::DATE:        return SEXTANT_COL_INT64;
			case LogicalTypeId::TIMESTAMP:   return SEXTANT_COL_INT64;
			case LogicalTypeId::TIMESTAMP_SEC: return SEXTANT_COL_INT64;
			case LogicalTypeId::TIMESTAMP_MS: return SEXTANT_COL_INT64;
			case LogicalTypeId::DOUBLE:      return SEXTANT_COL_FLOAT;
			case LogicalTypeId::LIST:
				// VARCHAR[] only: the engine set column holds strings.
				return ListType::GetChildType(t) == LogicalType::VARCHAR ? SEXTANT_COL_SET : -1;
			default:                         return -1;
		}
	}

	/// Set when the sidecar could not be opened at catalog-load time
	/// (deleted file, moved directory, mismatched tree UUID). The entry
	/// stays droppable and other DDL on the table stays possible; only
	/// operations that actually USE this index re-verify (scan bind calls
	/// AttachAndVerify) and fail with the precise error.
	/// Written by the load path (create_instance) only.
	bool sidecar_unusable = false;
	string sidecar_error;

	/// Cached engine handle (null until attached).
	void *GetEngineHandle() const {
		return engine_handle;
	}

private:
	/// Serializes lazy attach / handle teardown. AttachAndVerify is
	/// called from query threads (sextant_query bind, scan init) which
	/// race on a cold index after a database reopen: without the lock,
	/// the losing thread's CloseHandle() frees the handle the winner
	/// just published and is searching (use-after-free).
	mutable std::mutex attach_mutex;
	/// Serialize (path, uuid, n_build) into the linked-block blob.
	void PersistToDisk();
	/// Deserialize the metadata blob (constructor, storage-valid path).
	void LoadFromStorage();

	unique_ptr<FixedSizeAllocator> linked_block_allocator;
	IndexPointer root_block_ptr;
	mutable StorageLock rwlock;
	bool is_dirty = false;
	/// Set by SerializeToDisk/SerializeToWAL so the checkpoint-rebuild call
	/// to ResetStorage does not delete the sidecar (only a committed DROP,
	/// which arrives without a preceding serialization, may delete).
	bool serialized_this_generation = false;


	string sidecar_path;  // resolved (absolute) path to the .tree sidecar
	string tree_uuid;     // 32 lowercase hex chars; empty = pre-UUID tree
	idx_t n_build = 0;    // table row count at CREATE INDEX time
	bool delta_scan = false; // WITH (delta_scan = true): append-only serving
};

/// Register the "sextant" index type on a database instance.
void RegisterSextantIndexType(DatabaseInstance &db);

/// Register the plan-time DELETE/UPDATE fence for tables with a sextant
/// index (commit-time index exceptions are escalated to fatal by DuckDB,
/// so the veto must happen at optimization time).
void RegisterSextantImmutabilityOptimizer(DatabaseInstance &db);

} // namespace duckdb
