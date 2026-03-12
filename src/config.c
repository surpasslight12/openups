#include "openups.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- Compile-time defaults ---- */

#define OPENUPS_DEFAULT_TARGET         "1.1.1.1"
#define OPENUPS_DEFAULT_INTERVAL_SEC   10
#define OPENUPS_DEFAULT_FAIL_THRESHOLD 5
#define OPENUPS_DEFAULT_TIMEOUT_MS     2000
#define OPENUPS_DEFAULT_DELAY_MINUTES  0
#define OPENUPS_MAX_DELAY_MINUTES      (365 * 24 * 60)
#define OPENUPS_DEFAULT_SYSTEMD        true

/* ---- Option tables ---- */

typedef struct {
  const char *name;
  log_level_t level;
} config_log_level_option_t;

typedef struct {
  const char *name;
  shutdown_mode_t mode;
} config_shutdown_mode_option_t;

static const struct option CONFIG_LONG_OPTIONS[] = {
    {"target",        required_argument, 0, 't'},
    {"interval",      required_argument, 0, 'i'},
    {"threshold",     required_argument, 0, 'n'},
    {"timeout",       required_argument, 0, 'w'},
    {"shutdown-mode", required_argument, 0, 'S'},
    {"delay",         required_argument, 0, 'D'},
    {"log-level",     required_argument, 0, 'L'},
    {"systemd",       optional_argument, 0, 'M'},
    {"version",       no_argument,       0, 'v'},
    {"help",          no_argument,       0, 'h'},
    {0, 0, 0, 0},
};

static const char *const CONFIG_OPTSTRING = "t:i:n:w:S:D:L:M::vh";

static const config_log_level_option_t CONFIG_LOG_LEVEL_OPTIONS[] = {
  {"silent", LOG_LEVEL_SILENT},
  {"none",   LOG_LEVEL_SILENT},
  {"error",  LOG_LEVEL_ERROR},
  {"warn",   LOG_LEVEL_WARN},
  {"info",   LOG_LEVEL_INFO},
  {"debug",  LOG_LEVEL_DEBUG},
};

static const config_shutdown_mode_option_t CONFIG_SHUTDOWN_MODE_OPTIONS[] = {
  {"dry-run",  SHUTDOWN_MODE_DRY_RUN},
  {"true-off", SHUTDOWN_MODE_TRUE_OFF},
  {"log-only", SHUTDOWN_MODE_LOG_ONLY},
};

/* ---- Shared helpers ---- */

static bool set_error(char *restrict error_msg, size_t error_size,
                      const char *restrict fmt, ...)
    __attribute__((format(printf, 3, 4)));

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

static bool string_equals_ignore_case(const char *restrict lhs,
                                      const char *restrict rhs) {
  return lhs != NULL && rhs != NULL && strcasecmp(lhs, rhs) == 0;
}

