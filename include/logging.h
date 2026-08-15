// ============================================================================
// logging.h - 日志配置 (spdlog) 与 get_logger 声明
// ----------------------------------------------------------------------------
// 只做两件事: 配置编译期日志级别 (SPDLOG_ACTIVE_LEVEL) 并包含 spdlog 头,
// 声明库内使用的 logger 获取函数 (定义在 src/log.cpp)。
// ============================================================================
#ifndef MMEMORY_LOGGING_H
#define MMEMORY_LOGGING_H

#include <memory>  // std::shared_ptr

// 编译期日志级别: DEBUG 构建保留 debug/trace; Release 构建剥离 debug/trace
// (SPDLOG_ACTIVE_LEVEL 使 SPDLOG_LOGGER_DEBUG/TRACE 宏在编译期展开为空,
//  参数不求值、零开销 —— 保证 benchmark 测的是纯分配器逻辑, 不被日志污染)
#ifdef DEBUG
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#else
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#endif
#include <spdlog/spdlog.h>
// 注意: spdlog 1.17.0 中 stderr_logger_mt 声明在 stdout_sinks.h,
//       basic_logger_mt 声明在 basic_file_sink.h (不存在 stderr_sinks.h)
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace wageco
{
// 日志 logger 获取函数: 定义在 src/log.cpp
std::shared_ptr<spdlog::logger> get_logger();
}  // namespace wageco
#endif  // MMEMORY_LOGGING_H
