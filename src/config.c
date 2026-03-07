#include "openups.h"

static const struct option CONFIG_LONG_OPTIONS[] = {
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
    {0, 0, 0, 0},
};

static const char *const CONFIG_OPTSTRING = "t:i:n:w:S:D:d::L:T::M::vh";

static bool set_error(char *restrict error_msg, size_t error_size,
                      const char *restrict fmt, ...) {
  if (error_msg == NULL || error_size == 0 || fmt == NULL) {
    return false;
  }

  va_list args;
  va_start(args, fmt);
  vsnprintf(error_msg, error_size, fmt, args);
  va_end(args);
  return false;
}

static bool copy_string_value(char *restrict dest, size_t dest_size,
                              const char *restrict src,
                              const char *restrict name,
                              char *restrict error_msg,
                              size_t error_size) {
  if (dest == NULL || dest_size == 0 || src == NULL || name == NULL) {
    return false;
  }

  int written = snprintf(dest, dest_size, "%s", src);
  if (written < 0 || (size_t)written >= dest_size) {
    return set_error(error_msg, error_size,
                     "%s is too long (max %zu characters)", name,
                     dest_size - 1);
  }

  return true;
}

static bool parse_bool_value(const char *restrict arg,
                             bool implicit_true_when_null,
                             bool *restrict out_value) {
  if (out_value == NULL) {
    return false;
  }

  if (arg == NULL) {
    if (!implicit_true_when_null) {
      return false;
    }
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

static bool parse_log_level_value(const char *restrict arg,
                                  log_level_t *restrict out_value) {
  if (arg == NULL || out_value == NULL) {
    return false;
  }

  if (strcasecmp(arg, "silent") == 0 || strcasecmp(arg, "none") == 0) {
    *out_value = LOG_LEVEL_SILENT;
    return true;
  }
  if (strcasecmp(arg, "error") == 0) {
    *out_value = LOG_LEVEL_ERROR;
    return true;
  }
  if (strcasecmp(arg, "warn") == 0) {
    *out_value = LOG_LEVEL_WARN;
    return true;
  }
  if (strcasecmp(arg, "info") == 0) {
    *out_value = LOG_LEVEL_INFO;
    return true;
  }
  if (strcasecmp(arg, "debug") == 0) {
    *out_value = LOG_LEVEL_DEBUG;
    return true;
  }

  return false;
}

static bool parse_int_value(const char *restrict arg, int min_value,
                            int max_value, int *restrict out_value) {
  if (arg == NULL || out_value == NULL) {
    return false;
  }

  errno = 0;
  char *endptr = NULL;
  long value = strtol(arg, &endptr, 10);
  if (errno != 0 || endptr == arg || *endptr != '\0') {
    return false;
  }
  if (value < min_value || value > max_value) {
    return false;
  }

  *out_value = (int)value;
  return true;
}

static bool load_env_int(const char *restrict env_name,
                         const char *restrict label, int min_value,
                         int max_value, int *restrict out_value,
                         char *restrict error_msg, size_t error_size) {
  if (env_name == NULL || label == NULL || out_value == NULL) {
    return false;
  }

  const char *value = getenv(env_name);
  if (value == NULL) {
    return true;
  }

  int parsed_value = 0;
  if (!parse_int_value(value, min_value, max_value, &parsed_value)) {
    return set_error(error_msg, error_size,
                     "Invalid value for %s: %s (range %d..%d)", label, value,
                     min_value, max_value);
  }

  *out_value = parsed_value;
  return true;
}

static bool load_env_bool(const char *restrict env_name,
                          const char *restrict label,
                          bool *restrict out_value,
                          char *restrict error_msg, size_t error_size) {
  if (env_name == NULL || label == NULL || out_value == NULL) {
    return false;
  }

  const char *value = getenv(env_name);
  if (value == NULL) {
    return true;
  }

  bool parsed_value = false;
  if (!parse_bool_value(value, false, &parsed_value)) {
    return set_error(error_msg, error_size,
                     "Invalid value for %s: %s (use true|false)", label,
                     value);
  }

  *out_value = parsed_value;
  return true;
}

static bool is_valid_ip_literal(const char *restrict target) {
  if (target == NULL || target[0] == '\0') {
    return false;
  }

  unsigned char addr_buffer[sizeof(struct in6_addr)];
  if (inet_pton(AF_INET, target, addr_buffer) == 1) {
    return true;
  }
  if (inet_pton(AF_INET6, target, addr_buffer) == 1) {
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
  if (str == NULL || out_mode == NULL) {
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

  memset(config, 0, sizeof(*config));
  (void)snprintf(config->target, sizeof(config->target), "1.1.1.1");
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

bool config_load_from_env(config_t *restrict config, char *restrict error_msg,
                          size_t error_size) {
  if (config == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }

  error_msg[0] = '\0';

  const char *value = getenv("OPENUPS_TARGET");
  if (value != NULL &&
      !copy_string_value(config->target, sizeof(config->target), value,
                         "OPENUPS_TARGET", error_msg, error_size)) {
    return false;
  }

  if (!load_env_int("OPENUPS_INTERVAL", "OPENUPS_INTERVAL", 1, INT_MAX,
                    &config->interval_sec, error_msg, error_size) ||
      !load_env_int("OPENUPS_THRESHOLD", "OPENUPS_THRESHOLD", 1, INT_MAX,
                    &config->fail_threshold, error_msg, error_size) ||
      !load_env_int("OPENUPS_TIMEOUT", "OPENUPS_TIMEOUT", 1, INT_MAX,
                    &config->timeout_ms, error_msg, error_size) ||
      !load_env_int("OPENUPS_DELAY_MINUTES", "OPENUPS_DELAY_MINUTES", 1,
                    INT_MAX, &config->delay_minutes, error_msg,
                    error_size) ||
      !load_env_bool("OPENUPS_DRY_RUN", "OPENUPS_DRY_RUN",
                     &config->dry_run, error_msg, error_size) ||
      !load_env_bool("OPENUPS_SYSTEMD", "OPENUPS_SYSTEMD",
                     &config->enable_systemd, error_msg, error_size) ||
      !load_env_bool("OPENUPS_TIMESTAMP", "OPENUPS_TIMESTAMP",
                     &config->enable_timestamp, error_msg, error_size)) {
    return false;
  }

  value = getenv("OPENUPS_SHUTDOWN_MODE");
  if (value != NULL) {
    shutdown_mode_t parsed_mode;
    if (!shutdown_mode_parse(value, &parsed_mode)) {
      return set_error(
          error_msg, error_size,
          "Invalid value for OPENUPS_SHUTDOWN_MODE: %s (use immediate|delayed|log-only)",
          value);
    }
    config->shutdown_mode = parsed_mode;
  }

  value = getenv("OPENUPS_LOG_LEVEL");
  if (value != NULL) {
    log_level_t parsed_level;
    if (!parse_log_level_value(value, &parsed_level)) {
      return set_error(error_msg, error_size,
                       "Invalid value for OPENUPS_LOG_LEVEL: %s (use silent|error|warn|info|debug)",
                       value);
    }
    config->log_level = parsed_level;
  }

  return true;
}

bool config_load_from_cmdline(config_t *restrict config, int argc,
                              char **restrict argv,
                              bool *restrict exit_requested,
                              char *restrict error_msg, size_t error_size) {
  if (config == NULL || argv == NULL || exit_requested == NULL ||
      error_msg == NULL || error_size == 0) {
    return false;
  }

  *exit_requested = false;
  error_msg[0] = '\0';
  optind = 1;
  opterr = 0;

  int option_index = 0;
  int option = 0;
  while ((option = getopt_long(argc, argv, CONFIG_OPTSTRING,
                               CONFIG_LONG_OPTIONS, &option_index)) != -1) {
    switch (option) {
    case 't':
      if (!copy_string_value(config->target, sizeof(config->target), optarg,
                             "--target", error_msg, error_size)) {
        return false;
      }
      break;
    case 'i':
      if (!parse_int_value(optarg, 1, INT_MAX, &config->interval_sec)) {
        return set_error(error_msg, error_size,
                         "Invalid value for --interval: %s (range 1..%d)",
                         optarg ? optarg : "<empty>", INT_MAX);
      }
      break;
    case 'n':
      if (!parse_int_value(optarg, 1, INT_MAX, &config->fail_threshold)) {
        return set_error(error_msg, error_size,
                         "Invalid value for --threshold: %s (range 1..%d)",
                         optarg ? optarg : "<empty>", INT_MAX);
      }
      break;
    case 'w':
      if (!parse_int_value(optarg, 1, INT_MAX, &config->timeout_ms)) {
        return set_error(error_msg, error_size,
                         "Invalid value for --timeout: %s (range 1..%d)",
                         optarg ? optarg : "<empty>", INT_MAX);
      }
      break;
    case 'S': {
      shutdown_mode_t parsed_mode;
      if (!shutdown_mode_parse(optarg, &parsed_mode)) {
        return set_error(
            error_msg, error_size,
            "Invalid value for --shutdown-mode: %s (use immediate|delayed|log-only)",
            optarg ? optarg : "<empty>");
      }
      config->shutdown_mode = parsed_mode;
      break;
    }
    case 'D':
      if (!parse_int_value(optarg, 1, INT_MAX, &config->delay_minutes)) {
        return set_error(error_msg, error_size,
                         "Invalid value for --delay: %s (range 1..%d)",
                         optarg ? optarg : "<empty>", INT_MAX);
      }
      break;
    case 'd': {
      bool parsed_value = false;
      if (!parse_bool_value(optarg, true, &parsed_value)) {
        return set_error(error_msg, error_size,
                         "Invalid value for --dry-run: %s (use true|false)",
                         optarg ? optarg : "<empty>");
      }
      config->dry_run = parsed_value;
      break;
    }
    case 'L': {
      log_level_t parsed_level;
      if (!parse_log_level_value(optarg, &parsed_level)) {
        return set_error(error_msg, error_size,
                         "Invalid value for --log-level: %s (use silent|error|warn|info|debug)",
                         optarg ? optarg : "<empty>");
      }
      config->log_level = parsed_level;
      break;
    }
    case 'T': {
      bool parsed_value = false;
      if (!parse_bool_value(optarg, true, &parsed_value)) {
        return set_error(
            error_msg, error_size,
            "Invalid value for --timestamp: %s (use true|false)",
            optarg ? optarg : "<empty>");
      }
      config->enable_timestamp = parsed_value;
      break;
    }
    case 'M': {
      bool parsed_value = false;
      if (!parse_bool_value(optarg, true, &parsed_value)) {
        return set_error(error_msg, error_size,
                         "Invalid value for --systemd: %s (use true|false)",
                         optarg ? optarg : "<empty>");
      }
      config->enable_systemd = parsed_value;
      break;
    }
    case 'v':
      config_print_version();
      *exit_requested = true;
      return true;
    case 'h':
      config_print_usage();
      *exit_requested = true;
      return true;
    case '?':
      if (optopt != 0) {
        return set_error(error_msg, error_size,
                         "Option requires an argument or has invalid value: -%c",
                         optopt);
      }
      return set_error(error_msg, error_size,
                       "Unknown option: %s",
                       argv[optind - 1] ? argv[optind - 1] : "<unknown>");
    default:
      return set_error(error_msg, error_size,
                       "Failed to parse command line arguments");
    }
  }

  if (optind < argc) {
    return set_error(error_msg, error_size, "Unexpected argument: %s",
                     argv[optind]);
  }

  return true;
}

bool config_resolve(config_t *restrict config, int argc, char **restrict argv,
                    bool *restrict exit_requested,
                    char *restrict error_msg, size_t error_size) {
  if (config == NULL || argv == NULL || exit_requested == NULL ||
      error_msg == NULL || error_size == 0) {
    return false;
  }

  *exit_requested = false;
  config_init_default(config);

  if (!config_load_from_env(config, error_msg, error_size)) {
    return false;
  }

  if (!config_load_from_cmdline(config, argc, argv, exit_requested, error_msg,
                                error_size)) {
    return false;
  }

  if (*exit_requested) {
    return true;
  }

  return config_validate(config, error_msg, error_size);
}

bool config_validate(const config_t *restrict config, char *restrict error_msg,
                     size_t error_size) {
  if (config == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }

  if (config->target[0] == '\0') {
    return set_error(error_msg, error_size, "Target host cannot be empty");
  }
  if (!is_valid_ip_literal(config->target)) {
    return set_error(
        error_msg, error_size,
        "Target must be a valid IPv4 or IPv6 address (DNS is disabled)");
  }
  if (config->interval_sec <= 0) {
    return set_error(error_msg, error_size, "Interval must be positive");
  }
  if (config->fail_threshold <= 0) {
    return set_error(error_msg, error_size,
                     "Failure threshold must be positive");
  }
  if (config->timeout_ms <= 0) {
    return set_error(error_msg, error_size, "Timeout must be positive");
  }
  if (config->shutdown_mode == SHUTDOWN_MODE_DELAYED) {
    if (config->delay_minutes <= 0) {
      return set_error(error_msg, error_size,
                       "Delay minutes must be positive for delayed mode");
    }
    if (config->delay_minutes > 60 * 24 * 365) {
      return set_error(error_msg, error_size,
                       "Delay minutes too large (max 525600)");
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
  logger_debug(logger, "  Delay: %d minutes", config->delay_minutes);
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
