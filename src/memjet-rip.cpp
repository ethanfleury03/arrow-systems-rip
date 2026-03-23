#include <iostream>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <cmath>
#ifdef _WIN32
#include <intrin.h>
#endif

#include "jsl_wrapper.h"
#include "pdf_rasterizer.h"
#include "bilevel_converter.h"
#include "utils.h"
#include "pes_orchestrator.h"

using namespace memjet;
using namespace memjet::utils;

static const char* colorName(ColorPlane c) {
    switch (c) {
        case ColorPlane::CYAN:    return "Cyan";
        case ColorPlane::MAGENTA: return "Magenta";
        case ColorPlane::YELLOW:  return "Yellow";
        case ColorPlane::BLACK:   return "Black";
        default:                  return "Unknown";
    }
}

int main(int argc, char* argv[]) {
    CommandLineArgs args = parseCommandLine(argc, argv);

    if (args.inputPdf.empty()) {
        logError("No input PDF specified");
        printUsage(argv[0]);
        return 1;
    }

    if (!fileExists(args.inputPdf)) {
        logError("Input file not found: " + args.inputPdf);
        return 1;
    }

    if (!args.dryRun && args.pesIp.empty()) {
        logError("PES IP address required (use --pes-ip or --dry-run)");
        printUsage(argv[0]);
        return 1;
    }

    logInfo("Memjet RIP Proof of Concept");
    logInfo("Input: " + args.inputPdf);
    logInfo("Resolution: " + std::to_string(args.dpi) + " DPI");

    if (args.dryRun) {
        logInfo("Mode: DRY RUN (no printer output)");
    } else {
        logInfo("PES: " + args.pesIp + ":" + std::to_string(args.pesPort));
        if (args.legacyJsl) {
            logInfo("JSL mode: legacy (sequential single-color jobs)");
        }
    }

    try {
        // Step 1: Rasterize PDF
        PDFRasterizer rasterizer;
        if (!rasterizer.initialize()) {
            logError("Failed to initialize rasterizer: " + rasterizer.getLastError());
            return 1;
        }

        bool forceFastMono = false;
        if (const char* v = std::getenv("USE_FAST_MONO")) {
            std::string s = v;
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            forceFastMono = !(s.empty() || s == "0" || s == "false" || s == "no" || s == "off");
        }

        bool useTrueCmyk = false;
        if (const char* v = std::getenv("USE_TRUE_CMYK")) {
            std::string s = v;
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            useTrueCmyk = !(s.empty() || s == "0" || s == "false" || s == "no" || s == "off");
        }
        if (forceFastMono) {
            useTrueCmyk = false;
        }

        auto readEnvDouble = [](const char* name, double defVal) {
            if (const char* v = std::getenv(name)) {
                try {
                    return std::stod(std::string(v));
                } catch (...) {
                    return defVal;
                }
            }
            return defVal;
        };

        auto clampDouble = [](double v, double lo, double hi) {
            return (v < lo) ? lo : ((v > hi) ? hi : v);
        };
        auto clampInt = [](int v, int lo, int hi) {
            return (v < lo) ? lo : ((v > hi) ? hi : v);
        };

        // Baseline linearization profile (internal engine behavior) + user calibration layer.
        // Keep user layer neutral (all 1.0 / bias 0) to emulate Anyflow "Calibration=None" behavior.
        std::string baselineProfile = "NONE";
        if (const char* v = std::getenv("RIP_BASELINE_PROFILE")) {
            baselineProfile = v;
            std::transform(baselineProfile.begin(), baselineProfile.end(), baselineProfile.begin(), ::toupper);
        }

        double baseInkLimit = 1.0;
        double baseCScale = 1.0, baseMScale = 1.0, baseYScale = 1.0, baseKScale = 1.0;
        int baseThresholdBias = 0;

        if (baselineProfile == "ANYFLOW_V1") {
            baseInkLimit = 0.82;
            baseCScale = 0.86;
            baseMScale = 0.92;
            baseYScale = 0.75;
            baseKScale = 0.93;
            baseThresholdBias = 10;
        }

        // User-facing calibration knobs (env)
        // RIP_INK_LIMIT: 0..1 (or 0..100 as percent). Default 1.0
        // RIP_[C|M|Y|K]_SCALE: per-channel multiplier. Default 1.0
        // RIP_THRESHOLD_BIAS: -64..+64; positive = lighter (fewer dots)
        double userInkLimit = readEnvDouble("RIP_INK_LIMIT", 1.0);
        if (userInkLimit > 1.0) {
            userInkLimit = userInkLimit / 100.0;
        }
        userInkLimit = clampDouble(userInkLimit, 0.0, 1.0);

        const double userCScale = clampDouble(readEnvDouble("RIP_C_SCALE", 1.0), 0.0, 2.0);
        const double userMScale = clampDouble(readEnvDouble("RIP_M_SCALE", 1.0), 0.0, 2.0);
        const double userYScale = clampDouble(readEnvDouble("RIP_Y_SCALE", 1.0), 0.0, 2.0);
        const double userKScale = clampDouble(readEnvDouble("RIP_K_SCALE", 1.0), 0.0, 2.0);

        int userThresholdBias = static_cast<int>(readEnvDouble("RIP_THRESHOLD_BIAS", 0.0));
        userThresholdBias = clampInt(userThresholdBias, -64, 64);

        // Effective tuning = baseline linearization * user calibration
        const double globalInkLimit = clampDouble(baseInkLimit * userInkLimit, 0.0, 1.0);
        const double cScale = clampDouble(baseCScale * userCScale, 0.0, 2.0);
        const double mScale = clampDouble(baseMScale * userMScale, 0.0, 2.0);
        const double yScale = clampDouble(baseYScale * userYScale, 0.0, 2.0);
        const double kScale = clampDouble(baseKScale * userKScale, 0.0, 2.0);
        const int thresholdBias = clampInt(baseThresholdBias + userThresholdBias, -64, 64);

        if (useTrueCmyk) {
            std::ostringstream tune;
            tune << "RIP baseline=" << baselineProfile
                 << " BASE[INK=" << baseInkLimit << " C=" << baseCScale << " M=" << baseMScale
                 << " Y=" << baseYScale << " K=" << baseKScale << " BIAS=" << baseThresholdBias << "]"
                 << " USER[INK=" << userInkLimit << " C=" << userCScale << " M=" << userMScale
                 << " Y=" << userYScale << " K=" << userKScale << " BIAS=" << userThresholdBias << "]"
                 << " => EFFECTIVE[INK=" << globalInkLimit << " C=" << cScale << " M=" << mScale
                 << " Y=" << yScale << " K=" << kScale << " BIAS=" << thresholdBias << "]";
            logInfo(tune.str());
        }

        RasterParams rasterParams;
        rasterParams.dpi = args.dpi;
        rasterParams.yDpi = 0;  // same as dpi for GS rendering
        rasterParams.pageNumber = args.pageNumber;
        rasterParams.paperSize = args.paperSize;

        RasterFileInfo rasterInfo;
        std::unique_ptr<TempFileGuard> tempGuard;
        CmykRasterData cmykRaster;

        if (useTrueCmyk) {
            logInfo("Step 1: Rasterizing PDF to CMYK (pamcmyk32)...");
            cmykRaster = rasterizer.rasterizeToCmykPlanes(args.inputPdf, rasterParams);
            rasterInfo.width = cmykRaster.width;
            rasterInfo.height = cmykRaster.height;
            rasterInfo.pageNumber = cmykRaster.pageNumber;
            rasterInfo.dataOffset = 0;
            rasterInfo.fileSizeBytes = 0;
            logInfo("Rasterized CMYK " + std::to_string(rasterInfo.width) + "x" +
                    std::to_string(rasterInfo.height) + " pixels");
        } else {
            logInfo("Step 1: Rasterizing PDF to disk PGM...");
            rasterInfo = rasterizer.rasterizeToFile(args.inputPdf, rasterParams);
            tempGuard.reset(new TempFileGuard(rasterInfo.filePath));
            logInfo("Rasterized " + std::to_string(rasterInfo.width) + "x" +
                    std::to_string(rasterInfo.height) + " pixels");
        }

        // Step 2: Bilevel conversion for each color plane
        logInfo(std::string("Step 2: Bilevel conversion (") + (forceFastMono ? "FAST_MONO" : (useTrueCmyk ? "TRUE_CMYK->MCKY" : "GRAY_CLONE_MCKY")) + ")...");

        BilevelConverter converter;

        // Anyflow/JSL channel order expected by this printer profile: M, C, K, Y
        std::vector<ColorPlane> planeColors = forceFastMono
            ? std::vector<ColorPlane>{ColorPlane::BLACK}
            : std::vector<ColorPlane>{
                ColorPlane::MAGENTA,
                ColorPlane::CYAN,
                ColorPlane::BLACK,
                ColorPlane::YELLOW
              };

        std::vector<PageData> planes;

        bool invertBits = false;
        if (const char* v = std::getenv("JSL_INVERT_BITS")) {
            std::string s = v;
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            invertBits = !(s.empty() || s == "0" || s == "false" || s == "no" || s == "off");
        }

        bool testPattern = false;
        if (const char* v = std::getenv("JSL_TEST_PATTERN")) {
            std::string s = v;
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            testPattern = !(s.empty() || s == "0" || s == "false" || s == "no" || s == "off");
        }

        if (testPattern) {
            logInfo("JSL_TEST_PATTERN=1 enabled (Magenta plane forced solid, others zero)");
        }
        if (invertBits) {
            logInfo("JSL_INVERT_BITS=1 enabled (bitwise invert on packed payload)");
        }

        // Target strip width for packed payload geometry (must match JSL job stripWidth).
        uint32_t targetStripWidth = static_cast<uint32_t>(rasterInfo.width);
        if (const char* sw = std::getenv("JSL_STRIP_WIDTH")) {
            uint32_t parsed = static_cast<uint32_t>(std::strtoul(sw, nullptr, 10));
            if (parsed > 0) targetStripWidth = parsed;
        }

        auto repackToStripWidth = [&](const std::vector<uint8_t>& srcPacked, int srcWidth, int height, int dstWidth) -> std::vector<uint8_t> {
            if (dstWidth <= srcWidth) return srcPacked;
            const int srcWords = ((srcWidth + 31) / 32 + 3) / 4 * 4;
            const int dstWords = ((dstWidth + 31) / 32 + 3) / 4 * 4;
            const int srcLineBytes = srcWords * 4;
            const int dstLineBytes = dstWords * 4;
            std::vector<uint8_t> out(static_cast<size_t>(dstLineBytes) * height, 0);

            for (int y = 0; y < height; ++y) {
                const uint8_t* srow = srcPacked.data() + static_cast<size_t>(y) * srcLineBytes;
                uint8_t* drow = out.data() + static_cast<size_t>(y) * dstLineBytes;
                for (int x = 0; x < srcWidth; ++x) {
                    const int sw = x / 32, sb = x % 32;
                    const int dw = x / 32, db = x % 32;
                    uint32_t sWord = static_cast<uint32_t>(srow[sw*4]) |
                                     (static_cast<uint32_t>(srow[sw*4+1]) << 8) |
                                     (static_cast<uint32_t>(srow[sw*4+2]) << 16) |
                                     (static_cast<uint32_t>(srow[sw*4+3]) << 24);
                    if (sWord & (1u << db)) {
                        uint32_t dWord = static_cast<uint32_t>(drow[dw*4]) |
                                         (static_cast<uint32_t>(drow[dw*4+1]) << 8) |
                                         (static_cast<uint32_t>(drow[dw*4+2]) << 16) |
                                         (static_cast<uint32_t>(drow[dw*4+3]) << 24);
                        dWord |= (1u << db);
                        drow[dw*4]   = static_cast<uint8_t>(dWord & 0xFF);
                        drow[dw*4+1] = static_cast<uint8_t>((dWord >> 8) & 0xFF);
                        drow[dw*4+2] = static_cast<uint8_t>((dWord >> 16) & 0xFF);
                        drow[dw*4+3] = static_cast<uint8_t>((dWord >> 24) & 0xFF);
                    }
                }
            }
            return out;
        };

        for (size_t i = 0; i < planeColors.size(); ++i) {
            logInfo(std::string("Converting plane: ") + colorName(planeColors[i]));

            std::vector<uint8_t> packed;
            if (useTrueCmyk) {
                const std::vector<uint8_t>* src = nullptr;
                switch (planeColors[i]) {
                    case ColorPlane::MAGENTA: src = &cmykRaster.m; break;
                    case ColorPlane::BLACK:   src = &cmykRaster.k; break;
                    case ColorPlane::CYAN:    src = &cmykRaster.c; break;
                    case ColorPlane::YELLOW:  src = &cmykRaster.y; break;
                    default: break;
                }
                if (!src || src->empty()) {
                    throw std::runtime_error("Missing CMYK source data for plane " + std::string(colorName(planeColors[i])));
                }

                // pamcmyk32 yields channel intensity where 0=white/no ink and 255=full ink.
                // Existing converter expects grayscale luminance where 0=black/ink and 255=white/no ink.
                // Apply optional ink/tone tuning, then convert semantics.
                const double chScale = (planeColors[i] == ColorPlane::CYAN)    ? cScale
                                     : (planeColors[i] == ColorPlane::MAGENTA) ? mScale
                                     : (planeColors[i] == ColorPlane::YELLOW)  ? yScale
                                                                              : kScale;

                std::vector<uint8_t> planeLuma(src->size());
                for (size_t px = 0; px < src->size(); ++px) {
                    const double ink = static_cast<double>((*src)[px]) / 255.0;
                    const double inkTuned = clampDouble(ink * globalInkLimit * chScale, 0.0, 1.0);
                    int luma = static_cast<int>(std::lround((1.0 - inkTuned) * 255.0));
                    luma = clampInt(luma + thresholdBias, 0, 255);
                    planeLuma[px] = static_cast<uint8_t>(luma);
                }

                std::vector<uint8_t> bilevel = converter.convertToBilevelErrorDiffusion(planeLuma, rasterInfo.width, rasterInfo.height);
                packed = converter.packToJSLFormat(bilevel, rasterInfo.width, rasterInfo.height);
            } else {
                packed = converter.convertAndPackStreaming(
                    rasterInfo.filePath, rasterInfo.dataOffset,
                    rasterInfo.width, rasterInfo.height);
            }

            if (targetStripWidth > static_cast<uint32_t>(rasterInfo.width)) {
                packed = repackToStripWidth(packed, rasterInfo.width, rasterInfo.height,
                                            static_cast<int>(targetStripWidth));
            }

            if (testPattern) {
                const bool isMagenta = (planeColors[i] == ColorPlane::MAGENTA);
                std::fill(packed.begin(), packed.end(), isMagenta ? static_cast<uint8_t>(0xFF) : static_cast<uint8_t>(0x00));
            }

            if (invertBits) {
                for (auto& b : packed) {
                    b = static_cast<uint8_t>(~b);
                }
            }

            PageData pd;
            pd.color = planeColors[i];
            pd.pageNumber = 1;
            pd.data = std::move(packed);
            pd.width = targetStripWidth;
            pd.height = rasterInfo.height;

            size_t nonZeroBytes = 0;
            size_t oneBits = 0;
            for (uint8_t b : pd.data) {
                if (b != 0) {
                    ++nonZeroBytes;
                    oneBits += static_cast<size_t>(__popcnt16(static_cast<unsigned short>(b)));
                }
            }
            const double nonZeroPct = pd.data.empty() ? 0.0 : (100.0 * static_cast<double>(nonZeroBytes) / static_cast<double>(pd.data.size()));
            logInfo(std::string(colorName(planeColors[i])) + ": " +
                    std::to_string(pd.data.size()) + " bytes packed"
                    + ", nonZeroBytes=" + std::to_string(nonZeroBytes)
                    + " (" + std::to_string(nonZeroPct) + "%)"
                    + ", oneBits=" + std::to_string(oneBits));

            planes.push_back(std::move(pd));
        }

        // Step 3: Submit to printer or dry run
        if (args.dryRun) {
            logInfo("Step 3: DRY RUN - Would send to printer:");
            for (const auto& p : planes) {
                logInfo("  " + std::string(colorName(p.color)) + ": " +
                       std::to_string(p.data.size()) + " bytes");
            }
            logInfo("Done! (dry run)");
            return 0;
        }

        logInfo("Step 3: Sending to printer via JSL...");

        JSLWrapper jsl;
        if (!jsl.initialize()) {
            logError("Failed to initialize JSL");
            return 1;
        }

        std::string jobId = generateJobId();
        logInfo("Job ID: " + jobId);

        JobConfig jobConfig;
        jobConfig.destination = args.pesIp;
        jobConfig.port = args.pesPort;
        jobConfig.resolution = static_cast<uint32_t>(args.dpi);

        // Strip geometry: default to full raster width, but allow fast runtime overrides.
        // Useful because some printer/JSL profiles expect specific strip metadata.
        uint32_t stripStart = 0;
        // Default to payload geometry width unless explicitly overridden.
        uint32_t stripWidth = targetStripWidth;

        if (const char* ss = std::getenv("JSL_STRIP_START")) {
            stripStart = static_cast<uint32_t>(std::strtoul(ss, nullptr, 10));
        }
        if (const char* sw = std::getenv("JSL_STRIP_WIDTH")) {
            uint32_t parsed = static_cast<uint32_t>(std::strtoul(sw, nullptr, 10));
            if (parsed > 0) {
                stripWidth = parsed;
            }
        }

        jobConfig.stripStart = stripStart;
        jobConfig.stripWidth = stripWidth;
        jobConfig.jobId = jobId;

        logInfo("JSL strip metadata: start=" + std::to_string(jobConfig.stripStart) +
                ", width=" + std::to_string(jobConfig.stripWidth));
        if (!planes.empty() && jobConfig.stripWidth != planes[0].width) {
            logInfo("[WARN] stripWidth/job payload width mismatch: stripWidth=" +
                    std::to_string(jobConfig.stripWidth) + ", payloadWidth=" +
                    std::to_string(planes[0].width));
        }

        bool submitOk = false;

        // Optional PES preflight via existing thrift_controller.py
        // Required on some Kareela builds where JSL data path (Gymea) needs an active print session.
        const char* thriftControllerPathEnv = std::getenv("THRIFT_CONTROLLER_PATH");
        std::string thriftControllerPath = (thriftControllerPathEnv && std::strlen(thriftControllerPathEnv) > 0)
            ? std::string(thriftControllerPathEnv)
            : std::string("C:\\Users\\Arrow\\Arrow-Rip\\src\\thrift_controller_fullcycle.py");
        int thriftControlPort = 13001;
        if (const char* p = std::getenv("THRIFT_CONTROL_PORT")) {
            thriftControlPort = std::atoi(p);
            if (thriftControlPort <= 0) thriftControlPort = 13001;
        }

        // Force deterministic runtime (no PATH roulette)
        std::string pythonExe = "C:\\Python27\\python.exe";
        if (const char* py = std::getenv("THRIFT_PYTHON_EXE")) {
            if (std::strlen(py) > 0) pythonExe = py;
        }

        // Ensure SDK imports always resolve
        std::string pdlThriftRoot = "C:\\Users\\Arrow\\Arrow-Rip\\vendor\\pdl_py";
        if (const char* root = std::getenv("PDL_THRIFT_ROOT")) {
            if (std::strlen(root) > 0) pdlThriftRoot = root;
        }

        // C5: Feature flag from day 1 - allows instant rollback without rebuild
        // CLI flag must take precedence in day-to-day debugging.
        bool useLegacyOrchestration = args.legacyJsl;
        if (const char* legacyEnv = std::getenv("USE_LEGACY_ORCHESTRATION")) {
            std::string v = legacyEnv;
            std::transform(v.begin(), v.end(), v.begin(), ::tolower);
            bool envEnabled = !(v.empty() || v == "0" || v == "false" || v == "no" || v == "off");
            useLegacyOrchestration = useLegacyOrchestration || envEnabled;
        }

        if (useLegacyOrchestration) {
            if (args.legacyJsl) {
                logInfo("Using legacy orchestration (--legacy-jsl)");
            } else {
                logInfo("Using legacy orchestration (USE_LEGACY_ORCHESTRATION=1)");
            }
            // --- BEGIN ORIGINAL CODE (preserved verbatim) ---
            auto runThriftCommands = [&](const std::string& commands) -> bool {
                if (thriftControllerPath.empty()) {
                    return true; // no-op unless explicitly enabled
                }

                std::ostringstream cmd;
#ifdef _WIN32
                cmd << "set \"PDL_THRIFT_ROOT=" << pdlThriftRoot << "\" && "
                    << "\"" << pythonExe << "\" "
                    << "\"" << thriftControllerPath << "\" "
                    << args.pesIp << " " << thriftControlPort << " " << commands;
#else
                cmd << "PDL_THRIFT_ROOT=\"" << pdlThriftRoot << "\" "
                    << "\"" << pythonExe << "\" "
                    << "\"" << thriftControllerPath << "\" "
                    << args.pesIp << " " << thriftControlPort << " " << commands;
#endif

                logInfo("PES control: " + cmd.str());

                int rc = std::system(cmd.str().c_str());
                if (rc != 0) {
                    logError("PES control command failed (rc=" + std::to_string(rc) + "): " + commands);
                    return false;
                }
                return true;
            };

            auto waitForQueuedJob = [&]() -> bool {
                if (thriftControllerPath.empty()) {
                    return true; // no-op unless thrift is enabled
                }

                int timeoutMs = 8000;
                int pollMs = 250;
                if (const char* v = std::getenv("THRIFT_WAIT_JOB_TIMEOUT_MS")) {
                    int parsed = std::atoi(v);
                    if (parsed > 0) timeoutMs = parsed;
                }
                if (const char* v = std::getenv("THRIFT_WAIT_JOB_POLL_MS")) {
                    int parsed = std::atoi(v);
                    if (parsed > 0) pollMs = parsed;
                }

                const int maxTries = std::max(1, timeoutMs / pollMs);

                for (int i = 0; i < maxTries; ++i) {
#ifdef _WIN32
                    // Ask controller for machine-parseable status JSON, then check queueLen
                    const std::string statusFile = "pes_status_tmp.json";

                    std::ostringstream runCmd;
                    runCmd << "set \"PDL_THRIFT_ROOT=" << pdlThriftRoot << "\" && "
                           << "\"" << pythonExe << "\" "
                           << "\"" << thriftControllerPath << "\" "
                           << args.pesIp << " " << thriftControlPort
                           << " statusjson > \"" << statusFile << "\" 2>&1";

                    int runRc = std::system(runCmd.str().c_str());
                    if (runRc == 0) {
                        std::ifstream in(statusFile);
                        std::string line, all;
                        while (std::getline(in, line)) {
                            all += line;
                            all += "\n";
                        }

                        // queueLen > 0 => queued
                        if (all.find("\"queueLen\": 0") == std::string::npos &&
                            all.find("\"queueLen\":0") == std::string::npos) {
                            logInfo("PES job queue detected (non-empty)");
                            return true;
                        }
                    }
#else
                    std::ostringstream cmd;
                    cmd << "PDL_THRIFT_ROOT=\"" << pdlThriftRoot << "\" "
                        << "\"" << pythonExe << "\" "
                        << "\"" << thriftControllerPath << "\" "
                        << args.pesIp << " " << thriftControlPort
                        << " statusjson 2>&1 | grep -Fq '\"queueLen\": 0'";

                    int grepRc = std::system(cmd.str().c_str());
                    if (grepRc != 0) {
                        logInfo("PES job queue detected (non-empty)");
                        return true;
                    }
#endif
                    std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
                }

                logError("Timed out waiting for PES job queue to become non-empty");
                return false;
            };

            if (args.legacyJsl) {
                // Legacy path: sequential single-color jobs
                for (size_t i = 0; i < planes.size(); ++i) {
                    JslibHdl handle = nullptr;

                    if (!jsl.openJob(jobConfig, planes[i].color, 1, handle)) {
                        logError("Failed to open job for " +
                                 std::string(colorName(planes[i].color)));
                        return 1;
                    }

                    if (!jsl.addPage(handle, planes[i], true)) {
                        logError("Failed to add page for " +
                                 std::string(colorName(planes[i].color)));
                        (void)jsl.abortJob(handle);
                        (void)jsl.closeJob(handle);
                        return 1;
                    }

                    if (!waitForQueuedJob()) {
                        (void)jsl.abortJob(handle);
                        (void)jsl.closeJob(handle);
                        return 1;
                    }

                    if (!runThriftCommands("prepare=1=20 start")) {
                        (void)jsl.abortJob(handle);
                        (void)jsl.closeJob(handle);
                        return 1;
                    }

                    if (!jsl.closeJob(handle)) {
                        logError("Failed to close job for " +
                                 std::string(colorName(planes[i].color)));
                        return 1;
                    }

                    // finish may return non-zero if session already ended; best-effort cleanup
                    runThriftCommands("finish");
                }
                submitOk = true;
            } else {
                // Default: single multi-color job
                JslibHdl handle = nullptr;

                if (!jsl.openJobMultiColor(jobConfig, planeColors, handle)) {
                    logError("Failed to open multi-color JSL job");
                    return 1;
                }

                if (!jsl.addPageMultiColor(handle, planes, true)) {
                    logError("Failed to add page to multi-color job");
                    (void)jsl.abortJob(handle);
                    (void)jsl.closeJob(handle);
                    return 1;
                }

                if (!waitForQueuedJob()) {
                    (void)jsl.abortJob(handle);
                    (void)jsl.closeJob(handle);
                    return 1;
                }

                // Snapshot queue/state before transition.
                (void)runThriftCommands("statusjson");

                if (!runThriftCommands("prepare=1=20 start")) {
                    (void)jsl.abortJob(handle);
                    (void)jsl.closeJob(handle);
                    return 1;
                }

                // Keep session alive briefly so PES can consume queued data before close/finish.
                int postStartHoldMs = 8000;
                if (const char* v = std::getenv("JSL_POST_START_HOLD_MS")) {
                    int parsed = std::atoi(v);
                    if (parsed >= 0) postStartHoldMs = parsed;
                }
                if (postStartHoldMs > 0) {
                    logInfo("Holding after start for " + std::to_string(postStartHoldMs) + "ms");
                    std::this_thread::sleep_for(std::chrono::milliseconds(postStartHoldMs));
                }

                // Snapshot queue/state after transition.
                (void)runThriftCommands("statusjson");

                if (!jsl.closeJob(handle)) {
                    logError("Failed to close JSL job");
                    return 1;
                }

                // Optional immediate finish (default OFF for debug to avoid draining too early).
                bool doImmediateFinish = false;
                if (const char* v = std::getenv("JSL_IMMEDIATE_FINISH")) {
                    std::string s = v;
                    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                    doImmediateFinish = !(s.empty() || s == "0" || s == "false" || s == "no" || s == "off");
                }
                if (doImmediateFinish) {
                    runThriftCommands("finish");
                } else {
                    logInfo("Skipping immediate finish (set JSL_IMMEDIATE_FINISH=1 to enable)");
                }
                submitOk = true;
            }
            // --- END ORIGINAL CODE ---
        } else {
            logInfo("Using PES orchestrator (new path)");
            
            PesOrchestrator orch(thriftControllerPath,
                                args.pesIp, thriftControlPort);
            
            submitOk = orch.runPrintSession(jsl, jobConfig, planeColors, planes, args.legacyJsl);
            
            if (!submitOk) {
                logError("PES orchestrator print session failed");
                return 1;
            }
        }

        // Free packed plane data
        planes.clear();
        planes.shrink_to_fit();

        if (!submitOk) {
            logError("Print job submission failed");
            return 1;
        }

        logInfo("JSL submission complete for job " + jobId);

        // Step 4: Verify print execution via Gymea log
        logInfo("Step 4: Verifying print execution...");

        PrintVerificationResult vr = verifyPrintExecution(
            jobId, args.gymeaLogPath, args.verifyTimeoutSec);

        if (vr.passed()) {
            logInfo(vr.summary);
            logInfo("Done!");
            return 0;
        } else {
            std::string summaryLower = vr.summary;
            std::transform(summaryLower.begin(), summaryLower.end(), summaryLower.begin(), ::tolower);
            bool verificationUnavailable = (summaryLower.find("verification skipped") != std::string::npos) ||
                                           (summaryLower.find("gymea log not found") != std::string::npos);
            if (verificationUnavailable) {
                logInfo("Print submission completed; verification unavailable in this environment.");
                logInfo(vr.summary);
                logInfo("Done (submission path).");
                return 0;
            }

            logError("Print verification FAILED for job " + jobId);
            logError(vr.summary);
            logError("Job was submitted but not executed. "
                     "Check printer status and Gymea log.");
            return 1;
        }

    } catch (const std::exception& e) {
        logError(std::string("Exception: ") + e.what());
        return 1;
    }
}
















