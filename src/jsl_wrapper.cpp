#include "jsl_wrapper.h"
#include <iostream>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <vector>
#include <filesystem>

namespace memjet {

namespace {

static bool isHexJobId(const std::string& s) {
    if (s.size() != JSLIB_LEN_JOB_ID) return false;
    for (char c : s) {
        bool isHex = (c >= '0' && c <= '9') ||
                     (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
        if (!isHex) return false;
    }
    return true;
}

} // namespace

JSLWrapper::JSLWrapper()
    : initialized_(false), yResolution_(0), printMode_(2) {}

JSLWrapper::~JSLWrapper() {
    if (initialized_) {
        jslibShutdown();
    }
}

uint32_t JSLWrapper::parseYResolutionFromConfig(const std::string& configPath,
                                                 uint32_t printMode) {
    std::ifstream file(configPath);
    if (!file) {
        std::cerr << "[ERROR] Cannot open JslConfigs.xml: " << configPath << std::endl;
        return 0;
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // Find <printMode index="N"> matching our printMode
    std::string modeTag = "index=\"" + std::to_string(printMode) + "\"";
    size_t modePos = content.find(modeTag);
    if (modePos == std::string::npos) {
        std::cerr << "[ERROR] printMode " << printMode
                  << " not found in " << configPath << std::endl;
        return 0;
    }

    // Find <verticalResolution res="NNNN"> after the mode tag
    std::string resTag = "verticalResolution";
    size_t resPos = content.find(resTag, modePos);
    if (resPos == std::string::npos) {
        std::cerr << "[ERROR] No verticalResolution in printMode "
                  << printMode << std::endl;
        return 0;
    }

    // Extract res="NNNN"
    std::string resAttr = "res=\"";
    size_t attrPos = content.find(resAttr, resPos);
    if (attrPos == std::string::npos) {
        std::cerr << "[ERROR] No res= attribute in verticalResolution" << std::endl;
        return 0;
    }
    attrPos += resAttr.length();
    size_t endPos = content.find('"', attrPos);
    if (endPos == std::string::npos) {
        return 0;
    }

    std::string resStr = content.substr(attrPos, endPos - attrPos);
    uint32_t res = static_cast<uint32_t>(std::stoul(resStr));
    return res;
}

bool JSLWrapper::initialize() {
    if (initialized_) {
        return true;
    }

    // Config path: JSL_CONFIG_PATH env, else probe common repo/build locations.
    const char* envPath = std::getenv("JSL_CONFIG_PATH");
    if (envPath && envPath[0] != '\0') {
        configPath_ = envPath;
    } else {
        const std::vector<std::string> candidates = {
            "C:/Users/Arrow/Arrow-Rip/jsl-sdk/JslConfigs.xml",
            "C:/Users/Arrow/Arrow-Rip/src/jsl-sdk/JslConfigs.xml",
            "C:/Users/Arrow/Arrow-Rip/src/build/jsl-sdk/JslConfigs.xml",
            "C:/Users/Arrow/Arrow-Rip/build/jsl-sdk/JslConfigs.xml",
            "jsl-sdk/JslConfigs.xml",
            "../jsl-sdk/JslConfigs.xml",
            "../../jsl-sdk/JslConfigs.xml"
        };

        for (const auto& p : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) {
                configPath_ = p;
                break;
            }
        }

        if (configPath_.empty()) {
            configPath_ = "C:/Users/Arrow/Arrow-Rip/jsl-sdk/JslConfigs.xml";
        }
    }

    // Print mode: JSL_PRINT_MODE env, default 1
    const char* envMode = std::getenv("JSL_PRINT_MODE");
    if (envMode && envMode[0] != '\0') {
        printMode_ = static_cast<uint32_t>(std::stoul(envMode));
    } else {
        printMode_ = 2;
    }

    // Derive yResolution from config + mode
    yResolution_ = parseYResolutionFromConfig(configPath_, printMode_);
    if (yResolution_ == 0) {
        std::cerr << "[ERROR] Failed to parse yResolution from "
                  << configPath_ << " for printMode=" << printMode_ << std::endl;
        return false;
    }

    std::cout << "[INFO] yResolution=" << yResolution_
              << " (from " << configPath_ << " printMode=" << printMode_ << ")"
              << std::endl;

    JslibResultCode result = jslibInit(configPath_.c_str(), printMode_);

    if (result != JSLIB_RETURN_CODE_OK) {
        std::cerr << "[ERROR] jslibInit failed: " << getErrorString(result) << std::endl;
        return false;
    }

    jslibSetLoggingCallback(loggingCallback);
    jslibSetLoggingLevel(JSLIB_LOGGING_LEVEL_INFO);

    initialized_ = true;
    std::cout << "[JSL INFO] Memjet Job Submission Library initialized successfully"
              << std::endl;
    return true;
}

void JSLWrapper::setLoggingLevel(JslibLoggingLevel level) {
    jslibSetLoggingLevel(level);
}

bool JSLWrapper::isRetryable(JslibResultCode code) const {
    return code == JSLIB_RETURN_CODE_NOT_READY ||
           code == JSLIB_RETURN_CODE_BUSY ||
           code == JSLIB_RETURN_CODE_COMMS;
}

// --- Multi-color job API ---

bool JSLWrapper::openJobMultiColor(const JobConfig& config,
                                    const std::vector<ColorPlane>& colors,
                                    JslibHdl& handle) {
    if (!initialized_) {
        std::cerr << "[ERROR] JSL not initialized" << std::endl;
        return false;
    }

    JslibJobDescription jobDesc = {};

    std::cout << "[DEBUG] sizeof(JslibJobDescription)="
              << sizeof(JslibJobDescription) << std::endl;

    jobDesc.engineStage = 1;
    jobDesc.stripIndex = 1;
    jobDesc.xResolution = 1600;   // SDK: "Must be 1600"
    jobDesc.yResolution = yResolution_;
    jobDesc.stripStart = config.stripStart;
    jobDesc.stripWidth = config.stripWidth;

    jobDesc.numColors = static_cast<uint32_t>(colors.size());
    std::vector<JslibColor> colorArray(colors.size());
    for (size_t i = 0; i < colors.size(); ++i) {
        colorArray[i] = static_cast<JslibColor>(colors[i]);
    }
    jobDesc.colorArray = colorArray.data();
    jobDesc.colorInstance = 1;  // 1-based per SDK

    // Job ID -- 32 hex chars
    std::string paddedJobId = config.jobId;
    if (paddedJobId.length() < JSLIB_LEN_JOB_ID) {
        paddedJobId = std::string(JSLIB_LEN_JOB_ID - paddedJobId.length(), '0') + paddedJobId;
    } else if (paddedJobId.length() > JSLIB_LEN_JOB_ID) {
        paddedJobId = paddedJobId.substr(0, JSLIB_LEN_JOB_ID);
    }
    memcpy(jobDesc.jobId, paddedJobId.c_str(), JSLIB_LEN_JOB_ID);

    // Build30 ABI requires explicit interleave fields when not interleaving.
    jobDesc.lineInterleaveSize = 1;
    jobDesc.lineInterleaveIndex = nullptr;

    jobDesc.pageCount = 1;
    jobDesc.endianness = 0;
    jobDesc.displayableJobName = nullptr;
    jobDesc.ripVersion = nullptr;
    jobDesc.isHorizontalAlignmentDisabled = false;
    jobDesc.numCustomOemDataElements = 0;
    jobDesc.customOemData = nullptr;
    jobDesc.numCustomMemjetDataElements = 0;
    jobDesc.customMemjetData = nullptr;

    if (jobDesc.stripWidth == 0) {
        std::cerr << "[WARN] stripWidth was 0; forcing to 1" << std::endl;
        jobDesc.stripWidth = 1;
    }

    std::cout << "[DEBUG] jobDesc: engineStage=" << jobDesc.engineStage
              << " stripIndex=" << jobDesc.stripIndex
              << " xRes=" << jobDesc.xResolution
              << " yRes=" << jobDesc.yResolution
              << " stripStart=" << jobDesc.stripStart
              << " stripWidth=" << jobDesc.stripWidth
              << " numColors=" << jobDesc.numColors
              << " colorInstance=" << jobDesc.colorInstance
              << " pageCount=" << jobDesc.pageCount
              << " endianness=" << jobDesc.endianness
              << " jobId=" << paddedJobId
              << " jobIdHex=" << (isHexJobId(paddedJobId) ? "true" : "false")
              << std::endl;

    std::cout << "[INFO] Opening JSL job: numColors=" << jobDesc.numColors
              << ", xRes=" << jobDesc.xResolution
              << ", yRes=" << jobDesc.yResolution
              << ", colorInstance=" << jobDesc.colorInstance << std::endl;

    // Retry loop
    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        JslibResultCode result = jslibOpenJob(&jobDesc, nullptr, &handle);

        if (result == JSLIB_RETURN_CODE_OK) {
            return true;
        }

        if (isRetryable(result) && attempt < MAX_RETRIES) {
            int delayMs = BASE_RETRY_MS * (1 << attempt);
            std::cerr << "[WARN] jslibOpenJob returned " << getErrorString(result)
                      << ", retry " << (attempt + 1) << "/" << MAX_RETRIES
                      << " in " << delayMs << "ms" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            continue;
        }

        std::cerr << "[ERROR] jslibOpenJob failed: " << getErrorString(result) << std::endl;
        return false;
    }
    return false;
}

bool JSLWrapper::addPageMultiColor(JslibHdl handle,
                                    const std::vector<PageData>& planes,
                                    bool isLastPage) {
    if (!handle) {
        std::cerr << "[ERROR] Invalid JSL handle" << std::endl;
        return false;
    }

    JslibPageDescription pageDesc = {};
    pageDesc.pageHeight = planes[0].height;
    int copies = 1;
    if (const char* v = std::getenv("JSL_NUM_COPIES")) {
        try {
            copies = std::max(1, std::stoi(v));
        } catch (...) {
            copies = 1;
        }
    }
    pageDesc.numCopies = static_cast<uint32_t>(copies);
    pageDesc.isLastPage = isLastPage;

    for (size_t i = 0; i < planes.size() && i < JSLIB_MAX_NUM_COLORS_PER_PRINTHEAD; ++i) {
        pageDesc.len[i] = static_cast<uint32_t>(planes[i].data.size());
        pageDesc.dataPtr[i] = const_cast<uint8_t*>(planes[i].data.data());
    }

    size_t totalBytes = 0;
    for (size_t i = 0; i < planes.size(); ++i) totalBytes += planes[i].data.size();
    std::cout << "[INFO] Adding page: " << planes.size()
              << " planes, isLastPage=" << (isLastPage ? "true" : "false")
              << ", copies=" << pageDesc.numCopies
              << ", totalBytes=" << totalBytes
              << std::endl;

    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        auto t0 = std::chrono::steady_clock::now();
        std::cout << "[JSL TRACE] addPageMultiColor attempt=" << attempt
                  << " start" << std::endl;

        JslibResultCode result = jslibAddPage(handle, &pageDesc);

        auto t1 = std::chrono::steady_clock::now();
        auto durMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        std::cout << "[JSL TRACE] addPageMultiColor attempt=" << attempt
                  << " done result=" << getErrorString(result)
                  << " elapsed_ms=" << durMs << std::endl;

        if (result == JSLIB_RETURN_CODE_OK) {
            return true;
        }

        if (isRetryable(result) && attempt < MAX_RETRIES) {
            int delayMs = BASE_RETRY_MS * (1 << attempt);
            std::cerr << "[WARN] jslibAddPage returned " << getErrorString(result)
                      << ", retry " << (attempt + 1) << "/" << MAX_RETRIES
                      << " in " << delayMs << "ms" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            continue;
        }

        std::cerr << "[ERROR] jslibAddPage failed: " << getErrorString(result) << std::endl;
        // Abort + close on failure per SDK contract
        abortJob(handle);
        closeJob(handle);
        return false;
    }
    return false;
}

