#ifndef JSL_WRAPPER_H
#define JSL_WRAPPER_H

#include <string>
#include <vector>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "jobSubmissionLib.h"

namespace memjet {

// Color plane enumeration
enum class ColorPlane {
    CYAN = JSLIB_CYAN,
    MAGENTA = JSLIB_MAGENTA,
    YELLOW = JSLIB_YELLOW,
    BLACK = JSLIB_BLACK
};

// Job configuration
struct JobConfig {
    std::string destination;        // PES IP address
    uint16_t port;                  // PES port (default: 9090)
    uint32_t resolution;            // DPI (600, 954, or 1600)
    uint32_t stripStart;            // Offset in dots
    uint32_t stripWidth;            // Width in dots
    std::string jobId;              // Unique job identifier
};

// Raster page data for a single color plane
struct PageData {
    ColorPlane color;
    uint32_t pageNumber;
    std::vector<uint8_t> data;      // Bilevel raster data
    size_t width;                   // Width in pixels
    size_t height;                  // Height in pixels
};

class JSLWrapper {
public:
    JSLWrapper();
    ~JSLWrapper();

    // Initialize JSL library (reads JSL_CONFIG_PATH + JSL_PRINT_MODE env)
    bool initialize();
    
    // Set logging callback
    void setLoggingLevel(JslibLoggingLevel level);

    // --- Multi-color job API (correct for single-printhead CMYK) ---

    // Open a multi-color job: one job handle for all planes
    bool openJobMultiColor(const JobConfig& config,
                           const std::vector<ColorPlane>& colors,
                           JslibHdl& handle);

    // Add a page with data for all color planes at once
    bool addPageMultiColor(JslibHdl handle,
                           const std::vector<PageData>& planes,
                           bool isLastPage);

    // --- Legacy single-color API (for --legacy-jsl fallback) ---

    bool openJob(const JobConfig& config, ColorPlane color, 
                 uint32_t colorInstance, JslibHdl& handle);
    
    bool addPage(JslibHdl handle, const PageData& page, bool isLastPage);
    
    // --- Common ---

    bool closeJob(JslibHdl handle);
    bool abortJob(JslibHdl handle);
    
    static std::string getErrorString(JslibResultCode code);
    bool isInitialized() const { return initialized_; }
    uint32_t getYResolution() const { return yResolution_; }
    uint32_t getPrintMode() const { return printMode_; }

private:
    bool initialized_;
    uint32_t yResolution_;          // derived from JslConfigs.xml at init
    uint32_t printMode_;            // from JSL_PRINT_MODE env
    std::string configPath_;

    static const int MAX_RETRIES = 5;
    static const int BASE_RETRY_MS = 2000;

    bool isRetryable(JslibResultCode code) const;
    static void CALLBACK loggingCallback(JslibLoggingLevel level, const char* message);

    // Parse yResolution from JslConfigs.xml for the given printMode
    static uint32_t parseYResolutionFromConfig(const std::string& configPath,
                                               uint32_t printMode);
};

} // namespace memjet

#endif // JSL_WRAPPER_H
