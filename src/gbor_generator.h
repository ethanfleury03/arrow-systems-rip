#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstring>  // For memcpy

namespace GborGenerator {

// Constants from borealis.py and Memjet docs
const uint32_t GBOR_MAGIC_CHARS_TAG = 50000;
const std::string GBOR_MAGIC_CHARS_VALUE = "GBG BOREALIS";
const uint32_t GBOR_VERSION_TAG = 50013;
const uint32_t GBOR_VERSION_VALUE = 1;
const uint32_t GBOR_DATA_CHUNK_SIZE_TAG = 100002;
const uint32_t GBOR_DATA_CHUNK_SIZE_VALUE = 4 * 1024 * 1024; // 4MB per channel for JOB_PARAMS

const uint32_t GBOR_STRIP_STARTS_TAG = 50002;
const uint32_t GBOR_STRIP_WIDTHS_TAG = 50003;
const uint32_t GBOR_PRINT_SPEED_NUMERATOR_TAG = 203;
const uint32_t GBOR_PRINT_SPEED_DENOMINATOR_TAG = 204;
const uint32_t GBOR_HEAD_INDEX_TAG = 199110;
const uint32_t GBOR_ENGINE_STAGE_INDEX_TAG = 199112;

const uint32_t GBOR_JOB_PARAM_BLOCK_ID = 1000;
const uint32_t GBOR_PAGE_BLOCK_ID = 1100;
const uint32_t GBOR_END_JOB_BLOCK_ID = 1016;

const uint32_t GBOR_CHANNEL_JOB_INFO_TAG = 100001; // Nested TLV
const uint32_t GBOR_CHANNEL_NUMBER_TAG = 100101;
const uint32_t GBOR_DATA_FORMAT_TAG = 100102;
const uint32_t GBOR_CHANNEL_INK_NAME_TAG = 100103;
const uint32_t GBOR_RIP_VERSION_TAG = 199101;
const uint32_t GBOR_DISPLAYABLE_JOB_NAME_TAG = 199103;
const uint32_t GBOR_JOB_ID_TAG = 199105;  // Job ID tag
const uint32_t GBOR_X_RESOLUTION_TAG = 100204;
const uint32_t GBOR_Y_RESOLUTION_TAG = 250001; // Not in borealis.py, infer from JSL

const uint32_t GBOR_PAGE_NUMBER_TAG = 100201;
const uint32_t GBOR_PAGE_HEIGHT_TAG = 609; // Note: this is page height in lines (not pixels)
const uint32_t GBOR_STRIP_INDEX_TAG = 50005;
const uint32_t GBOR_CHANNEL_PAGE_INFO_TAG = 100202; // Nested TLV
const uint32_t GBOR_CHANNEL_DATA_INFO_TAG = 100203; // Nested TLV
const uint32_t GBOR_CHANNEL_DATA_CHUNK_TAG = 0x80000000 + 100401; // Deferred value mask (Python: DEFERRED_VALUE_MASK)


// Helper to write data in little-endian format
template<typename T>
void write_le(FILE* f, const T& value) {
    // Ensure little-endian writing, regardless of host endianness (assuming host is also little-endian)
    // For cross-platform, would need proper endian conversion
    fwrite(&value, sizeof(T), 1, f);
}

// Basic TLV structure (simplified for GBOR building)
struct Tlv {
    uint32_t tag;
    uint32_t length;
    std::vector<uint8_t> value_data;

    Tlv(uint32_t t, const std::vector<uint8_t>& v_data) : tag(t), value_data(v_data) { length = v_data.size(); }
    Tlv(uint32_t t, uint32_t val) : tag(t) { 
        length = sizeof(uint32_t);
        value_data.resize(length);
        // Assume host is little-endian for simplicity; otherwise need byte swapping
        memcpy(value_data.data(), &val, length);
    }
    Tlv(uint32_t t, const std::string& s) : tag(t) {
        length = s.length();
        value_data.resize(length);
        memcpy(value_data.data(), s.data(), length);
    }

    void write(FILE* f) const {
        write_le(f, tag);
        write_le(f, length);
        fwrite(value_data.data(), 1, value_data.size(), f);
    }
};

// Function to pack bilevel data (1-bit per pixel) into 32-bit words
// Follows the spec: 32-bit words, bit 0 = leftmost, 128-bit padded
std::vector<uint8_t> packBilevelData(
    const std::vector<std::vector<bool>>& pixel_data, // [row][col] bool for black pixel
    uint32_t strip_width_pixels, // Total width of the strip in pixels
    uint32_t strip_height_lines  // Total height of the strip in lines
);

// Generates a simple GBOR file with a test pattern
bool generateAndSaveGborFile(
    const std::string& filename, 
    uint32_t strip_width_dots, 
    uint32_t strip_height_lines,  // Total height of the strip in lines
    uint32_t x_resolution,
    uint32_t y_resolution,
    const std::string& job_id_str,
    const std::string& display_job_name,
    const std::string& rip_version
);

} // namespace GborGenerator
