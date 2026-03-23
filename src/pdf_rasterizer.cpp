#include "pdf_rasterizer.h"
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <array>
#include <memory>
#include <fstream>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace memjet {
namespace {
namespace fs = std::filesystem;

std::string getTempDirPath() {
    if (const char* env = std::getenv("RIP_TEMP_DIR")) {
        if (*env) return std::string(env);
    }
    std::error_code ec;
    fs::path p = fs::temp_directory_path(ec);
    if (!ec && !p.empty()) return p.string();
#ifdef _WIN32
    return "C:/Windows/Temp";
#else
    return "/tmp";
#endif
}

bool envBool(const char* name, bool defValue) {
    if (const char* v = std::getenv(name)) {
        std::string s(v);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        if (s.empty() || s == "0" || s == "false" || s == "no" || s == "off") return false;
        return true;
    }
    return defValue;
}

double envDouble(const char* name, double defValue) {
    if (const char* v = std::getenv(name)) {
        try { return std::stod(std::string(v)); } catch (...) { return defValue; }
    }
    return defValue;
}

long long envLongLong(const char* name, long long defValue) {
    if (const char* v = std::getenv(name)) {
        try { return std::stoll(std::string(v)); } catch (...) { return defValue; }
    }
    return defValue;
}

bool shouldCleanupTempFile(const fs::path& p) {
    const std::string name = p.filename().string();
    const bool ripPam = (name.rfind("rip_", 0) == 0) && (name.find(".pam") != std::string::npos);
    const bool debugPam = (name.rfind("debug_", 0) == 0) && (name.find(".pam") != std::string::npos);
    return ripPam || debugPam;
}

struct TempCleanupStats {
    size_t scanned = 0;
    size_t candidates = 0;
    size_t deleted = 0;
    uintmax_t freedBytes = 0;
    size_t failed = 0;
};

TempCleanupStats cleanupRipTempArtifacts(const fs::path& dir,
                                         int maxAgeHours,
                                         int keepLatest,
                                         bool dryRun,
                                         bool verbose) {
    TempCleanupStats stats;
    std::vector<std::pair<fs::path, fs::file_time_type>> files;
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        ++stats.scanned;
        if (!entry.is_regular_file(ec) || ec) continue;
        const fs::path p = entry.path();
        if (!shouldCleanupTempFile(p)) continue;
        ++stats.candidates;
        auto mt = fs::last_write_time(p, ec);
        if (ec) {
            ++stats.failed;
            continue;
        }
        files.push_back({p, mt});
    }

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });

    const auto now = fs::file_time_type::clock::now();
    const auto maxAge = std::chrono::hours(std::max(0, maxAgeHours));

    for (size_t i = 0; i < files.size(); ++i) {
        const fs::path& p = files[i].first;
        const auto mtime = files[i].second;
        const bool olderThanAge = (maxAgeHours >= 0) ? ((now - mtime) > maxAge) : false;
        const bool overKeep = (keepLatest >= 0) ? (static_cast<int>(i) >= keepLatest) : false;
        if (!(olderThanAge || overKeep)) continue;

        uintmax_t sz = fs::file_size(p, ec);
        if (ec) sz = 0;

        if (dryRun) {
            ++stats.deleted;
            stats.freedBytes += sz;
            continue;
        }

        if (fs::remove(p, ec) && !ec) {
            ++stats.deleted;
            stats.freedBytes += sz;
        } else {
            ++stats.failed;
        }
    }

    if (verbose) {
        std::cout << "[INFO] Temp cleanup dir=" << dir.string()
                  << " scanned=" << stats.scanned
                  << " candidates=" << stats.candidates
                  << " deleted=" << stats.deleted
                  << " freedBytes=" << stats.freedBytes
                  << " failed=" << stats.failed
                  << (dryRun ? " (dry-run)" : "")
                  << std::endl;
    }

    return stats;
}

struct TempSpaceCheck {
    uintmax_t availableBytes = 0;
    uintmax_t requiredBytes = 0;
    uintmax_t minFreeBytes = 0;
};

