#include "config.h"
#include <cstdlib>
#include <algorithm>

namespace memjet {
namespace {

std::string lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return v;
}

std::string upper(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
    return v;
}

bool parseBool(const char* name, bool fallback, bool* wasSet = nullptr) {
    const char* v = std::getenv(name);
    if (!v) {
        if (wasSet) *wasSet = false;
        return fallback;
    }
    if (wasSet) *wasSet = true;
    std::string s = lower(v);
    return !(s.empty() || s == "0" || s == "false" || s == "no" || s == "off");
}

bool parseInt(const char* name, int fallback, int minV, int maxV, int& out, std::string& err) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        out = fallback;
        return true;
    }
    try {
        int parsed = std::stoi(v);
        if (parsed < minV || parsed > maxV) {
            err = std::string(name) + " must be in range [" + std::to_string(minV) + ", " + std::to_string(maxV) + "]";
            return false;
        }
        out = parsed;
        return true;
    } catch (...) {
        err = std::string(name) + " must be an integer";
        return false;
    }
}

double parseDouble(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try { return std::stod(v); } catch (...) { return fallback; }
}

double clamp(double v, double lo, double hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

} // namespace

bool loadRuntimeConfig(const memjet::utils::CommandLineArgs& args,
                       RuntimeConfig& out,
                       std::string& error,
                       std::vector<std::string>& warnings) {
    out.forceFastMono = parseBool("USE_FAST_MONO", false, nullptr);
    out.useTrueCmyk = parseBool("USE_TRUE_CMYK", args.cmyk, &out.envTrueCmykSet);
    if (out.forceFastMono) {
        out.useTrueCmyk = false;
        out.modeReason = "mono forced by USE_FAST_MONO";
    } else if (out.envTrueCmykSet) {
        out.modeReason = std::string("USE_TRUE_CMYK=") + (out.useTrueCmyk ? "1" : "0");
    } else {
        out.modeReason = args.cmyk ? "CLI default/--cmyk" : "CLI --gray/--mono override";
    }

    if (const char* v = std::getenv("RIP_BASELINE_PROFILE")) {
        out.baselineProfile = upper(v);
    }

    double baseInkLimit = 1.0;
    double baseCScale = 1.0, baseMScale = 1.0, baseYScale = 1.0, baseKScale = 1.0;
    int baseThresholdBias = 0;
    if (out.baselineProfile == "ANYFLOW_V1") {
        baseInkLimit = 0.82;
        baseCScale = 0.86;
        baseMScale = 0.92;
        baseYScale = 0.75;
        baseKScale = 0.93;
        baseThresholdBias = 10;
    } else if (out.baselineProfile != "NONE") {
        warnings.push_back("Unknown RIP_BASELINE_PROFILE='" + out.baselineProfile + "'; using NONE");
        out.baselineProfile = "NONE";
    }

    double userInkLimit = parseDouble("RIP_INK_LIMIT", 1.0);
    if (userInkLimit > 1.0) userInkLimit /= 100.0;
    userInkLimit = clamp(userInkLimit, 0.0, 1.0);

    const double userCScale = clamp(parseDouble("RIP_C_SCALE", 1.0), 0.0, 2.0);
    const double userMScale = clamp(parseDouble("RIP_M_SCALE", 1.0), 0.0, 2.0);
    const double userYScale = clamp(parseDouble("RIP_Y_SCALE", 1.0), 0.0, 2.0);
    const double userKScale = clamp(parseDouble("RIP_K_SCALE", 1.0), 0.0, 2.0);
    const int userThresholdBias = clampi(static_cast<int>(parseDouble("RIP_THRESHOLD_BIAS", 0.0)), -64, 64);

    out.globalInkLimit = clamp(baseInkLimit * userInkLimit, 0.0, 1.0);
    out.cScale = clamp(baseCScale * userCScale, 0.0, 2.0);
    out.mScale = clamp(baseMScale * userMScale, 0.0, 2.0);
    out.yScale = clamp(baseYScale * userYScale, 0.0, 2.0);
    out.kScale = clamp(baseKScale * userKScale, 0.0, 2.0);
    out.thresholdBias = clampi(baseThresholdBias + userThresholdBias, -64, 64);

    out.invertBits = parseBool("JSL_INVERT_BITS", false, nullptr);
    out.testPattern = parseBool("JSL_TEST_PATTERN", false, nullptr);

    out.targetStripWidth = static_cast<uint32_t>(args.dryRun ? 0 : 0); // set by caller from raster width
    int stripStart = 0;
    if (!parseInt("JSL_STRIP_START", 0, 0, 1000000, stripStart, error)) return false;
    out.stripStart = static_cast<uint32_t>(stripStart);

    int stripWidth = 0;
    if (!parseInt("JSL_STRIP_WIDTH", 0, 0, 1000000, stripWidth, error)) return false;
    out.stripWidth = static_cast<uint32_t>(stripWidth);

    if (const char* v = std::getenv("THRIFT_CONTROLLER_PATH")) {
        if (*v) out.thriftControllerPath = v;
    }
    if (!parseInt("THRIFT_CONTROL_PORT", 13001, 1, 65535, out.thriftControlPort, error)) return false;
    if (const char* v = std::getenv("THRIFT_PYTHON_EXE")) {
        if (*v) out.pythonExe = v;
    }
    if (const char* v = std::getenv("PDL_THRIFT_ROOT")) {
        if (*v) out.pdlThriftRoot = v;
    }

    out.useLegacyOrchestration = args.legacyJsl || parseBool("USE_LEGACY_ORCHESTRATION", false, nullptr);

    if (!parseInt("THRIFT_WAIT_JOB_TIMEOUT_MS", 8000, 1, 300000, out.thriftWaitJobTimeoutMs, error)) return false;
    if (!parseInt("THRIFT_WAIT_JOB_POLL_MS", 250, 1, 30000, out.thriftWaitJobPollMs, error)) return false;
    if (!parseInt("JSL_POST_START_HOLD_MS", 8000, 0, 300000, out.postStartHoldMs, error)) return false;
    out.immediateFinish = parseBool("JSL_IMMEDIATE_FINISH", false, nullptr);

    return true;
}

} // namespace memjet
