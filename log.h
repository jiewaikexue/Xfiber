
#pragma once
#include <cstdio>
#include <string>

#define USE_LOG
#ifndef USE_LOG 
    // debug
    #define DEBUG_ENABLE 0
    // 信息
    #define INFO_ENABLE 0
    // 警告
    #define WARNING_ENABLE 0
    // 错误
    #define ERROR_ENABLE 0

#else 
    // debug
    #define DEBUG_ENABLE 1
    // 信息
    #define INFO_ENABLE 1
    // 警告
    #define WARNING_ENABLE 1
    // 错误
    #define ERROR_ENABLE 1


#endif 

static std::string log_date()
{
    //>注意<: time_t time(time_t *calptr);
    // time_t类型相当于一个long， time用于取Epoch记年以来到现在经过的秒数（系统当前时间），
    // Epoch记年从1970年1月1日开始。把取到的时间存在指针指向的变量中。

    //>解答<: 获取了当前时间
    time_t now = time(0);
    // tm 详细的事件类 结构体
    struct tm tstruct;
    char buf[80];
    // 当前时间

    // 原型: struct tm *localtime(const time_t *calptr);
    //  localtime 作用:将time_t的值转换为tm结构体。
    tstruct = *localtime(&now);
    // 格式化输出当前时间
    strftime(buf, sizeof(buf), "%Y-%m-%d.%X", &tstruct);
    return buf;
}

#ifndef __LOG_DATE__
#define __LOG_DATE__ log_date().c_str()
#endif

//>重点<: 参数说明
//=========================
// 1. args: 可变参数列表===> 表示其余的参数可以是零个或多个 ==> 参数以及参数之间的逗号构成arg 的值，在宏扩展时替换arg
// 2. ##args : 连续 输出 可变参数列表
// 3. fmt : 自定义的宏
// 3. #fmt : 把宏#fmt的话，就是把fmt传进来的内容以字符串形式输出
//=========================

// >注意<: 宏说明
//  1. __FILE__ 文件名
//  2. __LINE__ 第几行
//  3. ... 其余的参数

#ifndef __LOG_DATE__
#define __LOG_DATE__ log_date().c_str()
#endif

#if DEBUG_ENABLE
#define LOG_DEBUG(fmt, args...) fprintf(stderr, "[D][%s][%s %d] " fmt "\n", __LOG_DATE__, __FILE__, __LINE__, ##args);
#else
#define LOG_DEBUG(fmt, ...)
#endif

#if INFO_ENABLE
#define LOG_INFO(fmt, args...) fprintf(stderr, "[I][%s][%s %d] " fmt "\n", __LOG_DATE__, __FILE__, __LINE__, ##args);
#else
#define LOG_INFO(fmt, ...)
#endif

#if WARNING_ENABLE
#define LOG_WARNING(fmt, args...) fprintf(stderr, "[W][%s][%s %d] " fmt "\n", __LOG_DATE__, __FILE__, __LINE__, ##args);
#else
#define LOG_WARNING(fmt, ...)
#endif

#if ERROR_ENABLE
#define LOG_ERROR(fmt, args...) fprintf(stderr, "[E][%s][%s %d] " fmt "\n", __LOG_DATE__, __FILE__, __LINE__, ##args);
#else
#define LOG_ERROR(fmt, ...)
#endif