TempSpaceCheck preflightTempSpaceForPam(const fs::path& tempDir,
                                        int width,
                                        int height,
                                        double safetyMultiplier,
                                        uintmax_t minFreeBytes,
                                        bool verbose) {
    std::error_code ec;
    const auto si = fs::space(tempDir, ec);
    if (ec) {
        throw RasterizationException("Failed to query temp free space for " + tempDir.string() + ": " + ec.message());
    }

    const uintmax_t payloadBytes = static_cast<uintmax_t>(width) * static_cast<uintmax_t>(height) * 4ull;
    const uintmax_t withSafety = static_cast<uintmax_t>(static_cast<long double>(payloadBytes) * std::max(1.0, safetyMultiplier));
    const uintmax_t required = withSafety + minFreeBytes;

    if (verbose) {
        std::cout << "[INFO] Temp preflight dir=" << tempDir.string()
                  << " width=" << width
                  << " height=" << height
                  << " rawPamBytes=" << payloadBytes
                  << " safetyMultiplier=" << safetyMultiplier
                  << " minFreeBytes=" << minFreeBytes
                  << " requiredBytes=" << required
                  << " freeBytes=" << si.available
                  << std::endl;
    }

    if (si.available < required) {
        std::ostringstream oss;
        oss << "Insufficient temp space for CMYK raster output. "
            << "requiredBytes=" << required
            << " (rawPamBytes=" << payloadBytes
            << ", safetyMultiplier=" << safetyMultiplier
            << ", minFreeBytes=" << minFreeBytes
            << "), freeBytes=" << si.available
            << ", tempPath=" << tempDir.string();
        throw RasterizationException(oss.str());
    }

    TempSpaceCheck out;
    out.availableBytes = si.available;
    out.requiredBytes = required;
    out.minFreeBytes = minFreeBytes;
    return out;
}

} // namespace

PDFRasterizer::PDFRasterizer() : initialized_(false) {}

PDFRasterizer::~PDFRasterizer() {}

bool PDFRasterizer::initialize() {
    // Check if ghostscript is available
#ifdef _WIN32
    int result = system("where gswin64c >NUL 2>&1 || where gs >NUL 2>&1");
#else
    int result = system("which gs > /dev/null 2>&1");
#endif
    if (result != 0) {
        lastError_ = "Ghostscript (gswin64c/gs) not found in PATH";
        return false;
    }
    
    initialized_ = true;
    return true;
}

std::vector<RasterPage> PDFRasterizer::rasterize(const std::string& pdfPath,
                                                  const RasterParams& params) {
    std::vector<RasterPage> pages;
    
    if (!initialized_) {
        throw RasterizationException("Rasterizer not initialized");
    }

    // Use temp file instead of stdout for reliable binary data handling
#ifdef _WIN32
    std::string tempPpm = "C:/Windows/Temp/rip_output.ppm";
#else
    std::string tempPpm = "/tmp/rip_output.ppm";
#endif
    
    // Build Ghostscript command
    std::ostringstream cmd;
    cmd << "gswin64c ";
    cmd << "-q ";                          // Quiet mode
    cmd << "-dNOPAUSE ";                   // No pause between pages
    cmd << "-dBATCH ";                     // Batch mode (exit after)
    cmd << "-sDEVICE=ppmraw ";             // Raw PPM output
    cmd << "-r" << params.dpi << " ";      // Resolution
    cmd << "-sOutputFile=" << tempPpm << " ";  // Output to temp file
    
    // Paper size
    if (!params.paperSize.empty()) {
        cmd << "-sPAPERSIZE=" << params.paperSize << " ";
    }
    
    // Page selection
    if (params.pageNumber > 0) {
        cmd << "-dFirstPage=" << params.pageNumber << " ";
        cmd << "-dLastPage=" << params.pageNumber << " ";
    }
    
    // Grayscale
    cmd << "-dGrayValues=256 ";
    
    // Input file
    cmd << "-f \"" << pdfPath << "\"";
    
    std::cout << "Executing: " << cmd.str() << std::endl;
    
    // Execute Ghostscript
    int status = system(cmd.str().c_str());
    if (status != 0) {
        throw RasterizationException("Ghostscript failed with code " + 
                                     std::to_string(status));
    }
    
    // Read the temp file
    std::ifstream ppmFile(tempPpm, std::ios::binary);
    if (!ppmFile) {
        throw RasterizationException("Failed to read PPM output file");
    }
    
    // Get file size
    ppmFile.seekg(0, std::ios::end);
    size_t fileSize = ppmFile.tellg();
    ppmFile.seekg(0, std::ios::beg);
    
    if (fileSize == 0) {
        throw RasterizationException("Empty PPM output file");
    }
    
    // Read file into string
    std::string ppmData(fileSize, '\0');
    ppmFile.read(&ppmData[0], fileSize);
    ppmFile.close();
    
    // Parse PPM output
    try {
        RasterPage page = parsePPM(ppmData);
        page.pageNumber = params.pageNumber > 0 ? params.pageNumber : 1;
        pages.push_back(page);
        std::cout << "PNM: P6 " << page.width << "x" << page.height << " max=255" << std::endl;
    } catch (const std::exception& e) {
        throw RasterizationException(std::string("Failed to parse PPM: ") + e.what());
    }
    
    // Cleanup temp file
    std::remove(tempPpm.c_str());
    
    return pages;
}

