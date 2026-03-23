#ifndef BILEVEL_CONVERTER_H
#define BILEVEL_CONVERTER_H

#include <vector>
#include <cstdint>
#include <stdexcept>

namespace memjet {

// Exception for conversion errors
class ConversionException : public std::runtime_error {
public:
    explicit ConversionException(const std::string& msg) 
        : std::runtime_error(msg) {}
};

// Halftone algorithm selection
enum class HalftoneAlgorithm {
    THRESHOLD,      // Simple threshold
    ERROR_DIFFUSION // Floyd-Steinberg error diffusion
};

// Dither matrix configuration
struct DitherConfig {
    std::vector<uint8_t> matrix;  // Threshold matrix
    int matrixWidth;
    int matrixHeight;
};

class BilevelConverter {
public:
    BilevelConverter();
    ~BilevelConverter();

    // Convert grayscale to bilevel using threshold
    std::vector<uint8_t> convertToBilevel(
        const std::vector<uint8_t>& grayscale,
        int width,
        int height,
        uint8_t threshold = 128
    );

    // Convert grayscale to bilevel using error diffusion
    std::vector<uint8_t> convertToBilevelErrorDiffusion(
        const std::vector<uint8_t>& grayscale,
        int width,
        int height
    );

    // Convert grayscale to bilevel using dither matrix
    std::vector<uint8_t> convertToBilevelDither(
        const std::vector<uint8_t>& grayscale,
        int width,
        int height,
        const DitherConfig& dither
    );

    // Pack bilevel data into 32-bit words (JSL format)
    // JSL requires:
    // - 32-bit words (uint32_t)
    // - Bit 0 = leftmost dot
    // - Lines padded to 128-bit (4 x 32-bit) boundaries
    std::vector<uint8_t> packToJSLFormat(
        const std::vector<uint8_t>& bilevel,
        int width,
        int height
    );

    // Streaming conversion: reads PGM rows from disk, applies Floyd-Steinberg
    // error diffusion with a 2-row buffer, and packs directly to JSL format.
    // Peak memory: ~2 rows of int16 + full packed output buffer.
    std::vector<uint8_t> convertAndPackStreaming(
        const std::string& pgmFilePath,
        size_t dataOffset,
        int width,
        int height
    );

    // Get last error
    std::string getLastError() const { return lastError_; }

private:
    std::string lastError_;
};

} // namespace memjet

#endif // BILEVEL_CONVERTER_H
