# Produce a clang-tidy-safe compile DB. The Orthodoxy plugin is built against a
# different LLVM than clang-tidy; loading it (-fplugin=...orthodoxy...) crashes
# clang-tidy. Strip that token into a tidy-only copy of the DB — compile-time
# enforcement is unaffected, since only this copy is sanitized. No-op when the
# plugin is absent (the regex simply matches nothing).
#
# Invoked: cmake -DTIDY_DB_IN=<in.json> -DTIDY_DB_OUT_DIR=<dir> -P <this>.cmake

file(READ "${TIDY_DB_IN}" _db)
# The plugin path has no embedded spaces, so [^ "]* is a safe, non-crossing match;
# the " guard also covers the JSON-`arguments` array form. REGEX REPLACE is global.
string(REGEX REPLACE " -fplugin=[^ \"]*orthodoxy[^ \"]*" "" _db "${_db}")
file(MAKE_DIRECTORY "${TIDY_DB_OUT_DIR}")
file(WRITE "${TIDY_DB_OUT_DIR}/compile_commands.json" "${_db}")
