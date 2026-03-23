#include "pes_orchestrator.h"
#include "utils.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <ctime>
#include <cctype>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace memjet {

using utils::logInfo;
using utils::logError;
using utils::logStructured;

namespace {
    std::string stateToString(PesEngineState state) {
        switch (state) {
            case PesEngineState::OFF: return "OFF";
            case PesEngineState::INITIALISING: return "INITIALISING";
            case PesEngineState::PRIMED_IDLE: return "PRIMED_IDLE";
            case PesEngineState::DEPRIMED_IDLE: return "DEPRIMED_IDLE";
            case PesEngineState::PREPARING: return "PREPARING";
            case PesEngineState::PRE_JOB: return "PRE_JOB";
            case PesEngineState::PRINT_READY: return "PRINT_READY";
            case PesEngineState::PRINTING: return "PRINTING";
            case PesEngineState::MID_JOB: return "MID_JOB";
            case PesEngineState::PAUSED: return "PAUSED";
            case PesEngineState::SESSION_COMPLETE: return "SESSION_COMPLETE";
            case PesEngineState::SHUTTING_DOWN: return "SHUTTING_DOWN";
            case PesEngineState::FAULT: return "FAULT";
            default: return "UNKNOWN";
        }
    }

    bool envVarNonEmpty(const char* key, std::string& out) {
        const char* v = std::getenv(key);
        if (!v || std::strlen(v) == 0) return false;
        out.assign(v);
        return true;
    }

    std::string toLowerCopy(const std::string& in) {
        std::string out = in;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){
            return static_cast<char>(std::tolower(c));
        });
        return out;
    }

    bool isValidJustify(const std::string& raw) {
        const std::string j = toLowerCopy(raw);
        return j == "left" || j == "center" || j == "centre" || j == "right" ||
               j == "0" || j == "1" || j == "2";
    }

    std::string buildAlignmentCommandFromEnv() {
        std::string xadjust;
        std::string yoffset;
        std::string justify;
        std::string pulse;
        const bool hasX = envVarNonEmpty("PES_X_ADJUST", xadjust);
        const bool hasY = envVarNonEmpty("PES_Y_OFFSET", yoffset);
        const bool hasJ = envVarNonEmpty("PES_JUSTIFY", justify);
        bool hasP = envVarNonEmpty("PES_PULSE_WIDTH", pulse);
        if (!hasP) {
            pulse = "1.0";  // default: no pulse-width adjustment unless overridden
            hasP = true;
        }

        if (!hasX && !hasY && !hasJ && !hasP) return "";

        if (hasJ && !isValidJustify(justify)) {
            logError("Invalid PES_JUSTIFY value: '" + justify + "' (allowed: left|center|right|0|1|2)");
            return "";
        }

        std::ostringstream cmd;
        bool first = true;
        if (hasX) {
            cmd << "xadjust=" << xadjust;
            first = false;
        }
        if (hasY) {
            if (!first) cmd << " ";
            cmd << "yoffset=" << yoffset;
            first = false;
        }
        if (hasJ) {
            if (!first) cmd << " ";
            cmd << "justify=" << toLowerCopy(justify);
            first = false;
        }
        if (hasP) {
            if (!first) cmd << " ";
            cmd << "pulse=" << pulse;
        }
        return cmd.str();
    }

    int getEnvIntOrDefault(const char* key, int fallback) {
        const char* v = std::getenv(key);
        if (!v || std::strlen(v) == 0) return fallback;
        int parsed = std::atoi(v);
        return parsed > 0 ? parsed : fallback;
    }

    const char* phaseToString(SessionPhase phase) {
        switch (phase) {
            case SessionPhase::IDLE: return "IDLE";
            case SessionPhase::DATA_SUBMITTING: return "DATA_SUBMITTING";
            case SessionPhase::TX_DONE: return "TX_DONE";
            case SessionPhase::WAITING_QUEUE: return "WAITING_QUEUE";
            case SessionPhase::PREPARING: return "PREPARING";
            case SessionPhase::WAITING_PRINT_READY: return "WAITING_PRINT_READY";
            case SessionPhase::STARTING: return "STARTING";
            case SessionPhase::WAIT_PRINT_COMPLETE: return "WAIT_PRINT_COMPLETE";
            case SessionPhase::FINISHING: return "FINISHING";
            case SessionPhase::WAIT_JOB_DONE: return "WAIT_JOB_DONE";
            case SessionPhase::SUCCESS: return "SUCCESS";
            case SessionPhase::FAILED: return "FAILED";
            default: return "UNKNOWN_PHASE";
        }
    }
}


void PesOrchestrator::logOrchestrationStep(const std::string& step, const std::string& stateBefore,
                                           const std::string& stateAfter, bool isReadyForPrintData,
                                           int queueLen, const std::string& result,
                                           const std::string& errorCode, int timeoutMs) {
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
    std::ostringstream ts;
    ts << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    ts << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    std::ostringstream json;
    json << "{" << "\"ts\":\"" << ts.str() << "\"," << "\"component\":\"PES_ORCH\","
         << "\"step\":\"" << step << "\"," << "\"state_before\":\"" << stateBefore << "\"," 
         << "\"state_after\":\"" << stateAfter << "\"," << "\"is_ready_for_print_data\":"
         << (isReadyForPrintData ? "true" : "false") << "," << "\"queue_len\":" << queueLen << ","
         << "\"result\":\"" << result << "\"";
    if (!errorCode.empty()) json << ",\"error_code\":\"" << errorCode << "\"";
    if (timeoutMs > 0) json << ",\"timeout_ms\":" << timeoutMs;
    json << "}";
    std::cout << json.str() << std::endl;
}

PesOrchestrator::PesOrchestrator(const std::string& thriftControllerPath,
                                 const std::string& pesIp,
                                 int controlPort)
    : thriftControllerPath_(thriftControllerPath)
    , pesIp_(pesIp)
    , controlPort_(controlPort)
    , phase_(SessionPhase::IDLE)
    , dataDone_(false)
{}

PesOrchestrator::~PesOrchestrator() {}

