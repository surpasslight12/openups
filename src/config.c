#include "config.h"
#include "common.h"
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* 解析布尔值参数 (仅支持 true/false, 可选参数)
 * 参数: arg - 字符串值 (true/false, 不区分大小写)
 * 设计: 简化用户接口，废除旧有的 yes/no、1/0、on/off 等格式
 * 返回: true / false (当参数为空时默认返回 true)
 */
static bool parse_bool_arg(const char* arg) {
    if (arg == nullptr) {
        return true;
    }
    if (strcasecmp(arg, "true") == 0) {
        return true;
    }
    if (strcasecmp(arg, "false") == 0) {
        return false;
    }
    /* 默认为 true */
    return true;
}

void config_init_default(config_t* restrict config) {
    if (config == nullptr) {
        return;
    }
    
    /* 网络配置 */
    snprintf(config->target, sizeof(config->target), "1.1.1.1");  /* 默认公网 DNS */
    config->interval_sec = 10;      /* 检测间隔: 10 秒 */
    config->fail_threshold = 5;     /* 连续失败达到此阈值时触发关机 */
    config->timeout_ms = 2000;      /* 每次 ping 超时: 2 秒 */
    config->packet_size = 56;       /* IPv4/IPv6 标准 ping 负载体大小 */
    config->max_retries = 2;        /* 失败后的重试次数 */
    config->use_ipv6 = false;       /* 默认 IPv4 */
    
    /* 关机配置 */
    config->shutdown_mode = SHUTDOWN_MODE_IMMEDIATE;  /* 默认: 立即关机 */
    config->delay_minutes = 1;      /* 延迟关机: 1 分钟 */
    config->dry_run = true;         /* 干运行: 不实际执行关机 */
    config->shutdown_cmd[0] = '\0';     /* 自定义关机命令 (空为默认) */
    config->custom_script[0] = '\0';    /* 自定义脚本路径 */
    
    /* 行为配置 */
    config->enable_timestamp = true;    /* 是否在日志中输出时间戳 (systemd 场景下建议禁用) */
    config->log_level = LOG_LEVEL_INFO; /* 日志级别: INFO 等级 */
    
    /* systemd 配置 (仅当由 systemd 管理程序时需要) */
    config->enable_systemd = true;      /* 是否启用 systemd 集成 */
    config->enable_watchdog = true;     /* 是否发送 watchdog 心跳 */
}

void config_load_from_env(config_t* restrict config) {
    if (config == nullptr) {
        return;
    }
    
    const char* value;
    
    /* 网络配置 */
    value = getenv("OPENUPS_TARGET");
    if (value) {
        snprintf(config->target, sizeof(config->target), "%s", value);
    }
    
    config->interval_sec = get_env_int("OPENUPS_INTERVAL", config->interval_sec);
    config->fail_threshold = get_env_int("OPENUPS_THRESHOLD", config->fail_threshold);
    config->timeout_ms = get_env_int("OPENUPS_TIMEOUT", config->timeout_ms);
    config->packet_size = get_env_int("OPENUPS_PACKET_SIZE", config->packet_size);
    config->max_retries = get_env_int("OPENUPS_RETRIES", config->max_retries);
    config->use_ipv6 = get_env_bool("OPENUPS_IPV6", config->use_ipv6);
    
    /* 关机配置 */
    value = getenv("OPENUPS_SHUTDOWN_MODE");
    if (value != nullptr) {
        config->shutdown_mode = string_to_shutdown_mode(value);
    }
    
    config->delay_minutes = get_env_int("OPENUPS_DELAY_MINUTES", config->delay_minutes);
    config->dry_run = get_env_bool("OPENUPS_DRY_RUN", config->dry_run);
    
    value = getenv("OPENUPS_SHUTDOWN_CMD");
    if (value != nullptr) {
        snprintf(config->shutdown_cmd, sizeof(config->shutdown_cmd), "%s", value);
    }
    
    value = getenv("OPENUPS_CUSTOM_SCRIPT");
    if (value != nullptr) {
        snprintf(config->custom_script, sizeof(config->custom_script), "%s", value);
    }
    
    /* 行为配置 */
    value = getenv("OPENUPS_LOG_LEVEL");
    if (value != nullptr) {
        config->log_level = string_to_log_level(value);
    }
    
    config->enable_systemd = get_env_bool("OPENUPS_SYSTEMD", config->enable_systemd);
    config->enable_watchdog = get_env_bool("OPENUPS_WATCHDOG", config->enable_watchdog);
    config->enable_timestamp = get_env_bool("OPENUPS_TIMESTAMP", config->enable_timestamp);
}

