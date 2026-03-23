#include "gbor_generator.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <algorithm> // For std::reverse
#include <numeric>   // For std::iota

namespace GborGenerator {

// Helper to write a common Block Header (simplified for Job and Page Blocks)
void writeBlockHeader(FILE* f, uint32_t block_id, uint32_t payload_length) {
    write_le(f, block_id);
    write_le(f, payload_length);
}

std::vector<uint8_t> packBilevelData(
    const std::vector<std::vector<bool>>& pixel_data, // [row][col] bool for black pixel
    uint32_t strip_width_pixels, // Total width of the strip in pixels
    uint32_t strip_height_lines  // Total height of the strip in lines
) {
    if (pixel_data.empty() || pixel_data[0].empty() || strip_width_pixels == 0 || strip_height_lines == 0) {
        return {}; // Return empty for invalid input
    }

    // Number of 32-bit words per line (unpadded)
    uint32_t words_per_line_unpadded = (strip_width_pixels + 31) / 32;
    // Each line must be padded to a 128-bit (4-word, 16-byte) boundary.
    uint32_t words_per_line_padded = ((words_per_line_unpadded + 3) / 4) * 4;
    
    // Size of the packed data for one line in bytes
    uint32_t bytes_per_line_padded = words_per_line_padded * sizeof(uint32_t);

    std::vector<uint8_t> packed_data;
    packed_data.reserve(bytes_per_line_padded * strip_height_lines);

    for (uint32_t row = 0; row < strip_height_lines; ++row) {
        std::vector<uint32_t> current_line_words(words_per_line_padded, 0); // Initialize with zeros
        const std::vector<bool>& current_row_pixels = (row < pixel_data.size()) ? pixel_data[row] : std::vector<bool>(strip_width_pixels, false);

        for (uint32_t col = 0; col < strip_width_pixels; ++col) {
            if (col < current_row_pixels.size() && current_row_pixels[col]) {
                uint32_t word_idx = col / 32;
                uint32_t bit_offset = col % 32;
                // Bit 0 is leftmost dot; assumes little-endian word storage
                current_line_words[word_idx] |= (1U << bit_offset);
            }
        }
        
        // Write the padded line to packed_data (little-endian bytes)
        for (uint32_t i = 0; i < words_per_line_padded; ++i) {
            uint32_t word = current_line_words[i];
            packed_data.push_back(static_cast<uint8_t>(word & 0xFF));
            packed_data.push_back(static_cast<uint8_t>((word >> 8) & 0xFF));
            packed_data.push_back(static_cast<uint8_t>((word >> 16) & 0xFF));
            packed_data.push_back(static_cast<uint8_t>((word >> 24) & 0xFF));
        }
    }
    return packed_data;
}

bool generateAndSaveGborFile(
    const std::string& filename, 
    uint32_t strip_width_dots, 
    uint32_t strip_height_lines, 
    uint32_t x_resolution,
    uint32_t y_resolution,
    const std::string& job_id_str,
    const std::string& display_job_name,
    const std::string& rip_version
) {
    FILE* f = fopen(filename.c_str(), "wb");
    if (!f) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return false;
    }

    // Lambda to write TLVs into a vector (avoiding temporary issues)
    auto write_tlv_to_vec = [&](std::vector<uint8_t>& target_vec, const Tlv& tlv) { 
        std::vector<uint8_t> buffer;
        buffer.reserve(sizeof(uint32_t) * 2 + tlv.value_data.size()); // Tag + Length + Value
        uint32_t tag_le = tlv.tag; // Assume host is LE
        uint32_t length_le = tlv.length;
        
        // Append tag, length, and value data in little-endian byte order
        for (size_t i = 0; i < sizeof(uint32_t); ++i) buffer.push_back(static_cast<uint8_t>((tag_le >> (i * 8)) & 0xFF));
        for (size_t i = 0; i < sizeof(uint32_t); ++i) buffer.push_back(static_cast<uint8_t>((length_le >> (i * 8)) & 0xFF));
        buffer.insert(buffer.end(), tlv.value_data.begin(), tlv.value_data.end());
        target_vec.insert(target_vec.end(), buffer.begin(), buffer.end());
    };

