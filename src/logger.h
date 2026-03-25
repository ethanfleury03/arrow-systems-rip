#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <map>
#include "error_codes.h"

namespace memjet {

class Logger {
public:
    static void info(const std::string& message);
    static void warn(const std::string& message);
    static void error(const std::string& message, ErrorCode code = ErrorCode::RuntimeException);
    static void event(const std::string& event,
                      const std::map<std::string, std::string>& fields = {});
};

} // namespace memjet

#endif // LOGGER_H