RasterPage PDFRasterizer::parsePPM(const std::string& ppmData) {
    RasterPage page;
    
    // PPM format: P6\nwidth height\n255\nbinary_data
    const char* data = ppmData.data();
    size_t pos = 0;
    size_t len = ppmData.length();
    
    // Read magic number (P6)
    if (pos + 2 > len || data[0] != 'P' || data[1] != '6') {
        throw RasterizationException("Not a valid P6 PPM file");
    }
    pos += 2;
    
    // Skip whitespace
    while (pos < len && (data[pos] == ' ' || data[pos] == '\t' || 
                         data[pos] == '\n' || data[pos] == '\r')) {
        pos++;
    }
    
    // Skip comments
    while (pos < len && data[pos] == '#') {
        while (pos < len && data[pos] != '\n') {
            pos++;
        }
        pos++;
        while (pos < len && (data[pos] == ' ' || data[pos] == '\t' || 
                             data[pos] == '\n' || data[pos] == '\r')) {
            pos++;
        }
    }
    
    // Read width
    page.width = 0;
    while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
        page.width = page.width * 10 + (data[pos] - '0');
        pos++;
    }
    
    // Skip whitespace
    while (pos < len && (data[pos] == ' ' || data[pos] == '\t' || 
                         data[pos] == '\n' || data[pos] == '\r')) {
        pos++;
    }
    
    // Read height
    page.height = 0;
    while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
        page.height = page.height * 10 + (data[pos] - '0');
        pos++;
    }
    
    if (page.width == 0 || page.height == 0) {
        throw RasterizationException("Invalid image dimensions in PPM");
    }
    
    // Skip whitespace
    while (pos < len && (data[pos] == ' ' || data[pos] == '\t' || 
                         data[pos] == '\n' || data[pos] == '\r')) {
        pos++;
    }
    
    // Read max value (should be 255)
    int maxVal = 0;
    while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
        maxVal = maxVal * 10 + (data[pos] - '0');
        pos++;
    }
    
    // Skip single whitespace character after max value
    if (pos < len) {
        pos++;
    }
    
    // Read binary data (RGB triplets)
    size_t pixelCount = page.width * page.height;
    size_t expectedBytes = pixelCount * 3;
    
    if (pos + expectedBytes > len) {
        throw RasterizationException("Insufficient data in PPM file");
    }
    
    const uint8_t* rgbData = reinterpret_cast<const uint8_t*>(data + pos);
    
    // Convert RGB to grayscale
    page.bitsPerPixel = 8;
    page.data.resize(pixelCount);
    
    for (size_t i = 0; i < pixelCount; ++i) {
        uint8_t r = rgbData[i * 3];
        uint8_t g = rgbData[i * 3 + 1];
        uint8_t b = rgbData[i * 3 + 2];
        
        // Standard luminance formula
        page.data[i] = static_cast<uint8_t>(0.299 * r + 0.587 * g + 0.114 * b);
    }
    
    return page;
}

