#include "sextant_index.hpp"

#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_simple.hpp"
#include "duckdb/planner/operator/logical_update.hpp"

namespace duckdb {

namespace {

bool TableHasSextantIndex(TableCatalogEntry &table) {
	if (!table.IsDuckTable()) {
		return false;
	}
	auto &indexes = table.Cast<DuckTableEntry>().GetStorage().GetDataTableInfo()->GetIndexes();
	return indexes.DistinctIndexTypes().count(SextantIndex::TYPE_NAME) > 0;
}

/// True when every sextant index on the table opted into append-only
/// serving (WITH (delta_scan = true)) — INSERT is then allowed and the
/// rows are brute-forced past n_build at query time.
bool TableAllowsAppend(TableCatalogEntry &table) {
	bool any = false;
	for (auto &index_entry : table.Cast<DuckTableEntry>().GetStorage().GetDataTableInfo()->GetIndexes().IndexEntries()) {
		if (!index_entry.index || index_entry.index->GetIndexType() != SextantIndex::TYPE_NAME) {
			continue;
		}
		any = true;
		if (!index_entry.index->Cast<SextantIndex>().GetDeltaScan()) {
			return false;
		}
	}
	return any;
}

/// DROP INDEX on an unbound sextant index would bypass BoundIndex::
/// ResetStorage (and thus sidecar deletion): binding is otherwise only
/// triggered by ALTER/INSERT/DELETE/checkpoint. Force-bind before the drop
/// executes so our ResetStorage runs.
void BindIndexesBeforeDropIndex(ClientContext &context, DropInfo &drop) {
	if (drop.type != CatalogType::INDEX_ENTRY) {
		return;
	}
	auto index_entry_ptr =
	    Catalog::GetEntry(context, drop.catalog, drop.schema,
	                      EntryLookupInfo(CatalogType::INDEX_ENTRY, drop.name), OnEntryNotFound::RETURN_NULL);
	if (!index_entry_ptr || index_entry_ptr->type != CatalogType::INDEX_ENTRY) {
		return;
	}
	auto &index_entry = index_entry_ptr->Cast<IndexCatalogEntry>();
	auto table_catalog_ptr =
	    Catalog::GetEntry(context, drop.catalog, index_entry.GetSchemaName(),
	                      EntryLookupInfo(CatalogType::TABLE_ENTRY, index_entry.GetTableName()),
	                      OnEntryNotFound::RETURN_NULL);
	if (!table_catalog_ptr || table_catalog_ptr->type != CatalogType::TABLE_ENTRY) {
		return;
	}
	auto &table = table_catalog_ptr->Cast<DuckTableEntry>();
	table.GetStorage().BindIndexes(context);
}

void EnforceImmutable(ClientContext &context, LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DELETE || op.type == LogicalOperatorType::LOGICAL_UPDATE ||
	    op.type == LogicalOperatorType::LOGICAL_INSERT) {
		TableCatalogEntry *table = nullptr;
		if (op.type == LogicalOperatorType::LOGICAL_DELETE) {
			table = &op.Cast<LogicalDelete>().table;
		} else if (op.type == LogicalOperatorType::LOGICAL_UPDATE) {
			table = &op.Cast<LogicalUpdate>().table;
		} else {
			table = &op.Cast<LogicalInsert>().table;
		}
		if (table && table->IsDuckTable()) {
			// Force-bind lazily-bound indexes first: after a restart the
			// sextant index may still be unbound (binding normally happens
			// only at first data modification — which our fence rejects).
			auto &duck_table = table->Cast<DuckTableEntry>();
			if (duck_table.GetStorage().GetDataTableInfo()->GetIndexes().HasUnbound()) {
				duck_table.GetStorage().BindIndexes(context);
			}
			// INSERT is allowed on delta_scan indexes (append-only
			// serving); DELETE/UPDATE never are.
			if (op.type == LogicalOperatorType::LOGICAL_INSERT && TableAllowsAppend(*table)) {
				table = nullptr; // no fence for this insert
			}
		}
		if (table && TableHasSextantIndex(*table)) {
			throw InvalidInputException(
			    "Immutable sextant index: %s is not supported on table '%s'. Drop and re-create the index "
			    "(WITH (delta_scan = true) enables append-only serving).",
			    op.type == LogicalOperatorType::LOGICAL_DELETE ? "DELETE"
			    : op.type == LogicalOperatorType::LOGICAL_UPDATE ? "UPDATE"
			                                                     : "INSERT",
			    table->name);
		}
	} else if (op.type == LogicalOperatorType::LOGICAL_DROP) {
		auto &info = op.Cast<LogicalSimple>().info;
		if (info && info->info_type == ParseInfoType::DROP_INFO) {
			BindIndexesBeforeDropIndex(context, info->Cast<DropInfo>());
		}
	}
	for (auto &child : op.children) {
		EnforceImmutable(context, *child);
	}
}

class SextantImmutabilityOptimizer : public OptimizerExtension {
public:
	SextantImmutabilityOptimizer() {
		optimize_function = [](OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
			EnforceImmutable(input.context, *plan);
		};
	}
};

} // namespace

void RegisterSextantImmutabilityOptimizer(DatabaseInstance &db) {
	OptimizerExtension::Register(db.config, SextantImmutabilityOptimizer());
}

} // namespace duckdb
