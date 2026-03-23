#include "bilevel_converter.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>

namespace memjet {

BilevelConverter::BilevelConverter() {}

BilevelConverter::~BilevelConverter() {}

std::vector<uint8_t> BilevelConverter::convertToBilevel(
    const std::vector<uint8_t>& grayscale,
    int width,
    int height,
    uint8_t threshold) {
    
    if (grayscale.size() != static_cast<size_t>(width * height)) {
        throw ConversionException("Grayscale data size doesn't match dimensions");
    }
    
    std::vector<uint8_t> bilevel(width * height);
    
    for (size_t i = 0; i < grayscale.size(); ++i) {
        // Threshold: 0-127 = black (1), 128-255 = white (0)
        bilevel[i] = (grayscale[i] < threshold) ? 1 : 0;
    }
    
    return bilevel;
}

std::vector<uint8_t> BilevelConverter::convertToBilevelErrorDiffusion(
    const std::vector<uint8_t>& grayscale,
    int width,
    int height) {
    
    if (grayscale.size() != static_cast<size_t>(width * height)) {
        throw ConversionException("Grayscale data size doesn't match dimensions");
    }
    
    // Make a mutable copy for error diffusion
    std::vector<int16_t> working(grayscale.begin(), grayscale.end());
    std::vector<uint8_t> bilevel(width * height);
    
    // Floyd-Steinberg error diffusion
    // https://en.wikipedia.org/wiki/Floyd%E2%80%93Steinberg_dithering
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = y * width + x;
            int oldPixel = working[idx];
            int newPixel = (oldPixel < 128) ? 0 : 255;
            bilevel[idx] = (newPixel == 0) ? 1 : 0;
            
            int error = oldPixel - newPixel;
            
            // Distribute error to neighboring pixels
            if (x + 1 < width) {
                working[idx + 1] += error * 7 / 16;
            }
            if (y + 1 < height) {
                if (x > 0) {
                    working[idx + width - 1] += error * 3 / 16;
                }
                working[idx + width] += error * 5 / 16;
                if (x + 1 < width) {
                    working[idx + width + 1] += error / 16;
                }
            }
        }
    }
    
    return bilevel;
}

std::vector<uint8_t> BilevelConverter::convertToBilevelDither(
    const std::vector<uint8_t>& grayscale,
    int width,
    int height,
    const DitherConfig& dither) {
    
    if (grayscale.size() != static_cast<size_t>(width * height)) {
        throw ConversionException("Grayscale data size doesn't match dimensions");
    }
    
    std::vector<uint8_t> bilevel(width * height);
    
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = y * width + x;
            int threshold = dither.matrix[(y % dither.matrixHeight) * 
                                          dither.matrixWidth + 
                                          (x % dither.matrixWidth)];
            bilevel[idx] = (grayscale[idx] < threshold) ? 1 : 0;
        }
    }
    
    return bilevel;
}

std::vector<uint8_t> BilevelConverter::packToJSLFormat(
    const std::vector<uint8_t>& bilevel,
    int width,
    int height) {
    
    // JSL format requirements:
    // - Each line is packed into 32-bit words (uint32_t)
    // - Bit 0 is leftmost dot
    // - Lines must be padded to 128-bit (4 x 32-bit) boundaries
    
    // Calculate words per line (32 bits per word)
    int bitsPerLine = width;
    int wordsPerLine = (bitsPerLine + 31) / 32;  // Round up
    
    // Pad to 128-bit boundary (4 words)
    if (wordsPerLine % 4 != 0) {
        wordsPerLine += (4 - (wordsPerLine % 4));
    }
    
    // Total size in bytes
    size_t totalBytes = wordsPerLine * 4 * height;
    std::vector<uint8_t> packed(totalBytes, 0);
    
    for (int y = 0; y < height; ++y) {
        for (int w = 0; w < wordsPerLine; ++w) {
            uint32_t word = 0;
            
            for (int b = 0; b < 32; ++b) {
                int x = w * 32 + b;
                if (x < width) {
                    int srcIdx = y * width + x;
                    // Bit 0 is leftmost, so shift by (31 - b)
                    if (bilevel[srcIdx]) {
                        word |= (1u << b);
                    }
                }
            }
            
            // Store as little-endian (standard for most systems)
            size_t destIdx = (y * wordsPerLine + w) * 4;
            packed[destIdx] = word & 0xFF;
            packed[destIdx + 1] = (word >> 8) & 0xFF;
            packed[destIdx + 2] = (word >> 16) & 0xFF;
            packed[destIdx + 3] = (word >> 24) & 0xFF;
        }
    }
    
    std::cout << "Packed " << bilevel.size() << " bilevel pixels into " 
              << packed.size() << " bytes (JSL format)" << std::endl;
    
    return packed;
}

