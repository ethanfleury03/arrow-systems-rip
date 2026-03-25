#ifndef RIP_CONFIG_H
#define RIP_CONFIG_H

#include <string>
#include <vector>
#include "utils.h"

namespace memjet {

struct RuntimeConfig {
    bool forceFastMono = false;
    bool useTrueCmyk = true;
    bool envTrueCmykSet = false;
    std::string modeReason;

    std::string baselineProfile = "NONE";
    double globalInkLimit = 1.0;
    double cScale = 1.0;
    double mScale = 1.0;
    double yScale = 1.0;
    double kScale = 1.0;
    int thresholdBias = 0;

    bool invertBits = false;
    bool testPattern = false;
    uint32_t targetStripWidth = 0;
    uint32_t stripStart = 0;
    uint32_t stripWidth = 0;

    std::string thriftControllerPath = "src\\thrift_controller_fullcycle.py";
    int thriftControlPort = 13001;
    std::string pythonExe = "C:\\Python27\\python.exe";
    std::string pdlThriftRoot = "vendor\\pdl_py";

    bool useLegacyOrchestration = false;
    int postStartHoldMs = 8000;
    bool immediateFinish = false;
    int thriftWaitJobTimeoutMs = 8000;
    int thriftWaitJobPollMs = 250;
};

bool loadRuntimeConfig(const memjet::utils::CommandLineArgs& args,
                       RuntimeConfig& out,
                       std::string& error,
                       std::vector<std::string>& warnings);

} // namespace memjet

#endif // RIP_CONFIG_H
