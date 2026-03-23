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
#include <cctype>
#include <cerrno>
#include <ctime>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/statvfs.h>
#endif

namespace memjet {
namespace {

std::string baseName(const std::string& path) {
    const size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
#ifdef _WIN32
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

uintmax_t safeFileSize(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<uintmax_t>(st.st_size);
}

std::time_t fileMTime(const std::string& path, bool* ok = nullptr) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        if (ok) *ok = false;
        return 0;
    }
    if (ok) *ok = true;
    return st.st_mtime;
}

bool removeFile(const std::string& path) {
    return std::remove(path.c_str()) == 0;
}

std::string getTempDirPath() {
    if (const char* env = std::getenv("RIP_TEMP_DIR")) {
        if (*env) return std::string(env);
    }
#ifdef _WIN32
    if (const char* env = std::getenv("TEMP")) if (*env) return std::string(env);
    if (const char* env = std::getenv("TMP")) if (*env) return std::string(env);
    return "C:/Windows/Temp";
#else
    if (const char* env = std::getenv("TMPDIR")) if (*env) return std::string(env);
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

bool shouldCleanupTempFile(const std::string& path) {
    const std::string name = baseName(path);
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

struct TempFileInfo {
    std::string path;
    std::time_t mtime = 0;
};

std::vector<std::string> listDirectoryFiles(const std::string& dir) {
    std::vector<std::string> files;
#ifdef _WIN32
    const std::string pattern = joinPath(dir, "*");
    WIN32_FIND_DATAA data;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &data);
    if (hFind == INVALID_HANDLE_VALUE) return files;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            files.push_back(joinPath(dir, data.cFileName));
        }
    } while (FindNextFileA(hFind, &data));
    FindClose(hFind);
#else
    DIR* dp = opendir(dir.c_str());
    if (!dp) return files;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dp)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
        std::string p = joinPath(dir, ent->d_name);
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            files.push_back(p);
        }
    }
    closedir(dp);
#endif
    return files;
}

