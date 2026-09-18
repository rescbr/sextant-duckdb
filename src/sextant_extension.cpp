#define DUCKDB_EXTENSION_MAIN

#include "sextant_extension.hpp"
#include "sextant_index.hpp"
#include "sextant_scan.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include "sextant/sextant_c.h"

namespace duckdb {

// Smoke: proves the C seam is linked and callable.
inline void SextantVersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		sextant_build_opts opts = sextant_default_build_opts();
		return StringVector::AddString(
		    result, "sextant (" + name.GetString() + "): libsextant_c linked, k_root default=" +
		                std::to_string(opts.k_root));
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(
	    ScalarFunction("sextant_version", {LogicalType::VARCHAR}, LogicalType::VARCHAR, SextantVersionFun));
	RegisterSextantIndexType(loader.GetDatabaseInstance());
	RegisterSextantImmutabilityOptimizer(loader.GetDatabaseInstance());
	RegisterSextantScanFunction(loader.GetDatabaseInstance());
}

void SextantExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string SextantExtension::Name() {
	return "sextant";
}

std::string SextantExtension::Version() const {
#ifdef EXT_VERSION_SEXTANT
	return EXT_VERSION_SEXTANT;
#else
	return "v0.0.1";
#endif
}

} // namespace duckdb
