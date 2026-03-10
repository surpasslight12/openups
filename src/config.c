#include "openups.h"

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
