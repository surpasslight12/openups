#include "openups.h"

#include "openups_internal.h"

#include <stddef.h>

int openups_run(int argc, char** argv, char* error_msg, size_t error_size)
{
    char local_error[256];
    if (error_msg == NULL || error_size == 0) {
        error_msg = local_error;
        error_size = sizeof(local_error);
    }

    openups_ctx_t ctx;
    if (!openups_ctx_init(&ctx, argc, argv, error_msg, error_size)) {
        return 1;
    }

    int rc = openups_ctx_run(&ctx);
    openups_ctx_destroy(&ctx);
    return rc;
}
