
#include "util.h"

/*
    关于超时任务:
        1. 如何判断是否超时: 时间戳 < 当前时间戳 ? 超时 放弃调度 : 不超时 可以调度
        2. wakeup时 : 是在 不超时的前提下 才能进行调度: 一旦未超时的 fiber 被唤醒, 超时时间作废
*/
namespace util {

//获取一个相对时间
int64_t NowMs() {
    struct timeval tv;// long int tv_sec long int tv_usec
    //获得当前精确时间（1970年1月1日到现在的时间），或者为执行计时
    //函数原型:int gettimeofday(struct timeval*tv, struct timezone *tz);
    gettimeofday(&tv, nullptr);
    // 秒数*1000 + 微妙/1000
    // int64_t 防止溢出 32 最多到2038年
    return int64_t(tv.tv_sec * 1000) + tv.tv_usec / 1000;
}

}
