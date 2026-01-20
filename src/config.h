#ifndef OPENUPS_CONFIG_H
#define OPENUPS_CONFIG_H

#include "logger.h"
#include <stdbool.h>
#include <stddef.h>

/* 关机模式 */
typedef enum {
    SHUTDOWN_MODE_IMMEDIATE,
    SHUTDOWN_MODE_DELAYED,
    SHUTDOWN_MODE_LOG_ONLY
} shutdown_mode_t;

/* 配置结构 */
typedef struct {
    /* 网络配置 */
    char target[256];
    int interval_sec;
    int fail_threshold;
    int timeout_ms;
    int packet_size;
    int max_retries;
    bool use_ipv6;

    /* 关机配置 */
    shutdown_mode_t shutdown_mode;
    int delay_minutes;
    bool dry_run;

    /* 行为配置 */
    bool enable_timestamp;
    log_level_t log_level;

    /* systemd 配置 */
    bool enable_systemd;
    bool enable_watchdog;
} config_t;

/* 初始化默认配置 */
void config_init_default(config_t* restrict config);

/* 从环境变量加载 */
void config_load_from_env(config_t* restrict config);

/* 从命令行加载 */
[[nodiscard]] bool config_load_from_cmdline(config_t* restrict config, int argc,
                                            char** restrict argv);

/* 验证配置 */
[[nodiscard]] bool config_validate(const config_t* restrict config, char* restrict error_msg,
                                   size_t error_size);

/* 打印配置 */
void config_print(const config_t* restrict config);

/* 打印帮助信息 */
void config_print_usage(void);

/* 打印版本信息 */
void config_print_version(void);

/* 枚举转换 */
[[nodiscard]] const char* shutdown_mode_to_string(shutdown_mode_t mode);
[[nodiscard]] shutdown_mode_t string_to_shutdown_mode(const char* restrict str);

/* 解析关机模式（严格：未知值返回 false） */
[[nodiscard]] bool shutdown_mode_parse(const char* restrict str,
                                       shutdown_mode_t* restrict out_mode);

#endif /* OPENUPS_CONFIG_H */
