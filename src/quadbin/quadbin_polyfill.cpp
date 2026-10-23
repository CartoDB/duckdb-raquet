#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "quadbin.hpp"
#include "vector_args.hpp"
#include <vector>
#include <cstring>
#include <cmath>

namespace duckdb {

// ============================================================================
// Geometry helpers - extract coordinates from GEOMETRY (WKB) type
// ============================================================================

// Extract bounding box from a GEOMETRY value
static bool ExtractGeometryBoundingBox(const string_t &geom, double &min_x, double &min_y, double &max_x, double &max_y) {
    const uint8_t *data = reinterpret_cast<const uint8_t*>(geom.GetData());
    idx_t size = geom.GetSize();

    if (size < 5) return false;

    uint8_t byte_order = data[0];
    // Valid WKB byte orders: 0 (big-endian) or 1 (little-endian)
    if (byte_order > 1) return false;
    bool little_endian = (byte_order == 1);

    uint32_t geom_type;
    if (little_endian) {
        memcpy(&geom_type, data + 1, 4);
    } else {
        geom_type = (static_cast<uint32_t>(data[1]) << 24) |
                    (static_cast<uint32_t>(data[2]) << 16) |
                    (static_cast<uint32_t>(data[3]) << 8) |
                    static_cast<uint32_t>(data[4]);
    }

    uint32_t base_type = geom_type & 0xFF;
    idx_t offset = 5;

    // Handle SRID prefix
    if (geom_type & 0x20000000) {
        offset += 4;
    }

    // Initialize min/max values
    min_x = std::numeric_limits<double>::max();
    min_y = std::numeric_limits<double>::max();
    max_x = std::numeric_limits<double>::lowest();
    max_y = std::numeric_limits<double>::lowest();

    auto update_bounds = [&](double x, double y) {
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    };

    auto read_double_le = [&](idx_t pos) -> double {
        double val;
        memcpy(&val, data + pos, 8);
        return val;
    };

    if (base_type == 1) {  // POINT
        if (size < offset + 16) return false;
        double x = read_double_le(offset);
        double y = read_double_le(offset + 8);
        update_bounds(x, y);
        return true;
    } else if (base_type == 3) {  // POLYGON
        if (size < offset + 4) return false;
        uint32_t num_rings;
        memcpy(&num_rings, data + offset, 4);
        offset += 4;

        for (uint32_t r = 0; r < num_rings; r++) {
            if (size < offset + 4) return false;
            uint32_t num_points;
            memcpy(&num_points, data + offset, 4);
            offset += 4;

            if (size < offset + idx_t(num_points) * 16) return false;
            for (uint32_t p = 0; p < num_points; p++) {
                double x = read_double_le(offset + p * 16);
                double y = read_double_le(offset + p * 16 + 8);
                update_bounds(x, y);
            }
            offset += idx_t(num_points) * 16;
        }
        return true;
    } else if (base_type == 6) {  // MULTIPOLYGON
        if (size < offset + 4) return false;
        uint32_t num_polygons;
        memcpy(&num_polygons, data + offset, 4);
        offset += 4;

        for (uint32_t poly = 0; poly < num_polygons; poly++) {
            // Each polygon has its own WKB header
            if (size < offset + 9) return false;
            uint8_t poly_byte_order = data[offset];
            bool poly_le = (poly_byte_order == 1);
            offset += 5;  // Skip byte order + type

            // Handle SRID in nested polygon if present
            uint32_t poly_type;
            memcpy(&poly_type, data + offset - 4, 4);
            if (poly_type & 0x20000000) {
                offset += 4;
            }

            if (size < offset + 4) return false;
            uint32_t num_rings;
            memcpy(&num_rings, data + offset, 4);
            offset += 4;

            for (uint32_t r = 0; r < num_rings; r++) {
                if (size < offset + 4) return false;
                uint32_t num_points;
                memcpy(&num_points, data + offset, 4);
                offset += 4;

                if (size < offset + idx_t(num_points) * 16) return false;
                for (uint32_t p = 0; p < num_points; p++) {
                    double x = read_double_le(offset + p * 16);
                    double y = read_double_le(offset + p * 16 + 8);
                    update_bounds(x, y);
                }
                offset += idx_t(num_points) * 16;
            }
        }
        return true;
    } else if (base_type == 7) {  // GEOMETRYCOLLECTION
        // For geometry collections, recursively process each geometry
        if (size < offset + 4) return false;
        uint32_t num_geoms;
        memcpy(&num_geoms, data + offset, 4);
        offset += 4;

        // This is simplified - for full support we'd recursively parse each geometry
        // For now, scan all remaining bytes looking for coordinate pairs
        // This is a fallback approach that's less precise but handles complex cases
        while (offset + 16 <= size) {
            // Try to read as coordinates (heuristic)
            double x = read_double_le(offset);
            double y = read_double_le(offset + 8);

            // Basic sanity check for valid coordinates
            if (x >= -180.0 && x <= 180.0 && y >= -90.0 && y <= 90.0) {
                update_bounds(x, y);
            }
            offset += 8;  // Move forward in smaller steps to not miss coordinates
        }
        return (min_x <= max_x);
    }

    return false;
}

// Check if a point is inside a polygon ring using ray casting
static bool PointInRing(double px, double py, const double* ring_coords, uint32_t num_points) {
    bool inside = false;
    double x1, y1, x2, y2;

    for (uint32_t i = 0, j = num_points - 1; i < num_points; j = i++) {
        x1 = ring_coords[i * 2];
        y1 = ring_coords[i * 2 + 1];
        x2 = ring_coords[j * 2];
        y2 = ring_coords[j * 2 + 1];

        if (((y1 > py) != (y2 > py)) &&
            (px < (x2 - x1) * (py - y1) / (y2 - y1) + x1)) {
            inside = !inside;
        }
    }
    return inside;
}

// Check if a point is inside a polygon (handles holes)
static bool PointInPolygon(double px, double py, const uint8_t* data, idx_t& offset, idx_t size) {
    if (size < offset + 4) return false;
    uint32_t num_rings;
    memcpy(&num_rings, data + offset, 4);
    offset += 4;

    if (num_rings == 0) return false;

    // First ring is outer boundary
    if (size < offset + 4) return false;
    uint32_t outer_points;
    memcpy(&outer_points, data + offset, 4);
    offset += 4;

    if (size < offset + idx_t(outer_points) * 16) return false;

    std::vector<double> outer_coords(idx_t(outer_points) * 2);
    for (uint32_t i = 0; i < outer_points; i++) {
        memcpy(&outer_coords[i * 2], data + offset + i * 16, 8);
        memcpy(&outer_coords[i * 2 + 1], data + offset + i * 16 + 8, 8);
    }
    offset += idx_t(outer_points) * 16;

    // Check if inside outer boundary
    if (!PointInRing(px, py, outer_coords.data(), outer_points)) {
        // Skip remaining rings
        for (uint32_t r = 1; r < num_rings; r++) {
            if (size < offset + 4) return false;
            uint32_t ring_points;
            memcpy(&ring_points, data + offset, 4);
            offset += 4 + idx_t(ring_points) * 16;
        }
        return false;
    }

    // Check holes (if inside a hole, point is outside polygon)
    for (uint32_t r = 1; r < num_rings; r++) {
        if (size < offset + 4) return false;
        uint32_t hole_points;
        memcpy(&hole_points, data + offset, 4);
        offset += 4;

        if (size < offset + idx_t(hole_points) * 16) return false;

        std::vector<double> hole_coords(idx_t(hole_points) * 2);
        for (uint32_t i = 0; i < hole_points; i++) {
            memcpy(&hole_coords[i * 2], data + offset + i * 16, 8);
            memcpy(&hole_coords[i * 2 + 1], data + offset + i * 16 + 8, 8);
        }
        offset += idx_t(hole_points) * 16;

        if (PointInRing(px, py, hole_coords.data(), hole_points)) {
            return false;  // Inside a hole
        }
    }

    return true;
}

// Check if a point is inside a geometry
static bool PointInGeometry(double px, double py, const string_t &geom) {
    const uint8_t *data = reinterpret_cast<const uint8_t*>(geom.GetData());
    idx_t size = geom.GetSize();

    if (size < 5) return false;

    uint32_t geom_type;
    memcpy(&geom_type, data + 1, 4);

    uint32_t base_type = geom_type & 0xFF;
    idx_t offset = 5;

    // Handle SRID prefix
    if (geom_type & 0x20000000) {
        offset += 4;
    }

    if (base_type == 3) {  // POLYGON
        return PointInPolygon(px, py, data, offset, size);
    } else if (base_type == 6) {  // MULTIPOLYGON
        if (size < offset + 4) return false;
        uint32_t num_polygons;
        memcpy(&num_polygons, data + offset, 4);
        offset += 4;

        for (uint32_t poly = 0; poly < num_polygons; poly++) {
            // Skip WKB header for nested polygon
            if (size < offset + 5) return false;
            offset += 5;

            // Handle SRID in nested polygon if present
            uint32_t poly_type;
            memcpy(&poly_type, data + offset - 4, 4);
            if (poly_type & 0x20000000) {
                offset += 4;
            }

            idx_t poly_offset = offset;
            if (PointInPolygon(px, py, data, poly_offset, size)) {
                return true;
            }
            offset = poly_offset;
        }
        return false;
    }

    return false;
}

// Check if two bounding boxes intersect
static bool BBoxIntersects(double ax1, double ay1, double ax2, double ay2,
                           double bx1, double by1, double bx2, double by2) {
    return !(ax2 < bx1 || ax1 > bx2 || ay2 < by1 || ay1 > by2);
}

// Read one ring's coordinates as a flat [x0, y0, x1, y1, ...] list
static bool ReadRing(const uint8_t* data, idx_t& offset, idx_t size, std::vector<double>& ring) {
    if (size < offset + 4) return false;
    uint32_t num_points;
    memcpy(&num_points, data + offset, 4);
    offset += 4;

    if (size < offset + idx_t(num_points) * 16) return false;
    ring.resize(idx_t(num_points) * 2);
    for (uint32_t i = 0; i < num_points; i++) {
        memcpy(&ring[i * 2], data + offset + i * 16, 8);
        memcpy(&ring[i * 2 + 1], data + offset + i * 16 + 8, 8);
    }
    offset += idx_t(num_points) * 16;
    return true;
}

// Extract the rings of a POLYGON or MULTIPOLYGON (a POINT becomes a one-vertex ring)
static std::vector<std::vector<double>> ExtractRings(const string_t &geom) {
    std::vector<std::vector<double>> rings;
    const uint8_t *data = reinterpret_cast<const uint8_t*>(geom.GetData());
    idx_t size = geom.GetSize();
    if (size < 5) return rings;

    uint32_t geom_type;
    memcpy(&geom_type, data + 1, 4);
    uint32_t base_type = geom_type & 0xFF;
    idx_t offset = 5;
    if (geom_type & 0x20000000) {
        offset += 4;
    }

    auto read_polygon = [&](idx_t& pos) -> bool {
        if (size < pos + 4) return false;
        uint32_t num_rings;
        memcpy(&num_rings, data + pos, 4);
        pos += 4;
        for (uint32_t r = 0; r < num_rings; r++) {
            std::vector<double> ring;
            if (!ReadRing(data, pos, size, ring)) return false;
            rings.push_back(std::move(ring));
        }
        return true;
    };

    if (base_type == 1) {  // POINT
        if (size < offset + 16) return rings;
        std::vector<double> ring(2);
        memcpy(&ring[0], data + offset, 8);
        memcpy(&ring[1], data + offset + 8, 8);
        rings.push_back(std::move(ring));
    } else if (base_type == 3) {  // POLYGON
        read_polygon(offset);
    } else if (base_type == 6) {  // MULTIPOLYGON
        if (size < offset + 4) return rings;
        uint32_t num_polygons;
        memcpy(&num_polygons, data + offset, 4);
        offset += 4;
        for (uint32_t poly = 0; poly < num_polygons; poly++) {
            if (size < offset + 5) return rings;
            uint32_t poly_type;
            memcpy(&poly_type, data + offset + 1, 4);
            offset += 5;
            if (poly_type & 0x20000000) {
                offset += 4;
            }
            if (!read_polygon(offset)) return rings;
        }
    }
    return rings;
}

// Check if segment (x1,y1)-(x2,y2) passes through the interior of the rectangle.
// Contact with the boundary only (an edge lying on a grid line, or touching a
// corner) does not count, so cells that merely touch the geometry are excluded.
static bool SegmentCrossesRectInterior(double x1, double y1, double x2, double y2,
                                       double min_x, double min_y, double max_x, double max_y) {
    double dx = x2 - x1;
    double dy = y2 - y1;
    double t0 = 0.0, t1 = 1.0;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {x1 - min_x, max_x - x1, y1 - min_y, max_y - y1};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0) return false;  // Parallel to this edge and outside it
        } else {
            double t = q[i] / p[i];
            if (p[i] < 0.0) {
                if (t > t1) return false;
                if (t > t0) t0 = t;
            } else {
                if (t < t0) return false;
                if (t < t1) t1 = t;
            }
        }
    }
    // The clipped part is a chord of the closed rectangle: it reaches the
    // interior iff its midpoint does (unless it lies along the boundary)
    double tm = (t0 + t1) / 2.0;
    double mx = x1 + tm * dx;
    double my = y1 + tm * dy;
    return mx > min_x && mx < max_x && my > min_y && my < max_y;
}