std::string PesOrchestrator::runThriftCmd(const std::string& command) {
    if (thriftControllerPath_.empty()) return "";

    std::string pythonExe = "C:\\Python27\\python.exe";
    if (const char* v = std::getenv("THRIFT_PYTHON_EXE")) {
        if (std::strlen(v) > 0) pythonExe = v;
    } else if (const char* v2 = std::getenv("THRIFT_PYTHON")) {
        if (std::strlen(v2) > 0) pythonExe = v2;
    }

    std::string pdlThriftRoot = "C:\\Users\\Arrow\\Arrow-Rip\\vendor\\pdl_py";
    if (const char* v = std::getenv("PDL_THRIFT_ROOT")) {
        if (std::strlen(v) > 0) pdlThriftRoot = v;
    }

    std::ostringstream cmd;
#ifdef _WIN32
    cmd << "cmd /C \"set \"\"PDL_THRIFT_ROOT=" << pdlThriftRoot << "\"\" && "
        << "\"" << pythonExe << "\" "
        << "\"" << thriftControllerPath_ << "\" "
        << pesIp_ << " " << controlPort_ << " " << command << " 2^>^&1\"";
#else
    cmd << "PDL_THRIFT_ROOT=\"" << pdlThriftRoot << "\" "
        << "\"" << pythonExe << "\" "
        << "\"" << thriftControllerPath_ << "\" "
        << pesIp_ << " " << controlPort_ << " " << command << " 2>&1";
#endif

    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) return "";

    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) result += buffer;
    pclose(pipe);

    if (const char* v = std::getenv("PES_LOG_THRIFT_CMD")) {
        std::string s(v); std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        if (s == "1" || s == "true" || s == "yes" || s == "on") {
            std::string snip = result.substr(0, std::min<size_t>(result.size(), 400));
            logInfo(std::string("thrift cmd=") + cmd.str());
            logInfo(std::string("thrift out bytes=") + std::to_string(result.size()) + " snip=" + snip);
        }
    }

    if (result.find("Cannot locate SDK python packages") != std::string::npos) {
        logError("Thrift controller SDK path error. Verify PDL_THRIFT_ROOT=" + pdlThriftRoot);
    }
    if (result.find("not recognized as an internal or external command") != std::string::npos ||
        result.find("No such file or directory") != std::string::npos) {
        logError("Thrift controller python launcher failed. Verify THRIFT_PYTHON_EXE=" + pythonExe);
    }
    return result;
}
PesEngineState PesOrchestrator::parseEngineState(const std::string& stateStr) {
    std::string upper = stateStr;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    
    if (upper == "OFF") return PesEngineState::OFF;
    if (upper == "INITIALISING" || upper == "INITIALIZING") return PesEngineState::INITIALISING;
    if (upper == "PRIMED_IDLE") return PesEngineState::PRIMED_IDLE;
    if (upper == "DEPRIMED_IDLE") return PesEngineState::DEPRIMED_IDLE;
    if (upper == "PREPARING") return PesEngineState::PREPARING;
    if (upper == "PRE_JOB") return PesEngineState::PRE_JOB;
    if (upper == "PRINT_READY") return PesEngineState::PRINT_READY;
    if (upper == "PRINTING") return PesEngineState::PRINTING;
    if (upper == "MID_JOB") return PesEngineState::MID_JOB;
    if (upper == "PAUSED") return PesEngineState::PAUSED;
    if (upper == "SESSION_COMPLETE") return PesEngineState::SESSION_COMPLETE;
    if (upper == "SHUTTING_DOWN") return PesEngineState::SHUTTING_DOWN;
    if (upper == "FAULT") return PesEngineState::FAULT;
    
    return PesEngineState::UNKNOWN;
}

PesEngineState PesOrchestrator::parseEngineStateFromNumeric(int numericState) {
    // Map numeric engineStatus.state values to enum (per Memjet docs)
    // Common mappings (may vary by PES version):
    switch (numericState) {
        case 0: return PesEngineState::OFF;
        case 1: return PesEngineState::INITIALISING;
        case 2: return PesEngineState::PRIMED_IDLE;
        case 3: return PesEngineState::DEPRIMED_IDLE;
        case 4: return PesEngineState::PREPARING;
        case 5: return PesEngineState::PRE_JOB;
        case 6: return PesEngineState::PRINT_READY;
        case 7: return PesEngineState::PRINTING;
        case 8: return PesEngineState::MID_JOB;
        case 9: return PesEngineState::PAUSED;
        case 10: return PesEngineState::SESSION_COMPLETE;
        case 11: return PesEngineState::SHUTTING_DOWN;
        case 12: return PesEngineState::FAULT;
        default:
            // Log unknown numeric state but don't overwrite valid string state
            // Format: UNKNOWN(<int>)
            std::ostringstream oss;
            oss << "UNKNOWN(" << numericState << ")";
            logInfo("Unknown numeric engine state: " + oss.str());
            return PesEngineState::UNKNOWN;
    }
}

std::string PesOrchestrator::extractAndNormalizeState(const PesStatus& st) {
    // Source of truth: use stateAfter from latest parsed status
    std::string state = st.stateAfter;
    if (state.empty() || state == "UNKNOWN") {
        // Fallback to enum string representation
        state = stateToString(st.state);
    }
    
    // Normalize
    std::transform(state.begin(), state.end(), state.begin(), ::toupper);
    state.erase(0, state.find_first_not_of(" \t\n\r"));
    state.erase(state.find_last_not_of(" \t\n\r") + 1);
    
    return state.empty() ? "UNKNOWN" : state;
}

