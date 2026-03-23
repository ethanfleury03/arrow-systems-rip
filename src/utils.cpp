#include "utils.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <fstream>
#include <thread>
#include <sys/stat.h>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace memjet {
namespace utils {

std::string generateJobId() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    std::ostringstream oss;
    oss << std::hex << ms << dis(gen);

    std::string hexId = oss.str();

    if (hexId.length() < 32) {
        hexId = std::string(32 - hexId.length(), '0') + hexId;
    } else if (hexId.length() > 32) {
        hexId = hexId.substr(hexId.length() - 32);
    }

    return hexId;
}

CommandLineArgs parseCommandLine(int argc, char* argv[]) {
    CommandLineArgs args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            exit(0);
        } else if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
            args.inputPdf = argv[++i];
        } else if ((arg == "--pes-ip") && i + 1 < argc) {
            args.pesIp = argv[++i];
        } else if ((arg == "--pes-port") && i + 1 < argc) {
            args.pesPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-r" || arg == "--dpi") && i + 1 < argc) {
            args.dpi = std::stoi(argv[++i]);
        } else if ((arg == "-p" || arg == "--page") && i + 1 < argc) {
            args.pageNumber = std::stoi(argv[++i]);
        } else if ((arg == "--paper") && i + 1 < argc) {
            args.paperSize = argv[++i];
        } else if (arg == "--dry-run") {
            args.dryRun = true;
        } else if (arg == "-v" || arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "--cmyk") {
            args.cmyk = true;
        } else if (arg == "--gray" || arg == "--grayscale" || arg == "--mono") {
            args.cmyk = false;
        } else if (arg == "--legacy-jsl") {
            args.legacyJsl = true;
        } else if (arg == "--timeout" && i + 1 < argc) {
            args.verifyTimeoutSec = std::stoi(argv[++i]);
            args.verifyTimeoutExplicit = true;
        } else if (arg == "--gymea-log" && i + 1 < argc) {
            args.gymeaLogPath = argv[++i];
        } else if (arg[0] != '-') {
            if (args.inputPdf.empty()) {
                args.inputPdf = arg;
            }
        }
    }

    return args;
}

void printUsage(const std::string& programName) {
    std::cout << "Memjet RIP Proof of Concept\n"
              << "Usage: " << programName << " [options] <input.pdf>\n\n"
              << "Options:\n"
              << "  -i, --input <file>     Input PDF file\n"
              << "  --pes-ip <ip>          PES printer IP address\n"
              << "  --pes-port <port>      PES port (default: 9090)\n"
              << "  -r, --dpi <dpi>        Resolution (default: 1600)\n"
              << "  -p, --page <num>       Page number to print (default: all)\n"
              << "  --paper <size>         Paper size: a4, letter (default: a4)\n"
              << "  --cmyk                 CMYK color mode\n"
              << "  --gray|--grayscale     Grayscale mode (default)\n"
              << "  --legacy-jsl           Use sequential single-color JSL jobs\n"
              << "  --timeout <sec>        Force hard timeout in seconds (default: adaptive model)\n"
              << "  --gymea-log <path>     Gymea log path (default: /var/log/gymea/gymea.log)\n"
              << "  --dry-run              Simulate without sending to printer\n"
              << "  -v, --verbose          Verbose output\n"
              << "  -h, --help             Show this help\n\n"
              << "Examples:\n"
              << "  " << programName << " document.pdf --pes-ip 192.168.1.100\n"
              << "  " << programName << " -i doc.pdf -r 1600 --dry-run\n"
              << "  " << programName << " -i doc.pdf --pes-ip 10.0.0.1 --legacy-jsl\n";
}

bool fileExists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

size_t getFileSize(const std::string& path) {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) == 0) {
        return buffer.st_size;
    }
    return 0;
}

void logInfo(const std::string& msg) {
    std::cout << "[INFO] " << msg << std::endl;
}

void logError(const std::string& msg) {
    std::cerr << "[ERROR] " << msg << std::endl;
}

void logVerbose(const std::string& msg, bool verbose) {
    if (verbose) {
        std::cout << "[VERBOSE] " << msg << std::endl;
    }
}

void logStructured(const std::string& component, const std::string& cmd,
                  const std::string& stateBefore, const std::string& stateAfter,
                  int queueLen, const std::string& result,
                  const std::string& errorCode,
                  int elapsedMs) {
    // Generate ISO 8601 timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::tm tm_buf;
    #ifdef _WIN32
    gmtime_s(&tm_buf, &time_t);
    #else
    gmtime_r(&time_t, &tm_buf);
    #endif
    
    char tsbuf[32];
    std::strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    std::ostringstream ts;
    ts << tsbuf;
    ts << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    
    // Build JSON object
    std::ostringstream json;
    json << "{"
         << "\"ts\":\"" << ts.str() << "\","
         << "\"component\":\"" << component << "\","
         << "\"cmd\":\"" << cmd << "\","
         << "\"state_before\":\"" << stateBefore << "\","
         << "\"state_after\":\"" << stateAfter << "\","
         << "\"queue_len\":" << queueLen << ","
         << "\"result\":\"" << result << "\"";
    
    if (!errorCode.empty()) {
        json << ",\"error_code\":\"" << errorCode << "\"";
    }
    
    json << "}";
    
    std::cout << json.str() << std::endl;
}

