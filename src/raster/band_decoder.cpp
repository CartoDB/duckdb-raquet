#include "band_decoder.hpp"
#include <zlib.h>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <string>
#include <limits>
#include <cmath>

// Optional JPEG support via libjpeg
#ifdef RAQUET_HAS_JPEG
#include <jpeglib.h>
#endif

// Optional WebP support via libwebp
#ifdef RAQUET_HAS_WEBP
#include <webp/decode.h>
#endif

namespace duckdb {
namespace raquet {

// Upper bound for one decompressed band. A 4096x4096 float64 tile is 128 MiB; anything past
// this is corrupt input, not a real tile.
static constexpr size_t MAX_DECOMPRESSED_SIZE = size_t(1) << 30;

std::vector<uint8_t> decompress_gzip(const uint8_t *data, size_t size) {
    if (size == 0) {
        return {};
    }

    if (data == nullptr) {
        throw std::runtime_error("Null data pointer for decompression");
    }

    // For gzip: estimate decompressed size
    // A 256x256 tile with uint16 = 131072 bytes, float64 = 524288 bytes
    // Start with a larger initial estimate to avoid resizes
    size_t estimated_size = 256 * 256 * 8;  // Assume worst case (float64)
    if (size > 1000) {
        // For larger compressed data, use a ratio-based estimate
        estimated_size = std::max(estimated_size, size * 100);
    }
    estimated_size = std::min(estimated_size, MAX_DECOMPRESSED_SIZE);

    std::vector<uint8_t> result(estimated_size);

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = const_cast<Bytef*>(data);
    strm.avail_in = static_cast<uInt>(size);
    strm.next_out = result.data();
    strm.avail_out = static_cast<uInt>(result.size());

    // 15 + 32 = auto-detect gzip or zlib format
    int ret = inflateInit2(&strm, 15 + 32);
    if (ret != Z_OK) {
        throw std::runtime_error("inflateInit2 failed: " + std::to_string(ret));
    }

    // Use Z_NO_FLUSH and loop for potentially large data
    size_t total_out = 0;
    while (ret != Z_STREAM_END) {
        ret = inflate(&strm, Z_NO_FLUSH);

        if (ret != Z_STREAM_END && strm.avail_in == 0 && strm.avail_out > 0) {
            // All input consumed without reaching the end of the stream: more output space
            // will not help, and doubling the buffer here would grow it forever
            inflateEnd(&strm);
            throw std::runtime_error("inflate failed: truncated or corrupt gzip data");
        }

        if (ret == Z_BUF_ERROR || (ret == Z_OK && strm.avail_out == 0)) {
            // Need more output space
            total_out = result.size() - strm.avail_out;
            size_t new_size = result.size() * 2;
            if (new_size > MAX_DECOMPRESSED_SIZE) {
                inflateEnd(&strm);
                throw std::runtime_error("inflate failed: decompressed band exceeds " +
                                         std::to_string(MAX_DECOMPRESSED_SIZE) + " bytes");
            }
            result.resize(new_size);
            strm.next_out = result.data() + total_out;
            strm.avail_out = static_cast<uInt>(result.size() - total_out);
            continue;
        }

        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&strm);
            throw std::runtime_error("inflate failed: " + std::to_string(ret));
        }
    }

    size_t decompressed_size = result.size() - strm.avail_out;
    inflateEnd(&strm);

    result.resize(decompressed_size);
    return result;
}

// v0.4.0: JPEG decompression
std::vector<uint8_t> decompress_jpeg(const uint8_t *data, size_t size,
                                      int &width_out, int &height_out, int &channels_out) {
#ifdef RAQUET_HAS_JPEG
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);

    jpeg_mem_src(&cinfo, data, size);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        throw std::runtime_error("Invalid JPEG header");
    }

    // Force RGB output
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    width_out = cinfo.output_width;
    height_out = cinfo.output_height;
    channels_out = cinfo.output_components;

    size_t row_stride = width_out * channels_out;
    std::vector<uint8_t> result(width_out * height_out * channels_out);

    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t *row_ptr = result.data() + cinfo.output_scanline * row_stride;
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    return result;
#else
    (void)data; (void)size;
    (void)width_out; (void)height_out; (void)channels_out;
    throw std::runtime_error("JPEG decompression not available: compile with RAQUET_HAS_JPEG");
