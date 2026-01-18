#include "common.h"
#include "config.h"
#include "logger.h"
#include "monitor.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#error "This program currently targets Linux."
#endif

/* OpenUPS 程序主入口
 * 初始化流程：
 *   1. 默认配置
 *   2. 环境变量（覆盖默认值）
 *   3. 命令行参数（最高优先级）
 *   4. 验证配置的有效性
 *   5. 初始化日志器
 *   6. 初始化监控器 (socket, systemd 集成)
 *   7. 运行主监控循环
 *   8. 清理资源并退出
 */
int main(int argc, char** argv) {
    /* 配置优先级：CLI > ENV > 默认 */
    config_t config;
    config_init_default(&config);
    
    /* 从环境变量加载配置 */
    config_load_from_env(&config);
    
    /* 从命令行加载配置 */
    if (!config_load_from_cmdline(&config, argc, argv)) {
        fprintf(stderr, "Failed to parse command line arguments\n");
        config_print_usage();  /* 打印使用方法 */
        return 1;
    }
    
    /* 验证配置 */
    char error_msg[256];
    if (!config_validate(&config, error_msg, sizeof(error_msg))) {
        fprintf(stderr, "Configuration error: %s\n", error_msg);
        return 2;  /* 退出码: 配置错误 */
    }
    
    /* 初始化日志器 */
    logger_t logger;
    logger_init(&logger, config.log_level,
                config.enable_timestamp);
    
    /* 打印配置（debug 模式） */
    if (config.log_level == LOG_LEVEL_DEBUG) {
        config_print(&config);
    }
    
    /* 初始化监控器 (创建 raw socket, 通过 systemd 集成) */
    monitor_t monitor;
    if (!monitor_init(&monitor, &config, &logger, error_msg, sizeof(error_msg))) {
        fprintf(stderr, "Failed to initialize monitor: %s\n", error_msg);
        logger_destroy(&logger);
        return 3;  /* 退出码: 监控器初始化失败 */
    }
    
    /* 运行监控循环 */
    int result = monitor_run(&monitor);
    
    /* 清理资源 */
    monitor_destroy(&monitor);
    logger_destroy(&logger);
    
    return result;
}
