#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/table_index_list.hpp"
#include "sextant/sextant_c.h"
#include "sextant_index.hpp"
#include "sextant_index_scan.hpp"

namespace duckdb {

//------------------------------------------------------------------------------
// ORDER BY array_distance(<indexed col>, <const>) ASC LIMIT k
//   =>
// sextant_index_scan (rows stream back in distance order, TopN dropped)
//
// Mirrors the vss pattern: match LogicalTopN over a projection whose
// order expression is a distance function over a plain seq_scan, then
// swap the scan's function for the index scan and remove the TopN.
//------------------------------------------------------------------------------

/// Extract (constant query vector, indexed-column binding) from an
/// array_distance(colref/const, const/colref) expression.
static bool TryMatchDistanceExpression(const Expression &expr, const ColumnBinding &probe,
                                       vector<float> &query_out) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	if (func.children.size() != 2 || func.function.name != "array_distance") {
		return false;
	}
	Expression *const_side = nullptr;
	Expression *col_side = nullptr;
	for (auto &child : func.children) {
		if (child->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			const_side = child.get();
		} else if (child->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			col_side = child.get();
		} else {
			return false;
		}
	}
	if (!const_side || !col_side) {
		return false;
	}
	auto &colref = col_side->Cast<BoundColumnRefExpression>();
	if (colref.binding != probe) {
		return false;
	}
	const auto &value = const_side->Cast<BoundConstantExpression>().value;
	const auto tclass = value.type().id();
	if (tclass != LogicalTypeId::ARRAY && tclass != LogicalTypeId::LIST) {
		return false;
	}
	const auto &children = tclass == LogicalTypeId::ARRAY ? ArrayValue::GetChildren(value)
	                                                     : ListValue::GetChildren(value);
	if (children.empty()) {
		return false;
	}
	query_out.clear();
	for (const auto &v : children) {
		Value fv;
		string cast_err;
		if (!v.DefaultTryCastAs(LogicalType::FLOAT, fv, &cast_err)) {
			return false;
		}
		query_out.push_back(fv.GetValue<float>());
	}
	return true;
}

static bool TryRewriteTopN(ClientContext &context, unique_ptr<LogicalOperator> &plan) {
	if (plan->type != LogicalOperatorType::LOGICAL_TOP_N) {
		return false;
	}
	auto &top_n = plan->Cast<LogicalTopN>();
	if (top_n.orders.size() != 1 || top_n.limit == 0 || top_n.offset != 0) {
		return false;
	}
	const auto &order = top_n.orders[0];
	if (order.type != OrderType::ASCENDING || order.null_order != OrderByNullType::NULLS_LAST) {
		return false;
	}
	if (order.expression->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &order_col = order.expression->Cast<BoundColumnRefExpression>();
	(void)order_col;

	if (top_n.children.size() != 1 || top_n.children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return false;
	}
	auto &projection = top_n.children[0]->Cast<LogicalProjection>();
	if (projection.children.size() != 1 || projection.children[0]->type != LogicalOperatorType::LOGICAL_GET) {
		return false;
	}
	auto &get = projection.children[0]->Cast<LogicalGet>();
	if (get.function.name != "seq_scan" || !get.GetTable()) {
		return false;
	}
	if (!get.table_filters.filters.empty() || (get.dynamic_filters && get.dynamic_filters->HasFilters())) {
		// Filtered ANN search is a separate serving path (filter SQL).
		return false;
	}

	// The projection expression the TopN orders by.
	const idx_t order_index = order_col.binding.column_index;
	if (order_index >= projection.expressions.size()) {
		return false;
	}
	const Expression &distance_expr = *projection.expressions[order_index];

	auto &table = get.GetTable()->Cast<DuckTableEntry>();
	if (!table.IsDuckTable()) {
		return false;
	}
	auto &storage = table.GetStorage();
	storage.BindIndexes(context);

	for (auto &index_entry : storage.GetDataTableInfo()->GetIndexes().IndexEntries()) {
		if (!index_entry.index || index_entry.index->GetIndexType() != SextantIndex::TYPE_NAME) {
			continue;
		}
		auto &index = index_entry.index->Cast<SextantIndex>();
		auto handle = index.GetEngineHandle();
		if (!handle) {
			continue;
		}
		// array_distance is euclidean: only valid for L2 trees.
		if (sextant_index_metric(handle) != SEXTANT_METRIC_L2SQ) {
			continue;
		}
		// The index must be over exactly the column the distance
		// expression references.
		if (index.GetColumnIds().size() != 1) {
			continue;
		}

		vector<float> query;
		// The distance argument must reference the projection's child
		// (the get) — the binding the projection passes through.
		const auto &get_bindings = get.GetColumnBindings();
		const auto &get_columns = get.GetColumnIds();
		for (idx_t col = 0; col < get_bindings.size(); col++) {
			if (col >= get_columns.size() ||
			    get_columns[col].GetPrimaryIndex() != static_cast<idx_t>(index.GetColumnIds()[0])) {
				continue;
			}
			if (!TryMatchDistanceExpression(distance_expr, get_bindings[col], query)) {
				continue;
			}
			const idx_t dim = ArrayType::GetSize(index.logical_types[0]);
			if (query.size() != dim) {
				continue;
			}

			auto bind_data = make_uniq<SextantIndexScanBindData>();
			bind_data->table = &table;
			bind_data->index = &index;
			bind_data->query = std::move(query);
			bind_data->k = top_n.limit;

			get.function = GetSextantIndexScanFunction();
			get.bind_data = std::move(bind_data);
			// Keep the TopN: the scan emits at most `limit` rows in
			// engine-distance order, and the TopN re-sorts those few rows
			// by the exact SQL expression — so the displayed ORDER BY is
			// honored exactly, not just approximately.
			return true;
		}
	}
	return false;
}

static void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (TryRewriteTopN(input.context, plan)) {
		return;
	}
	// The pattern can sit under a projection/CTE limb (subqueries).
	for (auto &child : plan->children) {
		Optimize(input, child);
	}
}

class SextantTopKOptimizer : public OptimizerExtension {
public:
	SextantTopKOptimizer() {
		optimize_function = Optimize;
	}
};

void RegisterSextantTopKOptimizer(DatabaseInstance &db) {
	OptimizerExtension::Register(db.config, SextantTopKOptimizer());
}

} // namespace duckdb
