#include "config.h"
#include "common.h"
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void config_init_default(config_t* restrict config) {
    if (config == nullptr) {
        return;
    }
    
    /* 网络配置 */
    snprintf(config->target, sizeof(config->target), "1.1.1.1");
    config->interval_sec = 10;
    config->fail_threshold = 5;
    config->timeout_ms = 2000;
    config->packet_size = 56;
    config->max_retries = 2;
    config->use_ipv6 = false;
    
    /* 关机配置 */
    config->shutdown_mode = SHUTDOWN_MODE_IMMEDIATE;
    config->delay_minutes = 1;
    config->dry_run = true;
    config->shutdown_cmd[0] = '\0';
    config->custom_script[0] = '\0';
    
    /* 行为配置 */
    config->enable_timestamp = true;
    config->log_level = LOG_LEVEL_INFO;
    config->use_syslog = false;
    
    /* systemd 配置 */
    config->enable_systemd = true;
    config->enable_watchdog = true;
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
    
    config->use_syslog = get_env_bool("OPENUPS_SYSLOG", config->use_syslog);
    config->enable_systemd = get_env_bool("OPENUPS_SYSTEMD", config->enable_systemd);
    
    /* NO_TIMESTAMP 的处理：如果设置为 true，则 enable_timestamp 设为 false */
    if (get_env_bool("OPENUPS_NO_TIMESTAMP", false)) {
        config->enable_timestamp = false;
    }
}

bool config_load_from_cmdline(config_t* restrict config, int argc, char** restrict argv) {
    if (config == nullptr || argv == nullptr) {
        return false;
    }
    
    static struct option long_options[] = {
        {"target",          required_argument, 0, 't'},
        {"interval",        required_argument, 0, 'i'},
        {"threshold",       required_argument, 0, 'n'},
        {"timeout",         required_argument, 0, 'w'},
        {"packet-size",     required_argument, 0, 's'},
        {"retries",         required_argument, 0, 'r'},
        {"shutdown-mode",   required_argument, 0, 'S'},
        {"delay-minutes",   required_argument, 0, 'D'},
        {"shutdown-cmd",    required_argument, 0, 'C'},
        {"custom-script",   required_argument, 0, 'P'},
        {"log-level",       required_argument, 0, 'L'},
        {"ipv6",            no_argument,       0, '6'},
        {"dry-run",         no_argument,       0, 'd'},
        {"no-dry-run",      no_argument,       0, 'N'},
        {"syslog",          no_argument,       0, 'Y'},
        {"no-timestamp",    no_argument,       0, 'T'},
        {"no-systemd",      no_argument,       0, 'M'},
        {"no-watchdog",     no_argument,       0, 'W'},
        {"version",         no_argument,       0, 'Z'},  /* version 改用 Z */
        {"help",            no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int c;
    int option_index = 0;
    
    while ((c = getopt_long(argc, argv, "t:i:n:w:s:r:6dh", 
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
                config->use_ipv6 = true;
                break;
            case 'd':
                config->dry_run = true;
                break;
            case 'N':
                config->dry_run = false;
                break;
            case 'Y':
                config->use_syslog = true;
                break;
            case 'T':
                config->enable_timestamp = false;
                break;
            case 'M':
                config->enable_systemd = false;
                break;
            case 'W':
                config->enable_watchdog = false;
                break;
            case 'Z':  /* --version */
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
    
    /* 验证自定义脚本路径安全性 */
    if (strlen(config->custom_script) > 0 && !is_safe_path(config->custom_script)) {
        snprintf(error_msg, error_size, "Custom script path contains unsafe characters");
        return false;
    }
    
    /* 验证自定义关机命令安全性 */
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
    printf("  Syslog: %s\n", config->use_syslog ? "true" : "false");
    printf("  Systemd: %s\n", config->enable_systemd ? "true" : "false");
}

void config_print_usage(void) {
    printf("Usage: %s [options]\n\n", PROGRAM_NAME);
    printf("Options:\n");
    printf("  -t, --target <host>       Host/IP to ping (default: 1.1.1.1)\n");
    printf("  -i, --interval <sec>      Interval between pings (default: 10)\n");
    printf("  -n, --threshold <num>     Consecutive failures to trigger (default: 5)\n");
    printf("  -w, --timeout <ms>        Ping timeout in ms (default: 2000)\n");
    printf("  -s, --packet-size <bytes> ICMP payload size (default: 56)\n");
    printf("  -r, --retries <num>       Retries per ping (default: 2)\n");
    printf("      --shutdown-mode <mode> immediate|delayed|log-only|custom (default: immediate)\n");
    printf("      --delay-minutes <num> Minutes to delay shutdown (default: 1)\n");
    printf("      --shutdown-cmd <cmd>  Override shutdown command\n");
    printf("      --custom-script <path> Path to custom shutdown script\n");
    printf("      --dry-run             Do not actually shutdown (default)\n");
    printf("      --no-dry-run          Actually run shutdown command\n");
    printf("  -6, --ipv6                Use IPv6 ping\n");
    printf("      --log-level <level>   silent|error|warn|info|debug (default: info)\n");
    printf("      --syslog              Enable syslog logging\n");
    printf("      --no-timestamp        Disable timestamp in logs\n");
    printf("      --no-systemd          Disable systemd integration\n");
    printf("      --no-watchdog         Disable systemd watchdog\n");
    printf("      --version             Show version information\n");
    printf("  -h, --help                Show this help message\n\n");
    printf("Environment Variables:\n");
    printf("  OPENUPS_TARGET            Override target host\n");
    printf("  OPENUPS_INTERVAL          Override interval\n");
    printf("  OPENUPS_THRESHOLD         Override threshold\n");
    printf("  OPENUPS_TIMEOUT           Override timeout\n");
    printf("  OPENUPS_DRY_RUN           Set dry-run mode (true/false)\n");
    printf("  OPENUPS_SHUTDOWN_MODE     Override shutdown mode\n");
    printf("  OPENUPS_LOG_LEVEL         Override log level\n");
    printf("  OPENUPS_SYSLOG            Enable syslog (true/false)\n");
    printf("  OPENUPS_SYSTEMD           Enable systemd integration (true/false)\n\n");
}

void config_print_version(void) {
    printf("%s version %s\n", PROGRAM_NAME, VERSION);
    printf("C implementation of OpenUPS network monitor\n");
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