// Check if any ring edge passes through the interior of the rectangle
static bool RingsCrossRectInterior(const std::vector<std::vector<double>>& rings,
                                   double min_x, double min_y, double max_x, double max_y) {
    for (const auto& ring : rings) {
        size_t n = ring.size() / 2;
        for (size_t i = 0; i + 1 < n; i++) {
            if (SegmentCrossesRectInterior(ring[i * 2], ring[i * 2 + 1], ring[i * 2 + 2], ring[i * 2 + 3],
                                           min_x, min_y, max_x, max_y)) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// QUADBIN_POLYFILL Implementation
// ============================================================================

enum class PolyfillMode {
    CENTER,     // Cell centers inside polygon (default, fastest)
    INTERSECTS, // Cells that intersect polygon (complete coverage)
    CONTAINS    // Cells fully contained in polygon (conservative)
};

static PolyfillMode ParsePolyfillMode(const std::string& mode) {
    if (mode == "center" || mode.empty()) return PolyfillMode::CENTER;
    if (mode == "intersects") return PolyfillMode::INTERSECTS;
    if (mode == "contains") return PolyfillMode::CONTAINS;
    throw InvalidInputException("QUADBIN_POLYFILL: Invalid mode '" + mode + "'. Valid modes: 'center', 'intersects', 'contains'");
}

// Core polyfill algorithm
static std::vector<uint64_t> ComputePolyfill(const string_t &geom, int resolution, PolyfillMode mode) {
    std::vector<uint64_t> result;

    // Get geometry bounding box
    double min_lon, min_lat, max_lon, max_lat;
    if (!ExtractGeometryBoundingBox(geom, min_lon, min_lat, max_lon, max_lat)) {
        throw InvalidInputException("QUADBIN_POLYFILL: Could not extract bounding box from geometry");
    }

    // Convert bbox corners to tile coordinates
    int min_tile_x, min_tile_y, max_tile_x, max_tile_y;
    quadbin::lonlat_to_tile(min_lon, max_lat, resolution, min_tile_x, min_tile_y);  // NW corner
    quadbin::lonlat_to_tile(max_lon, min_lat, resolution, max_tile_x, max_tile_y);  // SE corner

    std::vector<std::vector<double>> rings;
    if (mode == PolyfillMode::INTERSECTS) {
        rings = ExtractRings(geom);
        // A POINT intersects the one cell quadbin_from_lonlat assigns it to,
        // even when it lies exactly on a cell boundary
        if (rings.size() == 1 && rings[0].size() == 2) {
            result.push_back(quadbin::tile_to_cell(min_tile_x, min_tile_y, resolution));
            return result;
        }
    }

    // Iterate all tiles in bbox range
    for (int tile_y = min_tile_y; tile_y <= max_tile_y; tile_y++) {
        for (int tile_x = min_tile_x; tile_x <= max_tile_x; tile_x++) {
            // Get tile bounds
            double tile_min_lon, tile_min_lat, tile_max_lon, tile_max_lat;
            quadbin::tile_to_bbox_wgs84(tile_x, tile_y, resolution, tile_min_lon, tile_min_lat, tile_max_lon, tile_max_lat);

            bool include = false;

            switch (mode) {
                case PolyfillMode::CENTER: {
                    // Check if tile center is inside geometry
                    double center_lon = (tile_min_lon + tile_max_lon) / 2.0;
                    double center_lat = (tile_min_lat + tile_max_lat) / 2.0;
                    include = PointInGeometry(center_lon, center_lat, geom);
                    break;
                }
                case PolyfillMode::INTERSECTS: {
                    // The tile intersects the geometry iff a geometry edge passes through the
                    // tile's interior (this covers geometries smaller than a tile), or the tile
                    // lies wholly inside the geometry, in which case its center is inside too.
                    if (RingsCrossRectInterior(rings, tile_min_lon, tile_min_lat, tile_max_lon, tile_max_lat)) {
                        include = true;
                    } else {
                        double center_lon = (tile_min_lon + tile_max_lon) / 2.0;
                        double center_lat = (tile_min_lat + tile_max_lat) / 2.0;
                        include = PointInGeometry(center_lon, center_lat, geom);
                    }
                    break;
                }
                case PolyfillMode::CONTAINS: {
                    // All four corners must be inside geometry
                    include = PointInGeometry(tile_min_lon, tile_min_lat, geom) &&
                              PointInGeometry(tile_max_lon, tile_min_lat, geom) &&
                              PointInGeometry(tile_min_lon, tile_max_lat, geom) &&
                              PointInGeometry(tile_max_lon, tile_max_lat, geom);
                    break;
                }
            }

            if (include) {
                result.push_back(quadbin::tile_to_cell(tile_x, tile_y, resolution));
            }
        }
    }

    return result;
}

// QUADBIN_POLYFILL(geometry, resolution) -> LIST(UBIGINT)
static void QuadbinPolyfillFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    args.data[0].Flatten();
    args.data[1].Flatten();

    auto geom_data = FlatVector::GetData<string_t>(args.data[0]);
    auto res_data = FlatVector::GetData<int32_t>(args.data[1]);

    auto &list_entries = ListVector::GetChildMutable(result);
    auto list_data = FlatVector::GetDataMutable<list_entry_t>(result);
    auto &result_validity = FlatVector::ValidityMutable(result);

    idx_t total_cells = 0;
    for (idx_t i = 0; i < args.size(); i++) {
        if (raquet::AnyInputNull(args, i)) {
            result_validity.SetInvalid(i);
            list_data[i].offset = total_cells;
            list_data[i].length = 0;
            continue;
        }

        auto geom = geom_data[i];
        auto resolution = res_data[i];

        if (resolution < 0 || resolution > 26) {
            throw InvalidInputException("QUADBIN_POLYFILL: Resolution must be between 0 and 26");
        }

        auto cells = ComputePolyfill(geom, resolution, PolyfillMode::CENTER);

        list_data[i].offset = total_cells;
        list_data[i].length = cells.size();

        ListVector::Reserve(result, total_cells + cells.size());
        auto cell_data = FlatVector::GetDataMutable<uint64_t>(list_entries);
        for (size_t j = 0; j < cells.size(); j++) {
            cell_data[total_cells + j] = cells[j];
        }
        total_cells += cells.size();
    }
    ListVector::SetListSize(result, total_cells);
}

// QUADBIN_POLYFILL(geometry, resolution, mode) -> LIST(UBIGINT)
static void QuadbinPolyfillModeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    args.data[0].Flatten();
    args.data[1].Flatten();
    args.data[2].Flatten();

    auto geom_data = FlatVector::GetData<string_t>(args.data[0]);
    auto res_data = FlatVector::GetData<int32_t>(args.data[1]);
    auto mode_data = FlatVector::GetData<string_t>(args.data[2]);

    auto &list_entries = ListVector::GetChildMutable(result);
    auto list_data = FlatVector::GetDataMutable<list_entry_t>(result);
    auto &result_validity = FlatVector::ValidityMutable(result);

    idx_t total_cells = 0;
    for (idx_t i = 0; i < args.size(); i++) {
        if (raquet::AnyInputNull(args, i)) {
            result_validity.SetInvalid(i);
            list_data[i].offset = total_cells;
            list_data[i].length = 0;
            continue;
        }

        auto geom = geom_data[i];
        auto resolution = res_data[i];
        auto mode_str = mode_data[i].GetString();

        if (resolution < 0 || resolution > 26) {
            throw InvalidInputException("QUADBIN_POLYFILL: Resolution must be between 0 and 26");
        }

        auto mode = ParsePolyfillMode(mode_str);
        auto cells = ComputePolyfill(geom, resolution, mode);

        list_data[i].offset = total_cells;
        list_data[i].length = cells.size();

        ListVector::Reserve(result, total_cells + cells.size());
        auto cell_data = FlatVector::GetDataMutable<uint64_t>(list_entries);
        for (size_t j = 0; j < cells.size(); j++) {
            cell_data[total_cells + j] = cells[j];
        }
        total_cells += cells.size();
    }
    ListVector::SetListSize(result, total_cells);
}

void RegisterPolyfillFunctions(ExtensionLoader &loader) {
    // QUADBIN_POLYFILL(geometry, resolution) -> LIST(UBIGINT)
    ScalarFunctionSet polyfill_set("QUADBIN_POLYFILL");

    // Two-argument version (default mode = center)
    polyfill_set.AddFunction(ScalarFunction(
        {LogicalType::GEOMETRY(), LogicalType::INTEGER},
        LogicalType::LIST(LogicalType::UBIGINT),
        QuadbinPolyfillFunction));

    // Three-argument version (with mode)
    polyfill_set.AddFunction(ScalarFunction(
        {LogicalType::GEOMETRY(), LogicalType::INTEGER, LogicalType::VARCHAR},
        LogicalType::LIST(LogicalType::UBIGINT),
        QuadbinPolyfillModeFunction));

    polyfill_set.SetFallible();

    loader.RegisterFunction(polyfill_set);
}

} // namespace duckdb