// --- Legacy single-color API ---

bool JSLWrapper::openJob(const JobConfig& config, ColorPlane color,
                        uint32_t colorInstance, JslibHdl& handle) {
    if (!initialized_) {
        std::cerr << "[ERROR] JSL not initialized" << std::endl;
        return false;
    }

    JslibJobDescription jobDesc = {};

    std::cout << "[DEBUG] sizeof(JslibJobDescription)="
              << sizeof(JslibJobDescription) << std::endl;

    jobDesc.engineStage = 1;
    jobDesc.stripIndex = 1;
    jobDesc.xResolution = 1600;
    jobDesc.yResolution = yResolution_;
    jobDesc.stripStart = config.stripStart;
    jobDesc.stripWidth = config.stripWidth;

    jobDesc.numColors = 1;
    JslibColor colorArray[1] = {static_cast<JslibColor>(color)};
    jobDesc.colorArray = colorArray;
    jobDesc.colorInstance = colorInstance;

    std::string paddedJobId = config.jobId;
    if (paddedJobId.length() < JSLIB_LEN_JOB_ID) {
        paddedJobId = std::string(JSLIB_LEN_JOB_ID - paddedJobId.length(), '0') + paddedJobId;
    } else if (paddedJobId.length() > JSLIB_LEN_JOB_ID) {
        paddedJobId = paddedJobId.substr(0, JSLIB_LEN_JOB_ID);
    }
    memcpy(jobDesc.jobId, paddedJobId.c_str(), JSLIB_LEN_JOB_ID);

    // Build30 ABI requires explicit interleave fields when not interleaving.
    jobDesc.lineInterleaveSize = 1;
    jobDesc.lineInterleaveIndex = nullptr;

    jobDesc.pageCount = 1;
    jobDesc.endianness = 0;
    jobDesc.displayableJobName = nullptr;
    jobDesc.ripVersion = nullptr;
    jobDesc.isHorizontalAlignmentDisabled = false;
    jobDesc.numCustomOemDataElements = 0;
    jobDesc.customOemData = nullptr;
    jobDesc.numCustomMemjetDataElements = 0;
    jobDesc.customMemjetData = nullptr;

    if (jobDesc.stripWidth == 0) {
        std::cerr << "[WARN] stripWidth was 0; forcing to 1" << std::endl;
        jobDesc.stripWidth = 1;
    }

    std::cout << "[DEBUG] jobDesc: engineStage=" << jobDesc.engineStage
              << " stripIndex=" << jobDesc.stripIndex
              << " xRes=" << jobDesc.xResolution
              << " yRes=" << jobDesc.yResolution
              << " stripStart=" << jobDesc.stripStart
              << " stripWidth=" << jobDesc.stripWidth
              << " numColors=" << jobDesc.numColors
              << " colorInstance=" << jobDesc.colorInstance
              << " pageCount=" << jobDesc.pageCount
              << " endianness=" << jobDesc.endianness
              << " jobId=" << paddedJobId
              << " jobIdHex=" << (isHexJobId(paddedJobId) ? "true" : "false")
              << std::endl;

    std::cout << "[INFO] Opening JSL job for color " << static_cast<int>(color)
              << " (legacy mode)..." << std::endl;

    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        JslibResultCode result = jslibOpenJob(&jobDesc, nullptr, &handle);

        if (result == JSLIB_RETURN_CODE_OK) {
            return true;
        }

        if (isRetryable(result) && attempt < MAX_RETRIES) {
            int delayMs = BASE_RETRY_MS * (1 << attempt);
            std::cerr << "[WARN] jslibOpenJob returned " << getErrorString(result)
                      << ", retry " << (attempt + 1) << "/" << MAX_RETRIES
                      << " in " << delayMs << "ms" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            continue;
        }

        std::cerr << "[ERROR] jslibOpenJob failed: " << getErrorString(result) << std::endl;
        return false;
    }
    return false;
}