PesStatus PesOrchestrator::getStatus() {
    PesStatus status;
    status.state = PesEngineState::UNKNOWN;
    status.queueLen = 0;
    status.queueHeadJobId = "";
    status.raw = "";
    status.extraction = "unknown";
    status.isReadyForPrintData = false;
    status.stateAfter = "UNKNOWN";
    status.engineStatusNumeric = -1;
    
    std::string output = runThriftCmd("statusjson");
    if (output.find("ERROR") != std::string::npos ||
        output.find("Cannot locate SDK") != std::string::npos ||
        output.find("not recognized") != std::string::npos) {
        logError("PES statusjson command output: " + output);
    }
    if (output.empty()) {
        // Fallback to regular status command
        output = runThriftCmd("status");
        status.raw = output;
        status.extraction = "fallback_text";
        return status;
    }
    
    status.raw = output;
    
    // Parse JSON output
    // Look for JSON object in output (may have other text before/after)
    size_t jsonStart = output.find('{');
    if (jsonStart == std::string::npos) {
        status.extraction = "no_json";
        return status;
    }
    
    size_t jsonEnd = output.rfind('}');
    if (jsonEnd == std::string::npos || jsonEnd <= jsonStart) {
        status.extraction = "malformed_json";
        return status;
    }
    
    std::string jsonStr = output.substr(jsonStart, jsonEnd - jsonStart + 1);
    
    // Helper lambda to extract JSON string field
    auto extractJsonString = [&jsonStr](const std::string& fieldName) -> std::string {
        size_t pos = jsonStr.find("\"" + fieldName + "\"");
        if (pos != std::string::npos) {
            size_t colonPos = jsonStr.find(':', pos);
            if (colonPos != std::string::npos) {
                size_t quoteStart = jsonStr.find('"', colonPos);
                if (quoteStart != std::string::npos) {
                    size_t quoteEnd = jsonStr.find('"', quoteStart + 1);
                    if (quoteEnd != std::string::npos) {
                        return jsonStr.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
                    }
                }
            }
        }
        return "";
    };

    // Helper lambda to extract JSON bool/number field (true/false/1/0)
    auto extractJsonBool = [&jsonStr](const std::string& fieldName, bool& found) -> bool {
        found = false;
        size_t pos = jsonStr.find("\"" + fieldName + "\"");
        if (pos == std::string::npos) return false;
        size_t colonPos = jsonStr.find(':', pos);
        if (colonPos == std::string::npos) return false;
        size_t v = colonPos + 1;
        while (v < jsonStr.size() && (jsonStr[v] == ' ' || jsonStr[v] == '\t' || jsonStr[v] == '\n' || jsonStr[v] == '\r')) v++;
        if (v >= jsonStr.size()) return false;
        if (jsonStr.compare(v, 4, "true") == 0) { found = true; return true; }
        if (jsonStr.compare(v, 5, "false") == 0) { found = true; return false; }
        if (jsonStr[v] == '1') { found = true; return true; }
        if (jsonStr[v] == '0') { found = true; return false; }
        return false;
    };
    
    // Parse state in order: state_after first, then fallback to engineState, then state
    std::string stateAfter = extractJsonString("state_after");
    std::string engineState = extractJsonString("engineState");
    std::string stateField = extractJsonString("state");
    
    // Extract numeric engineStatus if available
    size_t engineStatusPos = jsonStr.find("\"engineStatus\"");
    if (engineStatusPos != std::string::npos) {
        size_t colonPos = jsonStr.find(':', engineStatusPos);
        if (colonPos != std::string::npos) {
            size_t numStart = colonPos + 1;
            while (numStart < jsonStr.length() && (jsonStr[numStart] == ' ' || jsonStr[numStart] == '\t')) {
                numStart++;
            }
            size_t numEnd = numStart;
            while (numEnd < jsonStr.length() && 
                   (jsonStr[numEnd] >= '0' && jsonStr[numEnd] <= '9')) {
                numEnd++;
            }
            if (numEnd > numStart) {
                std::string numStr = jsonStr.substr(numStart, numEnd - numStart);
                status.engineStatusNumeric = std::atoi(numStr.c_str());
            }
        }
    }
    
    // Normalize: trim and uppercase
    auto normalizeState = [](std::string s) -> std::string {
        // Trim whitespace
        s.erase(0, s.find_first_not_of(" \t\n\r"));
        s.erase(s.find_last_not_of(" \t\n\r") + 1);
        // Uppercase
        std::transform(s.begin(), s.end(), s.begin(), ::toupper);
        return s;
    };
    
    std::string state = !stateAfter.empty() ? stateAfter : (!engineState.empty() ? engineState : stateField);
    state = normalizeState(state);
    
    // Store stateAfter as source of truth
    status.stateAfter = state;
    
    if (!state.empty() && state != "UNKNOWN") {
        status.state = parseEngineState(state);
    } else if (status.engineStatusNumeric >= 0) {
        // Fallback to numeric mapping if string parsing failed
        PesEngineState parsedFromNumeric = parseEngineStateFromNumeric(status.engineStatusNumeric);
        if (parsedFromNumeric != PesEngineState::UNKNOWN) {
            status.state = parsedFromNumeric;
            status.stateAfter = stateToString(status.state);
        } else {
            // Unknown numeric state - format as UNKNOWN(<int>) but don't overwrite valid state
            std::ostringstream oss;
            oss << "UNKNOWN(" << status.engineStatusNumeric << ")";
            status.stateAfter = oss.str();
        }
    }
    
    // Extract isReadyForPrintData (supports bool, numeric, or string)
    bool readyFound = false;
    bool readyBool = extractJsonBool("isReadyForPrintData", readyFound);
    if (readyFound) {
        status.isReadyForPrintData = readyBool;
    } else {
        std::string isReadyStr = extractJsonString("isReadyForPrintData");
        if (!isReadyStr.empty()) {
            std::string readyLower = isReadyStr;
            std::transform(readyLower.begin(), readyLower.end(), readyLower.begin(), ::tolower);
            status.isReadyForPrintData = (readyLower == "true" || readyLower == "1");
        }
    }
    
    // Extract queueLen
    size_t queueLenPos = jsonStr.find("\"queueLen\"");
    if (queueLenPos != std::string::npos) {
        size_t colonPos = jsonStr.find(':', queueLenPos);
        if (colonPos != std::string::npos) {
            size_t numStart = colonPos + 1;
            while (numStart < jsonStr.length() && (jsonStr[numStart] == ' ' || jsonStr[numStart] == '\t')) {
                numStart++;
            }
            size_t numEnd = numStart;
            while (numEnd < jsonStr.length() && 
                   (jsonStr[numEnd] >= '0' && jsonStr[numEnd] <= '9')) {
                numEnd++;
            }
            if (numEnd > numStart) {
                std::string numStr = jsonStr.substr(numStart, numEnd - numStart);
                status.queueLen = std::atoi(numStr.c_str());
            }
        }
    }
    
    // Extract queueHeadJobId
    size_t jobIdPos = jsonStr.find("\"queueHeadJobId\"");
    if (jobIdPos != std::string::npos) {
        size_t colonPos = jsonStr.find(':', jobIdPos);
        if (colonPos != std::string::npos) {
            size_t quoteStart = jsonStr.find('"', colonPos);
            if (quoteStart != std::string::npos) {
                size_t quoteEnd = jsonStr.find('"', quoteStart + 1);
                if (quoteEnd != std::string::npos) {
                    status.queueHeadJobId = jsonStr.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
                }
            }
        }
    }
    
    // Extract extraction method
    size_t extractionPos = jsonStr.find("\"extraction\"");
    if (extractionPos != std::string::npos) {
        size_t colonPos = jsonStr.find(':', extractionPos);
        if (colonPos != std::string::npos) {
            size_t quoteStart = jsonStr.find('"', colonPos);
            if (quoteStart != std::string::npos) {
                size_t quoteEnd = jsonStr.find('"', quoteStart + 1);
                if (quoteEnd != std::string::npos) {
                    status.extraction = jsonStr.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
                }
            }
        }
    }
    
    // Optional raw status logging for parser diagnostics
    if (status.state == PesEngineState::UNKNOWN) {
        if (const char* v = std::getenv("PES_LOG_RAW_STATUS")) {
            std::string s(v);
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            if (s == "1" || s == "true" || s == "yes" || s == "on") {
                std::string snippet = output.substr(0, std::min<size_t>(output.size(), 600));
                logStructured("PES_ORCH", "raw_status_unknown", stateToString(status.state), status.stateAfter,
                             status.queueLen, "DEBUG", "RAW=" + snippet, 0);
            }
        }
    }

    // Fallback: detect known state tokens when JSON key parsing misses
    if (status.state == PesEngineState::UNKNOWN) {
        static const char* kStates[] = {
            "PRIMED_IDLE", "PRINT_READY", "PRINTING", "SESSION_COMPLETE",
            "PRE_JOB", "PREPARING", "PAUSED", "INITIALISING", "OFF", "FAULT"
        };
        std::string up = output;
        std::transform(up.begin(), up.end(), up.begin(), ::toupper);
        for (const char* k : kStates) {
            if (up.find(k) != std::string::npos) {
                status.state = parseEngineState(k);
                status.stateAfter = k;
                if (status.extraction == "unknown") status.extraction = "fallback_regex";
                break;
            }
        }
    }

    return status;
}

