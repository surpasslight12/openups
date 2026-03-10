#include "config_internal.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
  const char *env_name;
  const char *label;
  size_t offset;
  int min_value;
  int max_value;
} config_env_int_option_t;

typedef struct {
  const char *env_name;
  const char *label;
  size_t offset;
} config_env_bool_option_t;

typedef struct {
  const char *name;
  log_level_t level;
} config_log_level_option_t;

typedef struct {
  const char *name;
  shutdown_mode_t mode;
} config_shutdown_mode_option_t;

static const struct option CONFIG_LONG_OPTIONS[] = {
    {"target", required_argument, 0, 't'},
    {"interval", required_argument, 0, 'i'},
    {"threshold", required_argument, 0, 'n'},
    {"timeout", required_argument, 0, 'w'},
    {"shutdown-mode", required_argument, 0, 'S'},
    {"delay", required_argument, 0, 'D'},
    {"log-level", required_argument, 0, 'L'},
    {"systemd", optional_argument, 0, 'M'},
    {"version", no_argument, 0, 'v'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0},
};

static const char *const CONFIG_OPTSTRING = "t:i:n:w:S:D:L:M::vh";

static const config_env_int_option_t CONFIG_ENV_INT_OPTIONS[] = {
  {"OPENUPS_INTERVAL", "OPENUPS_INTERVAL", offsetof(config_t, interval_sec),
   1, INT_MAX},
  {"OPENUPS_THRESHOLD", "OPENUPS_THRESHOLD",
   offsetof(config_t, fail_threshold), 1, INT_MAX},
  {"OPENUPS_TIMEOUT", "OPENUPS_TIMEOUT", offsetof(config_t, timeout_ms), 1,
   INT_MAX},
  {"OPENUPS_DELAY_MINUTES", "OPENUPS_DELAY_MINUTES",
   offsetof(config_t, delay_minutes), 0, INT_MAX},
};

static const config_env_bool_option_t CONFIG_ENV_BOOL_OPTIONS[] = {
  {"OPENUPS_SYSTEMD", "OPENUPS_SYSTEMD",
   offsetof(config_t, enable_systemd)},
};

static const config_log_level_option_t CONFIG_LOG_LEVEL_OPTIONS[] = {
  {"silent", LOG_LEVEL_SILENT},
  {"none", LOG_LEVEL_SILENT},
  {"error", LOG_LEVEL_ERROR},
  {"warn", LOG_LEVEL_WARN},
  {"info", LOG_LEVEL_INFO},
  {"debug", LOG_LEVEL_DEBUG},
};

static const config_shutdown_mode_option_t CONFIG_SHUTDOWN_MODE_OPTIONS[] = {
  {"dry-run", SHUTDOWN_MODE_DRY_RUN},
  {"true-off", SHUTDOWN_MODE_TRUE_OFF},
  {"log-only", SHUTDOWN_MODE_LOG_ONLY},
};

static bool string_equals_ignore_case(const char *restrict lhs,
                                      const char *restrict rhs) {
  return lhs != NULL && rhs != NULL && strcasecmp(lhs, rhs) == 0;
}

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
    const config_log_level_option_t *option = &CONFIG_LOG_LEVEL_OPTIONS[i];
    if (string_equals_ignore_case(arg, option->name)) {
      *out_value = option->level;
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
    const config_shutdown_mode_option_t *option =
        &CONFIG_SHUTDOWN_MODE_OPTIONS[i];
    if (string_equals_ignore_case(str, option->name)) {
      *out_mode = option->mode;
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
    return set_error(
        error_msg, error_size,
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
                     "Invalid value for %s: %s (use true|false)", label,
                     value);
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
    return set_error(
        error_msg, error_size,
        "Invalid value for %s: %s (use dry-run|true-off|log-only)", env_name,
        value);
  }

  return true;
}

static bool load_env_log_level(const char *restrict env_name,
                               log_level_t *restrict out_value,
                               char *restrict error_msg,
                               size_t error_size) {
  if (env_name == NULL || out_value == NULL) {
    return false;
  }

  const char *value = getenv(env_name);
  if (value == NULL) {
    return true;
  }

  if (!parse_log_level_value(value, out_value)) {
    return set_error(
        error_msg, error_size,
        "Invalid value for %s: %s (use silent|error|warn|info|debug)",
        env_name, value);
  }

  return true;
}

static int *config_int_field(config_t *restrict config, size_t offset) {
  if (config == NULL || offset > sizeof(*config) - sizeof(int)) {
    return NULL;
  }

  return (int *)((char *)config + offset);
}

static bool *config_bool_field(config_t *restrict config, size_t offset) {
  if (config == NULL || offset > sizeof(*config) - sizeof(bool)) {
    return NULL;
  }

  return (bool *)((char *)config + offset);
}

static bool load_env_int_options(config_t *restrict config,
                                 char *restrict error_msg,
                                 size_t error_size) {
  if (config == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }

  for (size_t i = 0;
       i < sizeof(CONFIG_ENV_INT_OPTIONS) / sizeof(CONFIG_ENV_INT_OPTIONS[0]);
       i++) {
    const config_env_int_option_t *option = &CONFIG_ENV_INT_OPTIONS[i];
    int *field = config_int_field(config, option->offset);
    if (field == NULL ||
        !load_env_int(option->env_name, option->label, option->min_value,
                      option->max_value, field, error_msg, error_size)) {
      return false;
    }
  }

  return true;
}

static bool load_env_bool_options(config_t *restrict config,
                                  char *restrict error_msg,
                                  size_t error_size) {
  if (config == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }

  for (size_t i = 0;
       i < sizeof(CONFIG_ENV_BOOL_OPTIONS) /
               sizeof(CONFIG_ENV_BOOL_OPTIONS[0]);
       i++) {
    const config_env_bool_option_t *option = &CONFIG_ENV_BOOL_OPTIONS[i];
    bool *field = config_bool_field(config, option->offset);
    if (field == NULL ||
        !load_env_bool(option->env_name, option->label, field, error_msg,
                       error_size)) {
      return false;
    }
  }

  return true;
}

void config_init_default(config_t *restrict config) {
  if (config == NULL) {
    return;
  }

  memset(config, 0, sizeof(*config));
  (void)snprintf(config->target, sizeof(config->target), "%s",
                 OPENUPS_DEFAULT_TARGET);
  config->interval_sec = OPENUPS_DEFAULT_INTERVAL_SEC;
  config->fail_threshold = OPENUPS_DEFAULT_FAIL_THRESHOLD;
  config->timeout_ms = OPENUPS_DEFAULT_TIMEOUT_MS;
  config->shutdown_mode = SHUTDOWN_MODE_DRY_RUN;
  config->delay_minutes = OPENUPS_DEFAULT_DELAY_MINUTES;
  config->log_level = LOG_LEVEL_INFO;
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

  if (!load_env_shutdown_mode("OPENUPS_SHUTDOWN_MODE",
                              &config->shutdown_mode, error_msg,
                              error_size)) {
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