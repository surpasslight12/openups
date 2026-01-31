#include "context.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#error "This program currently targets Linux."
#endif

/**
 * @brief OpenUPS 程序入口
 *
 * 初始化上下文并运行主监控循环。
 */
int main(int argc, char** argv)
{
    char error_msg[256];

    openups_ctx_t ctx;
    if (!openups_ctx_init(&ctx, argc, argv, error_msg, sizeof(error_msg))) {
        fprintf(stderr, "OpenUPS failed: %s\n", error_msg);
        return 1;
    }

    int rc = openups_ctx_run(&ctx);
    openups_ctx_destroy(&ctx);

    if (rc != 0) {
        fprintf(stderr, "OpenUPS exited with code %d\n", rc);
    }
    return rc;
}
