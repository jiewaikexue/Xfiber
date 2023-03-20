
#ifndef UTIL_H_
#define UTIL_H_


#include <unistd.h>
//unistd.h 中所定义的接口通常都是大量针对 系统调用的封装
//（ 英语：wrapper functions），如 fork、pipe 以及各种  I/O 原语（read、write、close 等等）。
//类似于  Cygwin 和  MinGW 的 Unix 兼容层也提供相应版本的 unistd.h。 
#include <inttypes.h>
#include <sys/time.h>

namespace util {

int64_t NowMs();

}

#endif
