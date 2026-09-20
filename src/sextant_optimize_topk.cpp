#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
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
/// array_distance(colref/const, const/colref) (L2 trees) or
/// array_inner_product(...) (IP trees) expression. `is_ip` receives the
/// matched kind.
static bool TryMatchDistanceExpression(const Expression &expr, const ColumnBinding &probe,
                                       vector<float> &query_out, bool &is_ip) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	if (func.children.size() != 2) {
		return false;
	}
	if (func.function.name == "array_distance") {
		is_ip = false;
	} else if (func.function.name == "array_inner_product") {
		is_ip = true;
	} else {
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

//------------------------------------------------------------------------------
// Filter translation: DuckDB pushed-down TableFilters -> engine predicates.
// Only translatable subsets qualify the rewrite (numeric/string/bool
// comparisons, IN, IS [NOT] NULL on engine filter columns); anything else
// bails. Trees built with nullable filter columns evaluate predicates
// with exact SQL semantics, so the engine predicates ARE the semantics.
// Legacy trees (non-nullable columns, NULLs stored as 0/"") still get an
// exact SQL LogicalFilter re-injected above the scan + a k overfetch.
//------------------------------------------------------------------------------

static int EnginePredOp(ExpressionType t) {
	switch (t) {
		case ExpressionType::COMPARE_EQUAL:                return SEXTANT_PRED_EQ;
		case ExpressionType::COMPARE_NOTEQUAL:             return SEXTANT_PRED_NEQ;
		case ExpressionType::COMPARE_LESSTHAN:             return SEXTANT_PRED_LT;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:    return SEXTANT_PRED_LE;
		case ExpressionType::COMPARE_GREATERTHAN:          return SEXTANT_PRED_GT;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO: return SEXTANT_PRED_GE;
		default:                                           return -1;
	}
}

static int EngineColType(const LogicalType &t) {
	switch (t.id()) {
		case LogicalTypeId::INTEGER: return SEXTANT_COL_INT32;
		case LogicalTypeId::BIGINT:  return SEXTANT_COL_INT64;
		case LogicalTypeId::FLOAT:   return SEXTANT_COL_FLOAT;
		case LogicalTypeId::VARCHAR: return SEXTANT_COL_STRING;
		case LogicalTypeId::BOOLEAN: return SEXTANT_COL_BOOL;
		default:                     return -1;
	}
}

static bool TryTranslateOneFilter(const TableFilter &filter, const string &col_name, int engine_col_type,
                                  vector<SextantScanPredicate> &preds, vector<unique_ptr<Expression>> &sql_exprs,
                                  const ColumnBinding &col_binding, const LogicalType &col_type) {
	auto colref = make_uniq<BoundColumnRefExpression>(col_name, col_type, col_binding);
	switch (filter.filter_type) {
		case TableFilterType::EXPRESSION_FILTER: {
			// The filter combiner canonicalizes IS [NOT] NULL and NOT IN
			// into a generic ExpressionFilter over a BoundReference.
			// Unwrap the recognizable forms; anything else bails.
			auto &ef = filter.Cast<ExpressionFilter>();
			if (!ef.expr || ef.expr->GetExpressionClass() != ExpressionClass::BOUND_OPERATOR) {
				return false;
			}
			if (ef.expr->GetExpressionType() == ExpressionType::OPERATOR_IS_NULL ||
			    ef.expr->GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
				const bool is_null = ef.expr->GetExpressionType() == ExpressionType::OPERATOR_IS_NULL;
				auto &pred = preds.emplace_back();
				pred.column = col_name;
				pred.op = is_null ? SEXTANT_PRED_IS_NULL : SEXTANT_PRED_IS_NOT_NULL;
				auto is_expr = make_uniq<BoundOperatorExpression>(
				    is_null ? ExpressionType::OPERATOR_IS_NULL : ExpressionType::OPERATOR_IS_NOT_NULL,
				    LogicalType::BOOLEAN);
				is_expr->children.push_back(std::move(colref));
				sql_exprs.push_back(std::move(is_expr));
				return true;
			}
			if (ef.expr->GetExpressionType() == ExpressionType::COMPARE_NOT_IN) {
				// `col NOT IN (consts)`: translate to the engine NOT_IN set
				// predicate (children[0] is the canonicalized BoundReference).
				auto &op_expr = ef.expr->Cast<BoundOperatorExpression>();
				if (op_expr.children.size() < 2) {
					return false;
				}
				auto &pred = preds.emplace_back();
				pred.column = col_name;
				pred.op = SEXTANT_PRED_NOT_IN;
				vector<Value> values;
				for (size_t v = 1; v < op_expr.children.size(); v++) {
					if (op_expr.children[v]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
						return false;
					}
					values.push_back(op_expr.children[v]->Cast<BoundConstantExpression>().value);
					if (engine_col_type == SEXTANT_COL_STRING) {
						if (values.back().type().id() != LogicalTypeId::VARCHAR) {
							return false;
						}
						pred.values.push_back(values.back().ToString());
					} else {
						Value dv;
						if (!values.back().DefaultTryCastAs(LogicalType::DOUBLE, dv, nullptr)) {
							return false;
						}
						// %.17g roundtrips a double exactly; the engine
						// parses IN lists back through strtod.
						char buf[40];
						snprintf(buf, sizeof(buf), "%.17g", dv.GetValue<double>());
						pred.values.push_back(buf);
					}
				}
				if (pred.values.empty()) {
					return false;
				}
				// Exact re-injection form for legacy (non-nullable) trees.
				auto not_in_expr = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_NOT_IN,
				                                                 LogicalType::BOOLEAN);
				not_in_expr->children.push_back(std::move(colref));
				for (auto &v : values) {
					not_in_expr->children.push_back(make_uniq<BoundConstantExpression>(std::move(v)));
				}
				sql_exprs.push_back(std::move(not_in_expr));
				return true;
			}
			return false;
		}
		case TableFilterType::IS_NULL:
		case TableFilterType::IS_NOT_NULL: {
			const bool is_null = filter.filter_type == TableFilterType::IS_NULL;
			auto &pred = preds.emplace_back();
			pred.column = col_name;
			pred.op = is_null ? SEXTANT_PRED_IS_NULL : SEXTANT_PRED_IS_NOT_NULL;
			auto is_expr = make_uniq<BoundOperatorExpression>(
			    is_null ? ExpressionType::OPERATOR_IS_NULL : ExpressionType::OPERATOR_IS_NOT_NULL,
			    LogicalType::BOOLEAN);
			is_expr->children.push_back(std::move(colref));
			sql_exprs.push_back(std::move(is_expr));
			return true;
		}
		case TableFilterType::CONSTANT_COMPARISON: {
			auto &cf = filter.Cast<ConstantFilter>();
			const int op = EnginePredOp(cf.comparison_type);
			if (op < 0) {
				return false;
			}
			auto &pred = preds.emplace_back();
			pred.column = col_name;
			pred.op = op;
			if (engine_col_type == SEXTANT_COL_STRING) {
				if (cf.constant.type().id() != LogicalTypeId::VARCHAR ||
				    (op != SEXTANT_PRED_EQ && op != SEXTANT_PRED_NEQ)) {
					return false;
				}
				pred.str_value = cf.constant.ToString();
			} else if (engine_col_type == SEXTANT_COL_BOOL) {
				if (op != SEXTANT_PRED_EQ && op != SEXTANT_PRED_NEQ) {
					return false;
				}
				Value bv;
				if (!cf.constant.DefaultTryCastAs(LogicalType::BOOLEAN, bv, nullptr)) {
					return false;
				}
				pred.value = bv.GetValue<bool>() ? 1.0 : 0.0;
			} else {
				Value dv;
				if (!cf.constant.DefaultTryCastAs(LogicalType::DOUBLE, dv, nullptr)) {
					return false;
				}
				pred.value = dv.GetValue<double>();
			}
			sql_exprs.push_back(make_uniq<BoundComparisonExpression>(
			    cf.comparison_type, colref->Copy(),
			    make_uniq<BoundConstantExpression>(cf.constant)));
			return true;
		}
		case TableFilterType::IN_FILTER: {
			auto &inf = filter.Cast<InFilter>();
			if (inf.values.empty()) {
				return false;
			}
			auto &pred = preds.emplace_back();
			pred.column = col_name;
			pred.op = SEXTANT_PRED_IN;
			if (engine_col_type == SEXTANT_COL_STRING) {
				for (const auto &v : inf.values) {
					if (v.type().id() != LogicalTypeId::VARCHAR) {
						return false;
					}
					pred.values.push_back(v.ToString());
				}
			} else {
				for (const auto &v : inf.values) {
					Value dv;
					if (!v.DefaultTryCastAs(LogicalType::DOUBLE, dv, nullptr)) {
						return false;
					}
					// %.17g roundtrips a double exactly; the engine
					// parses IN lists back through strtod.
					char buf[40];
					snprintf(buf, sizeof(buf), "%.17g", dv.GetValue<double>());
					pred.values.push_back(buf);
				}
			}
			auto in_expr = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_IN, LogicalType::BOOLEAN);
			in_expr->children.push_back(std::move(colref));
			for (const auto &v : inf.values) {
				in_expr->children.push_back(make_uniq<BoundConstantExpression>(v));
			}
			sql_exprs.push_back(std::move(in_expr));
			return true;
		}
		case TableFilterType::CONJUNCTION_AND: {
			auto &and_filter = filter.Cast<ConjunctionAndFilter>();
			if (and_filter.child_filters.empty()) {
				return false;
			}
			// DuckDB pushes `col LIKE 'p%'` down as the bytewise range
			// `col >= 'p' AND col < 'p[last]++'` (filter_combiner,
			// PUSHED_DOWN_PARTIALLY — the LIKE itself stays above, so
			// exact semantics are preserved regardless). The range is
			// bytewise-equivalent to a prefix test, which the engine
			// evaluates natively (SEXTANT_PRED_PREFIX).
			if (and_filter.child_filters.size() == 2 && engine_col_type == SEXTANT_COL_STRING) {
				const ConstantFilter *ge = nullptr;
				const ConstantFilter *lt = nullptr;
				for (const auto &child : and_filter.child_filters) {
					if (child->filter_type != TableFilterType::CONSTANT_COMPARISON) {
						ge = nullptr;
						break;
					}
					auto &cf = child->Cast<ConstantFilter>();
					if (cf.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO && !ge) {
						ge = &cf;
					} else if (cf.comparison_type == ExpressionType::COMPARE_LESSTHAN && !lt) {
						lt = &cf;
					} else {
						ge = nullptr;
						break;
					}
				}
				if (ge && lt && ge->constant.type().id() == LogicalTypeId::VARCHAR &&
				    lt->constant.type().id() == LogicalTypeId::VARCHAR) {
					const string lo = ge->constant.ToString();
					const string hi = lt->constant.ToString();
					// Mirror DuckDB's transform: increment the last
					// byte of the prefix.
					if (!lo.empty() && hi.size() == lo.size()) {
						string inc = lo;
						inc.back()++;
						if (hi == inc) {
							auto &pred = preds.emplace_back();
							pred.column = col_name;
							pred.op = SEXTANT_PRED_PREFIX;
							pred.str_value = lo;
							// Re-injection keeps the exact range form
							// (equivalent to the prefix bytewise).
							auto conj = make_uniq<BoundConjunctionExpression>(
							    ExpressionType::CONJUNCTION_AND);
							conj->children.push_back(make_uniq<BoundComparisonExpression>(
							    ExpressionType::COMPARE_GREATERTHANOREQUALTO, colref->Copy(),
							    make_uniq<BoundConstantExpression>(ge->constant)));
							conj->children.push_back(make_uniq<BoundComparisonExpression>(
							    ExpressionType::COMPARE_LESSTHAN, colref->Copy(),
							    make_uniq<BoundConstantExpression>(lt->constant)));
							sql_exprs.push_back(std::move(conj));
							return true;
						}
					}
				}
			}
			auto conj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			for (const auto &child : and_filter.child_filters) {
				if (!TryTranslateOneFilter(*child, col_name, engine_col_type, preds, conj->children, col_binding,
				                        col_type)) {
					return false;
				}
			}
			sql_exprs.push_back(std::move(conj));
			return true;
		}
		default:
			return false;
	}
}

