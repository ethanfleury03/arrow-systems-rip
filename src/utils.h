#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>
#include <cstdint>

namespace memjet {
namespace utils {

// Generate a unique job ID
std::string generateJobId();

// Parse command line arguments
struct CommandLineArgs {
    std::string inputPdf;
    std::string pesIp;
    uint16_t pesPort = 9090;
    int dpi = 1600;
    bool dryRun = false;
    bool verbose = false;
    int pageNumber = 0;  // 0 = all pages
    std::string paperSize = "a4";
    bool cmyk = false;
    bool legacyJsl = false;
    int verifyTimeoutSec = 45;
    std::string gymeaLogPath = "/var/log/gymea/gymea.log";
};

CommandLineArgs parseCommandLine(int argc, char* argv[]);

void printUsage(const std::string& programName);

// File utilities
bool fileExists(const std::string& path);
size_t getFileSize(const std::string& path);

// Logging
void logInfo(const std::string& msg);
void logError(const std::string& msg);
void logVerbose(const std::string& msg, bool verbose);

// Structured logging (JSON format for observability)
void logStructured(const std::string& component, const std::string& cmd,
                  const std::string& stateBefore, const std::string& stateAfter,
                  int queueLen, const std::string& result,
                  const std::string& errorCode = "",
                  int elapsedMs = -1);

// RAII guard for temp files -- calls std::remove in destructor
class TempFileGuard {
public:
    explicit TempFileGuard(const std::string& path);
    ~TempFileGuard();
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
    void release();  // disarm the guard (don't delete on destruction)
    const std::string& path() const { return path_; }
private:
    std::string path_;
    bool active_;
};

// Print verification (jobId-scoped signals from Gymea log)
struct PrintVerificationResult {
    bool jobDetected;       // onNewJobDetected(<jobId>) -- REQUIRED
    bool pageActivated;     // newPageHasBeenActivated(<jobId>,...) -- REQUIRED
    bool markingComplete;   // Marking job <jobId> ... PAGE_COMPLETE_OK -- REQUIRED
    bool inPrintMode;       // IN_PRINT_MODE -- supportive (global)
    bool pageCompleteOk;    // onPageComplete(PAGE_COMPLETE_OK) -- supportive
    bool faultDetected;     // FAULT_ with jobId context
    bool cancelDetected;    // CANCEL with jobId context
    std::string summary;

    bool passed() const {
        return jobDetected && pageActivated && markingComplete
               && !faultDetected && !cancelDetected;
    }
};

PrintVerificationResult verifyPrintExecution(
    const std::string& jobId,
    const std::string& gymeaLogPath,
    int timeoutSec);

} // namespace utils
} // namespace memjet

#endif // UTILS_H
