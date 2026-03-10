#include "config_internal.h"

#include <stdio.h>

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
         "disabled, default: %s)\n",
         OPENUPS_DEFAULT_TARGET);
  printf("  -i, --interval <sec>        Ping interval in seconds (default: %d)\n",
         OPENUPS_DEFAULT_INTERVAL_SEC);
  printf("  -n, --threshold <num>       Consecutive failures threshold "
         "(default: %d)\n",
         OPENUPS_DEFAULT_FAIL_THRESHOLD);
  printf("  -w, --timeout <ms>          Ping timeout in milliseconds (default: "
         "%d)\n\n",
         OPENUPS_DEFAULT_TIMEOUT_MS);
  printf("Shutdown Options:\n");
  printf("  -S, --shutdown-mode <mode>  Shutdown mode: "
         "dry-run|true-off|log-only\n");
  printf("                              (default: dry-run)\n");
  printf("  -D, --delay <min>           Shutdown countdown in minutes for dry-run/true-off "
         "mode (default: %d)\n",
         OPENUPS_DEFAULT_DELAY_MINUTES);
  printf("                              0 means immediate execution without countdown\n\n");
  printf("Logging Options:\n");
  printf("  -L, --log-level <level>     Log level: "
         "silent|error|warn|info|debug\n");
  printf("                              (default: info)\n");
  printf("System Integration:\n");
  printf("  -M[ARG], --systemd[=ARG]    Enable/disable systemd integration "
         "(default: %s)\n",
         OPENUPS_DEFAULT_SYSTEMD ? "true" : "false");
  printf("                              Watchdog is auto-enabled with "
         "systemd\n");
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
  printf("  %s -t 8.8.8.8 -L debug --systemd=false\n\n",
         OPENUPS_PROGRAM_NAME);
  printf("  # Short options (values must connect directly, no space)\n");
  printf("  %s -t 8.8.8.8 -i5 -n3 -Strue-off -D0 -Mfalse -Ldebug\n\n",
         OPENUPS_PROGRAM_NAME);
}

void config_print_version(void) {
  printf("%s version %s\n", OPENUPS_PROGRAM_NAME, OPENUPS_VERSION);
  printf("OpenUPS network monitor\n");
}