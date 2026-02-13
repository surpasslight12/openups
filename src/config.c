#include "config.h"
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* 解析布尔值参数 (仅支持 true/false, 可选参数)
 * 参数: arg - 字符串值 (true/false, 不区分大小写)
 * 返回: true 表示解析成功，false 表示非法值
 * 约定: 当参数为空时默认返回 true
 */
static bool parse_bool_arg(const char* arg, bool* out_value)
{
    if (OPENUPS_UNLIKELY(out_value == NULL)) {
        return false;
    }
    if (arg == NULL) {
        *out_value = true;
        return true;
    }
    if (strcasecmp(arg, "true") == 0) {
        *out_value = true;
        return true;
    }
    if (strcasecmp(arg, "false") == 0) {
        *out_value = false;
        return true;
    }
    return false;
}

/* 解析整数参数并进行范围验证 */
static bool parse_int_arg(const char* arg, int* out_value, int min_value, int max_value,
                          const char* restrict name)
{
    if (arg == NULL || out_value == NULL || name == NULL) {
        return false;
    }

    errno = 0;
    char* endptr = NULL;
    long value = strtol(arg, &endptr, 10);
    if (errno != 0 || endptr == arg || *endptr != '\0') {
        fprintf(stderr, "Invalid value for %s: %s (expect integer)\n", name, arg);
        return false;
    }
    if (value < min_value || value > max_value) {
        fprintf(stderr, "Invalid value for %s: %s (range %d..%d)\n", name, arg, min_value,
                max_value);
        return false;
    }
    *out_value = (int)value;
    return true;
}

static bool is_valid_ip_literal(const char* restrict target, bool use_ipv6)
{
    if (target == NULL || target[0] == '\0') {
        return false;
    }

    unsigned char buf[sizeof(struct in6_addr)];
    int family = use_ipv6 ? AF_INET6 : AF_INET;
    return inet_pton(family, target, buf) == 1;
}

void config_init_default(config_t* restrict config)
{
    if (config == NULL) {
        return;
    }

    /* 网络配置 */
    snprintf(config->target, sizeof(config->target), "1.1.1.1"); /* 默认公网 IP (Cloudflare) */
    config->interval_sec = 10;                                   /* 检测间隔: 10 秒 */
    config->fail_threshold = 5;                                  /* 连续失败达到此阈值时触发关机 */
    config->timeout_ms = 2000;                                   /* 每次 ping 超时: 2 秒 */
    config->payload_size = 56; /* IPv4/IPv6 标准 ping 负载体大小 */
    config->max_retries = 2;  /* 失败后的重试次数 */
    config->use_ipv6 = false; /* 默认 IPv4 */

    /* 关机配置 */
    config->shutdown_mode = SHUTDOWN_MODE_IMMEDIATE; /* 默认: 立即关机 */
    config->delay_minutes = 1;                       /* 延迟关机: 1 分钟 */
    config->dry_run = true;                          /* 干运行: 不实际执行关机 */

    /* 行为配置 */
    config->enable_timestamp = true;    /* 是否在日志中输出时间戳 (systemd 场景下建议禁用) */
    config->log_level = LOG_LEVEL_INFO; /* 日志级别: INFO 等级 */

    /* systemd 配置 (仅当由 systemd 管理程序时需要) */
    config->enable_systemd = true;  /* 是否启用 systemd 集成 */
    config->enable_watchdog = true; /* 是否发送 watchdog 心跳 */
}

