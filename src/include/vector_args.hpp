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
// the top of every row loop, after FlattenArgs, and give NULL for such rows.
inline bool AnyInputNull(DataChunk &args, idx_t row) {
    for (idx_t col = 0; col < args.ColumnCount(); col++) {
        if (!FlatVector::Validity(args.data[col]).RowIsValid(row)) {
            return true;
        }
    }
    return false;
}

} // namespace raquet
} // namespace duckdb