bool config_load_from_cmdline(config_t* restrict config, int argc, char** restrict argv) {
    if (config == nullptr || argv == nullptr) {
        return false;
    }
    
    static struct option long_options[] = {
        /* 网络参数 */
        {"target",          required_argument, 0, 't'},
        {"interval",        required_argument, 0, 'i'},
        {"threshold",       required_argument, 0, 'n'},
        {"timeout",         required_argument, 0, 'w'},
        {"packet-size",     required_argument, 0, 's'},
        {"retries",         required_argument, 0, 'r'},
        {"ipv6",            optional_argument, 0, '6'},
        
        /* 关机参数 */
        {"shutdown-mode",   required_argument, 0, 'S'},
        {"delay",           required_argument, 0, 'D'},
        {"shutdown-cmd",    required_argument, 0, 'C'},
        {"script",          required_argument, 0, 'P'},
        {"custom-script",   required_argument, 0, 'P'},
        {"dry-run",         optional_argument, 0, 'd'},
        
        /* 日志参数 */
        {"log-level",       required_argument, 0, 'L'},
        {"timestamp",       optional_argument, 0, 'T'},
        
        /* 系统集成 */
        {"systemd",         optional_argument, 0, 'M'},
        {"watchdog",        optional_argument, 0, 'W'},
        
        /* 其他 */
        {"version",         no_argument,       0, 'v'},
        {"help",            no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int c;
    int option_index = 0;
    
    while ((c = getopt_long(argc, argv, "t:i:n:w:s:r:6::S:D:C:P:d::L:T::M::W::vh", 
                           long_options, &option_index)) != -1) {
        switch (c) {
            case 't':
                snprintf(config->target, sizeof(config->target), "%s", optarg);
                break;
            case 'i':
                config->interval_sec = atoi(optarg);
                break;
            case 'n':
                config->fail_threshold = atoi(optarg);
                break;
            case 'w':
                config->timeout_ms = atoi(optarg);
                break;
            case 's':
                config->packet_size = atoi(optarg);
                break;
            case 'r':
                config->max_retries = atoi(optarg);
                break;
            case 'S':
                config->shutdown_mode = string_to_shutdown_mode(optarg);
                break;
            case 'D':
                config->delay_minutes = atoi(optarg);
                break;
            case 'C':
                snprintf(config->shutdown_cmd, sizeof(config->shutdown_cmd), "%s", optarg);
                break;
            case 'P':
                snprintf(config->custom_script, sizeof(config->custom_script), "%s", optarg);
                break;
            case 'L':
                config->log_level = string_to_log_level(optarg);
                break;
            case '6':
                if (optarg) {
                    config->use_ipv6 = parse_bool_arg(optarg);
                } else {
                    config->use_ipv6 = true;
                }
                break;
            case 'd':
                if (optarg) {
                    config->dry_run = parse_bool_arg(optarg);
                } else {
                    config->dry_run = true;
                }
                break;
            case 'T':
                if (optarg) {
                    config->enable_timestamp = parse_bool_arg(optarg);
                } else {
                    config->enable_timestamp = false;
                }
                break;
            case 'M':
                if (optarg) {
                    config->enable_systemd = parse_bool_arg(optarg);
                } else {
                    config->enable_systemd = false;
                }
                break;
            case 'W':
                if (optarg) {
                    config->enable_watchdog = parse_bool_arg(optarg);
                } else {
                    config->enable_watchdog = false;
                }
                break;
            /* --version */
            case 'v':
                config_print_version();
                exit(0);
            case 'h':
                config_print_usage();
                exit(0);
            case '?':
                return false;
            default:
                return false;
        }
    }
    
    return true;
}

bool config_validate(const config_t* restrict config, char* restrict error_msg, size_t error_size) {
    if (config == nullptr || error_msg == nullptr || error_size == 0) {
        return false;
    }
    
    /* 验证目标主机名 (必须不能为空) */
    if (config->target[0] == '\0') {
        snprintf(error_msg, error_size, "Target host cannot be empty");
        return false;
    }
    
    if (config->interval_sec <= 0) {
        snprintf(error_msg, error_size, "Interval must be positive");
        return false;
    }
    
    if (config->fail_threshold <= 0) {
        snprintf(error_msg, error_size, "Failure threshold must be positive");
        return false;
    }
    
    if (config->timeout_ms <= 0) {
        snprintf(error_msg, error_size, "Timeout must be positive");
        return false;
    }
    
    if (config->packet_size < 0 || config->packet_size > 65507) {
        snprintf(error_msg, error_size, "Packet size must be between 0 and 65507");
        return false;
    }
    
    if (config->max_retries < 0) {
        snprintf(error_msg, error_size, "Max retries cannot be negative");
        return false;
    }
    
    if (config->shutdown_mode == SHUTDOWN_MODE_CUSTOM && 
        strlen(config->custom_script) == 0) {
        snprintf(error_msg, error_size, "Custom script path required for custom mode");
        return false;
    }
    
    /* 验证自定义脚本路径安全性 (防止路径遍历、注入攻击等) */
    if (strlen(config->custom_script) > 0 && !is_safe_path(config->custom_script)) {
        snprintf(error_msg, error_size, "Custom script path contains unsafe characters");
        return false;
    }
    
    /* 验证自定义关机命令安全性 (仅允许已知的安全命令) */
    if (strlen(config->shutdown_cmd) > 0) {
        /* 允许基本的 shutdown 命令格式 */
        if (strncmp(config->shutdown_cmd, "shutdown", 8) != 0 &&
            strncmp(config->shutdown_cmd, "/sbin/shutdown", 14) != 0 &&
            strncmp(config->shutdown_cmd, "systemctl", 9) != 0) {
            /* 对其他命令进行安全检查 */
            if (!is_safe_path(config->shutdown_cmd)) {
                snprintf(error_msg, error_size, "Shutdown command contains unsafe characters");
                return false;
            }
        }
    }
    
    return true;
}

/* 打印当前配置（用于序列化和调试） */
void config_print(const config_t* restrict config) {
    if (config == nullptr) {
        return;
    }
    
    printf("Configuration:\n");
    printf("  Target: %s\n", config->target);
    printf("  Interval: %d seconds\n", config->interval_sec);
    printf("  Threshold: %d\n", config->fail_threshold);
    printf("  Timeout: %d ms\n", config->timeout_ms);
    printf("  Packet Size: %d bytes\n", config->packet_size);
    printf("  Max Retries: %d\n", config->max_retries);
    printf("  IPv6: %s\n", config->use_ipv6 ? "true" : "false");
    printf("  Shutdown Mode: %s\n", shutdown_mode_to_string(config->shutdown_mode));
    printf("  Dry Run: %s\n", config->dry_run ? "true" : "false");
    printf("  Log Level: %s\n", log_level_to_string(config->log_level));
    printf("  Timestamp: %s\n", config->enable_timestamp ? "true" : "false");
    printf("  Systemd: %s\n", config->enable_systemd ? "true" : "false");
    printf("  Watchdog: %s\n", config->enable_watchdog ? "true" : "false");
}

/* 打印帮助信息和可用参数 */
void config_print_usage(void) {
    printf("Usage: %s [options]\n\n", PROGRAM_NAME);
    printf("Network Options:\n");
    printf("  -t, --target <host>         Target host/IP to monitor (default: 1.1.1.1)\n");
    printf("  -i, --interval <sec>        Ping interval in seconds (default: 10)\n");
    printf("  -n, --threshold <num>       Consecutive failures threshold (default: 5)\n");
    printf("  -w, --timeout <ms>          Ping timeout in milliseconds (default: 2000)\n");
    printf("  -s, --packet-size <bytes>   ICMP packet payload size (default: 56)\n");
    printf("  -r, --retries <num>         Retry attempts per ping (default: 2)\n");
    printf("  -6, --ipv6[=true|false]     Enable/disable IPv6 mode (default: false)\n\n");
    
    printf("Shutdown Options:\n");
    printf("  -S, --shutdown-mode <mode>  Shutdown mode: immediate|delayed|log-only|custom\n");
    printf("                              (default: immediate)\n");
    printf("  -D, --delay <min>           Shutdown delay in minutes for delayed mode (default: 1)\n");
    printf("  -C, --shutdown-cmd <cmd>    Custom shutdown command\n");
    printf("  -P, --script <path>         Custom shutdown script path\n");
    printf("  -d[ARG], --dry-run[=ARG]    Dry-run mode, no actual shutdown (default: true)\n");
    printf("                              ARG: true|false\n");
    printf("                              Note: Use -dfalse or --dry-run=false (no space)\n\n");
    
    printf("Logging Options:\n");
    printf("  -L, --log-level <level>     Log level: silent|error|warn|info|debug\n");
    printf("                              (default: info)\n");
    printf("  -T[ARG], --timestamp[=ARG]  Enable/disable log timestamps (default: true)\n");
    printf("                              ARG format: true|false\n\n");
    
    printf("System Integration:\n");
    printf("  -M[ARG], --systemd[=ARG]    Enable/disable systemd integration (default: true)\n");
    printf("  -W[ARG], --watchdog[=ARG]   Enable/disable systemd watchdog (default: true)\n");
    printf("                              ARG format: true|false\n\n");
    
    printf("General Options:\n");
    printf("  -v, --version               Show version information\n");
    printf("  -h, --help                  Show this help message\n\n");
    
    printf("Environment Variables (lower priority than CLI args):\n");
    printf("  Network:      OPENUPS_TARGET, OPENUPS_INTERVAL, OPENUPS_THRESHOLD,\n");
    printf("                OPENUPS_TIMEOUT, OPENUPS_PACKET_SIZE, OPENUPS_RETRIES,\n");
    printf("                OPENUPS_IPV6\n");
    printf("  Shutdown:     OPENUPS_SHUTDOWN_MODE, OPENUPS_DELAY_MINUTES,\n");
    printf("                OPENUPS_SHUTDOWN_CMD, OPENUPS_CUSTOM_SCRIPT,\n");
    printf("                OPENUPS_DRY_RUN\n");
    printf("  Logging:      OPENUPS_LOG_LEVEL, OPENUPS_TIMESTAMP\n");
    printf("  Integration:  OPENUPS_SYSTEMD, OPENUPS_WATCHDOG\n\n");
    
    printf("Examples:\n");
    printf("  # Basic monitoring with dry-run\n");
    printf("  %s -t 1.1.1.1 -i 10 -n 5\n\n", PROGRAM_NAME);
    printf("  # Production mode (actual shutdown)\n");
    printf("  %s -t 192.168.1.1 -i 5 -n 3 --dry-run=false\n\n", PROGRAM_NAME);
    printf("  # IPv6 with custom script\n");
    printf("  %s -6 -t 2606:4700:4700::1111 -S custom -P /root/shutdown.sh --dry-run=false\n\n", PROGRAM_NAME);
    printf("  # Debug mode without timestamp (for systemd)\n");
    printf("  %s -t 8.8.8.8 -L debug --timestamp=false\n\n", PROGRAM_NAME);
    printf("  # Short options (values must connect directly, no space)\n");
    printf("  %s -t 8.8.8.8 -i5 -n3 -dfalse -Tfalse -Ldebug\n\n", PROGRAM_NAME);
}

/* 打印程序版本信息 */
void config_print_version(void) {
    printf("%s version %s\n", PROGRAM_NAME, VERSION);
    printf("OpenUPS network monitor\n");
}

const char* shutdown_mode_to_string(shutdown_mode_t mode) {
    switch (mode) {
        case SHUTDOWN_MODE_IMMEDIATE: return "immediate";
        case SHUTDOWN_MODE_DELAYED:   return "delayed";
        case SHUTDOWN_MODE_LOG_ONLY:  return "log-only";
        case SHUTDOWN_MODE_CUSTOM:    return "custom";
        default:                      return "unknown";
    }
}

/* 将字符串转换为 shutdown_mode_t 枚举（不区分大小写） */
shutdown_mode_t string_to_shutdown_mode(const char* restrict str) {
    if (str == nullptr) {
        return SHUTDOWN_MODE_IMMEDIATE;
    }
    
    if (strcasecmp(str, "immediate") == 0) return SHUTDOWN_MODE_IMMEDIATE;
    if (strcasecmp(str, "delayed") == 0)   return SHUTDOWN_MODE_DELAYED;
    if (strcasecmp(str, "log-only") == 0)  return SHUTDOWN_MODE_LOG_ONLY;
    if (strcasecmp(str, "custom") == 0)    return SHUTDOWN_MODE_CUSTOM;
    return SHUTDOWN_MODE_IMMEDIATE;
}
