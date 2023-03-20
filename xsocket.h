
#pragma once

#include <memory>
#include <unistd.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include "util.h"



class Fd {
public:
    Fd();

    ~Fd();

    //>提问<: 怎么这里又出来一个静态变量?
    static uint32_t next_seq_;

    int RawFd();

    void RegisterFdToSched();

    // 检测fd是否可用
    bool Available();

protected:
    int fd_;

    int seq_;
};


class Connection;

// Listener 是侦听套接字对象
class Listener : public Fd {
public:

    Listener();

    ~Listener();

    //>注意<: 这个的返回值 似乎很厉害
    std::shared_ptr<Connection> Accept();

    void FromRawFd(int fd);

    static Listener ListenTCP(uint16_t port);

private:
    //端口号
    uint16_t port_;
};


class Connection : public Fd {
public:
    Connection();

    Connection(int fd);

    ~Connection();

    static std::shared_ptr<Connection> ConnectTCP(const char *ipv4, uint16_t port);

    ssize_t Write(const char *buf, size_t sz, int timeout_ms=-1) const;

    ssize_t Read(char *buf, size_t sz, int timeout_ms=-1) const;
};
