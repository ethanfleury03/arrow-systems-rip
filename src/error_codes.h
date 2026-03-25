#ifndef ERROR_CODES_H
#define ERROR_CODES_H

#include <string>

namespace memjet {

enum class ErrorCode {
    Ok = 0,
    InvalidArgs,
    InputFileMissing,
    ConfigInvalid,
    RasterizerInitFailed,
    RasterizationFailed,
    PlaneConversionFailed,
    JslInitFailed,
    JslSubmissionFailed,
    VerificationFailed,
    RuntimeException
};

const char* toErrorCodeString(ErrorCode code);
int toExitCode(ErrorCode code);

} // namespace memjet

#endif // ERROR_CODES_H
