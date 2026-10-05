# This file is included by DuckDB's build system

duckdb_extension_load(raquet
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/src/include
    LOAD_TESTS
)

# Include parquet (required for read_parquet)
duckdb_extension_load(parquet)

# Include json: most read_raster / metadata tests `require json`, and without it
# the unittest runner silently skips them.
duckdb_extension_load(json)