#endif
}

// v0.4.0: WebP decompression
std::vector<uint8_t> decompress_webp(const uint8_t *data, size_t size,
                                      int &width_out, int &height_out, int &channels_out) {
#ifdef RAQUET_HAS_WEBP
    // Get image info first
    if (!WebPGetInfo(data, size, &width_out, &height_out)) {
        throw std::runtime_error("Invalid WebP image");
    }

    // Decode to RGB (3 channels) or RGBA (4 channels)
    // Try RGBA first to preserve alpha if present
    int stride = width_out * 4;
    std::vector<uint8_t> result(width_out * height_out * 4);

    uint8_t *decoded = WebPDecodeRGBAInto(data, size, result.data(), result.size(), stride);
    if (!decoded) {
        throw std::runtime_error("WebP decompression failed");
    }

    channels_out = 4;  // RGBA
    return result;
#else
    (void)data; (void)size;
    (void)width_out; (void)height_out; (void)channels_out;
    throw std::runtime_error("WebP decompression not available: compile with RAQUET_HAS_WEBP");
#endif
}

double decode_pixel(const uint8_t *band_data, size_t band_size,
                    const std::string &dtype_str,
                    int pixel_x, int pixel_y, int width,
                    bool compressed) {
    BandDataType dtype = parse_dtype(dtype_str);

    if (pixel_x < 0 || pixel_y < 0 || width <= 0) {
        throw std::out_of_range("Invalid pixel coordinates or width");
    }

    const uint8_t *data;
    size_t data_size;
    std::vector<uint8_t> decompressed;

    if (compressed) {
        decompressed = decompress_gzip(band_data, band_size);
        data = decompressed.data();
        data_size = decompressed.size();
    } else {
        data = band_data;
        data_size = band_size;
    }

    // Row-major order: offset = y * width + x
    size_t offset = static_cast<size_t>(pixel_y) * width + pixel_x;

    return get_pixel_value(data, data_size, offset, dtype);
}

std::vector<double> decode_band(const uint8_t *band_data, size_t band_size,
                                 const std::string &dtype_str,
                                 int width, int height,
                                 bool compressed) {
    BandDataType dtype = parse_dtype(dtype_str);

    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("Invalid band dimensions");
    }

    const uint8_t *data;
    size_t data_size;
    std::vector<uint8_t> decompressed;

    if (compressed) {
        decompressed = decompress_gzip(band_data, band_size);
        data = decompressed.data();
        data_size = decompressed.size();
    } else {
        data = band_data;
        data_size = band_size;
    }

    size_t pixel_count = static_cast<size_t>(width) * height;
    size_t expected_size = pixel_count * dtype_size(dtype);
    if (expected_size > data_size) {
        throw std::out_of_range("Band data (" + std::to_string(data_size) +
                                " bytes) too small for " + std::to_string(width) + "x" +
                                std::to_string(height) + " " + dtype_str + " tile (" +
                                std::to_string(expected_size) + " bytes needed)");
    }

    std::vector<double> result(pixel_count);

    for (size_t i = 0; i < pixel_count; i++) {
        result[i] = get_pixel_value(data, data_size, i, dtype);
    }

    return result;
}

