#include "logger.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

namespace memjet {
namespace {

std::string isoUtcNow() {
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
    ts << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
       << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    return ts.str();
}

std::string jsonEscape(const std::string& s) {
    std::ostringstream out;
    for (char c : s) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << c; break;
        }
    }
    return out.str();
}

void writeJson(const std::string& level,
               const std::string& message,
               const std::string& event,
               const std::map<std::string, std::string>& fields,
               const char* errorCode = nullptr) {
    std::ostringstream json;
    json << "{";
    json << "\"ts\":\"" << isoUtcNow() << "\",";
    json << "\"component\":\"memjet-rip\",";
    json << "\"level\":\"" << level << "\"";
    if (!event.empty()) {
        json << ",\"event\":\"" << jsonEscape(event) << "\"";
    }
    if (!message.empty()) {
        json << ",\"message\":\"" << jsonEscape(message) << "\"";
    }
    if (errorCode && *errorCode) {
        json << ",\"error_code\":\"" << errorCode << "\"";
    }
    for (const auto& kv : fields) {
        json << ",\"" << jsonEscape(kv.first) << "\":\"" << jsonEscape(kv.second) << "\"";
    }
    json << "}";
    std::cout << json.str() << std::endl;
}

} // namespace

void Logger::info(const std::string& message) {
    std::cout << "[INFO] " << message << std::endl;
    writeJson("INFO", message, "", {});
}

void Logger::warn(const std::string& message) {
    std::cout << "[WARN] " << message << std::endl;
    writeJson("WARN", message, "", {});
}

void Logger::error(const std::string& message, ErrorCode code) {
    std::cerr << "[ERROR] " << message << " [" << toErrorCodeString(code) << "]" << std::endl;
    writeJson("ERROR", message, "", {}, toErrorCodeString(code));
}

void Logger::event(const std::string& event,
                   const std::map<std::string, std::string>& fields) {
    writeJson("INFO", "", event, fields);
}

} // namespace memjet
