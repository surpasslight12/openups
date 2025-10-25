#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <ctype.h>

uint64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

char* get_timestamp_str(char* buffer, size_t size) {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec,
             tv.tv_usec / 1000);
    
    return buffer;
}

const char* get_env_or_default(const char* name, const char* default_value) {
    const char* value = getenv(name);
    return value ? value : default_value;
}

bool get_env_bool(const char* name, bool default_value) {
    const char* value = getenv(name);
    if (!value) {
        return default_value;
    }
    
    if (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        return true;
    }
    if (strcasecmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        return false;
    }
    
    return default_value;
}

int get_env_int(const char* name, int default_value) {
    const char* value = getenv(name);
    if (!value) {
        return default_value;
    }
    
    char* endptr;
    long result = strtol(value, &endptr, 10);
    
    if (*endptr != '\0') {
        return default_value;
    }
    
    return (int)result;
}

char* trim_whitespace(char* str) {
    if (!str) return NULL;
    
    /* 跳过前导空白 */
    while (isspace((unsigned char)*str)) str++;
    
    if (*str == '\0') return str;
    
    /* 移除尾部空白 */
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    *(end + 1) = '\0';
    
    return str;
}

bool str_equals(const char* a, const char* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}