/// Translate every pushed-down table filter into engine predicates (+ the
/// exact SQL expressions for re-injection). Returns false when any filter
/// is not translatable.
static bool TryTranslateTableFilters(LogicalGet &get, DuckTableEntry &table, void *engine_handle,
                                     vector<SextantScanPredicate> &preds, vector<unique_ptr<Expression>> &sql_exprs,
                                     bool &preds_exact) {
	preds_exact = true;
	// Engine filter schema: name -> (SEXTANT_COL_*, nullable).
	case_insensitive_map_t<std::pair<int, bool>> engine_cols;
	const uint32_t n_engine_cols = sextant_index_filter_col_count(engine_handle);
	for (uint32_t i = 0; i < n_engine_cols; i++) {
		char name[128];
		int type = -1;
		int nullable = 0;
		if (sextant_index_filter_col(engine_handle, i, name, sizeof(name), &type, &nullable) != 0) {
			return false;
		}
		engine_cols[name] = {type, nullable != 0};
	}

	const auto &get_columns = get.GetColumnIds();
	for (const auto &entry : get.table_filters.filters) {
		const idx_t storage_col = entry.first;
		// Resolve the table column (name + type).
		const auto &columns = table.GetColumns();
		if (storage_col >= columns.PhysicalColumnCount()) {
			return false;
		}
		auto &column = columns.GetColumn(PhysicalIndex(storage_col));
		const string &col_name = column.Name();
		const auto it = engine_cols.find(col_name);
		if (it == engine_cols.end()) {
			return false; // filter on a non-indexed column
		}
		const int engine_type = EngineColType(column.Type());
		if (engine_type < 0 || engine_type != it->second.first) {
			return false; // schema drift between table and tree
		}
		// NULL divergence (engine 0/"" stand-ins) only exists on legacy
		// non-nullable tree columns — those still need the exact re-filter.
		if (!it->second.second) {
			preds_exact = false;
		}
		// Find the canonical binding for this column: binding index ==
		// position in column_ids, in both pruned and unpruned form. The
		// rewrite clears projection_ids, so the binding is advertised.
		ColumnBinding col_binding;
		bool found = false;
		for (idx_t c = 0; c < get_columns.size(); c++) {
			if (get_columns[c].GetPrimaryIndex() == storage_col) {
				col_binding = ColumnBinding(get.table_index, static_cast<column_t>(c));
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
		if (!TryTranslateOneFilter(*entry.second, col_name, engine_type, preds, sql_exprs, col_binding, column.Type())) {
			return false;
		}
	}
	return true;
}

static bool TryRewriteTopN(ClientContext &context, unique_ptr<LogicalOperator> &plan) {
	if (plan->type == LogicalOperatorType::LOGICAL_TOP_N) {	}
	if (plan->type != LogicalOperatorType::LOGICAL_TOP_N) {
		return false;
	}
	auto &top_n = plan->Cast<LogicalTopN>();
	if (top_n.orders.size() != 1 || top_n.limit == 0 || top_n.offset != 0) {		return false;
	}
	const auto &order = top_n.orders[0];
	if (order.null_order != OrderByNullType::NULLS_LAST) {		return false;
	}
	if (order.expression->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {		return false;
	}
	auto &order_col = order.expression->Cast<BoundColumnRefExpression>();
	(void)order_col;

	if (top_n.children.size() != 1 || top_n.children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION) {		return false;
	}
	auto &projection = top_n.children[0]->Cast<LogicalProjection>();
	if (projection.children.size() != 1 || projection.children[0]->type != LogicalOperatorType::LOGICAL_GET) {		return false;
	}
	auto &get = projection.children[0]->Cast<LogicalGet>();
	if (get.function.name != "seq_scan" || !get.GetTable()) {		return false;
	}
	if (!get.table_filters.filters.empty() && (get.dynamic_filters && get.dynamic_filters->HasFilters())) {
		return false;
	}

	// The projection expression the TopN orders by.
	const idx_t order_index = order_col.binding.column_index;
	if (order_index >= projection.expressions.size()) {
		return false;
	}
	const Expression &distance_expr = *projection.expressions[order_index];

	// Metric kind from the expression's function: array_distance (L2,
	// ASC) or array_inner_product (IP, DESC). The direction must match.
	{
		bool expr_is_ip = false;
		if (distance_expr.GetExpressionType() == ExpressionType::BOUND_FUNCTION) {
			const auto &name = distance_expr.Cast<BoundFunctionExpression>().function.name;
			expr_is_ip = name == "array_inner_product";
		}
		const bool want_desc = expr_is_ip;
		if (order.type != (want_desc ? OrderType::DESCENDING : OrderType::ASCENDING)) {			return false;
		}
	}

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
			// Unusable sidecar (deleted file, moved directory, mismatched
			// tree UUID): fail loudly on actual use instead of silently
			// falling back to the exact path — a swapped tree would
			// otherwise be invisible. DROP INDEX still works (it never
			// comes through here).
			if (index.sidecar_unusable) {
				throw InvalidInputException(index.sidecar_error);
			}
			continue;
		}
		// The expression kind must match the tree's metric
		// (array_distance ↔ L2, array_inner_product ↔ IP).
		const bool tree_is_ip = sextant_index_metric(handle) == SEXTANT_METRIC_IP;
		const bool expr_is_ip = distance_expr.GetExpressionType() == ExpressionType::BOUND_FUNCTION &&
		                        distance_expr.Cast<BoundFunctionExpression>().function.name == "array_inner_product";
		if (tree_is_ip != expr_is_ip) {			continue;
		}
		// The index must be over exactly the column the distance
		// expression references.
		if (index.GetColumnIds().size() != 1) {			continue;
		}

		vector<float> query;
		// Per-candidate filter translation (all-or-nothing).
		vector<SextantScanPredicate> preds;
		vector<unique_ptr<Expression>> filter_exprs;
		bool preds_exact = true;
		const bool has_filters = !get.table_filters.filters.empty();		if (has_filters &&
		    !TryTranslateTableFilters(get, table, handle, preds, filter_exprs, preds_exact)) {
			return false;
		}		// (the get). NOTE: LogicalGet::GetColumnBindings() is pruned by
		// projection_ids (column-lifetime analyzer), so it can be
		// shorter than column_ids — probe the canonical per-position
		// binding for every occurrence of the indexed column as well.
		const auto &get_bindings = get.GetColumnBindings();
		const auto &get_columns = get.GetColumnIds();
		for (idx_t col = 0; col < get_columns.size(); col++) {
			if (get_columns[col].GetPrimaryIndex() != static_cast<idx_t>(index.GetColumnIds()[0])) {
				continue;
			}
			bool matched = false;
			bool is_ip = false;
			if (col < get_bindings.size() && get_bindings[col].column_index == col) {
				matched = TryMatchDistanceExpression(distance_expr, get_bindings[col], query, is_ip);
			}
			if (!matched) {
				matched = TryMatchDistanceExpression(
				    distance_expr, ColumnBinding(get.table_index, static_cast<column_t>(col)), query, is_ip);
			}
			(void)is_ip; // metric agreement checked above
			if (!matched) {
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
			bind_data->predicates = std::move(preds);
			if (!bind_data->predicates.empty() && !preds_exact) {
				// Legacy tree (non-nullable columns): engine/SQL semantics
				// diverge on NULLs, so re-filter exactly and overfetch so the
				// re-filter can still fill k rows.
				bind_data->k = top_n.limit * 4 + 64;
			}

			if (!filter_exprs.empty() && !preds_exact) {
				// Legacy tree: re-inject the pushed-down filters between
				// projection and scan for exact SQL semantics. Nullable
				// trees evaluate predicates with SQL semantics already.
				auto filter = make_uniq<LogicalFilter>();
				filter->expressions = std::move(filter_exprs);
				filter->children.push_back(std::move(projection.children[0]));
				projection.children[0] = std::move(filter);
			}
			get.table_filters.filters.clear();
			// Advertise every scanned column (filter-only columns were
			// pruned from the projection by filter_prune; the re-injected
			// LogicalFilter references them by canonical binding).
			get.projection_ids.clear();
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
