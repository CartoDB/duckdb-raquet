name: "Fix read-side tile_matrix_set blind spot from PR #13"
description: |

## Purpose

Complete the tile_matrix_set (TMS) support that PR #13 (`feat/wgs84-tile-matrix-set`) added to `read_raster()`, by making the *read* path (point queries and pixel-value extraction) TMS-aware too. PR #13 only updated the *write* path — reading a `GoogleCRS84Quad` file back through this repo's own functions today silently returns wrong data.

## Core Principles

1. **Byte-identical WebMercatorQuad**: every change here must be a no-op for existing WebMercatorQuad files/queries.
2. **Fail loud, not silent**: an unrecognized `tile_matrix_set` must raise a clear error, never fall back to Mercator silently.
3. **Mirror existing patterns**: reuse the exact style PR #13 already established (`TileMatrixSet` struct, `ScalarFunctionSet` overloads, macro CTEs) — don't introduce new abstractions.
4. **No polymorphism**: keep `TileMatrixSet` a small value type (enum + branch), not a class hierarchy — this was explicitly discussed and rejected as over-engineering for a closed 2-member set (YAGNI).

---

## Goal

Make `ST_RasterAt`, `read_raquet_at`, `ST_Raster` (point-query overloads), `ST_RasterValue`, `quadbin_from_lonlat`, `quadbin_to_lonlat`, and `quadbin_pixel_xy` correctly honor a file's `tile_matrix_set` (`WebMercatorQuad` default, or `GoogleCRS84Quad`), instead of unconditionally assuming Web Mercator.

## Why

