#include "openups.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#error "This program currently targets Linux."
#endif

/* OpenUPS 程序入口：初始化上下文并运行主循环。 */
int main(int argc, char** argv)
{
    char error_msg[256];

    int rc = openups_run(argc, argv, error_msg, sizeof(error_msg));
    if (rc != 0) {
        fprintf(stderr, "OpenUPS failed: %s\n", error_msg);
    }
    return rc;
}