RasterFileInfo PDFRasterizer::rasterizeToFile(const std::string& pdfPath,
                                              const RasterParams& params) {
    if (!initialized_) {
        throw RasterizationException("Rasterizer not initialized");
    }

    // Unique temp path using PID + high-res timestamp to avoid collisions
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    const std::string tempDir = getTempDirPath();
    std::ostringstream tmpPath;
#ifdef _WIN32
    tmpPath << tempDir << "/rip_" << _getpid() << "_" << us << ".pgm";
#else
    tmpPath << tempDir << "/rip_" << getpid() << "_" << us << ".pgm";
#endif
    std::string tempPgm = tmpPath.str();

    std::string stderrFile = tempPgm + ".stderr";

    // Build Ghostscript command -- pgmraw outputs P5 grayscale directly
    std::ostringstream cmd;
    cmd << "gswin64c ";
    cmd << "-q ";
    cmd << "-dNOPAUSE ";
    cmd << "-dBATCH ";
    cmd << "-sDEVICE=pgmraw ";

    int xDpi = params.dpi;
    int yDpi = (params.yDpi > 0) ? params.yDpi : params.dpi;
    if (xDpi == yDpi) {
        cmd << "-r" << xDpi << " ";
    } else {
        cmd << "-r" << xDpi << "x" << yDpi << " ";
    }

    cmd << "-sOutputFile=" << tempPgm << " ";

    if (!params.paperSize.empty()) {
        cmd << "-sPAPERSIZE=" << params.paperSize << " ";
    }

    if (params.pageNumber > 0) {
        cmd << "-dFirstPage=" << params.pageNumber << " ";
        cmd << "-dLastPage=" << params.pageNumber << " ";
    }

    cmd << "-f \"" << pdfPath << "\"";
#ifdef _WIN32
    cmd << " 2>\"" << stderrFile << "\"";
#else
    cmd << " 2>" << stderrFile;
#endif

    std::cout << "[INFO] Executing: " << cmd.str() << std::endl;

    int status = system(cmd.str().c_str());
    if (status != 0) {
        int exitCode = status;
        // Read stderr for diagnostics
        std::ifstream errFile(stderrFile);
        std::string errMsg;
        if (errFile) {
            std::ostringstream ess;
            ess << errFile.rdbuf();
            errMsg = ess.str();
        }
        std::remove(stderrFile.c_str());
        std::remove(tempPgm.c_str());
        throw RasterizationException("Ghostscript failed (exit " +
            std::to_string(exitCode) + "): " + errMsg);
    }
    std::remove(stderrFile.c_str());

    RasterFileInfo info = parsePGMHeader(tempPgm);
    info.pageNumber = params.pageNumber > 0 ? params.pageNumber : 1;

    struct stat st;
    if (stat(tempPgm.c_str(), &st) == 0) {
        info.fileSizeBytes = st.st_size;
    }

    std::cout << "[INFO] Rasterized to " << tempPgm
              << " (" << info.width << "x" << info.height
              << ", " << (info.fileSizeBytes / (1024 * 1024)) << " MB on disk)"
              << std::endl;
    std::cout << "[INFO] Effective DPI: " << xDpi << "x" << yDpi << std::endl;

    return info;
}


