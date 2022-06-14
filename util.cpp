
#include "util.h"

namespace util {

//获取一个相对时间
int64_t NowMs() {
    struct timeval tv;// long int tv_sec long int tv_usec
    //获得当前精确时间（1970年1月1日到现在的时间），或者为执行计时
    //函数原型:int gettimeofday(struct timeval*tv, struct timezone *tz);
    gettimeofday(&tv, nullptr);
    // 秒数*1000 + 微妙/1000
    // int64_t 防止移除 32 最多到2038年
    return int64_t(tv.tv_sec * 1000) + tv.tv_usec / 1000;
}

}
