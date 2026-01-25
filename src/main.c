#include "context.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#error "This program currently targets Linux."
#endif

/* OpenUPS 程序主入口（重构版）
 * 统一上下文架构：
 *   - 所有状态封装在 openups_ctx_t 中
 *   - 初始化流程自动化（config -> logger -> pinger -> systemd -> metrics）
 *   - 单参数传递，减少函数调用开销
 *   - 优化内存布局（热数据靠前）
 */
int main(int argc, char** argv)
{
    openups_ctx_t ctx;
    char error_msg[256];

    /* 初始化上下文（包含所有配置加载和组件初始化） */
    if (!openups_ctx_init(&ctx, argc, argv, error_msg, sizeof(error_msg))) {
        fprintf(stderr, "Initialization failed: %s\n", error_msg);
        return 1;
    }

    /* 运行监控循环 */
    int result = openups_ctx_run(&ctx);

    /* 清理资源 */
    openups_ctx_destroy(&ctx);

    return result;
}
