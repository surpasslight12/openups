#include "common.h"
#include "config.h"
#include "logger.h"
#include "monitor.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#error "This program currently targets Linux."
#endif

int main(int argc, char** argv) {
    /* 初始化默认配置 */
    config_t config;
    config_init_default(&config);
    
    /* 从环境变量加载配置 */
    config_load_from_env(&config);
    
    /* 从命令行加载配置 */
    if (!config_load_from_cmdline(&config, argc, argv)) {
        fprintf(stderr, "Failed to parse command line arguments\n");
        config_print_usage();
        return 1;
    }
    
    /* 验证配置 */
    char error_msg[256];
    if (!config_validate(&config, error_msg, sizeof(error_msg))) {
        fprintf(stderr, "Configuration error: %s\n", error_msg);
        return 2;
    }
    
    /* 初始化日志器 */
    logger_t logger;
    logger_init(&logger, config.log_level,
                config.enable_timestamp);
    
    /* 打印配置（debug 模式） */
    if (config.log_level == LOG_LEVEL_DEBUG) {
        config_print(&config);
    }
    
    /* 初始化监控器 */
    monitor_t monitor;
    if (!monitor_init(&monitor, &config, &logger, error_msg, sizeof(error_msg))) {
        fprintf(stderr, "Failed to initialize monitor: %s\n", error_msg);
        logger_destroy(&logger);
        return 3;
    }
    
    /* 运行监控循环 */
    int result = monitor_run(&monitor);
    
    /* 清理资源 */
    monitor_destroy(&monitor);
    logger_destroy(&logger);
    
    return result;
}
