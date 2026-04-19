// ============================================================================
// ZeptoDB: Migration CLI utilities (devlog 086, D2)
// ----------------------------------------------------------------------------
// Shared helpers for `zepto-migrate` and friends. Header-only so tests can
// exercise the CLI-level wrapping logic without linking against the tool
// binary.
// ============================================================================
#pragma once

#include "zeptodb/storage/schema_registry.h"

#include <filesystem>
#include <string>

namespace zeptodb::migration {

// Ensure `dest_table` exists in the SchemaRegistry at `{hdb_dir}/_schema.json`.
// - Loads any existing registry (ignores missing file).
// - Creates the table (empty column list) if absent — subsequent SQL DDL can
//   still add columns; migrator writes the on-disk column files directly.
// - Saves atomically back to disk so the next pipeline startup picks it up.
//
// Returns the assigned `table_id` on success, or 0 if `hdb_dir` / `name` is
// empty or the create/save failed.
inline uint16_t ensure_dest_table(const std::string& hdb_dir,
                                  const std::string& name) {
    if (hdb_dir.empty() || name.empty()) return 0;

    std::error_code ec;
    std::filesystem::create_directories(hdb_dir, ec);  // best-effort

    const std::string schema_path = hdb_dir + "/_schema.json";
    zeptodb::storage::SchemaRegistry reg;
    reg.load_from(schema_path);  // ignore failure (fresh dir)

    if (!reg.exists(name)) {
        if (!reg.create(name, {})) return 0;
    }
    if (!reg.save_to(schema_path)) return 0;
    return reg.get_table_id(name);
}

}  // namespace zeptodb::migration