BandStats compute_band_stats(const uint8_t *band_data, size_t band_size,
                              const std::string &dtype_str,
                              int width, int height,
                              bool compressed,
                              bool has_nodata,
                              double nodata) {
    BandDataType dtype = parse_dtype(dtype_str);

    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("Invalid band dimensions");
    }

    const uint8_t *data;
    size_t data_size;
    std::vector<uint8_t> decompressed;

    if (compressed) {
        decompressed = decompress_gzip(band_data, band_size);
        data = decompressed.data();
        data_size = decompressed.size();
    } else {
        data = band_data;
        data_size = band_size;
    }

    size_t pixel_count = static_cast<size_t>(width) * height;
    size_t expected_size = pixel_count * dtype_size(dtype);
    if (expected_size > data_size) {
        throw std::out_of_range("Band data too small for declared dimensions");
    }

    BandStats stats;
    stats.min = std::numeric_limits<double>::max();
    stats.max = std::numeric_limits<double>::lowest();

    // Welford's online algorithm variables
    double m2 = 0.0;

    for (size_t i = 0; i < pixel_count; i++) {
        double val = get_pixel_value(data, data_size, i, dtype);

        // Skip nodata values (handle NaN specially since NaN != NaN)
        if (has_nodata && (val == nodata || (std::isnan(val) && std::isnan(nodata)))) {
            continue;
        }

        stats.count++;
        stats.sum += val;

        if (val < stats.min) stats.min = val;
        if (val > stats.max) stats.max = val;

        // Welford's algorithm for mean and variance
        double delta = val - stats.mean;
        stats.mean += delta / stats.count;
        double delta2 = val - stats.mean;
        m2 += delta * delta2;
    }

    // Finalize statistics
    if (stats.count > 0) {
        stats.mean = stats.sum / stats.count;
        if (stats.count > 1) {
            stats.stddev = std::sqrt(m2 / (stats.count - 1));
        }
    } else {
        stats.min = 0;
        stats.max = 0;
    }

    return stats;
}

// v0.4.0: Decode pixel from interleaved (BIP) layout
double decode_pixel_interleaved(const uint8_t *pixels_data, size_t pixels_size,
                                 const std::string &dtype_str,
                                 int pixel_x, int pixel_y, int width,
                                 int band_index, int num_bands,
                                 const std::string &compression) {
    if (pixel_x < 0 || pixel_y < 0 || width <= 0 || band_index < 0 || num_bands <= 0) {
        throw std::out_of_range("Invalid pixel coordinates, width, or band index");
    }

    const uint8_t *data;
    size_t data_size;
    std::vector<uint8_t> decompressed;
    int decoded_width = width;
    int decoded_height = 0;
    int decoded_channels = 0;

    if (compression == "gzip") {
        // Gzip compression: decompress to raw interleaved bytes
        decompressed = decompress_gzip(pixels_data, pixels_size);
        data = decompressed.data();
        data_size = decompressed.size();
    } else if (compression == "jpeg") {
        // JPEG: decode to RGB image, then extract channel
        decompressed = decompress_jpeg(pixels_data, pixels_size,
                                        decoded_width, decoded_height, decoded_channels);
        data = decompressed.data();
        data_size = decompressed.size();

        // For JPEG, data type is always uint8, and we extract by channel index
        if (band_index >= decoded_channels) {
            throw std::invalid_argument("Band index exceeds JPEG channels");
        }
        // JPEG pixel offset: (y * width + x) * channels + channel_index
        size_t offset = (static_cast<size_t>(pixel_y) * decoded_width + pixel_x) * decoded_channels + band_index;
        if (offset >= data_size) {
            throw std::out_of_range("JPEG pixel offset out of bounds");
        }
        return static_cast<double>(data[offset]);
    } else if (compression == "webp") {
        // WebP: decode to RGBA image, then extract channel
        decompressed = decompress_webp(pixels_data, pixels_size,
                                        decoded_width, decoded_height, decoded_channels);
        data = decompressed.data();
        data_size = decompressed.size();

        // For WebP, data type is always uint8, and we extract by channel index
        if (band_index >= decoded_channels) {
            throw std::invalid_argument("Band index exceeds WebP channels");
        }
        // WebP pixel offset: (y * width + x) * channels + channel_index
        size_t offset = (static_cast<size_t>(pixel_y) * decoded_width + pixel_x) * decoded_channels + band_index;
        if (offset >= data_size) {
            throw std::out_of_range("WebP pixel offset out of bounds");
        }
        return static_cast<double>(data[offset]);
    } else if (compression == "none" || compression.empty()) {
        // No compression: use raw data directly
        data = pixels_data;
        data_size = pixels_size;
    } else {
        throw std::invalid_argument("Unknown compression: " + compression);
    }

    // For gzip or no compression: interleaved (BIP) layout
    // Data layout: [B0_P0, B1_P0, B2_P0, B0_P1, B1_P1, B2_P1, ...]
    // where B=band, P=pixel
    BandDataType dtype = parse_dtype(dtype_str);

    // Interleaved offset: (pixel_index * num_bands + band_index)
    size_t pixel_index = static_cast<size_t>(pixel_y) * width + pixel_x;
    size_t element_offset = pixel_index * num_bands + band_index;

    return get_pixel_value(data, data_size, element_offset, dtype);
}

