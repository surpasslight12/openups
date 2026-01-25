#ifndef OPENUPS_OPENUPS_H
#define OPENUPS_OPENUPS_H

/* OpenUPS 公共 API（偏库化）。
 * 约定：此头文件稳定；src/ 下其他头文件视为内部实现细节。
 */

#include <stddef.h>

/* 运行 OpenUPS 主流程：内部完成配置加载、初始化、监控循环与清理。
 * 失败时返回非 0，并尽力写入 error_msg（若 error_msg==NULL 则忽略）。
 */
[[nodiscard]] int openups_run(int argc, char** argv, char* error_msg, size_t error_size);

#endif /* OPENUPS_OPENUPS_H */