    // ------------------------------------------
    // Construct Job Parameter Block Payload
    // ------------------------------------------
    std::vector<uint8_t> job_block_payload_data;

    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_MAGIC_CHARS_TAG, GBOR_MAGIC_CHARS_VALUE));
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_VERSION_TAG, GBOR_VERSION_VALUE));
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_DATA_CHUNK_SIZE_TAG, GBOR_DATA_CHUNK_SIZE_VALUE));
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_STRIP_STARTS_TAG, (uint32_t)0)); 
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_STRIP_WIDTHS_TAG, strip_width_dots));
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_PRINT_SPEED_NUMERATOR_TAG, (uint32_t)1));
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_PRINT_SPEED_DENOMINATOR_TAG, (uint32_t)1));
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_HEAD_INDEX_TAG, (uint32_t)0));
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_ENGINE_STAGE_INDEX_TAG, (uint32_t)0));
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_RIP_VERSION_TAG, rip_version));
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_DISPLAYABLE_JOB_NAME_TAG, display_job_name));
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_X_RESOLUTION_TAG, x_resolution));
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_Y_RESOLUTION_TAG, y_resolution));
    
    // Job ID (need to convert hex string to byte array)
    std::vector<uint8_t> job_id_bytes;
    job_id_bytes.reserve(job_id_str.length() / 2);
    for (size_t i = 0; i < job_id_str.length(); i += 2) {
        std::string byte_str = job_id_str.substr(i, 2);
        job_id_bytes.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
    }
    write_tlv_to_vec(job_block_payload_data, Tlv(GBOR_JOB_ID_TAG, job_id_bytes));

    // Job Param Block Header
    writeBlockHeader(f, GBOR_JOB_PARAM_BLOCK_ID, job_block_payload_data.size());
    fwrite(job_block_payload_data.data(), 1, job_block_payload_data.size(), f);
    
    // ------------------------------------------
    // Construct Page Block Payload
    // ------------------------------------------
    std::vector<std::vector<bool>> cyan_pixel_data(strip_height_lines, std::vector<bool>(strip_width_dots, false));
    // Create a 100x100 black diagonal line for Cyan
    for (uint32_t r = 0; r < strip_height_lines; ++r) {
        for (uint32_t c = 0; c < strip_width_dots; ++c) {
            if (r == c) {
                cyan_pixel_data[r][c] = true; // Black pixel
            }
        }
    }
    std::vector<uint8_t> packed_cyan_data = packBilevelData(cyan_pixel_data, strip_width_dots, strip_height_lines);

    std::vector<uint8_t> page_block_payload_data;
    
    write_tlv_to_vec(page_block_payload_data, Tlv(GBOR_PAGE_NUMBER_TAG, (uint32_t)0));
    write_tlv_to_vec(page_block_payload_data, Tlv(GBOR_PAGE_HEIGHT_TAG, strip_height_lines));

    // CHANNEL_PAGE_INFO_TAG for Cyan
    std::vector<uint8_t> channel_page_info_payload; // Sub-TLVs for CH information
    write_tlv_to_vec(channel_page_info_payload, Tlv(GBOR_CHANNEL_NUMBER_TAG, (uint32_t)0)); // Cyan is channel 0
    
    // Add top/bottom/left/right margins if needed (simplified to 0 for POC)
    // These are implicit based on Borealis spec and are not directly in borealis.py as individual TLVs.
    // Typically: LEFT_MARGIN, RIGHT_MARGIN, TOP_MARGIN, BOTTOM_MARGIN
    
    write_tlv_to_vec(page_block_payload_data, Tlv(GBOR_CHANNEL_PAGE_INFO_TAG, channel_page_info_payload));

    // CHANNEL_DATA_INFO_TAG and CHANNEL_DATA_CHUNK_TAG for Cyan
    std::vector<uint8_t> channel_data_info_payload; // Sub-TLVs for CH data info
    write_tlv_to_vec(channel_data_info_payload, Tlv(GBOR_CHANNEL_NUMBER_TAG, (uint32_t)0)); // Cyan is channel 0
    write_tlv_to_vec(channel_data_info_payload, Tlv(GBOR_CHANNEL_DATA_CHUNK_TAG, packed_cyan_data)); // Actual pixel data
    write_tlv_to_vec(page_block_payload_data, Tlv(GBOR_CHANNEL_DATA_INFO_TAG, channel_data_info_payload));
    
    // Page Block Header
    writeBlockHeader(f, GBOR_PAGE_BLOCK_ID, page_block_payload_data.size());
    fwrite(page_block_payload_data.data(), 1, page_block_payload_data.size(), f);

    // End Job Block
    writeBlockHeader(f, GBOR_END_JOB_BLOCK_ID, 0); // End job block has 0 payload length


    fclose(f);
    std::cout << "Generated test GBOR file: " << filename << std::endl;
    return true;
}

} // namespace GborGenerator
