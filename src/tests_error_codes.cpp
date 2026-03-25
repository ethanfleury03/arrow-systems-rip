#include "error_codes.h"
#include <string>

int main() {
    using namespace memjet;
    if (std::string(toErrorCodeString(ErrorCode::Ok)) != "OK") return 1;
    if (std::string(toErrorCodeString(ErrorCode::InvalidArgs)).find("RIP_") != 0) return 2;
    if (toExitCode(ErrorCode::InvalidArgs) == 0) return 3;
    if (toExitCode(ErrorCode::RuntimeException) != 99) return 4;
    return 0;
}
