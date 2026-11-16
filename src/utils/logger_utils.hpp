#ifndef LOGGER_UTILS_
#define LOGGER_UTILS_

#include <memory>
#include <mutex>
#include <string>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace Logger
{
    inline std::shared_ptr<spdlog::logger> getLogger(const std::string& name)
    {
        static std::mutex mutex; // Static local is thread-safe
        std::lock_guard<std::mutex> lock(mutex);
        auto logger = spdlog::get(name);
        if (logger != nullptr)
            return logger;
        return spdlog::stdout_color_mt(name);
    }
}

#endif // LOGGER_UTILS_