// --- TempFileGuard ---

TempFileGuard::TempFileGuard(const std::string& path)
    : path_(path), active_(true) {}

TempFileGuard::~TempFileGuard() {
    if (active_ && !path_.empty()) {
        if (std::remove(path_.c_str()) == 0) {
            std::cout << "[INFO] Temp file " << path_ << " cleaned up" << std::endl;
        }
    }
}

void TempFileGuard::release() {
    active_ = false;
}

// --- Print Verification ---

namespace {
int getEnvIntOrDefault(const char* name, int defValue) {
    if (const char* v = std::getenv(name)) {
        try {
            int parsed = std::stoi(v);
            if (parsed > 0) return parsed;
        } catch (...) {
        }
    }
    return defValue;
}

double getDpiMultiplier(int dpi) {
    if (dpi <= 600) return 1.0;
    if (dpi <= 1200) return 1.2;
    if (dpi <= 1600) return 1.45;
    if (dpi <= 2400) return 1.8;
    return 2.0;
}
} // namespace

int computeAdaptiveVerifyTimeoutSec(const PrintWaitConfig& cfg) {
    const int pages = std::max(1, (cfg.effectivePages > 0) ? cfg.effectivePages : cfg.requestedPages);
    const double fileMb = static_cast<double>(cfg.fileSizeBytes) / (1024.0 * 1024.0);
    const double dpiMult = getDpiMultiplier(cfg.dpi);
    const double colorMult = cfg.color ? 1.35 : 1.0;

    const double baseSec = 18.0;
    const double perPageSec = 14.0;
    const double perMbSec = 0.55;

    double model = (baseSec + perPageSec * static_cast<double>(pages) + perMbSec * fileMb) * dpiMult * colorMult;
    int timeout = static_cast<int>(std::lround(model));

    const int minSec = 35;
    const int maxSec = 420;
    timeout = std::max(minSec, std::min(maxSec, timeout));
    return timeout;
}

int defaultStallTimeoutSec(int hardTimeoutSec) {
    int baseline = std::max(20, hardTimeoutSec / 3);
    return std::min(120, baseline);
}