CmykRasterData PDFRasterizer::rasterizeToCmykPlanes(const std::string& pdfPath,
                                                    const RasterParams& params) {
    if (!initialized_) {
        throw RasterizationException("Rasterizer not initialized");
    }

    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    const std::string tempDir = getTempDirPath();
    const fs::path tempDirPath(tempDir);

    std::ostringstream tmpPath;
#ifdef _WIN32
    tmpPath << tempDir << "/rip_" << _getpid() << "_" << us << ".pam";
#else
    tmpPath << tempDir << "/rip_" << getpid() << "_" << us << ".pam";
#endif
    std::string tempPam = tmpPath.str();
    std::string tempCmykPdf = tempPam + ".cmyk.pdf";
    std::string stderrFile = tempPam + ".stderr";

    auto computeSha256 = [](const std::string& path) -> std::string {
#ifdef _WIN32
        std::string escaped = path;
        size_t pos = 0;
        while ((pos = escaped.find("'", pos)) != std::string::npos) {
            escaped.replace(pos, 1, "''");
            pos += 2;
        }
        std::string cmd = "powershell -NoProfile -Command \"(Get-FileHash -Algorithm SHA256 -Path '" + escaped + "').Hash\"";
        FILE* pipe = _popen(cmd.c_str(), "r");
        if (!pipe) return "";
        char buffer[256];
        std::string out;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            out += buffer;
        }
        _pclose(pipe);
        out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
        out.erase(std::remove(out.begin(), out.end(), '\n'), out.end());
        return out;
#else
        (void)path;
        return "";
#endif
    };

    std::ostringstream cmd;
    cmd << "gswin64c ";
    cmd << "-q -dNOPAUSE -dBATCH ";
    cmd << "-sDEVICE=pamcmyk32 ";

    // Force stable CMYK conversion profile for normal PDFs (RGB/ICC/spot mixed sources)
    const char* envCmykProfile = std::getenv("GS_CMYK_PROFILE");
    std::string cmykProfile = (envCmykProfile && envCmykProfile[0] != '\0')
        ? std::string(envCmykProfile)
        : std::string("C:/Windows/System32/spool/drivers/color/DemandJetSG 1600 120 PVT2.icm");

    std::cout << "[INFO] Using CMYK ICC profile: " << cmykProfile << std::endl;
    std::string profileSha = computeSha256(cmykProfile);
    if (!profileSha.empty()) {
        std::cout << "[INFO] ICC SHA256: " << profileSha << std::endl;
    }

    // Step 1: normalize source into CMYK PDF with DemandJet profile enforced.
    std::string rasterSourcePdf = pdfPath;
    bool profileApplied = false;
    {
        std::ifstream iccCheck(cmykProfile, std::ios::binary);
        if (!iccCheck.good()) {
            std::remove(tempPam.c_str());
            throw RasterizationException("CMYK ICC profile not found: " + cmykProfile);
        }

        std::ostringstream pre;
        pre << "gswin64c ";
        pre << "-q -dNOSAFER -dNOPAUSE -dBATCH ";
        pre << "-sDEVICE=pdfwrite ";
        pre << "-sProcessColorModel=DeviceCMYK ";
        pre << "-sColorConversionStrategy=CMYK ";
        pre << "-dOverrideICC ";
        pre << "-sDefaultCMYKProfile=\"" << cmykProfile << "\" ";
        pre << "-sOutputICCProfile=\"" << cmykProfile << "\" ";
        pre << "-sOutputFile=\"" << tempCmykPdf << "\" ";
        if (params.pageNumber > 0) {
            pre << "-dFirstPage=" << params.pageNumber << " ";
            pre << "-dLastPage=" << params.pageNumber << " ";
        }
        pre << "-f \"" << pdfPath << "\"";
#ifdef _WIN32
        pre << " 2>\"" << stderrFile << "\"";
#else
        pre << " 2>" << stderrFile;
#endif

        std::cout << "[INFO] Executing CMYK normalize: " << pre.str() << std::endl;
        int preStatus = system(pre.str().c_str());
        if (preStatus != 0) {
            std::ifstream errFile(stderrFile);
            std::string errMsg;
            if (errFile) {
                std::ostringstream ess;
                ess << errFile.rdbuf();
                errMsg = ess.str();
            }
            std::remove(stderrFile.c_str());
            std::remove(tempPam.c_str());
            std::remove(tempCmykPdf.c_str());
            throw RasterizationException("Ghostscript CMYK normalize failed (exit " +
                std::to_string(preStatus) + "): " + errMsg);
        }

        std::remove(stderrFile.c_str());
        rasterSourcePdf = tempCmykPdf;
        profileApplied = true;
        std::cout << "[INFO] CMYK normalize complete (DemandJet profile enforced)" << std::endl;
        std::string normSha = computeSha256(tempCmykPdf);
        if (!normSha.empty()) {
            std::cout << "[INFO] Normalized CMYK PDF SHA256: " << normSha << std::endl;
        }
    }

    // Temp storage guardrails (cleanup + free-space preflight) before PAM generation.
    const bool tempVerbose = envBool("RIP_TEMP_VERBOSE", true);
    if (envBool("RIP_TEMP_CLEANUP_ENABLE", true)) {
        const int maxAgeHours = static_cast<int>(envLongLong("RIP_TEMP_CLEANUP_MAX_AGE_HOURS", 24));
        const int keepLatest = static_cast<int>(envLongLong("RIP_TEMP_CLEANUP_KEEP_LATEST", 20));
        const bool cleanupDryRun = envBool("RIP_TEMP_CLEANUP_DRY_RUN", false);
        cleanupRipTempArtifacts(tempDirPath, maxAgeHours, keepLatest, cleanupDryRun, tempVerbose);
    }

    int xDpi = params.dpi;
    int yDpi = (params.yDpi > 0) ? params.yDpi : params.dpi;

    // Estimate raster dimensions with Ghostscript bbox so we can preflight temp free space.
    int estWidth = 0;
    int estHeight = 0;
    {
        std::string bboxErr = tempPam + ".bbox.stderr";
        std::ostringstream bboxCmd;
        bboxCmd << "gswin64c -q -dNOPAUSE -dBATCH -sDEVICE=bbox ";
        if (params.pageNumber > 0) {
            bboxCmd << "-dFirstPage=" << params.pageNumber << " -dLastPage=" << params.pageNumber << " ";
        }
        bboxCmd << "-f \"" << rasterSourcePdf << "\"";
#ifdef _WIN32
        bboxCmd << " 2>\"" << bboxErr << "\"";
#else
        bboxCmd << " 2>" << bboxErr;
#endif
        (void)system(bboxCmd.str().c_str()); // best effort estimation

        std::ifstream bboxFile(bboxErr);
        if (bboxFile) {
            std::string line;
            while (std::getline(bboxFile, line)) {
                if (line.find("%%HiResBoundingBox:") != std::string::npos ||
                    line.find("%%BoundingBox:") != std::string::npos) {
                    std::istringstream iss(line.substr(line.find(":") + 1));
                    double llx = 0.0, lly = 0.0, urx = 0.0, ury = 0.0;
                    if (iss >> llx >> lly >> urx >> ury) {
                        const double wPt = std::max(0.0, urx - llx);
                        const double hPt = std::max(0.0, ury - lly);
                        estWidth = std::max(estWidth, static_cast<int>(std::ceil(wPt * static_cast<double>(xDpi) / 72.0)));
                        estHeight = std::max(estHeight, static_cast<int>(std::ceil(hPt * static_cast<double>(yDpi) / 72.0)));
                    }
                }
            }
        }
        std::remove(bboxErr.c_str());
    }

    if (estWidth <= 0 || estHeight <= 0) {
        throw RasterizationException("Unable to estimate CMYK raster dimensions for temp-space preflight");
    }

    const double safetyMultiplier = std::max(1.0, envDouble("RIP_TEMP_SAFETY_MULTIPLIER", 1.35));
    const uintmax_t minFreeBytes = static_cast<uintmax_t>(std::max<long long>(0, envLongLong("RIP_TEMP_MIN_FREE_BYTES", 0)));
    preflightTempSpaceForPam(tempDirPath, estWidth, estHeight, safetyMultiplier, minFreeBytes, tempVerbose);

    if (xDpi == yDpi) {
        cmd << "-r" << xDpi << " ";
    } else {
        cmd << "-r" << xDpi << "x" << yDpi << " ";
    }

    cmd << "-sOutputFile=" << tempPam << " ";

    if (!params.paperSize.empty()) {
        cmd << "-sPAPERSIZE=" << params.paperSize << " ";
    }

    if (params.pageNumber > 0) {
        cmd << "-dFirstPage=" << params.pageNumber << " ";
        cmd << "-dLastPage=" << params.pageNumber << " ";
    }

    cmd << "-f \"" << rasterSourcePdf << "\"";