std::vector<uint8_t> BilevelConverter::convertAndPackStreaming(
    const std::string& pgmFilePath,
    size_t dataOffset,
    int width,
    int height) {

    std::ifstream file(pgmFilePath, std::ios::binary);
    if (!file) {
        throw ConversionException("Cannot open PGM file: " + pgmFilePath);
    }
    file.seekg(dataOffset);

    // JSL packing geometry
    int wordsPerLine = (width + 31) / 32;
    if (wordsPerLine % 4 != 0) {
        wordsPerLine += (4 - (wordsPerLine % 4));
    }
    int bytesPerPackedLine = wordsPerLine * 4;

    // Allocate only: packed output + 2 error-diffusion rows
    size_t packedSize = static_cast<size_t>(bytesPerPackedLine) * height;
    std::vector<uint8_t> packed(packedSize, 0);

    // Two rows of int16 for Floyd-Steinberg error propagation
    std::vector<int16_t> curRow(width, 0);
    std::vector<int16_t> nxtRow(width, 0);

    // One scanline read buffer
    std::vector<uint8_t> scanline(width);

    size_t peakMem = packedSize + (2 * width * sizeof(int16_t)) + width;
    std::cout << "[INFO] Streaming bilevel: packed=" << (packedSize / (1024*1024))
              << " MB, working=" << (2 * width * sizeof(int16_t) / 1024)
              << " KB, peak ~" << (peakMem / (1024*1024)) << " MB" << std::endl;

    for (int y = 0; y < height; ++y) {
        // Read one scanline of grayscale pixels from disk
        file.read(reinterpret_cast<char*>(scanline.data()), width);
        if (!file) {
            throw ConversionException("Premature end of PGM data at row " +
                std::to_string(y));
        }

        // Merge scanline pixels into current error row
        for (int x = 0; x < width; ++x) {
            curRow[x] += static_cast<int16_t>(scanline[x]);
        }

        // Floyd-Steinberg error diffusion on current row, pack directly
        for (int w = 0; w < wordsPerLine; ++w) {
            uint32_t word = 0;

            for (int b = 0; b < 32; ++b) {
                int x = w * 32 + b;
                if (x < width) {
                    int16_t oldPixel = curRow[x];
                    int16_t newPixel = (oldPixel < 128) ? 0 : 255;
                    bool isDot = (newPixel == 0);

                    if (isDot) {
                        word |= (1u << b);
                    }

                    int16_t error = oldPixel - newPixel;

                    // Distribute error: right, below-left, below, below-right
                    if (x + 1 < width) {
                        curRow[x + 1] += error * 7 / 16;
                    }
                    if (y + 1 < height) {
                        if (x > 0) {
                            nxtRow[x - 1] += error * 3 / 16;
                        }
                        nxtRow[x] += error * 5 / 16;
                        if (x + 1 < width) {
                            nxtRow[x + 1] += error / 16;
                        }
                    }
                }
            }

            // Store packed word little-endian
            size_t destIdx = static_cast<size_t>(y) * bytesPerPackedLine +
                             static_cast<size_t>(w) * 4;
            packed[destIdx]     =  word        & 0xFF;
            packed[destIdx + 1] = (word >> 8)  & 0xFF;
            packed[destIdx + 2] = (word >> 16) & 0xFF;
            packed[destIdx + 3] = (word >> 24) & 0xFF;
        }

        // Rotate rows: next becomes current, reset next
        curRow.swap(nxtRow);
        std::fill(nxtRow.begin(), nxtRow.end(), 0);
    }

    return packed;
}

} // namespace memjet