PrintVerificationResult verifyPrintExecution(
    const std::string& jobId,
    const std::string& gymeaLogPath,
    const PrintWaitConfig& waitCfg) {

    PrintVerificationResult result = {};

    if (!fileExists(gymeaLogPath)) {
        logError("Gymea log not found: " + gymeaLogPath + " -- skipping verification");
        result.summary = "Gymea log not found, verification skipped";
        return result;
    }

    const int hardTimeoutSec = waitCfg.timeoutExplicit
        ? std::max(1, waitCfg.timeoutSec)
        : computeAdaptiveVerifyTimeoutSec(waitCfg);

    const int pollMs = getEnvIntOrDefault("RIP_VERIFY_POLL_MS", 1000);
    const int stallTimeoutSec = getEnvIntOrDefault("RIP_VERIFY_STALL_SEC", defaultStallTimeoutSec(hardTimeoutSec));

    if (waitCfg.timeoutExplicit) {
        logInfo("Verification timeout: explicit --timeout=" + std::to_string(hardTimeoutSec) + "s");
    } else {
        const double fileMb = static_cast<double>(waitCfg.fileSizeBytes) / (1024.0 * 1024.0);
        std::ostringstream model;
        model << std::fixed << std::setprecision(2)
              << "Verification timeout (adaptive): " << hardTimeoutSec << "s"
              << " [file=" << fileMb << "MB"
              << ", pages(req/eff)=" << std::max(1, waitCfg.requestedPages) << "/" << std::max(1, waitCfg.effectivePages)
              << ", dpi=" << waitCfg.dpi
              << ", mode=" << (waitCfg.color ? "color" : "mono") << "]";
        logInfo(model.str());
    }

    logInfo("Verifying print execution for job " + jobId +
            " (hard-timeout=" + std::to_string(hardTimeoutSec) +
            "s, stall-timeout=" + std::to_string(stallTimeoutSec) + "s)...");

    std::string patJobDetected = "onNewJobDetected(" + jobId;
    std::string patPageActivated = "newPageHasBeenActivated(" + jobId;
    std::string patMarking = "Marking job " + jobId;
    std::string patMarkingOk = "PAGE_COMPLETE_OK";
    std::string patInPrintMode = "IN_PRINT_MODE";
    std::string patPageComplete = "onPageComplete(PAGE_COMPLETE_OK";
    std::string patFault = "FAULT_";
    std::string patCancel = "CANCEL";

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(hardTimeoutSec);
    auto lastProgress = std::chrono::steady_clock::now();
    std::streamoff lastPos = 0;

    std::string phase = "queued";
    logInfo("Print state: queued");

    auto setPhase = [&](const std::string& next) {
        if (phase != next) {
            phase = next;
            logInfo("Print state: " + phase);
        }
    };

    auto markProgress = [&]() {
        lastProgress = std::chrono::steady_clock::now();
    };

    std::string exitReason = "hard timeout exceeded without completion";

    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream logFile(gymeaLogPath);
        if (!logFile) {
            break;
        }

        logFile.seekg(lastPos);
        std::string line;
        bool sawNewData = false;

        while (std::getline(logFile, line)) {
            sawNewData = true;
            bool progressedThisLine = false;

            if (!result.jobDetected && line.find(patJobDetected) != std::string::npos) {
                result.jobDetected = true;
                setPhase("queued");
                progressedThisLine = true;
            }
            if (!result.pageActivated && line.find(patPageActivated) != std::string::npos) {
                result.pageActivated = true;
                setPhase("printing");
                progressedThisLine = true;
            }
            if (!result.markingComplete && line.find(patMarking) != std::string::npos && line.find(patMarkingOk) != std::string::npos) {
                result.markingComplete = true;
                setPhase("completed");
                progressedThisLine = true;
            }
            if (!result.inPrintMode && line.find(patInPrintMode) != std::string::npos) {
                result.inPrintMode = true;
                setPhase("printing");
                progressedThisLine = true;
            }
            if (!result.pageCompleteOk && line.find(patPageComplete) != std::string::npos) {
                result.pageCompleteOk = true;
                progressedThisLine = true;
            }
            if (line.find(jobId) != std::string::npos) {
                if (!result.faultDetected && line.find(patFault) != std::string::npos) {
                    result.faultDetected = true;
                    setPhase("fault");
                    progressedThisLine = true;
                }
                if (!result.cancelDetected && line.find(patCancel) != std::string::npos) {
                    result.cancelDetected = true;
                    setPhase("fault");
                    progressedThisLine = true;
                }
            }

            if (progressedThisLine) {
                markProgress();
            }
        }

        lastPos = logFile.tellg();
        if (lastPos < 0) {
            logFile.clear();
            logFile.seekg(0, std::ios::end);
            lastPos = logFile.tellg();
            if (lastPos < 0) lastPos = 0;
        }

        if (sawNewData && phase == "queued" && !result.pageActivated) {
            setPhase("sending");
        }

        if (result.passed()) {
            exitReason = "completion signals detected";
            break;
        }
        if (result.faultDetected || result.cancelDetected) {
            exitReason = result.faultDetected ? "fault detected in Gymea log" : "cancel detected in Gymea log";
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto stalledFor = std::chrono::duration_cast<std::chrono::seconds>(now - lastProgress).count();
        if (stalledFor >= stallTimeoutSec) {
            exitReason = "stalled: no new job progress for " + std::to_string(stalledFor) + "s";
            logError("Print verification stall detected for job " + jobId + ": " + exitReason);
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    }

    auto logSignal = [](const std::string& label, const std::string& level,
                        bool found) {
        std::string status = found ? "FOUND" : "NOT FOUND";
        if (found) {
            std::cout << "[INFO] Gymea [" << level << "]: " << label
                      << " -- " << status << std::endl;
        } else {
            std::cerr << "[WARN] Gymea [" << level << "]: " << label
                      << " -- " << status << std::endl;
        }
    };

    logSignal("onNewJobDetected(" + jobId + ")", "REQUIRED", result.jobDetected);
    logSignal("newPageHasBeenActivated(" + jobId + ",...)", "REQUIRED", result.pageActivated);
    logSignal("Marking job " + jobId + " ... PAGE_COMPLETE_OK", "REQUIRED", result.markingComplete);
    logSignal("IN_PRINT_MODE", "supportive", result.inPrintMode);
    logSignal("onPageComplete(PAGE_COMPLETE_OK)", "supportive", result.pageCompleteOk);

    if (result.faultDetected) {
        logError("FAULT detected for job " + jobId);
    }
    if (result.cancelDetected) {
        logError("CANCEL detected for job " + jobId);
    }

    std::ostringstream ss;
    if (result.passed()) {
        ss << "Print verified: job " << jobId << " completed successfully"
           << " (reason=" << exitReason << ")";
    } else {
        ss << "Print not completed (reason=" << exitReason << "). Missing REQUIRED signals:";
        if (!result.jobDetected) ss << " onNewJobDetected";
        if (!result.pageActivated) ss << " newPageHasBeenActivated";
        if (!result.markingComplete) ss << " Marking_job_PAGE_COMPLETE_OK";
        if (result.faultDetected) ss << " [FAULT detected]";
        if (result.cancelDetected) ss << " [CANCEL detected]";
    }
    result.summary = ss.str();

    return result;
}

PrintVerificationResult verifyPrintExecution(
    const std::string& jobId,
    const std::string& gymeaLogPath,
    int timeoutSec) {
    PrintWaitConfig cfg;
    cfg.timeoutSec = timeoutSec;
    cfg.timeoutExplicit = true;
    return verifyPrintExecution(jobId, gymeaLogPath, cfg);
}

} // namespace utils
} // namespace memjet