// v0.4.0: Decode entire band from interleaved layout
std::vector<double> decode_band_interleaved(const uint8_t *pixels_data, size_t pixels_size,
                                             const std::string &dtype_str,
                                             int width, int height,
                                             int band_index, int num_bands,
                                             const std::string &compression) {
    if (width <= 0 || height <= 0 || band_index < 0 || num_bands <= 0) {
        throw std::invalid_argument("Invalid dimensions or band index");
    }

    const uint8_t *data;
    size_t data_size;
    std::vector<uint8_t> decompressed;
    int decoded_width = width;
    int decoded_height = height;
    int decoded_channels = 0;

    if (compression == "gzip") {
        decompressed = decompress_gzip(pixels_data, pixels_size);
        data = decompressed.data();
        data_size = decompressed.size();
    } else if (compression == "jpeg") {
        decompressed = decompress_jpeg(pixels_data, pixels_size,
                                        decoded_width, decoded_height, decoded_channels);
        data = decompressed.data();
        data_size = decompressed.size();

        // JPEG always uint8, extract channel
        if (band_index >= decoded_channels) {
            throw std::invalid_argument("Band index exceeds JPEG channels");
        }

        size_t pixel_count = static_cast<size_t>(decoded_width) * decoded_height;
        std::vector<double> result(pixel_count);
        for (size_t i = 0; i < pixel_count; i++) {
            size_t offset = i * decoded_channels + band_index;
            if (offset >= data_size) {
                throw std::out_of_range("JPEG pixel offset out of bounds");
            }
            result[i] = static_cast<double>(data[offset]);
        }
        return result;
    } else if (compression == "webp") {
        decompressed = decompress_webp(pixels_data, pixels_size,
                                        decoded_width, decoded_height, decoded_channels);
        data = decompressed.data();
        data_size = decompressed.size();

        if (band_index >= decoded_channels) {
            throw std::invalid_argument("Band index exceeds WebP channels");
        }

        size_t pixel_count = static_cast<size_t>(decoded_width) * decoded_height;
        std::vector<double> result(pixel_count);
        for (size_t i = 0; i < pixel_count; i++) {
            size_t offset = i * decoded_channels + band_index;
            if (offset >= data_size) {
                throw std::out_of_range("WebP pixel offset out of bounds");
            }
            result[i] = static_cast<double>(data[offset]);
        }
        return result;
    } else if (compression == "none" || compression.empty()) {
        data = pixels_data;
        data_size = pixels_size;
    } else {
        throw std::invalid_argument("Unknown compression: " + compression);
    }

    // For gzip or no compression: interleaved (BIP) layout
    BandDataType dtype = parse_dtype(dtype_str);

    size_t pixel_count = static_cast<size_t>(width) * height;
    // Validate total interleaved data fits
    size_t total_elements = pixel_count * num_bands;
    size_t expected_size = total_elements * dtype_size(dtype);
    if (expected_size > data_size) {
        throw std::out_of_range("Interleaved band data too small for declared dimensions");
    }

    std::vector<double> result(pixel_count);

    for (size_t i = 0; i < pixel_count; i++) {
        // Interleaved offset: pixel_index * num_bands + band_index
        size_t element_offset = i * num_bands + band_index;
        result[i] = get_pixel_value(data, data_size, element_offset, dtype);
    }

    return result;
}

} // namespace raquet
} // namespace duckdb
