#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace raquet {

// Scalar functions here read their arguments with FlatVector::GetData(...)[i]. That is only
// correct for FLAT vectors: a CONSTANT argument (a literal zoom, tile size or k) stores one
// value, so rows 1..n read past it. Flatten first.
inline void FlattenArgs(DataChunk &args) {
    for (idx_t col = 0; col < args.ColumnCount(); col++) {
        args.data[col].Flatten(args.size());
    }
}

// With DEFAULT_NULL_HANDLING a row whose input is NULL still reaches the function, and the
// slot of a NULL row is uninitialized (for a BLOB, a random pointer and length). Ask this at
// the top of every row loop, after flattening, and give NULL for such rows. `nullable_col` is
// an argument where NULL has a meaning of its own (an optional nodata) and is left to the caller.
inline bool AnyInputNull(const Vector inputs[], idx_t input_count, idx_t row,
                         idx_t nullable_col = DConstants::INVALID_INDEX) {
    for (idx_t col = 0; col < input_count; col++) {
        if (col != nullable_col && !FlatVector::Validity(inputs[col]).RowIsValid(row)) {
            return true;
        }
    }
    return false;
}

inline bool AnyInputNull(DataChunk &args, idx_t row, idx_t nullable_col = DConstants::INVALID_INDEX) {
    return AnyInputNull(args.data.data(), args.ColumnCount(), row, nullable_col);
}

} // namespace raquet
} // namespace duckdb
