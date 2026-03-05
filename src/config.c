#include "openups.h"

static bool parse_bool_arg(const char *arg, bool *out_value) {
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

static bool parse_int_arg(const char *arg, int *out_value, int min_value,
                          int max_value, const char *restrict name) {
  if (arg == NULL || out_value == NULL || name == NULL) {
    return false;
  }
  errno = 0;
  char *endptr = NULL;
  long value = strtol(arg, &endptr, 10);
  if (errno != 0 || endptr == arg || *endptr != '\0') {
    fprintf(stderr, "Invalid value for %s: %s (expect integer)\n", name, arg);
    return false;
  }
  if (value < min_value || value > max_value) {
    fprintf(stderr, "Invalid value for %s: %s (range %d..%d)\n", name, arg,
            min_value, max_value);
    return false;
  }
  *out_value = (int)value;
  return true;
}

static bool is_valid_ip_literal(const char *restrict target) {
  if (target == NULL || target[0] == '\0') {
    return false;
  }
  unsigned char buf[sizeof(struct in6_addr)];
  if (inet_pton(AF_INET, target, buf) == 1) {
    return true;
  }
  if (inet_pton(AF_INET6, target, buf) == 1) {
    return true;
  }
  return false;
}

const char *shutdown_mode_to_string(shutdown_mode_t mode) {
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

static bool shutdown_mode_parse(const char *restrict str,
                                shutdown_mode_t *restrict out_mode) {
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

void config_init_default(config_t *restrict config) {
  if (config == NULL) {
    return;
  }
  snprintf(config->target, sizeof(config->target), "1.1.1.1");
  config->interval_sec = 10;
  config->fail_threshold = 5;
  config->timeout_ms = 2000;
  config->shutdown_mode = SHUTDOWN_MODE_IMMEDIATE;
  config->delay_minutes = 1;
  config->dry_run = true;
  config->enable_timestamp = true;
  config->log_level = LOG_LEVEL_INFO;
  config->enable_systemd = true;
}

void config_load_from_env(config_t *restrict config) {
  if (config == NULL) {
    return;
  }
  const char *value;
  value = getenv("OPENUPS_TARGET");
  if (value != NULL) {
    snprintf(config->target, sizeof(config->target), "%s", value);
  }
  config->interval_sec = get_env_int("OPENUPS_INTERVAL", config->interval_sec);
  config->fail_threshold =
      get_env_int("OPENUPS_THRESHOLD", config->fail_threshold);
  config->timeout_ms = get_env_int("OPENUPS_TIMEOUT", config->timeout_ms);
  value = getenv("OPENUPS_SHUTDOWN_MODE");
  if (value != NULL) {
    shutdown_mode_t parsed_mode;
    if (shutdown_mode_parse(value, &parsed_mode)) {
      config->shutdown_mode = parsed_mode;
    }
  }
  config->delay_minutes =
      get_env_int("OPENUPS_DELAY_MINUTES", config->delay_minutes);
  config->dry_run = get_env_bool("OPENUPS_DRY_RUN", config->dry_run);
  value = getenv("OPENUPS_LOG_LEVEL");
  if (value != NULL) {
    config->log_level = string_to_log_level(value);
  }
  config->enable_systemd =
      get_env_bool("OPENUPS_SYSTEMD", config->enable_systemd);
  config->enable_timestamp =
      get_env_bool("OPENUPS_TIMESTAMP", config->enable_timestamp);
}

bool config_load_from_cmdline(config_t *restrict config, int argc,
                              char **restrict argv) {
  if (config == NULL || argv == NULL) {
    return false;
  }
  static struct option long_options[] = {
      {"target", required_argument, 0, 't'},
      {"interval", required_argument, 0, 'i'},
      {"threshold", required_argument, 0, 'n'},
      {"timeout", required_argument, 0, 'w'},
      {"shutdown-mode", required_argument, 0, 'S'},
      {"delay", required_argument, 0, 'D'},
      {"dry-run", optional_argument, 0, 'd'},
      {"log-level", required_argument, 0, 'L'},
      {"timestamp", optional_argument, 0, 'T'},
      {"systemd", optional_argument, 0, 'M'},
      {"version", no_argument, 0, 'v'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}};
  int c;
  int option_index = 0;
  const char *optstring = "t:i:n:w:S:D:d::L:T::M::vh";
  while ((c = getopt_long(argc, argv, optstring, long_options,
                          &option_index)) != -1) {
    switch (c) {
    case 't':
      snprintf(config->target, sizeof(config->target), "%s", optarg);
      break;
    case 'i':
      if (!parse_int_arg(optarg, &config->interval_sec, 1, INT_MAX,
                         "--interval"))
        return false;
      break;
    case 'n':
      if (!parse_int_arg(optarg, &config->fail_threshold, 1, INT_MAX,
                         "--threshold"))
        return false;
      break;
    case 'w':
      if (!parse_int_arg(optarg, &config->timeout_ms, 1, INT_MAX, "--timeout"))
        return false;
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
      if (!parse_int_arg(optarg, &config->delay_minutes, 1, INT_MAX, "--delay"))
        return false;
      break;
    case 'L':
      config->log_level = string_to_log_level(optarg);
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
  if (optind < argc) {
    fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
    return false;
  }
  return true;
}

bool config_validate(const config_t *restrict config, char *restrict error_msg,
                     size_t error_size) {
  if (config == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }
  if (config->target[0] == '\0') {
    snprintf(error_msg, error_size, "Target host cannot be empty");
    return false;
  }
  if (!is_valid_ip_literal(config->target)) {
    snprintf(error_msg, error_size,
             "Target must be a valid IPv4 or IPv6 address (DNS is disabled)");
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
  if (config->shutdown_mode == SHUTDOWN_MODE_DELAYED) {
    if (config->delay_minutes <= 0) {
      snprintf(error_msg, error_size,
               "Delay minutes must be positive for delayed mode");
      return false;
    }
    if (config->delay_minutes > 60 * 24 * 365) {
      snprintf(error_msg, error_size, "Delay minutes too large (max 525600)");
      return false;
    }
  }
  return true;
}

void config_print(const config_t *restrict config,
                  const logger_t *restrict logger) {
  if (config == NULL || logger == NULL) {
    return;
  }
  logger_debug(logger, "Configuration:");
  logger_debug(logger, "  Target: %s", config->target);
  logger_debug(logger, "  Interval: %d seconds", config->interval_sec);
  logger_debug(logger, "  Threshold: %d", config->fail_threshold);
  logger_debug(logger, "  Timeout: %d ms", config->timeout_ms);
  logger_debug(logger, "  Shutdown Mode: %s",
               shutdown_mode_to_string(config->shutdown_mode));
  logger_debug(logger, "  Dry Run: %s", config->dry_run ? "true" : "false");
  logger_debug(logger, "  Log Level: %s",
               log_level_to_string(config->log_level));
  logger_debug(logger, "  Timestamp: %s",
               config->enable_timestamp ? "true" : "false");
  logger_debug(logger, "  Systemd: %s",
               config->enable_systemd ? "true" : "false");
}

void config_print_usage(void) {
  printf("Usage: %s [options]\n\n", OPENUPS_PROGRAM_NAME);
  printf("Network Options:\n");
  printf("  -t, --target <ip>           Target IP literal to monitor (DNS "
         "disabled, default: 1.1.1.1)\n");
  printf(
      "  -i, --interval <sec>        Ping interval in seconds (default: 10)\n");
  printf("  -n, --threshold <num>       Consecutive failures threshold "
         "(default: 5)\n");
  printf("  -w, --timeout <ms>          Ping timeout in milliseconds (default: "
         "2000)\n\n");
  printf("Shutdown Options:\n");
  printf("  -S, --shutdown-mode <mode>  Shutdown mode: "
         "immediate|delayed|log-only\n");
  printf("                              (default: immediate)\n");
  printf("  -D, --delay <min>           Shutdown delay in minutes for delayed "
         "mode (default: 1)\n");
  printf("  -d[ARG], --dry-run[=ARG]    Dry-run mode, no actual shutdown "
         "(default: true)\n");
  printf("                              ARG: true|false\n");
  printf("                              Note: Use -dfalse or --dry-run=false "
         "(no space)\n\n");
  printf("Logging Options:\n");
  printf("  -L, --log-level <level>     Log level: "
         "silent|error|warn|info|debug\n");
  printf("                              (default: info)\n");
  printf("  -T[ARG], --timestamp[=ARG]  Enable/disable log timestamps "
         "(default: true)\n");
  printf("                              ARG format: true|false\n");
  printf("System Integration:\n");
  printf("  -M[ARG], --systemd[=ARG]    Enable/disable systemd integration "
         "(default: true)\n");
  printf("                              Watchdog is auto-enabled with "
         "systemd\n");
  printf("                              ARG format: true|false\n\n");
  printf("General Options:\n");
  printf("  -v, --version               Show version information\n");
  printf("  -h, --help                  Show this help message\n\n");
  printf("Environment Variables (lower priority than CLI args):\n");
  printf(
      "  Network:      OPENUPS_TARGET, OPENUPS_INTERVAL, OPENUPS_THRESHOLD,\n");
  printf("                OPENUPS_TIMEOUT\n");
  printf("  Shutdown:     OPENUPS_SHUTDOWN_MODE, OPENUPS_DELAY_MINUTES,\n");
  printf("                OPENUPS_DRY_RUN\n");
  printf("  Integration:  OPENUPS_SYSTEMD\n");
  printf("\n");
  printf("Examples:\n");
  printf("  # Basic monitoring with dry-run\n");
  printf("  %s -t 1.1.1.1 -i 10 -n 5\n\n", OPENUPS_PROGRAM_NAME);
  printf("  # Production mode (actual shutdown)\n");
  printf("  %s -t 192.168.1.1 -i 5 -n 3 --dry-run=false\n\n",
         OPENUPS_PROGRAM_NAME);
  printf("  # Debug mode without timestamp (for systemd)\n");
  printf("  %s -t 8.8.8.8 -L debug --timestamp=false\n\n",
         OPENUPS_PROGRAM_NAME);
  printf("  # Short options (values must connect directly, no space)\n");
  printf("  %s -t 8.8.8.8 -i5 -n3 -dfalse -Tfalse -Ldebug\n\n",
         OPENUPS_PROGRAM_NAME);
}

void config_print_version(void) {
  printf("%s version %s\n", OPENUPS_PROGRAM_NAME, OPENUPS_VERSION);
  printf("OpenUPS network monitor\n");
}