- PR #13 lets `read_raster()` tag files as `GoogleCRS84Quad`, but every read-side point-lookup function in this repo still hardcodes Web Mercator math (`quadbin::lonlat_to_cell`, `quadbin::lonlat_to_pixel`).
- Net effect: `read_raster('x.tif', tile_matrix_set='GoogleCRS84Quad')` produces a file that this repo's own `ST_RasterAt`/`read_raquet_at` then reads back **incorrectly and silently** — computing the wrong `block`/pixel because it derives cell coordinates via the Mercator formula against CRS84-indexed cells.
- Reported as a review comment on PR #13: https://github.com/CartoDB/duckdb-raquet/pull/13#issuecomment-4958767246
- Without this fix, the new feature is unusable end-to-end even before external consumers (deck.gl/carto, explicitly deferred by PR #13 itself) catch up — it's broken for this repo's own SQL surface today.

## What

- `ST_RasterValue` picks the correct pixel for a `GoogleCRS84Quad` block (it already receives `metadata` as an argument — the TMS is one `parse_metadata()` call away).
- `quadbin_from_lonlat`/`quadbin_to_lonlat`/`quadbin_pixel_xy` gain an optional `tile_matrix_set VARCHAR` argument (new overload; existing 3-arg/1-arg/4-arg signatures untouched).
- `ST_RasterAt`/`read_raquet_at` (and the geometry-taking `ST_RasterAt` overloads) look up the file's `tile_matrix_set` from its metadata and pass it through to `quadbin_from_lonlat`.
- Anything passed as `tile_matrix_set` that isn't `WebMercatorQuad`/`GoogleCRS84Quad` (case-insensitive) raises `InvalidInputException` with a clear message.
- Geometry-based `ST_Raster(tbl, geometry[, resolution])`, which uses `QUADBIN_POLYFILL` rather than `quadbin_from_lonlat`, is explicitly **out of scope** — call this out in the PR description, don't silently leave it half-fixed.

### Success Criteria

- [ ] `raquet_parse_metadata` exposes `tile_matrix_set` and `crs` as struct fields
- [ ] `quadbin.hpp`'s `TileMatrixSet` gains `tile_to_lonlat`, `lonlat_to_cell`, `cell_to_lonlat`, `lonlat_to_pixel` member methods; WebMercatorQuad branch delegates to the pre-existing free functions (bit-identical)
- [ ] `quadbin_from_lonlat`, `quadbin_to_lonlat`, `quadbin_pixel_xy` each gain a TMS-aware overload registered via `ScalarFunctionSet`, existing overloads untouched
- [ ] `ST_RasterValue` (both overloads) uses the TMS from its own `metadata` argument
- [ ] `ST_RasterAt` / `read_raquet_at` macros pass the file's `tile_matrix_set` through
- [ ] Unknown `tile_matrix_set` string raises a clear `InvalidInputException` in the new scalar overloads
- [ ] New tests pass; the full existing `.test` suite stays green with byte-identical WebMercatorQuad output

## All Needed Context

### Documentation & References

```yaml
- url: https://github.com/CartoDB/duckdb-raquet/pull/13
  why: The feature this PRP completes. Read the description and diff for the write-side TileMatrixSet pattern.

- url: https://github.com/CartoDB/duckdb-raquet/pull/13#issuecomment-4958767246
  why: The review comment that reported this exact bug (silent misread via ST_RasterAt/read_raquet_at), with a concrete repro.

- file: src/include/quadbin.hpp
  why: |
    The `TileMatrixSet` struct (added by PR #13, ~line 305-388) is the pattern to extend.
    It already has write-side methods: `lonlat_to_tile`, `tile_to_bbox_projected`, `tile_to_bbox_wgs84`.
    WebMercatorQuad always delegates to the pre-existing free functions (`quadbin::lonlat_to_tile` etc.)
    so its output stays byte-identical — every new method added here MUST follow that same
    if-WebMercatorQuad-delegate-else-CRS84-math shape.
    The free functions this PRP needs read-side equivalents for: `lonlat_to_pixel` (~line 391-417),
    `cell_to_lonlat` (~line 133-141), `tile_to_lonlat` (~line 125-130). `tile_to_cell`/`cell_to_tile`
    (~line 35, 63) are projection-agnostic — reuse them as-is, don't touch them.

- file: src/quadbin/quadbin_functions.cpp
  why: |
    `QuadbinFromLonLatFunction` (~line 341), `QuadbinToLonLatFunction` (~line 354),
    `QuadbinPixelXYFunction` (~line 420) are the scalar functions to add TMS-aware overloads for.
    Registration is at ~line 942-989 (currently single-overload `ScalarFunction(...)` +
    `loader.RegisterFunction(...)`).
    Pattern to mirror for adding a same-name overload: `quadbin_to_parent`/`quadbin_to_children`
    at ~line 1049-1070 use `ScalarFunctionSet` + `.AddFunction(...)` + a single
    `loader.RegisterFunction(set)` — this repo already has 2+ overloads of the same function name
    registered this way, so it's a proven pattern here, not a new one.

- file: src/raster/st_raster_value.cpp
  why: |
    `STRasterValueWithGeometryFunction` (~line 363-442) and
    `STRasterValueWithGeometryAndBandNameFunction` (~line 447-537) are the cheapest fix in this PRP:
    both already take `metadata VARCHAR` as an argument and already call
    `raquet::parse_metadata(metadata_str)` (~line 394, ~481) to get `meta`. Since PR #13,
    `meta.tile_matrix_set` is already populated (defaults to "WebMercatorQuad" for old files).
    The only change needed is swapping the direct call
    `quadbin::lonlat_to_pixel(lon, lat, resolution, tile_size, pixel_x, pixel_y, calc_tile_x, calc_tile_y)`
    (~line 403, ~498) for `quadbin::TileMatrixSet::FromName(meta.tile_matrix_set).lonlat_to_pixel(...)`.
    No signature/macro changes needed for this file.

- file: src/raquet_extension.cpp
  why: |
    `RAQUET_TABLE_AT_MACROS` (~line 160-208, `ST_RasterAt` from a table) and
    `RAQUET_AT_TABLE_MACROS` (~line 214-262, `read_raquet_at` from a parquet file) are raw SQL
    string literals calling `quadbin_from_lonlat(lon, lat, resolution)`. They already have a
    `table_resolution`/`file_resolution` CTE pattern
    (`SELECT (raquet_parse_metadata(metadata)).max_zoom AS res FROM ... WHERE block = 0 LIMIT 1`)
    to mirror for a new `tile_matrix_set` CTE. There are 6 call sites of `quadbin_from_lonlat`
    across these two macro arrays (3 overloads each) — grep to confirm the exact count before starting.

- file: src/table_functions/raquet_table_functions.cpp
  why: |
    `RaquetParseMetadataFunction` (~line 33-76) and its registration (~line 124-140) define the
    struct fields `raquet_parse_metadata()` returns. `tile_matrix_set`/`crs` need to be added here
    following the existing VARCHAR field pattern (`compression`, `band_layout` — see
    `StringVector::AddString`, ~line 61/63).

- file: src/include/raquet_metadata.hpp
  why: "RaquetMetadata::tile_matrix_set already exists (added by PR #13) and defaults to \"WebMercatorQuad\" — reuse, don't reimplement the fallback."

- file: src/raster/read_raster.cpp
  why: |
    ~line 1203: the existing case-insensitive `tile_matrix_set` string validation
    (`"webmercatorquad"`/`"googlecrs84quad"` → canonical name, else `InvalidInputException`) in
    `ReadRasterBind`'s named-parameter parsing. Mirror this exact validation in the new scalar
    function overloads (Task 3) instead of relying on `TileMatrixSet::FromName`'s silent
    unknown-defaults-to-WebMercatorQuad fallback, which is correct for the metadata-parsing case
    but wrong for user-facing SQL input.

- file: test/sql/read_raquet_at.test
  why: |
    Existing pattern for point-query tests: hand-built parquet fixture with a metadata row
    (block=0) and pre-computed block IDs for known lon/lat. Mirror the structure, but for the new
    GoogleCRS84Quad test prefer ingesting via `read_raster()` (see next reference) over
    hand-computing a CRS84 cell ID by hand — much less error-prone.

- file: test/sql/tile_matrix_set.test
  why: |
    PR #13's own ingest test: `read_raster('test/data/test_palette.tif', tile_matrix_set='GoogleCRS84Quad', overviews='none')`
    against a small already-WGS84 fixture, gated on `require json` (GDAL-backed). Reuse this exact
    fixture and gating for the new read-path tests so no new test data needs to be created.

- file: test/sql/st_raster_macro.test
  why: Existing pattern for exercising ST_RasterAt/ST_Raster/ST_RasterValue together — check for conventions to follow (setup, teardown, column selection).
```

### Current Codebase tree (relevant subset)

```bash
src/
  include/
    quadbin.hpp              # TileMatrixSet struct (write-side, PR #13) — EXTEND (read-side methods)
    raquet_metadata.hpp       # RaquetMetadata.tile_matrix_set (already added by PR #13) — no change
  quadbin/
    quadbin_functions.cpp     # quadbin_from_lonlat / quadbin_to_lonlat / quadbin_pixel_xy — EXTEND (new overloads)
    quadbin_polyfill.cpp      # QUADBIN_POLYFILL — OUT OF SCOPE for this PRP
  raster/
    read_raster.cpp           # tile_matrix_set validation pattern to mirror (~line 1203) — reference only
    st_raster_value.cpp       # ST_RasterValue — EXTEND (use TMS from its own metadata arg)
  table_functions/
    raquet_table_functions.cpp # raquet_parse_metadata — EXTEND (expose tile_matrix_set/crs)
  raquet_extension.cpp        # ST_RasterAt / read_raquet_at / ST_Raster SQL macros — EXTEND (pass TMS through)
test/sql/
  read_raquet_at.test         # pattern reference
  st_raster_macro.test        # pattern reference
  tile_matrix_set.test        # EXTEND with read-path cases, or add tile_matrix_set_read.test alongside it
  metadata_validation.test    # must stay green, unchanged
  merge_bands.test            # must stay green, unchanged
```

### Desired Codebase tree (files touched, no new files except tests)

```bash
src/include/quadbin.hpp                       # + TileMatrixSet::{tile_to_lonlat, lonlat_to_cell, cell_to_lonlat, lonlat_to_pixel}
src/quadbin/quadbin_functions.cpp              # + TMS-aware overloads for quadbin_from_lonlat/quadbin_to_lonlat/quadbin_pixel_xy
src/raster/st_raster_value.cpp                 # ~2 call sites: quadbin::lonlat_to_pixel -> tms.lonlat_to_pixel
src/table_functions/raquet_table_functions.cpp # + tile_matrix_set, crs struct fields
src/raquet_extension.cpp                       # 6 macro call sites: quadbin_from_lonlat(...) -> quadbin_from_lonlat(..., tms)
test/sql/tile_matrix_set.test                  # + read-path test cases (or a new sibling .test file)
```

### Known Gotchas of our codebase & Library Quirks

```cpp
// CRITICAL: WebMercatorQuad output must stay byte-identical. Every new TileMatrixSet method
// must delegate to the pre-existing free function for id == TmsId::WebMercatorQuad, and only
// branch into new math for GoogleCRS84Quad — this is the same P1 regression guarantee PR #13
// established for the write-side methods.

// CRITICAL: TileMatrixSet::FromName() intentionally treats any unrecognized name as
// WebMercatorQuad (silent fallback) — that's correct when parsing trusted file metadata written
// by this same extension, but WRONG for user-supplied SQL arguments. New scalar function
// overloads (quadbin_from_lonlat(..., tile_matrix_set), etc.) must validate the string
// case-insensitively themselves (mirror src/raster/read_raster.cpp ~line 1203) and throw
// InvalidInputException on anything else, BEFORE calling FromName.

// CRITICAL: DuckDB same-name function overloading requires ScalarFunctionSet, not two separate
// loader.RegisterFunction(ScalarFunction(...)) calls with the same name — see
// quadbin_to_parent/quadbin_to_children in quadbin_functions.cpp (~line 1049-1070) for the
// existing, working pattern in this codebase.

// GOTCHA: raquet_extension.cpp macros are parsed once at extension load time from raw SQL string
// literals (CreateTableMacroFunction). There is no per-call templating — the tile_matrix_set CTE
// must be valid SQL text added directly into each macro's R"(...)" block, following the existing
// table_resolution/file_resolution CTE pattern exactly.

// GOTCHA: raquet::parse_metadata already defaults tile_matrix_set to "WebMercatorQuad" when the
// JSON key is absent (pre-0.6.0 files) — don't re-implement that fallback in the SQL macros or
// scalar functions, just read (raquet_parse_metadata(metadata)).tile_matrix_set directly.

// GOTCHA: quadbin::tile_to_cell / quadbin::cell_to_tile are projection-agnostic (pure quadkey
// packing) — never touch them. Only the lon/lat <-> tile conversions are projection-specific.
```

## Implementation Blueprint

### Data models and structure

No new structs. Extend `quadbin::TileMatrixSet` (src/include/quadbin.hpp) with read-side methods that mirror its existing write-side methods 1:1 in style.

```cpp
// Added to struct TileMatrixSet (quadbin.hpp), after tile_to_bbox_wgs84:

void tile_to_lonlat(int x, int y, int z, double &lon, double &lat) const {
    if (id == TmsId::WebMercatorQuad) {
        quadbin::tile_to_lonlat(x, y, z, lon, lat);
        return;
    }
    double n = std::pow(2.0, z);
    lon = (x + 0.5) / n * 360.0 - 180.0;
    lat = 90.0 - (y + 0.5) / n * 180.0;
}

uint64_t lonlat_to_cell(double lon, double lat, int z) const {
    int x, y;
    lonlat_to_tile(lon, lat, z, x, y);          // existing TMS-aware member
    return quadbin::tile_to_cell(x, y, z);      // projection-agnostic free function
}

void cell_to_lonlat(uint64_t cell, double &lon, double &lat) const {
    int x, y, z;
    quadbin::cell_to_tile(cell, x, y, z);       // projection-agnostic free function
    tile_to_lonlat(x, y, z, lon, lat);
}

void lonlat_to_pixel(double lon, double lat, int z, int tile_size,
                      int &pixel_x, int &pixel_y, int &tile_x, int &tile_y) const {
    if (id == TmsId::WebMercatorQuad) {
        quadbin::lonlat_to_pixel(lon, lat, z, tile_size, pixel_x, pixel_y, tile_x, tile_y);
        return;
    }
    double lat_c = lat;
    double ml = max_latitude();
    if (lat_c > ml) lat_c = ml;
    if (lat_c < -ml) lat_c = -ml;
    double n = std::pow(2.0, z);
    double tile_x_frac = (lon + 180.0) / 360.0 * n;
    double tile_y_frac = (90.0 - lat_c) / 180.0 * n;
    tile_x = static_cast<int>(std::floor(tile_x_frac));
    tile_y = static_cast<int>(std::floor(tile_y_frac));
    pixel_x = static_cast<int>((tile_x_frac - tile_x) * tile_size);
    pixel_y = static_cast<int>((tile_y_frac - tile_y) * tile_size);
    if (pixel_x >= tile_size) pixel_x = tile_size - 1;
    if (pixel_y >= tile_size) pixel_y = tile_size - 1;
    if (pixel_x < 0) pixel_x = 0;
    if (pixel_y < 0) pixel_y = 0;
}
```

### List of tasks (in order — TDD: tests first, then make them pass)

Per this project's CLAUDE.md ("TDD: Red-Green-Refactor. Write failing tests first, make them
pass, then refactor"), Task 1 below writes every new test up front. Confirm they fail for the
*right* reason (missing 4-arg overload / wrong pixel from Mercator-only math) before touching any
implementation file — this also double-checks the existing suite still passes untouched, giving a
clean baseline to diff against. Tasks 2-6 each turn a specific subset of Task 1's red tests green;
Task 7 is the final full-suite regression pass.

```yaml
Task 1 (RED — write first, must fail before any implementation task):
MODIFY test/sql/tile_matrix_set.test (or CREATE test/sql/tile_matrix_set_read.test):
  - ADD: ingest fixture via read_raster('test/data/test_palette.tif', tile_matrix_set='GoogleCRS84Quad', overviews='none')
    COPY'd to a parquet file (mirror the existing round-trip block in this same test file)
  - ADD: ST_RasterAt / read_raquet_at against that fixture at a lon/lat inside its bounds -> exactly 1 row, non-NULL value
  - ADD: quadbin_from_lonlat(lon, lat, z, 'GoogleCRS84Quad') != quadbin_from_lonlat(lon, lat, z) for a non-equatorial point
  - ADD: quadbin_from_lonlat(lon, lat, z, 'BogusQuad') -> statement error
  - ADD: quadbin_to_lonlat(cell, 'GoogleCRS84Quad') round-trips within ~1 pixel of the original point
  - PRESERVE: all existing test cases in this file untouched (byte-identical guarantee — these
    must already pass on the unmodified codebase; if they don't, stop and fix the baseline first)
  RUN: make -j8 && build/release/test/unittest --test-dir . "test/sql/tile_matrix_set*.test"
  EXPECT (RED): build succeeds (no C++ touched yet), but the new assertions fail —
    "Binder Error: No function matches" for the 4-arg quadbin_from_lonlat / quadbin_to_lonlat
    overloads, and ST_RasterAt/read_raquet_at against the CRS84 fixture return 0 rows or the wrong
    block (Mercator math applied to CRS84 cells). If a new assertion passes at this point, the
    test itself is not exercising the bug — revisit it before moving on.

Task 2 (GREEN groundwork — no test turns green yet, but unblocks Tasks 3-5):
MODIFY src/include/quadbin.hpp:
  - FIND struct: "struct TileMatrixSet {" ... "tile_to_bbox_wgs84(...)"
  - INJECT after tile_to_bbox_wgs84's closing brace, before the struct's closing "};"
  - ADD: tile_to_lonlat, lonlat_to_cell, cell_to_lonlat, lonlat_to_pixel (see pseudocode above)
  - PRESERVE: every existing free function and method untouched
  RUN: make -j8 (compiles; still no observable test change, this is pure plumbing)

Task 3 (unrelated GREEN — independent of Task 1's assertions, but needed for completeness):
MODIFY src/table_functions/raquet_table_functions.cpp:
  - FIND: "meta_struct.push_back(make_pair(\"tile_statistics\", LogicalType::BOOLEAN));"
  - INJECT after: two more push_back calls for "tile_matrix_set" and "crs" (both VARCHAR)
  - FIND: "FlatVector::GetData<bool>(tile_statistics_vec)[i] = meta.tile_statistics;"
  - INJECT after: matching StringVector::AddString(...) assignments for meta.tile_matrix_set and meta.crs
  - PRESERVE: struct field order for existing fields (additive only, append at the end)
  - CONSIDER: add one more Task-1-style assertion here first if you want this field itself
    covered directly, e.g. `SELECT (raquet_parse_metadata(metadata)).tile_matrix_set = 'GoogleCRS84Quad'`
    — optional, since Task 6's macro tests exercise it indirectly.

Task 4 (GREEN — turns the quadbin_from_lonlat/quadbin_to_lonlat assertions from Task 1 green):
MODIFY src/quadbin/quadbin_functions.cpp:
  - CREATE: QuadbinFromLonLatTmsFunction (4-arg: lon, lat, resolution, tile_matrix_set VARCHAR)
    - MIRROR validation from src/raster/read_raster.cpp ~line 1203 (case-insensitive, throw InvalidInputException on unknown)
    - CALL quadbin::TileMatrixSet::FromName(canonical_name).lonlat_to_cell(lon, lat, resolution)
  - CREATE: QuadbinToLonLatTmsFunction (2-arg: cell, tile_matrix_set VARCHAR) mirroring QuadbinToLonLatFunction
  - CREATE: QuadbinPixelXYTmsFunction (5-arg: lon, lat, resolution, tile_size, tile_matrix_set VARCHAR) mirroring QuadbinPixelXYFunction
  - MODIFY registration (~line 942-989): wrap each of quadbin_from_lonlat / quadbin_to_lonlat / quadbin_pixel_xy
    in a ScalarFunctionSet with the existing signature as one AddFunction(...) and the new TMS-aware
    signature as a second AddFunction(...), following quadbin_to_parent's pattern (~line 1049-1061)
  - PRESERVE: existing 3-arg/1-arg/4-arg overloads' behavior and registration order
  RUN: make -j8 && build/release/test/unittest --test-dir . "test/sql/tile_matrix_set*.test"
  EXPECT (partial GREEN): the quadbin_from_lonlat/quadbin_to_lonlat TMS assertions from Task 1
    now pass; the ST_RasterAt/read_raquet_at/ST_RasterValue-backed assertions are still red
    (macros don't pass tile_matrix_set through yet, st_raster_value.cpp still Mercator-only).

Task 5 (GREEN — turns the ST_RasterValue-backed assertions from Task 1 green):
MODIFY src/raster/st_raster_value.cpp:
  - FIND (x2, one per function): "quadbin::lonlat_to_pixel(lon, lat, resolution, tile_size, pixel_x, pixel_y, calc_tile_x, calc_tile_y);"
  - REPLACE with:
      auto tms = quadbin::TileMatrixSet::FromName(meta.tile_matrix_set);
      tms.lonlat_to_pixel(lon, lat, resolution, tile_size, pixel_x, pixel_y, calc_tile_x, calc_tile_y);
  - PRESERVE: everything else in both functions unchanged (meta already parsed just above each call site)
  RUN: make -j8 && build/release/test/unittest --test-dir . "test/sql/tile_matrix_set*.test"
  EXPECT: any assertion exercising ST_RasterValue directly against a GoogleCRS84Quad block is now green.

Task 6 (GREEN — turns the ST_RasterAt/read_raquet_at assertions from Task 1 green):
MODIFY src/raquet_extension.cpp:
  - FIND all 6 occurrences of "quadbin_from_lonlat(" inside RAQUET_TABLE_AT_MACROS and RAQUET_AT_TABLE_MACROS
  - For each macro's SQL text, ADD a CTE (mirroring table_resolution/file_resolution) e.g.:
      table_tms AS (
          SELECT (raquet_parse_metadata(metadata)).tile_matrix_set AS tms
          FROM src WHERE block = 0 LIMIT 1
      )
    (or file_tms / read_parquet(file) for the read_raquet_at macros)
  - REPLACE "quadbin_from_lonlat(<args>, resolution)" with
    "quadbin_from_lonlat(<args>, resolution, (SELECT tms FROM table_tms))" (or file_tms)
  - PRESERVE: macro parameter lists, overload arities, existing CTE names (table_resolution/file_resolution)
  - OUT OF SCOPE: RAQUET_FROM_TABLE_MACROS' geometry-based QUADBIN_POLYFILL overloads — do not touch
  RUN: make -j8 && build/release/test/unittest --test-dir . "test/sql/tile_matrix_set*.test"
  EXPECT: every assertion written in Task 1 is now green — this closes the loop.

Task 7 (REFACTOR + full regression):
RUN full build + full test suite (see Validation Loop below). This is the "Refactor" step of
Red-Green-Refactor: with everything green, revisit naming/duplication introduced across Tasks
2-6 (e.g. do the 6 macro edits in Task 6 share enough shape to extract a comment explaining the
pattern, even if the SQL itself must stay inlined per DuckDB's macro-parsing constraints) before
calling this done. Fix any regressions in the full suite before considering done.
```

## Integration Points

```yaml
DUCKDB EXTENSION REGISTRATION:
  - file: src/raquet_extension.cpp, function LoadInternal
  - no new Register*Functions() entry points needed — all changes are inside existing
    RegisterQuadbinFunctions / RegisterRasterValueFunctions / RegisterRaquetTableFunctions bodies,
    or in the existing macro arrays consumed by CreateReadRaquetTableAtMacroInfo /
    CreateReadRaquetAtMacroInfo.

TESTS:
  - require raquet / require parquet / require json gating, matching test/sql/tile_matrix_set.test
```

## Validation Loop

### Level 0: Confirm RED (run once, right after Task 1, before any implementation change)

```bash
make -j8 && build/release/test/unittest --test-dir . "test/sql/tile_matrix_set*.test"
# Expected: build succeeds unchanged; the tests added in Task 1 fail — either a Binder Error
# ("no function matches") for the new 4/5-arg overloads, or a wrong/missing row for
# ST_RasterAt/read_raquet_at against the GoogleCRS84Quad fixture. If everything already passes
# here, the new assertions aren't actually exercising the bug — fix the tests before proceeding.
```

### Level 1: Build (after each of Tasks 2-6)

```bash
make -j8
# Expected: clean build. If the TMS branch inside new methods uses if/else on TmsId, consider
# switch(id) instead so a future third TmsId value trips a compiler warning (-Wswitch) here too —
# not required, but consistent with the "no silent unknown-TMS" principle of this fix.
```

### Level 2: Targeted tests (after each of Tasks 4, 5, 6 — watch red turn green incrementally)

```bash
build/release/test/unittest --test-dir . "test/sql/tile_matrix_set*.test"
```

### Level 3: Full regression suite (after Task 6, and again after Task 7's refactor pass)

```bash
build/release/test/unittest --test-dir . "test/sql/*"
# Expected: 100% pass. Pay special attention to read_raquet_at.test, st_raster_macro.test,
# raster.test, tile_matrix_set.test, metadata_validation.test, merge_bands.test — none of their
# existing expected outputs should change (WebMercatorQuad byte-identical guarantee).
```

### Level 4: Manual smoke test

```bash
build/release/duckdb -c "
COPY (SELECT * FROM read_raster('test/data/test_palette.tif', tile_matrix_set='GoogleCRS84Quad', overviews='none'))
TO '/tmp/crs84_test.parquet' (FORMAT parquet);
SELECT * FROM read_raquet_at('/tmp/crs84_test.parquet', -3.7, 40.4);
"
# Expected: exactly one row, non-NULL pixel value (fixture is a small palette raster; pick a
# lon/lat known to be within test/data/test_palette.tif's WGS84 bounds).
```

## Final validation Checklist

- [ ] `make -j8` clean
- [ ] `test/sql/tile_matrix_set*.test` green
- [ ] Full `test/sql/*` suite green, no changes to existing WebMercatorQuad expected outputs
- [ ] Unknown `tile_matrix_set` string raises `InvalidInputException`, not a silent Mercator fallback
- [ ] `ST_RasterValue`, `ST_RasterAt`, `read_raquet_at` all correctly resolve `GoogleCRS84Quad` files
- [ ] No inheritance/polymorphism introduced into `TileMatrixSet`
- [ ] Geometry-based `ST_Raster`/`QUADBIN_POLYFILL` gap explicitly called out as out-of-scope in the commit/PR description, not silently left unmentioned
- [ ] `raquet_parse_metadata`'s new fields are additive (existing callers unaffected)

---

## Anti-Patterns to Avoid

- ❌ Don't turn `TileMatrixSet` into a polymorphic class hierarchy (`ITileMatrixSet` + subclasses) — explicitly discussed and rejected for this codebase's scope (2 known TMS, no third-party plugins expected). Stick to the enum + delegate-or-branch style already established by PR #13.
- ❌ Don't change WebMercatorQuad's numeric output even by floating-point epsilon — always delegate to the pre-existing free function for that branch.
- ❌ Don't let `TileMatrixSet::FromName`'s silent unknown→WebMercatorQuad fallback reach user-facing SQL arguments — validate explicitly first, exactly like `ReadRasterBind` does.
- ❌ Don't hand-compute QUADBIN cell IDs for the new CRS84 fixtures — ingest via `read_raster()` and cross-check self-consistently, like `test/sql/tile_matrix_set.test` already does.
- ❌ Don't silently extend this fix to geometry-based `ST_Raster`/`QUADBIN_POLYFILL` — that's a separate, larger piece of undeclared scope creep; call it out instead.
- ❌ Don't register a second same-name `ScalarFunction` via a second bare `loader.RegisterFunction(...)` call — use `ScalarFunctionSet` as the existing `quadbin_to_parent`/`quadbin_to_children` functions do.

---

## Confidence Score: 8/10

Reasoning: every file, line range, and pattern cited above was read directly from PR #13's actual branch (`pr-13-review` / commit `cc42447`) during research for this PRP, not inferred — including the crucial discovery that `ST_RasterValue` already receives `metadata` and thus needs no signature change. The main residual risk is the exact DuckDB C++ API surface for `ScalarFunctionSet`/`AddFunction` (verify against the `quadbin_to_parent`/`quadbin_to_children` precedent already in this file before writing new code) and getting the SQL macro CTE edits exactly syntactically right across all 6 call sites on the first pass — both are mechanical once the pattern is confirmed, not open design questions.