void config_load_from_env(config_t* restrict config)
{
    if (config == NULL) {
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
    config->payload_size = get_env_int("OPENUPS_PAYLOAD_SIZE", config->payload_size);
    config->max_retries = get_env_int("OPENUPS_RETRIES", config->max_retries);
    config->use_ipv6 = get_env_bool("OPENUPS_IPV6", config->use_ipv6);

    /* 关机配置 */
    value = getenv("OPENUPS_SHUTDOWN_MODE");
    if (value != NULL) {
        shutdown_mode_t parsed_mode;
        if (shutdown_mode_parse(value, &parsed_mode)) {
            config->shutdown_mode = parsed_mode;
        }
    }

    config->delay_minutes = get_env_int("OPENUPS_DELAY_MINUTES", config->delay_minutes);
    config->dry_run = get_env_bool("OPENUPS_DRY_RUN", config->dry_run);

    /* 行为配置 */
    value = getenv("OPENUPS_LOG_LEVEL");
    if (value != NULL) {
        config->log_level = string_to_log_level(value);
    }

    config->enable_systemd = get_env_bool("OPENUPS_SYSTEMD", config->enable_systemd);
    config->enable_watchdog = get_env_bool("OPENUPS_WATCHDOG", config->enable_watchdog);
    config->enable_timestamp = get_env_bool("OPENUPS_TIMESTAMP", config->enable_timestamp);
}

bool config_load_from_cmdline(config_t* restrict config, int argc, char** restrict argv)
{
    if (config == NULL || argv == NULL) {
        return false;
    }

    static struct option long_options[] = {/* 网络参数 */
                                           {"target", required_argument, 0, 't'},
                                           {"interval", required_argument, 0, 'i'},
                                           {"threshold", required_argument, 0, 'n'},
                                           {"timeout", required_argument, 0, 'w'},
                                           {"payload-size", required_argument, 0, 's'},
                                           {"retries", required_argument, 0, 'r'},
                                           {"ipv6", optional_argument, 0, '6'},

                                           /* 关机参数 */
                                           {"shutdown-mode", required_argument, 0, 'S'},
                                           {"delay", required_argument, 0, 'D'},
                                           {"dry-run", optional_argument, 0, 'd'},

                                           /* 日志参数 */
                                           {"log-level", required_argument, 0, 'L'},
                                           {"timestamp", optional_argument, 0, 'T'},

                                           /* 系统集成 */
                                           {"systemd", optional_argument, 0, 'M'},
                                           {"watchdog", optional_argument, 0, 'W'},

                                           /* 其他 */
                                           {"version", no_argument, 0, 'v'},
                                           {"help", no_argument, 0, 'h'},
                                           {0, 0, 0, 0}};

    int c;
    int option_index = 0;

    while ((c = getopt_long(argc, argv, "t:i:n:w:s:r:6::S:D:d::L:T::M::W::vh", long_options,
                            &option_index)) != -1) {
        switch (c) {
            case 't':
                snprintf(config->target, sizeof(config->target), "%s", optarg);
                break;
            case 'i':
                if (!parse_int_arg(optarg, &config->interval_sec, 1, INT_MAX, "--interval")) {
                    return false;
                }
                break;
            case 'n':
                if (!parse_int_arg(optarg, &config->fail_threshold, 1, INT_MAX, "--threshold")) {
                    return false;
                }
                break;
            case 'w':
                if (!parse_int_arg(optarg, &config->timeout_ms, 1, INT_MAX, "--timeout")) {
                    return false;
                }
                break;
            case 's':
                if (!parse_int_arg(optarg, &config->payload_size, 0, 65507, "--payload-size")) {
                    return false;
                }
                break;
            case 'r':
                if (!parse_int_arg(optarg, &config->max_retries, 0, INT_MAX, "--retries")) {
                    return false;
                }
                break;
            case 'S': {
                shutdown_mode_t parsed_mode;
                if (!shutdown_mode_parse(optarg, &parsed_mode)) {
                    fprintf(stderr,
                            "Invalid value for --shutdown-mode: %s (use "
                            "immediate|delayed|log-only)\n",
                            optarg ? optarg : "<empty>");
                    return false;
                }
                config->shutdown_mode = parsed_mode;
            } break;
            case 'D':
                if (!parse_int_arg(optarg, &config->delay_minutes, 1, INT_MAX, "--delay")) {
                    return false;
                }
                break;
            case 'L':
                config->log_level = string_to_log_level(optarg);
                break;
            case '6':
                if (!parse_bool_arg(optarg, &config->use_ipv6)) {
                    fprintf(stderr, "Invalid value for --ipv6: %s (use true|false)\n",
                            optarg ? optarg : "<empty>");
                    return false;
                }
                break;
            case 'd':
                if (!parse_bool_arg(optarg, &config->dry_run)) {
                    fprintf(stderr, "Invalid value for --dry-run: %s (use true|false)\n",
                            optarg ? optarg : "<empty>");
                    return false;
                }
                break;
            case 'T':
                if (!parse_bool_arg(optarg, &config->enable_timestamp)) {
                    fprintf(stderr, "Invalid value for --timestamp: %s (use true|false)\n",
                            optarg ? optarg : "<empty>");
                    return false;
                }
                break;
            case 'M':
                if (!parse_bool_arg(optarg, &config->enable_systemd)) {
                    fprintf(stderr, "Invalid value for --systemd: %s (use true|false)\n",
                            optarg ? optarg : "<empty>");
                    return false;
                }
                break;
            case 'W':
                if (!parse_bool_arg(optarg, &config->enable_watchdog)) {
                    fprintf(stderr, "Invalid value for --watchdog: %s (use true|false)\n",
                            optarg ? optarg : "<empty>");
                    return false;
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

    /* getopt_long 结束后，拒绝任何多余的非选项参数，避免误输入被静默忽略。 */
    if (optind < argc) {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        return false;
    }

    return true;
}

bool config_validate(const config_t* restrict config, char* restrict error_msg, size_t error_size)
{
    if (config == NULL || error_msg == NULL || error_size == 0) {
        return false;
    }

    /* 验证目标主机名 (必须不能为空) */
    if (config->target[0] == '\0') {
        snprintf(error_msg, error_size, "Target host cannot be empty");
        return false;
    }

    /* OpenUPS 不做 DNS 解析：目标必须是 IP 字面量 */
    if (!is_valid_ip_literal(config->target, config->use_ipv6)) {
        snprintf(error_msg, error_size, "Target must be a valid %s address (DNS is disabled)",
                 config->use_ipv6 ? "IPv6" : "IPv4");
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

    /* IPv4 最大 payload: 65535 - 20(IP) - 8(ICMP) = 65507
     * IPv6 最大 payload: 65535 - 40(IPv6) - 8(ICMPv6) = 65487
     */
    const int max_payload = config->use_ipv6 ? 65487 : 65507;
    if (config->payload_size < 0 || config->payload_size > max_payload) {
        snprintf(error_msg, error_size, "Payload size must be between 0 and %d", max_payload);
        return false;
    }

    if (config->max_retries < 0) {
        snprintf(error_msg, error_size, "Max retries cannot be negative");
        return false;
    }

    if (config->shutdown_mode == SHUTDOWN_MODE_DELAYED) {
        if (config->delay_minutes <= 0) {
            snprintf(error_msg, error_size, "Delay minutes must be positive for delayed mode");
            return false;
        }
        /* Hard cap to avoid ridiculous sleeps / overflows in conversions */
        if (config->delay_minutes > 60 * 24 * 365) {
            snprintf(error_msg, error_size, "Delay minutes too large (max 525600)");
            return false;
        }
    }

    return true;
}

/* 打印当前配置（用于序列化和调试，仅在 DEBUG 模式下由 main() 调用） */
void config_print(const config_t* restrict config)
{
    if (config == NULL) {
        return;
    }

    /* 打印到标准输出（仅在 DEBUG 模式下调用） */
    printf("Configuration:\n");
    printf("  Target: %s\n", config->target);
    printf("  Interval: %d seconds\n", config->interval_sec);
    printf("  Threshold: %d\n", config->fail_threshold);
    printf("  Timeout: %d ms\n", config->timeout_ms);
    printf("  Payload Size: %d bytes\n", config->payload_size);
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
void config_print_usage(void)
{
    printf("Usage: %s [options]\n\n", PROGRAM_NAME);
    printf("Network Options:\n");
    printf("  -t, --target <ip>           Target IP literal to monitor (DNS disabled, default: 1.1.1.1)\n");
    printf("  -i, --interval <sec>        Ping interval in seconds (default: 10)\n");
    printf("  -n, --threshold <num>       Consecutive failures threshold (default: 5)\n");
    printf("  -w, --timeout <ms>          Ping timeout in milliseconds (default: 2000)\n");
    printf("  -s, --payload-size <bytes>  ICMP payload size (default: 56)\n");
    printf("  -r, --retries <num>         Retry attempts per ping (default: 2)\n");
    printf("  -6, --ipv6[=true|false]     Enable/disable IPv6 mode (default: false)\n\n");

    printf("Shutdown Options:\n");
    printf("  -S, --shutdown-mode <mode>  Shutdown mode: immediate|delayed|log-only\n");
    printf("                              (default: immediate)\n");
    printf(
        "  -D, --delay <min>           Shutdown delay in minutes for delayed mode (default: 1)\n");
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
    printf("                OPENUPS_TIMEOUT, OPENUPS_PAYLOAD_SIZE, OPENUPS_RETRIES,\n");
    printf("                OPENUPS_IPV6\n");
    printf("  Shutdown:     OPENUPS_SHUTDOWN_MODE, OPENUPS_DELAY_MINUTES,\n");
    printf("                OPENUPS_DRY_RUN\n");
    printf("  Logging:      OPENUPS_LOG_LEVEL, OPENUPS_TIMESTAMP\n");
    printf("  Integration:  OPENUPS_SYSTEMD, OPENUPS_WATCHDOG\n\n");

    printf("Examples:\n");
    printf("  # Basic monitoring with dry-run\n");
    printf("  %s -t 1.1.1.1 -i 10 -n 5\n\n", PROGRAM_NAME);
    printf("  # Production mode (actual shutdown)\n");
    printf("  %s -t 192.168.1.1 -i 5 -n 3 --dry-run=false\n\n", PROGRAM_NAME);
    printf("  # Debug mode without timestamp (for systemd)\n");
    printf("  %s -t 8.8.8.8 -L debug --timestamp=false\n\n", PROGRAM_NAME);
    printf("  # Short options (values must connect directly, no space)\n");
    printf("  %s -t 8.8.8.8 -i5 -n3 -dfalse -Tfalse -Ldebug\n\n", PROGRAM_NAME);
}

/* 打印程序版本信息 */
void config_print_version(void)
{
    printf("%s version %s\n", PROGRAM_NAME, VERSION);
    printf("OpenUPS network monitor\n");
}

const char* shutdown_mode_to_string(shutdown_mode_t mode)
{
    switch (mode) {
        case SHUTDOWN_MODE_IMMEDIATE:
            return "immediate";
        case SHUTDOWN_MODE_DELAYED:
            return "delayed";
        case SHUTDOWN_MODE_LOG_ONLY:
            return "log-only";
        default:
            return "unknown";
    }
}

bool shutdown_mode_parse(const char* restrict str, shutdown_mode_t* restrict out_mode)
{
    if (out_mode == NULL) {
        return false;
    }
    if (str == NULL) {
        return false;
    }

    if (strcasecmp(str, "immediate") == 0) {
        *out_mode = SHUTDOWN_MODE_IMMEDIATE;
        return true;
    }
    if (strcasecmp(str, "delayed") == 0) {
        *out_mode = SHUTDOWN_MODE_DELAYED;
        return true;
    }
    if (strcasecmp(str, "log-only") == 0) {
        *out_mode = SHUTDOWN_MODE_LOG_ONLY;
        return true;
    }

    return false;
}

/* 将字符串转换为 shutdown_mode_t 枚举（不区分大小写） */
shutdown_mode_t string_to_shutdown_mode(const char* restrict str)
{
    if (str == NULL) {
        return SHUTDOWN_MODE_IMMEDIATE;
    }

    shutdown_mode_t parsed_mode;
    if (shutdown_mode_parse(str, &parsed_mode)) {
        return parsed_mode;
    }

    return SHUTDOWN_MODE_IMMEDIATE;
}
