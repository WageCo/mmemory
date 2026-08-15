// ============================================================================
// log.cpp - 日志系统实现 (spdlog 封装)
// ----------------------------------------------------------------------------
// 非热路径 (logger 只在首次调用时初始化), 与分配器主逻辑分离。
// 配置方式 (环境变量):
//   MMEMORY_LOG_LEVEL=trace|debug|info|warn|error|critical|off
//   MMEMORY_LOG_FILE=<path>   指定则改为追加模式写文件 (否则 stderr)
// ============================================================================
#include <stdlib.h> // getenv

#include "internal.h"

namespace wageco
{
std::shared_ptr<spdlog::logger> get_logger()
{
    static std::shared_ptr<spdlog::logger> logger = []() {
        std::shared_ptr<spdlog::logger> l;
        const char *file_path = std::getenv("MMEMORY_LOG_FILE");
        if (file_path && *file_path)
        {
            // 文件输出 (追加模式)
            l = spdlog::basic_logger_mt("mmemory", file_path, true);
        }
        else
        {
            // stderr 输出
            l = spdlog::stderr_logger_mt("mmemory");
        }
#ifdef DEBUG
        l->set_level(spdlog::level::debug); // 调试构建默认输出 debug 级别
#else
        l->set_level(spdlog::level::info);  // 发布构建默认 info 及以上
#endif
        const char *level_str = std::getenv("MMEMORY_LOG_LEVEL");
        if (level_str && *level_str)
        {
            l->set_level(spdlog::level::from_str(level_str));
        }
        l->flush_on(spdlog::level::info); // 教学: info 及以上立即落盘
        return l;
    }();
    return logger;
}
} // namespace wageco