bool JSLWrapper::addPage(JslibHdl handle, const PageData& page, bool isLastPage) {
    if (!handle) {
        std::cerr << "[ERROR] Invalid JSL handle" << std::endl;
        return false;
    }

    JslibPageDescription pageDesc = {};
    pageDesc.pageHeight = page.height;
    pageDesc.len[0] = static_cast<uint32_t>(page.data.size());
    pageDesc.dataPtr[0] = const_cast<uint8_t*>(page.data.data());
    int copies = 1;
    if (const char* v = std::getenv("JSL_NUM_COPIES")) {
        try {
            copies = std::max(1, std::stoi(v));
        } catch (...) {
            copies = 1;
        }
    }
    pageDesc.numCopies = static_cast<uint32_t>(copies);
    pageDesc.isLastPage = isLastPage;

    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        JslibResultCode result = jslibAddPage(handle, &pageDesc);

        if (result == JSLIB_RETURN_CODE_OK) {
            std::cout << "[INFO] Added page for color " << static_cast<int>(page.color)
                      << " (" << page.data.size() << " bytes, isLastPage="
                      << (isLastPage ? "true" : "false") << ")" << std::endl;
            return true;
        }

        if (isRetryable(result) && attempt < MAX_RETRIES) {
            int delayMs = BASE_RETRY_MS * (1 << attempt);
            std::cerr << "[WARN] jslibAddPage returned " << getErrorString(result)
                      << ", retry " << (attempt + 1) << "/" << MAX_RETRIES
                      << " in " << delayMs << "ms" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            continue;
        }

        std::cerr << "[ERROR] jslibAddPage failed: " << getErrorString(result) << std::endl;
        abortJob(handle);
        closeJob(handle);
        return false;
    }
    return false;
}

