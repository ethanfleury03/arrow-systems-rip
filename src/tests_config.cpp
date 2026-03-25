#include "config.h"
#include <iostream>

int main() {
    memjet::utils::CommandLineArgs args;
    args.cmyk = true;

    memjet::RuntimeConfig cfg;
    std::string err;
    std::vector<std::string> warns;

    if (!memjet::loadRuntimeConfig(args, cfg, err, warns)) {
        std::cerr << "loadRuntimeConfig failed: " << err << std::endl;
        return 1;
    }

    if (!cfg.useTrueCmyk) return 2;
    if (cfg.globalInkLimit < 0.0 || cfg.globalInkLimit > 1.0) return 3;
    if (cfg.thresholdBias < -64 || cfg.thresholdBias > 64) return 4;
    return 0;
}
