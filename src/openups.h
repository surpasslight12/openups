#ifndef OPENUPS_H
#define OPENUPS_H

/**
 * @file openups.h
 * @brief OpenUPS 公共头文件 - 版本、程序名常量、编译器优化提示
 *
 * 当前项目仅提供可执行程序，不承诺稳定的 C API；
 * 此头文件主要用于信息展示或未来可能的库化预留。
 */

#define OPENUPS_VERSION "1.3.0"
#define OPENUPS_PROGRAM_NAME "openups"

/* 向现有内部代码提供兼容宏别名 */
#define VERSION OPENUPS_VERSION
#define PROGRAM_NAME OPENUPS_PROGRAM_NAME

/* ===== 编译器优化提示宏 ===== */

#if defined(__GNUC__) || defined(__clang__)

/** 分支预测提示：高概率为真 / 为假 */
#define OPENUPS_LIKELY(x) __builtin_expect(!!(x), 1)
#define OPENUPS_UNLIKELY(x) __builtin_expect(!!(x), 0)

/** 函数温度标注：热路径 / 冷路径（影响代码布局和优化力度） */
#define OPENUPS_HOT __attribute__((hot))
#define OPENUPS_COLD __attribute__((cold))

/** 纯函数：返回值仅依赖参数和全局状态（可读内存，无副作用） */
#define OPENUPS_PURE __attribute__((pure))

/** 常量函数：返回值仅依赖参数值（不读内存，无副作用） */
#define OPENUPS_CONST __attribute__((const))

/** 强制展开所有被调用函数（用于减少调用开销的关键路径） */
#define OPENUPS_FLATTEN __attribute__((flatten))

/** 符号可见性：exported（配合 -fvisibility=hidden 使用） */
#define OPENUPS_EXPORT __attribute__((visibility("default")))

#else /* 非 GCC/Clang 回退 */

#define OPENUPS_LIKELY(x) (x)
#define OPENUPS_UNLIKELY(x) (x)
#define OPENUPS_HOT
#define OPENUPS_COLD
#define OPENUPS_PURE
#define OPENUPS_CONST
#define OPENUPS_FLATTEN
#define OPENUPS_EXPORT

#endif /* __GNUC__ || __clang__ */

#endif /* OPENUPS_H */