// --- Common ---

bool JSLWrapper::closeJob(JslibHdl handle) {
    if (!handle) {
        return false;
    }

    JslibResultCode result = jslibCloseJob(handle);

    if (result != JSLIB_RETURN_CODE_OK) {
        std::cerr << "[ERROR] jslibCloseJob failed: " << getErrorString(result) << std::endl;
        return false;
    }

    std::cout << "[INFO] JSL job closed successfully" << std::endl;
    return true;
}

bool JSLWrapper::abortJob(JslibHdl handle) {
    if (!handle) {
        return false;
    }

    JslibResultCode result = jslibAbortJob(handle);

    if (result != JSLIB_RETURN_CODE_OK) {
        std::cerr << "[WARN] jslibAbortJob returned: " << getErrorString(result) << std::endl;
        return false;
    }

    std::cout << "[INFO] JSL job aborted" << std::endl;
    return true;
}

std::string JSLWrapper::getErrorString(JslibResultCode code) {
    switch (code) {
        case JSLIB_RETURN_CODE_OK: return "OK";
        case JSLIB_RETURN_CODE_NOT_READY: return "NOT_READY";
        case JSLIB_RETURN_CODE_RESOLVE_DEST: return "RESOLVE_DEST";
        case JSLIB_RETURN_CODE_COMMS: return "COMMS";
        case JSLIB_RETURN_CODE_CLOSED: return "CLOSED";
        case JSLIB_RETURN_CODE_BAD_PARAM: return "BAD_PARAM";
        case JSLIB_RETURN_CODE_BUSY: return "BUSY";
        case JSLIB_RETURN_CODE_ABORT: return "ABORT";
        case JSLIB_RETURN_CODE_FATAL_ERROR: return "FATAL_ERROR";
        default: return "UNKNOWN";
    }
}

void CALLBACK JSLWrapper::loggingCallback(JslibLoggingLevel level, const char* message) {
    const char* levelStr = "UNKNOWN";
    switch (level) {
        case JSLIB_LOGGING_LEVEL_NONE: levelStr = "NONE"; break;
        case JSLIB_LOGGING_LEVEL_ERROR: levelStr = "ERROR"; break;
        case JSLIB_LOGGING_LEVEL_WARNING: levelStr = "WARN"; break;
        case JSLIB_LOGGING_LEVEL_INFO: levelStr = "INFO"; break;
        case JSLIB_LOGGING_LEVEL_DEBUG: levelStr = "DEBUG"; break;
        default: break;
    }
    std::cout << "[JSL " << levelStr << "] " << message << std::endl;
}

} // namespace memjet

