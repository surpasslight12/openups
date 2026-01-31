#ifndef OPENUPS_H
#define OPENUPS_H

/**
 * @file openups.h
 * @brief OpenUPS 公共头文件 - 版本和程序名常量
 *
 * 当前项目仅提供可执行程序，不承诺稳定的 C API；
 * 此头文件主要用于信息展示或未来可能的库化预留。
 */

#define OPENUPS_VERSION "1.3.0"
#define OPENUPS_PROGRAM_NAME "openups"

/* 向现有内部代码提供兼容宏别名 */
#define VERSION OPENUPS_VERSION
#define PROGRAM_NAME OPENUPS_PROGRAM_NAME

#endif /* OPENUPS_H */