bool PesOrchestrator::waitForState(PesEngineState target, int timeoutMs) {
    int elapsed = 0;
    int pollMs = 500;
    const std::string targetStr = stateToString(target);
    std::string lastObservedStateAfter = "UNKNOWN";  // Source of truth: latest parsed state_after
    
    // Normalize target state string for comparison
    auto normalizeState = [](std::string s) -> std::string {
        s.erase(0, s.find_first_not_of(" \t\n\r"));
        s.erase(s.find_last_not_of(" \t\n\r") + 1);
        std::transform(s.begin(), s.end(), s.begin(), ::toupper);
        return s;
    };
    const std::string targetNorm = normalizeState(targetStr);
    bool sawTargetOnce = false;
    int unknownAfterTarget = 0;

    while (elapsed < timeoutMs) {
        PesStatus st = getStatus();
        
        // Use latest parsed state_after as source of truth (primary and only match source)
        std::string stateAfter = extractAndNormalizeState(st);
        
        // Always update lastObservedStateAfter when we have non-empty state (use "UNKNOWN" if empty)
        if (!stateAfter.empty()) {
            lastObservedStateAfter = stateAfter;
        } else {
            lastObservedStateAfter = "UNKNOWN";
        }
        
        std::string stateBefore = stateToString(st.state);

        // Match condition: strictly use normalized stateAfter string comparison
        // Do not require st.state == target (enum may be stale/UNKNOWN)
        bool matched = (stateAfter == targetNorm);

        logOrchestrationStep("poll_state", stateBefore, stateAfter, st.isReadyForPrintData,
                            st.queueLen, matched ? "OK" : "WAITING", "", elapsed);

        // Return success immediately if state matches target
        if (matched) {
            sawTargetOnce = true;
            unknownAfterTarget = 0;
            return true;
        }

        // If target was observed but status probe regressed to UNKNOWN, accept for PRINT_READY.
        if (target == PesEngineState::PRINT_READY && sawTargetOnce && stateAfter == "UNKNOWN") {
            unknownAfterTarget++;
            if (unknownAfterTarget >= 2) {
                logOrchestrationStep("poll_state", stateBefore, stateAfter, st.isReadyForPrintData,
                                    st.queueLen, "WARN_CONTINUE", "TRANSIENT_UNKNOWN_AFTER_PRINT_READY", elapsed);
                return true;
            }
        }

        // FAULT detection: check both enum and normalized string
        if (st.state == PesEngineState::FAULT || (!stateAfter.empty() && stateAfter == "FAULT")) {
            logOrchestrationStep("poll_state", stateBefore, stateAfter, st.isReadyForPrintData,
                                st.queueLen, "FAULT", "STATE_FAULT", elapsed);
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        elapsed += pollMs;
    }

    // Timeout - report real lastObservedStateAfter value, not stale UNKNOWN
    logOrchestrationStep("poll_state_timeout", stateToString(target), lastObservedStateAfter,
                        false, 0, "TIMEOUT", 
                        "last=" + lastObservedStateAfter + ", accepted=[" + targetStr + "]", timeoutMs);

    return false;
}

bool PesOrchestrator::waitForQueueReady(int timeoutMs) {
    // Legacy function - redirect to new distinct function
    return waitJobQueuedAfterSubmit(timeoutMs);
}

bool PesOrchestrator::waitEngineReadyForSubmit(int timeoutMs) {
    // Accept when state in {PRIMED_IDLE, PRINT_READY} OR isReadyForPrintData==true.
    // Degraded-mode fallback: if status remains UNKNOWN for the full timeout and no FAULT is seen,
    // allow submit so JSL path can proceed (production-safe guard: hard-fail still on FAULT).
    int elapsed = 0;
    int pollMs = 500;
    std::string lastObservedStateAfter = "UNKNOWN";
    bool lastIsReady = false;
    int lastQueueLen = 0;
    int unknownPolls = 0;

    while (elapsed < timeoutMs) {
        PesStatus st = getStatus();
        std::string stateAfter = extractAndNormalizeState(st);

        if (!stateAfter.empty() && stateAfter != "UNKNOWN") {
            lastObservedStateAfter = stateAfter;
        } else {
            unknownPolls++;
        }
        lastIsReady = st.isReadyForPrintData;
        lastQueueLen = st.queueLen;

        std::string stateBefore = stateToString(st.state);

        bool ready = (st.state == PesEngineState::PRIMED_IDLE) ||
                     (st.state == PesEngineState::PRINT_READY) ||
                     (!stateAfter.empty() && (stateAfter == "PRIMED_IDLE" || stateAfter == "PRINT_READY")) ||
                     st.isReadyForPrintData;

        logOrchestrationStep("wait_engine_ready_for_submit", stateBefore, stateAfter,
                            st.isReadyForPrintData, st.queueLen,
                            ready ? "OK" : "WAITING", "", elapsed);

        if (ready) {
            return true;
        }

        if (st.state == PesEngineState::FAULT || (!stateAfter.empty() && stateAfter == "FAULT")) {
            logOrchestrationStep("wait_engine_ready_for_submit", stateBefore, stateAfter,
                                st.isReadyForPrintData, st.queueLen,
                                "FAULT", "STATE_FAULT", elapsed);
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        elapsed += pollMs;
    }

    // Degraded fallback when state probe is ambiguous.
    // If status remains UNKNOWN for the entire timeout and no FAULT was seen, continue.
    if (lastObservedStateAfter == "UNKNOWN") {
        logOrchestrationStep("wait_engine_ready_for_submit_timeout", "WAITING", lastObservedStateAfter,
                            lastIsReady, lastQueueLen, "WARN_CONTINUE",
                            "status_probe_ambiguous_unknown_polls=" + std::to_string(unknownPolls), timeoutMs);
        return true;
    }

    logOrchestrationStep("wait_engine_ready_for_submit_timeout", "WAITING", lastObservedStateAfter,
                        lastIsReady, lastQueueLen, "TIMEOUT",
                        "last=" + lastObservedStateAfter + ", is_ready=" +
                        (lastIsReady ? "true" : "false"), timeoutMs);
    return false;
}

bool PesOrchestrator::waitJobQueuedAfterSubmit(int timeoutMs) {
    // Accept when queue length > 0 and queue head is valid/assembled
    int elapsed = 0;
    int pollMs = 500;
    std::string lastObservedStateAfter = "UNKNOWN";
    int lastQueueLen = 0;
    
    while (elapsed < timeoutMs) {
        PesStatus st = getStatus();
        std::string stateAfter = extractAndNormalizeState(st);
        
        if (!stateAfter.empty() && stateAfter != "UNKNOWN") {
            lastObservedStateAfter = stateAfter;
        }
        lastQueueLen = st.queueLen;
        
        std::string stateBefore = stateToString(st.state);
        
        // Check queue ready: queueLen > 0 is sufficient on this PES build
        // (queueHeadJobId may remain empty even when queue is valid).
        bool queueReady = (st.queueLen > 0);
        
        logOrchestrationStep("wait_job_queued_after_submit", stateBefore, stateAfter,
                            st.isReadyForPrintData, st.queueLen,
                            queueReady ? "OK" : "WAITING", "", elapsed);
        
        if (queueReady) {
            return true;
        }
        
        if (st.state == PesEngineState::FAULT || (!stateAfter.empty() && stateAfter == "FAULT")) {
            logOrchestrationStep("wait_job_queued_after_submit", stateBefore, stateAfter,
                                st.isReadyForPrintData, st.queueLen,
                                "FAULT", "STATE_FAULT", elapsed);
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        elapsed += pollMs;
    }
    
    // Timeout - report real lastObservedStateAfter and queueLen
    logOrchestrationStep("wait_job_queued_after_submit_timeout", "WAITING", lastObservedStateAfter,
                        false, lastQueueLen, "TIMEOUT",
                        "last=" + lastObservedStateAfter + ", queue_len=" + 
                        std::to_string(lastQueueLen), timeoutMs);
    return false;
}

bool PesOrchestrator::waitEnginePrintReadyAfterPrepare(int timeoutMs) {
    // Accept when engine reaches a start-eligible state after prepare.
    // Some firmware transitions to PAUSED (with queued job) instead of exposing PRINT_READY.
    int elapsed = 0;
    int pollMs = 500;
    std::string lastObservedStateAfter = "UNKNOWN";

    while (elapsed < timeoutMs) {
        PesStatus st = getStatus();
        std::string stateAfter = extractAndNormalizeState(st);
        if (!stateAfter.empty() && stateAfter != "UNKNOWN") {
            lastObservedStateAfter = stateAfter;
        }

        std::string stateBefore = stateToString(st.state);

        bool ready = (st.state == PesEngineState::PRINT_READY) ||
                     (stateAfter == "PRINT_READY") ||
                     ((st.state == PesEngineState::PAUSED || stateAfter == "PAUSED") && st.queueLen > 0) ||
                     ((st.state == PesEngineState::MID_JOB || stateAfter == "MID_JOB") && st.queueLen > 0);

        logOrchestrationStep("poll_state", stateBefore, stateAfter,
                            st.isReadyForPrintData, st.queueLen,
                            ready ? "OK" : "WAITING", "", elapsed);

        if (ready) {
            return true;
        }

        if (st.state == PesEngineState::FAULT || stateAfter == "FAULT") {
            logOrchestrationStep("poll_state", stateBefore, stateAfter,
                                st.isReadyForPrintData, st.queueLen,
                                "FAULT", "STATE_FAULT", elapsed);
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        elapsed += pollMs;
    }

    logOrchestrationStep("poll_state_timeout", "PRINT_READY", lastObservedStateAfter,
                        false, 0, "TIMEOUT",
                        "last=" + lastObservedStateAfter + ", accepted=[PRINT_READY|PAUSED|MID_JOB]", timeoutMs);
    return false;
}

bool PesOrchestrator::waitSessionCompleteAfterStart(int timeoutMs) {
    // Robust completion gate:
    // 1) After start, first observe an active print state (PRE_JOB/PRINTING/MID_JOB),
    // 2) then accept SESSION_COMPLETE.
    // This prevents stale SESSION_COMPLETE from a prior session causing early finish/cancel.
    int elapsed = 0;
    int pollMs = 500;
    std::string lastObservedStateAfter = "UNKNOWN";
    bool activeSeen = false;

    while (elapsed < timeoutMs) {
        PesStatus st = getStatus();
        std::string stateAfter = extractAndNormalizeState(st);
        if (!stateAfter.empty() && stateAfter != "UNKNOWN") {
            lastObservedStateAfter = stateAfter;
        }

        std::string stateBefore = stateToString(st.state);

        bool isActive = (st.state == PesEngineState::PRE_JOB) ||
                        (st.state == PesEngineState::PRINTING) ||
                        (st.state == PesEngineState::MID_JOB) ||
                        (st.state == PesEngineState::PAUSED && st.queueLen > 0) ||
                        (stateAfter == "PRE_JOB") ||
                        (stateAfter == "PRINTING") ||
                        (stateAfter == "MID_JOB") ||
                        (stateAfter == "PAUSED" && st.queueLen > 0);

        if (isActive) {
            activeSeen = true;
        }

        bool completeNow = (st.state == PesEngineState::SESSION_COMPLETE) ||
                           (stateAfter == "SESSION_COMPLETE");

        logOrchestrationStep("wait_session_complete_after_start", stateBefore, stateAfter,
                            st.isReadyForPrintData, st.queueLen,
                            (activeSeen && completeNow) ? "OK" : "WAITING", "", elapsed);

        if (activeSeen && completeNow) {
            return true;
        }

        if (st.state == PesEngineState::FAULT || stateAfter == "FAULT") {
            logOrchestrationStep("wait_session_complete_after_start", stateBefore, stateAfter,
                                st.isReadyForPrintData, st.queueLen,
                                "FAULT", "STATE_FAULT", elapsed);
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        elapsed += pollMs;
    }

    logOrchestrationStep("wait_session_complete_after_start_timeout", "WAITING", lastObservedStateAfter,
                        false, 0, "TIMEOUT",
                        std::string("active_seen=") + (activeSeen ? "true" : "false") + ", last=" + lastObservedStateAfter,
                        timeoutMs);
    return false;
}

bool PesOrchestrator::waitIdleAfterFinish(int timeoutMs) {
    // Accept when PRIMED_IDLE or DEPRIMED_IDLE.
    // On this PES build, PRINT_READY is also a valid post-finish terminal state.
    int elapsed = 0;
    int pollMs = 500;
    std::string lastObservedStateAfter = "UNKNOWN";
    
    while (elapsed < timeoutMs) {
        PesStatus st = getStatus();
        std::string stateAfter = extractAndNormalizeState(st);
        
        if (!stateAfter.empty() && stateAfter != "UNKNOWN") {
            lastObservedStateAfter = stateAfter;
        }
        
        std::string stateBefore = stateToString(st.state);
        
        // Terminal ack requires an idle/ready state and an empty queue.
        bool terminalState = (st.state == PesEngineState::PRIMED_IDLE) ||
                             (st.state == PesEngineState::DEPRIMED_IDLE) ||
                             (st.state == PesEngineState::PRINT_READY) ||
                             (!stateAfter.empty() && (stateAfter == "PRIMED_IDLE" || stateAfter == "DEPRIMED_IDLE" || stateAfter == "PRINT_READY"));
        bool idle = terminalState && (st.queueLen == 0);
        
        logOrchestrationStep("wait_idle_after_finish", stateBefore, stateAfter,
                            st.isReadyForPrintData, st.queueLen,
                            idle ? "OK" : "WAITING",
                            (!terminalState ? "WAIT_TERMINAL_STATE" : "WAIT_QUEUE_DRAIN"), elapsed);
        
        if (idle) {
            return true;
        }
        
        if (st.state == PesEngineState::FAULT || (!stateAfter.empty() && stateAfter == "FAULT")) {
            logOrchestrationStep("wait_idle_after_finish", stateBefore, stateAfter,
                                st.isReadyForPrintData, st.queueLen,
                                "FAULT", "STATE_FAULT", elapsed);
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        elapsed += pollMs;
    }
    
    // Timeout
    logOrchestrationStep("wait_idle_after_finish_timeout", "WAITING", lastObservedStateAfter,
                        false, 0, "TIMEOUT",
                        "last=" + lastObservedStateAfter, timeoutMs);
    return false;
}

bool PesOrchestrator::ensureEngineReady(int timeoutSec) {
    // Treat engine as ready if waitEngineReadyForSubmit succeeds (which accepts PRIMED_IDLE/isReadyForPrintData)
    if (!waitEngineReadyForSubmit(timeoutSec * 1000)) {
        return false;
    }
    
    // If engine was OFF, initialize it (use normalized string from extractAndNormalizeState)
    PesStatus st = getStatus();
    std::string stateAfter = extractAndNormalizeState(st);
    if (stateAfter == "OFF" || st.state == PesEngineState::OFF) {
        logInfo("Engine state is OFF, calling initialiseEngine");
        std::string result = runThriftCmd("initialise");
        if (result.find("ERROR") != std::string::npos || 
            result.find("error") != std::string::npos) {
            logError("initialiseEngine failed: " + result);
            return false;
        }
        
        // Wait for PRIMED_IDLE after initialization
        return waitEngineReadyForSubmit(timeoutSec * 1000);
    }
    
    return true;
}

bool PesOrchestrator::guardedPrepare(int mediaSpeed) {
    // Avoid invalid 0 IPS prepare on this PES build. Default to 1 IPS unless overridden.
    if (mediaSpeed <= 0) {
        int ips = 1;
        if (const char* v = std::getenv("PES_PREPARE_IPS")) {
            int parsed = std::atoi(v);
            if (parsed > 0) ips = parsed;
        }
        mediaSpeed = ips;
    }

    PesStatus st = getStatus();
    std::string stateBefore = extractAndNormalizeState(st);
    
    // Precondition: allow PRIMED_IDLE/PAUSED/PRINT_READY, and optionally UNKNOWN fallback.
    bool allowUnknownReady = false;
    if (const char* v = std::getenv("PES_ALLOW_UNKNOWN_READY")) {
        std::string s(v);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        allowUnknownReady = (s == "1" || s == "true" || s == "yes" || s == "on");
    }

    bool preStateOk = (st.state == PesEngineState::PRIMED_IDLE) ||
                      (st.state == PesEngineState::PAUSED) ||
                      (st.state == PesEngineState::PRINT_READY) ||
                      (stateBefore == "PRIMED_IDLE") ||
                      (stateBefore == "PAUSED") ||
                      (stateBefore == "PRINT_READY") ||
                      (allowUnknownReady && stateBefore == "UNKNOWN");

    if (!preStateOk) {
        logOrchestrationStep("guarded_prepare", stateBefore, stateBefore,
                            st.isReadyForPrintData, st.queueLen,
                            "BLOCKED", "INVALID_STATE", 0);
        return false;
    }

    if (st.queueLen == 0) {
        logOrchestrationStep("guarded_prepare", stateBefore, stateBefore,
                            st.isReadyForPrintData, st.queueLen,
                            "WARN_CONTINUE", "NO_JOB_IN_QUEUE", 0);
    }
    
    std::ostringstream cmd;
    cmd << "prepare=" << mediaSpeed;
    std::string result = runThriftCmd(cmd.str());
    
    bool success = (result.find("ERROR") == std::string::npos && 
                    result.find("error") == std::string::npos);
    
    PesStatus afterPrepare = getStatus();
    std::string stateAfter = extractAndNormalizeState(afterPrepare);
    
    logOrchestrationStep("guarded_prepare", stateBefore, stateAfter,
                        afterPrepare.isReadyForPrintData, afterPrepare.queueLen,
                        success ? "OK" : "FAILED", success ? "" : "PREPARE_FAILED", 0);
    
    return success;
}

bool PesOrchestrator::guardedStart() {
    PesStatus st = getStatus();
    std::string stateBefore = extractAndNormalizeState(st);

    bool bypassStateGates = false;
    if (const char* v = std::getenv("PES_BYPASS_STATE_GATES")) {
        std::string s(v);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        bypassStateGates = (s == "1" || s == "true" || s == "yes" || s == "on");
    }

    bool allowUnknownReady = false;
    if (const char* v = std::getenv("PES_ALLOW_UNKNOWN_READY")) {
        std::string s(v);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        allowUnknownReady = (s == "1" || s == "true" || s == "yes" || s == "on");
    }

    // START_PRINT gating: align accepted pre-start states with post-prepare wait.
    const bool stateIsPrintReady = (st.state == PesEngineState::PRINT_READY) || (stateBefore == "PRINT_READY");
    const bool stateIsQueuedPaused =
        ((st.state == PesEngineState::PAUSED) || (stateBefore == "PAUSED") ||
         (st.state == PesEngineState::MID_JOB) || (stateBefore == "MID_JOB")) &&
        st.queueLen > 0;
    const bool readyFlagInKnownStartState =
        st.isReadyForPrintData &&
        (stateBefore == "PRINT_READY" || stateBefore == "PAUSED" || stateBefore == "MID_JOB");

    bool startStateAllowed = stateIsPrintReady || stateIsQueuedPaused || readyFlagInKnownStartState;

    // Optional degraded-mode fallback for ambiguous status probe.
    if (!startStateAllowed && allowUnknownReady && stateBefore == "UNKNOWN" && st.isReadyForPrintData) {
        startStateAllowed = true;
    }

    {
        std::ostringstream gate;
        gate << "START_PRINT gate check: state=" << stateBefore
             << " enum=" << stateToString(st.state)
             << " ready=" << (st.isReadyForPrintData ? "true" : "false")
             << " queueLen=" << st.queueLen
             << " bypass=" << (bypassStateGates ? "true" : "false")
             << " allowUnknownReady=" << (allowUnknownReady ? "true" : "false")
             << " decision=" << ((bypassStateGates || startStateAllowed) ? "ALLOW" : "BLOCK");
        logInfo(gate.str());
    }

    if (!bypassStateGates && !startStateAllowed) {
        logOrchestrationStep("guarded_start", stateBefore, stateBefore,
                            st.isReadyForPrintData, st.queueLen,
                            "BLOCKED", "START_PRINT_GATE_BLOCKED_INVALID_PES_STATE", 0);
        return false;
    }

    std::string result = runThriftCmd("start");
    bool success = (result.find("ERROR") == std::string::npos &&
                    result.find("error") == std::string::npos &&
                    result.find("Traceback") == std::string::npos);

    if (!success) {
        std::string snip = result.substr(0, std::min<size_t>(result.size(), 280));
        logError("START_PRINT thrift call failed: " + snip);
    }

    PesStatus afterStart = getStatus();
    std::string stateAfter = extractAndNormalizeState(afterStart);

    logOrchestrationStep("guarded_start", stateBefore, stateAfter,
                        afterStart.isReadyForPrintData, afterStart.queueLen,
                        success ? "OK" : "FAILED", success ? "" : "START_PRINT_THRIFT_FAILED", 0);

    return success;
}

bool PesOrchestrator::guardedFinish() {
    PesStatus st = getStatus();
    std::string stateBefore = extractAndNormalizeState(st);
    
    // Best-effort: always attempt finish
    std::string result = runThriftCmd("finish");
    bool success = (result.find("ERROR") == std::string::npos && 
                    result.find("error") == std::string::npos);
    
    PesStatus afterFinish = getStatus();
    std::string stateAfter = extractAndNormalizeState(afterFinish);
    
    logOrchestrationStep("guarded_finish", stateBefore, stateAfter,
                        afterFinish.isReadyForPrintData, afterFinish.queueLen,
                        success ? "OK" : "WARN", success ? "" : "FINISH_FAILED", 0);
    
    return success;
}

bool PesOrchestrator::timedJoin(std::thread& t, int timeoutMs) {
    if (!t.joinable()) {
        return true;
    }
    
    // Check environment override
    int actualTimeout = timeoutMs;
    if (const char* v = std::getenv("DATA_THREAD_TIMEOUT_MS")) {
        int parsed = std::atoi(v);
        if (parsed > 0) {
            actualTimeout = parsed;
        }
    }
    
    auto deadline = std::chrono::steady_clock::now() + 
                    std::chrono::milliseconds(actualTimeout);
    
    // Poll dataDone flag rather than blocking on join()
    int waitedMs = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (dataDone_.load()) {
            t.join();
            return true;
        }
        if (waitedMs > 0 && (waitedMs % 5000) == 0) {
            PesStatus st = getStatus();
            std::string sa = extractAndNormalizeState(st);
            logOrchestrationStep("data_thread_wait", sa, sa,
                                st.isReadyForPrintData, st.queueLen,
                                "WAITING", "JOIN_PENDING", waitedMs);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        waitedMs += 250;
    }
    
    // Timeout expired -- thread is still running
    logStructured("PES_ORCH", "timed_join", "RUNNING", "TIMEOUT", 0,
                 "TIMEOUT", "TIMED_JOIN_EXPIRED", actualTimeout);
    return false;
}

bool PesOrchestrator::runPrintSession(JSLWrapper& jsl,
                                      const JobConfig& config,
                                      const std::vector<ColorPlane>& colors,
                                      const std::vector<PageData>& planes,
                                      bool legacy) {
    phase_ = SessionPhase::IDLE;
    JslibHdl handle = nullptr;
    bool handleOpen = false;
    bool threadLaunched = false;
    std::thread dataThread;
    std::atomic<bool> dataOk{false};

    bool bypassStateGates = false;
    if (const char* v = std::getenv("PES_BYPASS_STATE_GATES")) {
        std::string s(v);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        bypassStateGates = (s == "1" || s == "true" || s == "yes" || s == "on");
    }

    // Keep new PES orchestrator flow, but optionally force single-plane JSL submit
    // to avoid addPageMultiColor hangs in some environments.
    bool singlePlaneSubmit = false;
    if (const char* v = std::getenv("JSL_SINGLE_PLANE")) {
        std::string s(v);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        singlePlaneSubmit = (s == "1" || s == "true" || s == "yes" || s == "on");
    }
    const bool useSinglePlanePath = legacy || singlePlaneSubmit;
    
    // C6: Deterministic cleanup on ANY failure after openJob
    // JSL lifecycle: abort then close, always call closeJob
    auto cleanup = [&](const std::string& reason) -> bool {
        PesStatus cleanupStartSt = getStatus();
        std::string cleanupStartStateAfter = extractAndNormalizeState(cleanupStartSt);
        logOrchestrationStep("cleanup_start", cleanupStartStateAfter, cleanupStartStateAfter,
                            cleanupStartSt.isReadyForPrintData, cleanupStartSt.queueLen,
                            "START", reason, 0);
        
        if (handleOpen) {
            jsl.abortJob(handle);  // C6 step 1: abort if open
        }
        
        if (threadLaunched) {
            timedJoin(dataThread, DATA_THREAD_TIMEOUT);  // C6 step 2 (C4)
        }
        
        if (handleOpen) {
            jsl.closeJob(handle);  // C6 step 3: always close after abort
            handleOpen = false;
        }
        
        guardedFinish();  // C6 step 4 (best-effort)
        PesStatus finalSt = getStatus();  // C6 step 5
        
        std::string finalStateAfter = extractAndNormalizeState(finalSt);
        
        logOrchestrationStep("cleanup_done", finalStateAfter, finalStateAfter,
                            finalSt.isReadyForPrintData, finalSt.queueLen,
                            "DONE", reason, 0);
        phase_ = SessionPhase::FAILED;
        return false;
    };
    
    // Pre-flight queue hygiene: avoid stale queue entries before new submit\r\n    {\r\n        std::string clr = runThriftCmd("clearJobQueue");\r\n        if (clr.find("ERROR") != std::string::npos || clr.find("error") != std::string::npos) {\r\n            logOrchestrationStep("clear_job_queue", "UNKNOWN", "UNKNOWN", false, 0, "WARN", "CLEAR_QUEUE_FAILED", 0);\r\n        } else {\r\n            logOrchestrationStep("clear_job_queue", "UNKNOWN", "UNKNOWN", false, 0, "OK", "", 0);\r\n        }\r\n    }\r\n\r\n    // GATE 1: Wait engine ready for submit (per Memjet docs)
    phase_ = SessionPhase::IDLE;
    PesStatus initialSt = getStatus();
    std::string initialStateAfter = extractAndNormalizeState(initialSt);
    logOrchestrationStep("ensure_engine_ready", stateToString(initialSt.state), initialStateAfter,
                        initialSt.isReadyForPrintData, initialSt.queueLen,
                        "START", "", 0);
    
    if (!bypassStateGates && !ensureEngineReady(30)) {
        PesStatus finalSt = getStatus();
        std::string finalStateAfter = extractAndNormalizeState(finalSt);
        logOrchestrationStep("ensure_engine_ready", stateToString(finalSt.state), finalStateAfter,
                            finalSt.isReadyForPrintData, finalSt.queueLen,
                            "FAILED", "ENGINE_NOT_READY", 30000);
        return false;
    }
    
    // GATE 4: Apply optional alignment settings before prepare/start.
    const std::string alignmentCmd = buildAlignmentCommandFromEnv();
    if (!alignmentCmd.empty()) {
        PesStatus beforeAlign = getStatus();
        std::string beforeAlignStateAfter = extractAndNormalizeState(beforeAlign);
        logOrchestrationStep("apply_alignment_settings", beforeAlignStateAfter, beforeAlignStateAfter,
                            beforeAlign.isReadyForPrintData, beforeAlign.queueLen,
                            "START", alignmentCmd, 0);

        const std::string alignResult = runThriftCmd(alignmentCmd + " statusjson");
        if (alignResult.find("ERROR") != std::string::npos ||
            alignResult.find("error") != std::string::npos ||
            alignResult.find("Traceback") != std::string::npos) {
            return cleanup("Failed to apply alignment settings");
        }

        PesStatus afterAlign = getStatus();
        std::string afterAlignStateAfter = extractAndNormalizeState(afterAlign);
        logOrchestrationStep("apply_alignment_settings", beforeAlignStateAfter, afterAlignStateAfter,
                            afterAlign.isReadyForPrintData, afterAlign.queueLen,
                            "OK", "", 0);
    }

    // GATE 2: Open JSL job on main thread (fast metadata send)
    // JSL lifecycle: jslibInit already called in JSLWrapper::initialize()
    phase_ = SessionPhase::DATA_SUBMITTING;
    PesStatus beforeOpen = getStatus();
    std::string beforeOpenStateAfter = extractAndNormalizeState(beforeOpen);
    logOrchestrationStep("jsl_open_job", beforeOpenStateAfter, beforeOpenStateAfter,
                        beforeOpen.isReadyForPrintData, beforeOpen.queueLen,
                        "START", "", 0);
    
    bool openOk = useSinglePlanePath
        ? jsl.openJob(config, planes[0].color, 1, handle)
        : jsl.openJobMultiColor(config, colors, handle);
    if (!openOk) {
        logOrchestrationStep("jsl_open_job", beforeOpenStateAfter, beforeOpenStateAfter,
                            beforeOpen.isReadyForPrintData, beforeOpen.queueLen,
                            "FAILED", "OPEN_JOB_FAILED", 0);
        return false;
    }
    handleOpen = true;
    logOrchestrationStep("jsl_open_job", beforeOpenStateAfter, beforeOpenStateAfter,
                        beforeOpen.isReadyForPrintData, beforeOpen.queueLen,
                        "OK", "", 0);
    
    // Phase: DATA_SUBMITTING -- synchronous addPage call (JSL appears non-thread-safe in this flow)
    // JSL lifecycle: jslibAddPage with isLastPage=true
    phase_ = SessionPhase::DATA_SUBMITTING;
    auto submitStart = std::chrono::steady_clock::now();
    bool addPageOk = false;
    std::string addPageError = "";
    if (useSinglePlanePath) {
        if (planes.empty()) {
            addPageOk = false;
            addPageError = "NO_PLANES";
        } else if (singlePlaneSubmit && !legacy && planes.size() > 1) {
            // New orchestrator + single-plane mode: submit each MKCY plane as a separate JSL page.
            addPageOk = true;
            for (size_t i = 0; i < planes.size(); ++i) {
                bool isLast = (i == planes.size() - 1);
                if (!jsl.addPage(handle, planes[i], isLast)) {
                    addPageOk = false;
                    addPageError = "ADD_PAGE_SINGLE_FAILED";
                    break;
                }
            }
        } else {
            addPageOk = jsl.addPage(handle, planes[0], true);  // isLastPage=true
            if (!addPageOk) addPageError = "ADD_PAGE_SINGLE_FAILED";
        }
    } else {
        // New path multi-color submit: run in worker + hard timeout so we never hang forever.
        dataDone_.store(false);
        std::atomic<bool> workerOk{false};
        std::thread submitThread([&]() {
            bool ok = jsl.addPageMultiColor(handle, planes, true);  // isLastPage=true
            workerOk.store(ok);
            dataDone_.store(true);
        });
        if (!timedJoin(submitThread, DATA_THREAD_TIMEOUT)) {
            addPageError = "MULTICOLOR_SUBMIT_TIMEOUT";
            jsl.abortJob(handle);
            timedJoin(submitThread, 5000);
            if (submitThread.joinable()) submitThread.detach();
            addPageOk = false;
        } else {
            addPageOk = workerOk.load();
            if (!addPageOk) addPageError = "ADD_PAGE_MULTICOLOR_FAILED";
        }
    }
    int submitMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - submitStart).count());

    PesStatus afterSubmit = getStatus();
    std::string afterSubmitStateAfter = extractAndNormalizeState(afterSubmit);
    logOrchestrationStep("add_page_submit", afterSubmitStateAfter, afterSubmitStateAfter,
                        afterSubmit.isReadyForPrintData, afterSubmit.queueLen,
                        addPageOk ? "OK" : "FAILED", addPageError, submitMs);

    if (!addPageOk) {
        return cleanup("addPage failed during printing: " + addPageError);
    }
    
    // JSL lifecycle: jslibCloseJob always called
    jsl.closeJob(handle);
    handleOpen = false;
    phase_ = SessionPhase::TX_DONE;
    PesStatus afterClose = getStatus();
    std::string afterCloseStateAfter = extractAndNormalizeState(afterClose);
    logOrchestrationStep("phase_transition", "DATA_SUBMITTING", phaseToString(phase_),
                        afterClose.isReadyForPrintData, afterClose.queueLen,
                        "OK", "JSL_TRANSFER_COMPLETED", submitMs);
    logOrchestrationStep("jsl_close_job", afterCloseStateAfter, afterCloseStateAfter,
                        afterClose.isReadyForPrintData, afterClose.queueLen,
                        "OK", "", 0);

    const int queueTimeoutMs = getEnvIntOrDefault("PES_QUEUE_TIMEOUT_MS", 30000);
    const int printCompleteTimeoutMs = getEnvIntOrDefault("PES_PRINT_COMPLETE_TIMEOUT_MS", 60000);
    const int jobDoneTimeoutMs = getEnvIntOrDefault("PES_JOB_DONE_TIMEOUT_MS", 30000);

    // GATE 3: hard queue gate AFTER closeJob
    phase_ = SessionPhase::WAITING_QUEUE;
    logOrchestrationStep("phase_transition", "TX_DONE", phaseToString(phase_),
                        afterClose.isReadyForPrintData, afterClose.queueLen,
                        "OK", "", queueTimeoutMs);
    if (!waitJobQueuedAfterSubmit(queueTimeoutMs)) {
        return cleanup("Queue not ready within timeout after closeJob");
    }

    // GATE 5: strict prepare guard
    phase_ = SessionPhase::PREPARING;
    PesStatus beforePrepare = getStatus();
    std::string beforePrepareStateAfter = extractAndNormalizeState(beforePrepare);
    logOrchestrationStep("phase_transition", "WAITING_QUEUE", phaseToString(phase_),
                        beforePrepare.isReadyForPrintData, beforePrepare.queueLen,
                        "OK", "", 0);

    if ((beforePrepare.state != PesEngineState::PRIMED_IDLE) &&
        (beforePrepare.state != PesEngineState::PRINT_READY) &&
        (beforePrepareStateAfter != "PRIMED_IDLE") &&
        (beforePrepareStateAfter != "PRINT_READY")) {
        return cleanup("prepareToPrint blocked: engine not PRIMED_IDLE/PRINT_READY");
    }
    if (beforePrepare.queueLen == 0) {
        return cleanup("prepareToPrint blocked: no job in queue");
    }
    if (!guardedPrepare(0)) {
        return cleanup("prepareToPrint rejected");
    }

    // GATE 6: wait print ready
    phase_ = SessionPhase::WAITING_PRINT_READY;
    if (!waitEnginePrintReadyAfterPrepare(15000)) {
        return cleanup("PRINT_READY timeout after prepare");
    }

    // GATE 7: start
    phase_ = SessionPhase::STARTING;
    PesStatus beforeStart = getStatus();
    std::string beforeStartStateAfter = extractAndNormalizeState(beforeStart);
    logOrchestrationStep("start_print_precheck", beforeStartStateAfter, beforeStartStateAfter,
                        beforeStart.isReadyForPrintData, beforeStart.queueLen,
                        "START", "", 0);

    if (!guardedStart()) {
        return cleanup("startPrinting rejected by START_PRINT gate/thrift failure");
    }

    PesStatus startAccepted = getStatus();
    std::string startAcceptedStateAfter = extractAndNormalizeState(startAccepted);
    bool startTransitionVisible = (startAcceptedStateAfter == "PRE_JOB") ||
                                  (startAcceptedStateAfter == "PRINTING") ||
                                  (startAcceptedStateAfter == "MID_JOB") ||
                                  (startAcceptedStateAfter == "PAUSED" && startAccepted.queueLen > 0) ||
                                  (startAcceptedStateAfter == "SESSION_COMPLETE");
    logOrchestrationStep("start_print_postcheck", beforeStartStateAfter, startAcceptedStateAfter,
                        startAccepted.isReadyForPrintData, startAccepted.queueLen,
                        startTransitionVisible ? "OK" : "WARN",
                        startTransitionVisible ? "" : "START_PRINT_NO_IMMEDIATE_ACTIVE_TRANSITION", 0);

    // GATE 8: Wait print completion ack after start
    phase_ = SessionPhase::WAIT_PRINT_COMPLETE;
    logOrchestrationStep("phase_transition", "STARTING", phaseToString(phase_),
                        startAccepted.isReadyForPrintData, startAccepted.queueLen,
                        "OK", "", printCompleteTimeoutMs);
    if (!waitSessionCompleteAfterStart(printCompleteTimeoutMs)) {
        guardedFinish();
        PesStatus timeoutSt = getStatus();
        std::string timeoutStateAfter = extractAndNormalizeState(timeoutSt);
        logOrchestrationStep("wait_session_complete_timeout", timeoutStateAfter, timeoutStateAfter,
                            timeoutSt.isReadyForPrintData, timeoutSt.queueLen,
                            "TIMEOUT", "SESSION_COMPLETE_TIMEOUT", printCompleteTimeoutMs);
        return false;
    }
    
    // GATE 9: finishPrinting (optional; some firmware paths cancel if called too early)
    bool callFinish = false;
    if (const char* v = std::getenv("PES_CALL_FINISH")) {
        std::string s(v);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        callFinish = (s == "1" || s == "true" || s == "yes" || s == "on");
    }

    phase_ = SessionPhase::FINISHING;
    if (callFinish) {
        guardedFinish();
    } else {
        PesStatus stNoFinish = getStatus();
        std::string saNoFinish = extractAndNormalizeState(stNoFinish);
        logOrchestrationStep("guarded_finish", saNoFinish, saNoFinish,
                            stNoFinish.isReadyForPrintData, stNoFinish.queueLen,
                            "SKIP", "PES_CALL_FINISH_DISABLED", 0);
    }

    // GATE 10: terminal print completion (idle/ready + queue drained)
    phase_ = SessionPhase::WAIT_JOB_DONE;
    PesStatus beforeDoneWait = getStatus();
    std::string beforeDoneStateAfter = extractAndNormalizeState(beforeDoneWait);
    logOrchestrationStep("phase_transition", "FINISHING", phaseToString(phase_),
                        beforeDoneWait.isReadyForPrintData, beforeDoneWait.queueLen,
                        "OK", "", jobDoneTimeoutMs);
    if (!waitIdleAfterFinish(jobDoneTimeoutMs)) {
        PesStatus timeoutSt = getStatus();
        std::string timeoutStateAfter = extractAndNormalizeState(timeoutSt);
        logOrchestrationStep("wait_job_done_timeout", beforeDoneStateAfter, timeoutStateAfter,
                            timeoutSt.isReadyForPrintData, timeoutSt.queueLen,
                            "TIMEOUT", "TERMINAL_PRINT_ACK_TIMEOUT", jobDoneTimeoutMs);
        return cleanup("Timed out waiting for terminal print completion state");
    }
    
    phase_ = SessionPhase::SUCCESS;
    PesStatus finalCompleteSt = getStatus();
    std::string finalCompleteStateAfter = extractAndNormalizeState(finalCompleteSt);
    logOrchestrationStep("session_complete", finalCompleteStateAfter, finalCompleteStateAfter,
                        finalCompleteSt.isReadyForPrintData, finalCompleteSt.queueLen,
                        "OK", "PRINT_COMPLETION_ACKED", 0);
    return true;
}

} // namespace memjet