#ifdef _WIN32
    cmd << " 2>\"" << stderrFile << "\"";
#else
    cmd << " 2>" << stderrFile;
#endif

    std::cout << "[INFO] Executing CMYK raster: " << cmd.str() << std::endl;
    int status = system(cmd.str().c_str());
    if (status != 0) {
        std::ifstream errFile(stderrFile);
        std::string errMsg;
        if (errFile) {
            std::ostringstream ess;
            ess << errFile.rdbuf();
            errMsg = ess.str();
        }
        std::remove(stderrFile.c_str());
        std::remove(tempPam.c_str());
        if (profileApplied) std::remove(tempCmykPdf.c_str());
        throw RasterizationException("Ghostscript CMYK failed (exit " +
            std::to_string(status) + "): " + errMsg);
    }
    std::remove(stderrFile.c_str());

    RasterFileInfo info = parsePAMHeader(tempPam);
    info.pageNumber = params.pageNumber > 0 ? params.pageNumber : 1;

    std::ifstream file(tempPam, std::ios::binary);
    if (!file) {
        std::remove(tempPam.c_str());
        if (profileApplied) std::remove(tempCmykPdf.c_str());
        throw RasterizationException("Failed to open CMYK PAM file: " + tempPam);
    }

    file.seekg(static_cast<std::streamoff>(info.dataOffset), std::ios::beg);
    const size_t pixelCount = static_cast<size_t>(info.width) * static_cast<size_t>(info.height);
    const size_t expectedBytes = pixelCount * 4;

    std::vector<uint8_t> interleaved(expectedBytes);
    file.read(reinterpret_cast<char*>(interleaved.data()), static_cast<std::streamsize>(expectedBytes));
    if (!file) {
        std::remove(tempPam.c_str());
        if (profileApplied) std::remove(tempCmykPdf.c_str());
        throw RasterizationException("Incomplete CMYK PAM payload");
    }

    CmykRasterData out;
    out.width = info.width;
    out.height = info.height;
    out.pageNumber = info.pageNumber;
    out.c.resize(pixelCount);
    out.m.resize(pixelCount);
    out.y.resize(pixelCount);
    out.k.resize(pixelCount);

    for (size_t i = 0; i < pixelCount; ++i) {
        out.c[i] = interleaved[i * 4 + 0];
        out.m[i] = interleaved[i * 4 + 1];
        out.y[i] = interleaved[i * 4 + 2];
        out.k[i] = interleaved[i * 4 + 3];
    }

    std::remove(tempPam.c_str());
    if (profileApplied) std::remove(tempCmykPdf.c_str());
    std::cout << "[INFO] Rasterized CMYK " << out.width << "x" << out.height
              << " (" << (expectedBytes / (1024 * 1024)) << " MB interleaved)" << std::endl;
    return out;
}

