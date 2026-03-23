#ifndef PDF_RASTERIZER_H
#define PDF_RASTERIZER_H

#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>

namespace memjet {

// Exception for rasterization errors
class RasterizationException : public std::runtime_error {
public:
    explicit RasterizationException(const std::string& msg) 
        : std::runtime_error(msg) {}
};

// Raster output format (full in-memory page -- legacy path)
struct RasterPage {
    int pageNumber;
    int width;
    int height;
    int bitsPerPixel;  // 8 for grayscale, typically
    std::vector<uint8_t> data;
};

// Disk-backed raster metadata (no pixel data in RAM)
struct RasterFileInfo {
    std::string filePath;  // PGM file on disk
    int width;
    int height;
    int pageNumber;
    size_t dataOffset;     // byte offset to start of pixel data in PGM
    size_t fileSizeBytes;
};

// In-memory CMYK separation from Ghostscript PAM CMYK output
struct CmykRasterData {
    int width;
    int height;
    int pageNumber;
    std::vector<uint8_t> c;
    std::vector<uint8_t> m;
    std::vector<uint8_t> y;
    std::vector<uint8_t> k;
};

// Rasterization parameters
struct RasterParams {
    int dpi;              // Horizontal resolution
    int yDpi;             // Vertical resolution (0 = same as dpi)
    int pageNumber;       // Page to rasterize (0 = all pages)
    std::string device;   // Ghostscript device (ppmgray, tiffgray, etc.)
    std::string paperSize; // A4, Letter, etc.
};

class PDFRasterizer {
public:
    PDFRasterizer();
    ~PDFRasterizer();

    // Initialize Ghostscript
    bool initialize();
    
    // Rasterize PDF to grayscale bitmap (legacy, loads into RAM)
    std::vector<RasterPage> rasterize(const std::string& pdfPath, 
                                      const RasterParams& params);

    // Rasterize PDF to disk-backed PGM file (low-memory path)
    RasterFileInfo rasterizeToFile(const std::string& pdfPath,
                                   const RasterParams& params);

    // Rasterize PDF to in-memory CMYK planar buffers
    CmykRasterData rasterizeToCmykPlanes(const std::string& pdfPath,
                                         const RasterParams& params);
    
    // Get last error message
    std::string getLastError() const { return lastError_; }

private:
    bool initialized_;
    std::string lastError_;
    
    // Execute Ghostscript command
    bool executeGhostscript(const std::vector<std::string>& args);
    
    // Parse PPM output (legacy)
    RasterPage parsePPM(const std::string& ppmData);

    // Parse PGM header only, returning metadata + data offset
    RasterFileInfo parsePGMHeader(const std::string& filePath);

    // Parse PAM header for CMYK (P7, DEPTH 4)
    RasterFileInfo parsePAMHeader(const std::string& filePath);
};

} // namespace memjet

#endif // PDF_RASTERIZER_H