TempCleanupStats cleanupRipTempArtifacts(const std::string& dir,
                                         int maxAgeHours,
                                         int keepLatest,
                                         bool dryRun,
                                         bool verbose) {
    TempCleanupStats stats;
    std::vector<TempFileInfo> files;

    for (const auto& path : listDirectoryFiles(dir)) {
        ++stats.scanned;
        if (!shouldCleanupTempFile(path)) continue;
        ++stats.candidates;
        bool ok = false;
        std::time_t mt = fileMTime(path, &ok);
        if (!ok) {
            ++stats.failed;
            continue;
        }
        TempFileInfo fi;
        fi.path = path;
        fi.mtime = mt;
        files.push_back(fi);
    }

    std::sort(files.begin(), files.end(), [](const TempFileInfo& a, const TempFileInfo& b) {
        return a.mtime > b.mtime;
    });

    const std::time_t now = std::time(nullptr);
    const long long maxAgeSec = static_cast<long long>(std::max(0, maxAgeHours)) * 3600ll;

    for (size_t i = 0; i < files.size(); ++i) {
        const std::string& path = files[i].path;
        const bool olderThanAge = (maxAgeHours >= 0) ? ((now - files[i].mtime) > maxAgeSec) : false;
        const bool overKeep = (keepLatest >= 0) ? (static_cast<int>(i) >= keepLatest) : false;
        if (!(olderThanAge || overKeep)) continue;

        const uintmax_t sz = safeFileSize(path);
        if (dryRun) {
            ++stats.deleted;
            stats.freedBytes += sz;
            continue;
        }

        if (removeFile(path)) {
            ++stats.deleted;
            stats.freedBytes += sz;
        } else {
            ++stats.failed;
        }
    }

    if (verbose) {
        std::cout << "[INFO] Temp cleanup dir=" << dir
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

uintmax_t getAvailableSpaceBytes(const std::string& dir) {
#ifdef _WIN32
    ULARGE_INTEGER freeBytesAvailable;
    if (!GetDiskFreeSpaceExA(dir.c_str(), &freeBytesAvailable, nullptr, nullptr)) {
        throw RasterizationException("Failed to query temp free space for " + dir);
    }
    return static_cast<uintmax_t>(freeBytesAvailable.QuadPart);
#else
    struct statvfs s;
    if (statvfs(dir.c_str(), &s) != 0) {
        throw RasterizationException("Failed to query temp free space for " + dir + ": " + std::strerror(errno));
    }
    return static_cast<uintmax_t>(s.f_bavail) * static_cast<uintmax_t>(s.f_frsize);
#endif
}

TempSpaceCheck preflightTempSpaceForPam(const std::string& tempDir,
                                        int width,
                                        int height,
                                        double safetyMultiplier,
                                        uintmax_t minFreeBytes,
                                        bool verbose) {
    const uintmax_t availableBytes = getAvailableSpaceBytes(tempDir);
    const uintmax_t payloadBytes = static_cast<uintmax_t>(width) * static_cast<uintmax_t>(height) * 4ull;
    const uintmax_t withSafety = static_cast<uintmax_t>(static_cast<long double>(payloadBytes) * std::max(1.0, safetyMultiplier));
    const uintmax_t required = withSafety + minFreeBytes;

    if (verbose) {
        std::cout << "[INFO] Temp preflight dir=" << tempDir
                  << " width=" << width
                  << " height=" << height
                  << " rawPamBytes=" << payloadBytes
                  << " safetyMultiplier=" << safetyMultiplier
                  << " minFreeBytes=" << minFreeBytes
                  << " requiredBytes=" << required
                  << " freeBytes=" << availableBytes
                  << std::endl;
    }

    if (availableBytes < required) {
        std::ostringstream oss;
        oss << "Insufficient temp space for CMYK raster output. "
            << "requiredBytes=" << required
            << " (rawPamBytes=" << payloadBytes
            << ", safetyMultiplier=" << safetyMultiplier
            << ", minFreeBytes=" << minFreeBytes
            << "), freeBytes=" << availableBytes
            << ", tempPath=" << tempDir;
        throw RasterizationException(oss.str());
    }

    TempSpaceCheck out;
    out.availableBytes = availableBytes;
    out.requiredBytes = required;
    out.minFreeBytes = minFreeBytes;
    return out;
}

std::string readTextFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    std::ostringstream oss;
    oss << file.rdbuf();
    return oss.str();
}

std::string tailText(const std::string& input, size_t maxChars) {
    if (input.size() <= maxChars) return input;
    return input.substr(input.size() - maxChars);
}

std::string shellQuote(const std::string& arg) {
#ifdef _WIN32
    std::string out = "\"";
    for (char ch : arg) {
        if (ch == '"') out += "\\\"";
        else out += ch;
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char ch : arg) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    out += "'";
    return out;
#endif
}

bool containsPageDrawFailure(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return (lower.find("page drawing error occurred") != std::string::npos) ||
           (lower.find("could not draw this page at all") != std::string::npos);
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

    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    const std::string tempDir = getTempDirPath();

    auto makeTempPgmPath = [&](int attempt) {
        std::ostringstream tmpPath;
#ifdef _WIN32
        tmpPath << tempDir << "/rip_" << _getpid() << "_" << us << "_a" << attempt << ".pgm";
#else
        tmpPath << tempDir << "/rip_" << getpid() << "_" << us << "_a" << attempt << ".pgm";
#endif
        return tmpPath.str();
    };

    int xDpi = params.dpi;
    int yDpi = (params.yDpi > 0) ? params.yDpi : params.dpi;

    auto buildGhostscriptCommand = [&](const std::string& tempPgm, const std::string& stderrFile) {
        std::ostringstream cmd;
#ifdef _WIN32
        cmd << "gswin64c ";
#else
        cmd << "gs ";
#endif
        cmd << "-q -dNOPAUSE -dBATCH -dSAFER ";
        cmd << "-dPDFSTOPONERROR ";
        cmd << "-dAutoRotatePages=/None ";
        cmd << "-dBandBufferSpace=200000000 -dBufferSpace=200000000 ";
        cmd << "-sDEVICE=pgmraw ";

        if (xDpi == yDpi) {
            cmd << "-r" << xDpi << " ";
        } else {
            cmd << "-r" << xDpi << "x" << yDpi << " ";
        }

        cmd << "-sOutputFile=" << shellQuote(tempPgm) << " ";

        if (!params.paperSize.empty()) {
            cmd << "-sPAPERSIZE=" << params.paperSize << " ";
        }

        if (params.pageNumber > 0) {
            cmd << "-dFirstPage=" << params.pageNumber << " ";
            cmd << "-dLastPage=" << params.pageNumber << " ";
        }

        cmd << "-f " << shellQuote(pdfPath);
        cmd << " 2>" << shellQuote(stderrFile);
        return cmd.str();
    };

    std::string finalTempPgm;
    std::string failureReason;

    for (int attempt = 1; attempt <= 2; ++attempt) {
        const std::string tempPgm = makeTempPgmPath(attempt);
        const std::string stderrFile = tempPgm + ".stderr";
        std::remove(tempPgm.c_str());
        std::remove(stderrFile.c_str());

        const std::string cmd = buildGhostscriptCommand(tempPgm, stderrFile);
        std::cout << "[INFO] Executing GS (attempt " << attempt << "/2): " << cmd << std::endl;
        int status = system(cmd.c_str());

        const std::string errMsg = readTextFile(stderrFile);
        const std::string errTail = tailText(errMsg, 1200);
        const uintmax_t outputSize = safeFileSize(tempPgm);
        const bool drawFailure = containsPageDrawFailure(errMsg);

        std::cout << "[INFO] GS attempt=" << attempt
                  << " exitCode=" << status
                  << " outputBytes=" << outputSize
                  << " stderrTail=" << (errTail.empty() ? "<empty>" : errTail)
                  << std::endl;

        if (status != 0 || drawFailure) {
            std::ostringstream reason;
            reason << "Ghostscript rasterization failed at " << xDpi << "x" << yDpi << " DPI"
                   << " (attempt " << attempt << ")"
                   << ", exitCode=" << status;
            if (drawFailure) {
                reason << ", detected page draw failure signature in stderr"
                       << " (e.g. 'Page drawing error occurred / Could not draw this page at all').";
            }
            if (!errTail.empty()) {
                reason << " stderrTail=" << errTail;
            }
            failureReason = reason.str();

            std::remove(stderrFile.c_str());
            std::remove(tempPgm.c_str());
            break;
        }

        try {
            RasterFileInfo info = parsePGMHeader(tempPgm);
            info.pageNumber = params.pageNumber > 0 ? params.pageNumber : 1;
            info.fileSizeBytes = static_cast<size_t>(outputSize);

            std::remove(stderrFile.c_str());
            finalTempPgm = tempPgm;

            std::cout << "[INFO] Rasterized to " << finalTempPgm
                      << " (" << info.width << "x" << info.height
                      << ", " << (info.fileSizeBytes / (1024 * 1024)) << " MB on disk)"
                      << std::endl;
            std::cout << "[INFO] Effective DPI: " << xDpi << "x" << yDpi << std::endl;
            return info;
        } catch (const std::exception& e) {
            const uintmax_t recheckSize = safeFileSize(tempPgm);
            std::ostringstream reason;
            reason << "PGM validation failed after successful Ghostscript run"
                   << " (attempt " << attempt << ")"
                   << ", outputBytes=" << recheckSize
                   << ", reason=" << e.what();
            if (!errTail.empty()) {
                reason << ", stderrTail=" << errTail;
            }
            failureReason = reason.str();

            std::cout << "[ERROR] " << failureReason << std::endl;

            std::remove(stderrFile.c_str());
            std::remove(tempPgm.c_str());

            // Guarded same-DPI retry for suspected transient write/read races.
            if (attempt == 1) {
                std::cout << "[WARN] Retrying Ghostscript once at same DPI due to validation failure" << std::endl;
                continue;
            }
            break;
        }
    }

    if (failureReason.empty()) {
        failureReason = "Ghostscript rasterization failed for unknown reason";
    }
    throw RasterizationException(failureReason + ". Aborting before bilevel conversion.");
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
        cleanupRipTempArtifacts(tempDir, maxAgeHours, keepLatest, cleanupDryRun, tempVerbose);
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
    preflightTempSpaceForPam(tempDir, estWidth, estHeight, safetyMultiplier, minFreeBytes, tempVerbose);

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

    char magic[3] = {};
    file.read(magic, 2);
    if (magic[0] != 'P' || magic[1] != '5') {
        throw RasterizationException("Not a valid P5 PGM file (got " +
            std::string(magic, 2) + ")");
    }

    auto skipWsAndComments = [&file]() {
        while (file) {
            int c = file.peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                file.get();
            } else if (c == '#') {
                while (file && file.get() != '\n') {}
            } else {
                break;
            }
        }
    };

    auto readInt = [&file, &skipWsAndComments]() -> int {
        skipWsAndComments();
        int val = 0;
        bool gotDigit = false;
        while (file) {
            int c = file.peek();
            if (c >= '0' && c <= '9') {
                gotDigit = true;
                val = val * 10 + (file.get() - '0');
            } else {
                break;
            }
        }
        if (!gotDigit) return 0;
        return val;
    };

    info.width = readInt();
    info.height = readInt();
    int maxVal = readInt();

    if (info.width <= 0 || info.height <= 0) {
        throw RasterizationException("Invalid PGM dimensions: " +
            std::to_string(info.width) + "x" + std::to_string(info.height));
    }
    if (maxVal != 255) {
        throw RasterizationException("Unsupported PGM maxval: " +
            std::to_string(maxVal) + " (expected 255)");
    }

    int delim = file.get();
    if (delim == EOF) {
        throw RasterizationException("PGM header ended unexpectedly before pixel data");
    }
    info.dataOffset = static_cast<size_t>(file.tellg());

    struct stat st;
    if (stat(filePath.c_str(), &st) != 0) {
        throw RasterizationException("Failed to stat PGM file for size validation: " + filePath);
    }
    info.fileSizeBytes = static_cast<size_t>(st.st_size);

    const uint64_t expectedDataBytes = static_cast<uint64_t>(info.width) * static_cast<uint64_t>(info.height);
    const uint64_t expectedTotalBytes = static_cast<uint64_t>(info.dataOffset) + expectedDataBytes;
    const uint64_t actualBytes = static_cast<uint64_t>(info.fileSizeBytes);

    if (actualBytes < expectedTotalBytes) {
        std::ostringstream oss;
        oss << "Truncated PGM raster output: actualBytes=" << actualBytes
            << ", expectedTotalBytes=" << expectedTotalBytes
            << ", headerBytes=" << info.dataOffset
            << ", expectedPixelBytes=" << expectedDataBytes
            << ", width=" << info.width
            << ", height=" << info.height;
        throw RasterizationException(oss.str());
    }

    return info;
}

} // namespace memjet

