#include "error_codes.h"

namespace memjet {

const char* toErrorCodeString(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return "OK";
        case ErrorCode::InvalidArgs: return "RIP_INVALID_ARGS";
        case ErrorCode::InputFileMissing: return "RIP_INPUT_FILE_MISSING";
        case ErrorCode::ConfigInvalid: return "RIP_CONFIG_INVALID";
        case ErrorCode::RasterizerInitFailed: return "RIP_RASTERIZER_INIT_FAILED";
        case ErrorCode::RasterizationFailed: return "RIP_RASTERIZATION_FAILED";
        case ErrorCode::PlaneConversionFailed: return "RIP_PLANE_CONVERSION_FAILED";
        case ErrorCode::JslInitFailed: return "RIP_JSL_INIT_FAILED";
        case ErrorCode::JslSubmissionFailed: return "RIP_JSL_SUBMISSION_FAILED";
        case ErrorCode::VerificationFailed: return "RIP_VERIFICATION_FAILED";
        case ErrorCode::RuntimeException: return "RIP_RUNTIME_EXCEPTION";
        default: return "RIP_UNKNOWN_ERROR";
    }
}

int toExitCode(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return 0;
        case ErrorCode::InvalidArgs: return 2;
        case ErrorCode::InputFileMissing: return 3;
        case ErrorCode::ConfigInvalid: return 4;
        case ErrorCode::RasterizerInitFailed: return 10;
        case ErrorCode::RasterizationFailed: return 11;
        case ErrorCode::PlaneConversionFailed: return 12;
        case ErrorCode::JslInitFailed: return 20;
        case ErrorCode::JslSubmissionFailed: return 21;
        case ErrorCode::VerificationFailed: return 30;
        case ErrorCode::RuntimeException: return 99;
        default: return 1;
    }
}

} // namespace memjet
