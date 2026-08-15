// ============================================================================
// log.cpp - 日志系统实现 (spdlog 封装)
// ----------------------------------------------------------------------------
// 非热路径 (logger 只在首次调用时初始化), 与分配器主逻辑分离。
// 配置方式 (环境变量):
//   MMEMORY_LOG_LEVEL=trace|debug|info|warn|error|critical|off
//   MMEMORY_LOG_FILE=<path>   指定则改为追加模式写文件 (否则 stderr)
// ============================================================================
#include <stdlib.h>  // getenv

#include "internal.h"

namespace wageco
{
std::shared_ptr<spdlog::logger> get_logger()
{
    // 进程生命周期: 底层 logger 对象用"空删除器"的别名持有, 永不析构,
    // 因此程序退出时 (全局对象析构阶段) 调用 get_logger() 依然安全,
    // 不依赖静态析构顺序 (泄漏检测/统计就打印在这个阶段)。
    static std::shared_ptr<spdlog::logger> logger = []()
    {
        std::shared_ptr<spdlog::logger> l;
        const char* file_path = std::getenv("MMEMORY_LOG_FILE");
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
        l->set_level(spdlog::level::debug);  // 调试构建默认输出 debug 级别
#else
        l->set_level(spdlog::level::info);  // 发布构建默认 info 及以上
#endif
        const char* level_str = std::getenv("MMEMORY_LOG_LEVEL");
        if (level_str && *level_str)
        {
            l->set_level(spdlog::level::from_str(level_str));
        }
        l->flush_on(spdlog::level::info);  // 教学: info 及以上立即落盘
        // 空删除器: logger 对象由 spdlog registry 持有, 别名不负责释放
        return std::shared_ptr<spdlog::logger>(l.get(), [](spdlog::logger*) {});
    }();
    return logger;
}
}  // namespace wageco