RasterFileInfo PDFRasterizer::parsePAMHeader(const std::string& filePath) {
    RasterFileInfo info;
    info.filePath = filePath;
    info.width = 0;
    info.height = 0;
    info.dataOffset = 0;
    info.fileSizeBytes = 0;

    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        throw RasterizationException("Failed to open PAM file: " + filePath);
    }

    std::string line;
    std::getline(file, line);
    if (line != "P7") {
        throw RasterizationException("Not a valid PAM file (expected P7)");
    }

    int depth = 0;
    int maxVal = 0;
    std::string tupleType;
    while (std::getline(file, line)) {
        if (line == "ENDHDR") break;
        std::istringstream iss(line);
        std::string key;
        iss >> key;
        if (key == "WIDTH") iss >> info.width;
        else if (key == "HEIGHT") iss >> info.height;
        else if (key == "DEPTH") iss >> depth;
        else if (key == "MAXVAL") iss >> maxVal;
        else if (key == "TUPLTYPE") iss >> tupleType;
    }

    if (info.width <= 0 || info.height <= 0) {
        throw RasterizationException("Invalid PAM dimensions");
    }
    if (depth != 4) {
        throw RasterizationException("Unsupported PAM DEPTH=" + std::to_string(depth) + " (expected 4)");
    }
    if (maxVal != 255) {
        throw RasterizationException("Unsupported PAM MAXVAL=" + std::to_string(maxVal));
    }

    info.dataOffset = static_cast<size_t>(file.tellg());
    if (info.dataOffset == static_cast<size_t>(-1)) {
        throw RasterizationException("Failed to read PAM data offset");
    }
    return info;
}

RasterFileInfo PDFRasterizer::parsePGMHeader(const std::string& filePath) {
    RasterFileInfo info;
    info.filePath = filePath;
    info.width = 0;
    info.height = 0;
    info.dataOffset = 0;
    info.fileSizeBytes = 0;

    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        throw RasterizationException("Failed to open PGM file: " + filePath);
    }

    // Read magic number
    char magic[3] = {};
    file.read(magic, 2);
    if (magic[0] != 'P' || magic[1] != '5') {
        throw RasterizationException("Not a valid P5 PGM file (got " +
            std::string(magic, 2) + ")");
    }

    // Helper: skip whitespace and comments
    auto skipWsAndComments = [&file]() {
        while (file) {
            int c = file.peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                file.get();
            } else if (c == '#') {
                // Skip comment line
                while (file && file.get() != '\n') {}
            } else {
                break;
            }
        }
    };

    auto readInt = [&file, &skipWsAndComments]() -> int {
        skipWsAndComments();
        int val = 0;
        while (file) {
            int c = file.peek();
            if (c >= '0' && c <= '9') {
                val = val * 10 + (file.get() - '0');
            } else {
                break;
            }
        }
        return val;
    };

    info.width = readInt();
    info.height = readInt();
    int maxVal = readInt();

    if (info.width == 0 || info.height == 0) {
        throw RasterizationException("Invalid PGM dimensions: " +
            std::to_string(info.width) + "x" + std::to_string(info.height));
    }
    if (maxVal != 255) {
        throw RasterizationException("Unsupported PGM maxval: " +
            std::to_string(maxVal) + " (expected 255)");
    }

    // Single whitespace char after maxval, then binary data begins
    file.get();
    info.dataOffset = file.tellg();

    return info;
}

} // namespace memjet