static bool copy_string_value(char *restrict dest, size_t dest_size,
                              const char *restrict src,
                              const char *restrict name,
                              char *restrict error_msg, size_t error_size) {
  if (dest == NULL || dest_size == 0 || src == NULL || name == NULL) {
    return false;
  }
  int written = snprintf(dest, dest_size, "%s", src);
  if (written < 0 || (size_t)written >= dest_size) {
    return set_error(error_msg, error_size,
                     "%s is too long (max %zu characters)", name, dest_size - 1);
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
  if (string_equals_ignore_case(arg, "true")) {
    *out_value = true;
    return true;
  }
  if (string_equals_ignore_case(arg, "false")) {
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
  for (size_t i = 0;
       i < sizeof(CONFIG_LOG_LEVEL_OPTIONS) / sizeof(CONFIG_LOG_LEVEL_OPTIONS[0]);
       i++) {
    if (string_equals_ignore_case(arg, CONFIG_LOG_LEVEL_OPTIONS[i].name)) {
      *out_value = CONFIG_LOG_LEVEL_OPTIONS[i].level;
      return true;
    }
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

static const char *optarg_or_empty(const char *restrict arg) {
  return arg != NULL ? arg : "<empty>";
}

static bool parse_cmdline_int_option(const char *restrict option_name,
                                     const char *restrict arg, int min_value,
                                     int max_value, int *restrict out_value,
                                     char *restrict error_msg,
                                     size_t error_size) {
  if (option_name == NULL || out_value == NULL || error_msg == NULL ||
      error_size == 0) {
    return false;
  }
  if (!parse_int_value(arg, min_value, max_value, out_value)) {
    return set_error(error_msg, error_size,
                     "Invalid value for %s: %s (range %d..%d)", option_name,
                     optarg_or_empty(arg), min_value, max_value);
  }
  return true;
}

static bool parse_cmdline_bool_option(const char *restrict option_name,
                                      const char *restrict arg,
                                      bool implicit_true_when_null,
                                      bool *restrict out_value,
                                      char *restrict error_msg,
                                      size_t error_size) {
  if (option_name == NULL || out_value == NULL || error_msg == NULL ||
      error_size == 0) {
    return false;
  }
  if (!parse_bool_value(arg, implicit_true_when_null, out_value)) {
    return set_error(error_msg, error_size,
                     "Invalid value for %s: %s (use true|false)",
                     option_name, optarg_or_empty(arg));
  }
  return true;
}

static bool parse_cmdline_log_level_option(const char *restrict option_name,
                                           const char *restrict arg,
                                           log_level_t *restrict out_value,
                                           char *restrict error_msg,
                                           size_t error_size) {
  if (option_name == NULL || out_value == NULL || error_msg == NULL ||
      error_size == 0) {
    return false;
  }
  if (!parse_log_level_value(arg, out_value)) {
    return set_error(error_msg, error_size,
                     "Invalid value for %s: %s (use silent|error|warn|info|debug)",
                     option_name, optarg_or_empty(arg));
  }
  return true;
}

static bool shutdown_mode_parse_internal(const char *restrict str,
                                         shutdown_mode_t *restrict out_mode) {
  if (str == NULL || out_mode == NULL) {
    return false;
  }
  for (size_t i = 0;
       i < sizeof(CONFIG_SHUTDOWN_MODE_OPTIONS) /
               sizeof(CONFIG_SHUTDOWN_MODE_OPTIONS[0]);
       i++) {
    if (string_equals_ignore_case(str, CONFIG_SHUTDOWN_MODE_OPTIONS[i].name)) {
      *out_mode = CONFIG_SHUTDOWN_MODE_OPTIONS[i].mode;
      return true;
    }
  }
  return false;
}

static bool parse_cmdline_shutdown_mode_option(
    const char *restrict option_name, const char *restrict arg,
    shutdown_mode_t *restrict out_mode, char *restrict error_msg,
    size_t error_size) {
  if (option_name == NULL || out_mode == NULL || error_msg == NULL ||
      error_size == 0) {
    return false;
  }
  if (!shutdown_mode_parse_internal(arg, out_mode)) {
    return set_error(error_msg, error_size,
                     "Invalid value for %s: %s (use dry-run|true-off|log-only)",
                     option_name, optarg_or_empty(arg));
  }
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
                     "Invalid value for %s: %s (use true|false)", label, value);
  }
  *out_value = parsed_value;
  return true;
}

static bool load_env_shutdown_mode(const char *restrict env_name,
                                   shutdown_mode_t *restrict out_value,
                                   char *restrict error_msg,
                                   size_t error_size) {
  if (env_name == NULL || out_value == NULL) {
    return false;
  }
  const char *value = getenv(env_name);
  if (value == NULL) {
    return true;
  }
  if (!shutdown_mode_parse_internal(value, out_value)) {
    return set_error(error_msg, error_size,
                     "Invalid value for %s: %s (use dry-run|true-off|log-only)",
                     env_name, value);
  }
  return true;
}

static bool load_env_log_level(const char *restrict env_name,
                               log_level_t *restrict out_value,
                               char *restrict error_msg, size_t error_size) {
  if (env_name == NULL || out_value == NULL) {
    return false;
  }
  const char *value = getenv(env_name);
  if (value == NULL) {
    return true;
  }
  if (!parse_log_level_value(value, out_value)) {
    return set_error(error_msg, error_size,
                     "Invalid value for %s: %s (use silent|error|warn|info|debug)",
                     env_name, value);
  }
  return true;
}

static bool load_env_int_options(config_t *restrict config,
                                 char *restrict error_msg, size_t error_size) {
  if (config == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }
  return load_env_int("OPENUPS_INTERVAL",      "OPENUPS_INTERVAL",      1, INT_MAX, &config->interval_sec,   error_msg, error_size) &&
         load_env_int("OPENUPS_THRESHOLD",     "OPENUPS_THRESHOLD",     1, INT_MAX, &config->fail_threshold, error_msg, error_size) &&
         load_env_int("OPENUPS_TIMEOUT",       "OPENUPS_TIMEOUT",       1, INT_MAX, &config->timeout_ms,     error_msg, error_size) &&
         load_env_int("OPENUPS_DELAY_MINUTES", "OPENUPS_DELAY_MINUTES", 0, INT_MAX, &config->delay_minutes,  error_msg, error_size);
}

static bool load_env_bool_options(config_t *restrict config,
                                  char *restrict error_msg, size_t error_size) {
  if (config == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }
  return load_env_bool("OPENUPS_SYSTEMD", "OPENUPS_SYSTEMD",
                       &config->enable_systemd, error_msg, error_size);
}

/* ---- Public API: init / env / cmdline ---- */

void config_init_default(config_t *restrict config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  (void)snprintf(config->target, sizeof(config->target), "%s",
                 OPENUPS_DEFAULT_TARGET);
  config->interval_sec   = OPENUPS_DEFAULT_INTERVAL_SEC;
  config->fail_threshold = OPENUPS_DEFAULT_FAIL_THRESHOLD;
  config->timeout_ms     = OPENUPS_DEFAULT_TIMEOUT_MS;
  config->shutdown_mode  = SHUTDOWN_MODE_DRY_RUN;
  config->delay_minutes  = OPENUPS_DEFAULT_DELAY_MINUTES;
  config->log_level      = LOG_LEVEL_INFO;
  config->enable_systemd = OPENUPS_DEFAULT_SYSTEMD;
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
  if (!load_env_int_options(config, error_msg, error_size) ||
      !load_env_bool_options(config, error_msg, error_size)) {
    return false;
  }
  if (!load_env_shutdown_mode("OPENUPS_SHUTDOWN_MODE", &config->shutdown_mode,
                              error_msg, error_size)) {
    return false;
  }
  if (!load_env_log_level("OPENUPS_LOG_LEVEL", &config->log_level,
                          error_msg, error_size)) {
    return false;
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
  int requested_exit_option = 0;
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
      if (!parse_cmdline_int_option("--interval", optarg, 1, INT_MAX,
                                    &config->interval_sec, error_msg,
                                    error_size)) {
        return false;
      }
      break;
    case 'n':
      if (!parse_cmdline_int_option("--threshold", optarg, 1, INT_MAX,
                                    &config->fail_threshold, error_msg,
                                    error_size)) {
        return false;
      }
      break;
    case 'w':
      if (!parse_cmdline_int_option("--timeout", optarg, 1, INT_MAX,
                                    &config->timeout_ms, error_msg,
                                    error_size)) {
        return false;
      }
      break;
    case 'S':
      if (!parse_cmdline_shutdown_mode_option("--shutdown-mode", optarg,
                                              &config->shutdown_mode,
                                              error_msg, error_size)) {
        return false;
      }
      break;
    case 'D':
      if (!parse_cmdline_int_option("--delay", optarg, 0, INT_MAX,
                                    &config->delay_minutes, error_msg,
                                    error_size)) {
        return false;
      }
      break;
    case 'L':
      if (!parse_cmdline_log_level_option("--log-level", optarg,
                                          &config->log_level, error_msg,
                                          error_size)) {
        return false;
      }
      break;
    case 'M':
      if (!parse_cmdline_bool_option("--systemd", optarg, true,
                                     &config->enable_systemd, error_msg,
                                     error_size)) {
        return false;
      }
      break;
    case 'v':
      requested_exit_option = 'v';
      break;
    case 'h':
      requested_exit_option = 'h';
      break;
    case '?':
      if (optopt != 0) {
        return set_error(error_msg, error_size,
                         "Option requires an argument or has invalid value: -%c",
                         optopt);
      }
      return set_error(error_msg, error_size, "Unknown option: %s",
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
  if (requested_exit_option == 'v') {
    config_print_version();
    *exit_requested = true;
  } else if (requested_exit_option == 'h') {
    config_print_usage();
    *exit_requested = true;
  }
  return true;
}

/* ---- Validation ---- */

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

static bool timeout_fits_interval(const config_t *restrict config) {
  if (config == NULL || config->interval_sec <= 0 || config->timeout_ms <= 0) {
    return false;
  }
  uint64_t interval_ms = 0;
  if (ckd_mul(&interval_ms, (uint64_t)config->interval_sec,
              OPENUPS_MS_PER_SEC)) {
    return false;
  }
  return (uint64_t)config->timeout_ms < interval_ms;
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
    return set_error(error_msg, error_size,
                     "Target must be a valid IPv4 or IPv6 address (DNS is disabled)");
  }
  if (config->interval_sec <= 0) {
    return set_error(error_msg, error_size, "Interval must be positive");
  }
  if (config->fail_threshold <= 0) {
    return set_error(error_msg, error_size, "Failure threshold must be positive");
  }
  if (config->timeout_ms <= 0) {
    return set_error(error_msg, error_size, "Timeout must be positive");
  }
  if (config->delay_minutes < 0) {
    return set_error(error_msg, error_size, "Delay minutes cannot be negative");
  }
  if (config->delay_minutes > OPENUPS_MAX_DELAY_MINUTES) {
    return set_error(error_msg, error_size, "Delay minutes too large (max 525600)");
  }
  if (!timeout_fits_interval(config)) {
    return set_error(error_msg, error_size,
                     "Timeout must be smaller than interval to avoid overlapping probes");
  }
  if (config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY &&
      config->delay_minutes != 0) {
    return set_error(error_msg, error_size,
                     "Delay is only valid with dry-run or true-off shutdown modes");
  }
  return true;
}

/* ---- Rendering / display ---- */

bool config_log_timestamps_enabled(const config_t *restrict config) {
  if (config == NULL) {
    return false;
  }
  return !config->enable_systemd;
}

const char *shutdown_mode_to_string(shutdown_mode_t mode) {
  switch (mode) {
  case SHUTDOWN_MODE_DRY_RUN:
    return "dry-run";
  case SHUTDOWN_MODE_TRUE_OFF:
    return "true-off";
  case SHUTDOWN_MODE_LOG_ONLY:
    return "log-only";
  default:
    return "unknown";
  }
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
  logger_debug(logger, "  Log Level: %s",
               log_level_to_string(config->log_level));
  logger_debug(logger, "  Timestamp: %s",
               config_log_timestamps_enabled(config) ? "true" : "false");
  logger_debug(logger, "  Systemd: %s",
               config->enable_systemd ? "true" : "false");
}

void config_print_usage(void) {
  printf("Usage: %s [options]\n\n", OPENUPS_PROGRAM_NAME);
  printf("Network Options:\n");
  printf("  -t, --target <ip>           Target IP literal to monitor (DNS "
         "disabled, default: %s)\n", OPENUPS_DEFAULT_TARGET);
  printf("  -i, --interval <sec>        Ping interval in seconds (default: %d)\n",
         OPENUPS_DEFAULT_INTERVAL_SEC);
  printf("  -n, --threshold <num>       Consecutive failures threshold "
         "(default: %d)\n", OPENUPS_DEFAULT_FAIL_THRESHOLD);
  printf("  -w, --timeout <ms>          Ping timeout in milliseconds (default: "
         "%d)\n\n", OPENUPS_DEFAULT_TIMEOUT_MS);
  printf("Shutdown Options:\n");
  printf("  -S, --shutdown-mode <mode>  Shutdown mode: "
         "dry-run|true-off|log-only\n");
  printf("                              (default: dry-run)\n");
  printf("  -D, --delay <min>           Shutdown countdown in minutes for dry-run/true-off "
         "mode (default: %d)\n", OPENUPS_DEFAULT_DELAY_MINUTES);
  printf("                              0 means immediate execution without countdown\n\n");
  printf("Logging Options:\n");
  printf("  -L, --log-level <level>     Log level: "
         "silent|error|warn|info|debug\n");
  printf("                              (default: info)\n");
  printf("System Integration:\n");
  printf("  -M[ARG], --systemd[=ARG]    Enable/disable systemd integration "
         "(default: %s)\n", OPENUPS_DEFAULT_SYSTEMD ? "true" : "false");
  printf("                              Watchdog is auto-enabled with systemd\n");
  printf("                              Log timestamps are auto-disabled when "
         "systemd is enabled\n");
  printf("                              Otherwise timestamps stay enabled\n");
  printf("                              ARG format: true|false\n\n");
  printf("General Options:\n");
  printf("  -v, --version               Show version information\n");
  printf("  -h, --help                  Show this help message\n\n");
  printf("Environment Variables (lower priority than CLI args):\n");
  printf("  Network:      OPENUPS_TARGET, OPENUPS_INTERVAL, OPENUPS_THRESHOLD,\n");
  printf("                OPENUPS_TIMEOUT\n");
  printf("  Shutdown:     OPENUPS_SHUTDOWN_MODE, OPENUPS_DELAY_MINUTES,\n");
  printf("  Logging:      OPENUPS_LOG_LEVEL\n");
  printf("  Integration:  OPENUPS_SYSTEMD\n");
  printf("\n");
  printf("Examples:\n");
  printf("  # Basic monitoring with dry-run mode\n");
  printf("  %s -t 1.1.1.1 -i 10 -n 5\n\n", OPENUPS_PROGRAM_NAME);
  printf("  # Production mode (actual shutdown)\n");
  printf("  %s -t 192.168.1.1 -i 5 -n 3 --shutdown-mode true-off\n\n",
         OPENUPS_PROGRAM_NAME);
  printf("  # Delayed countdown before shutdown\n");
  printf("  %s -t 192.168.1.1 -i 5 -n 3 --shutdown-mode true-off --delay 3\n\n",
         OPENUPS_PROGRAM_NAME);
  printf("  # Foreground debug mode with local timestamps\n");
  printf("  %s -t 8.8.8.8 -L debug --systemd=false\n\n", OPENUPS_PROGRAM_NAME);
  printf("  # Short options (values must connect directly, no space)\n");
  printf("  %s -t 8.8.8.8 -i5 -n3 -Strue-off -D0 -Mfalse -Ldebug\n\n",
         OPENUPS_PROGRAM_NAME);
}

void config_print_version(void) {
  printf("%s version %s\n", OPENUPS_PROGRAM_NAME, OPENUPS_VERSION);
  printf("OpenUPS network monitor\n");
}

/* ---- Orchestration: env + cmdline + validate ---- */

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
