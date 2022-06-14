
#include "xsocket.h"
#include "xfiber.h"



// 静态变量初始化
uint32_t Fd::next_seq_ = 0;


Fd::Fd()
{
    fd_ = -1;
    seq_ = Fd::next_seq_++; 
}

Fd::~Fd(){

}
bool Fd::Available() {
    return fd_ > 0;
}
int Fd::RawFd() {
    return fd_;
}
void Fd:: RegisterFdToSched() {

    //获取唯一的xfiber
    XFiber *  xfiber = XFiber::xfiber();
    //在epoll中注册 某个fd
    xfiber->TakeOver(fd_);
}



Listener::Listener(){}
Listener::~Listener(){

        /*close 和shutdown
            shutdown: 彻底关闭这个套接字,所有使用该套接字的协程都傻眼
                    ==> 本质是断开连接 
            close: 减少引用计数,直到引用计数 ==0 时才关闭
                    ==> 本质是 释放文件描述符
        */
    close(this->fd_);
}

Listener Listener::ListenTCP(uint16_t port)
{
    /* 函数说明: int socket(int domain, int type, int protocol)   
        1. domain:协议版本:
                    1. AF_INET :IPV4
                    2. AF_INET6: IPV6
                    3. AF_UNIX | AF_LOCAL  :本地套接字使用
        2.type:协议类型
                1.SOCK_STREAM 流式套接字 : 默认是TCP
                2.SOCK_DGRAM  报式套接字 : 默认是UDP
        3. protocal:一般填0, 表示使用对应类型的默认协议
        4. 返回值 :
                1. 成功:  返回一个大于0的文件描述符
                2. 失败: 返回-1, 并设置errno
        5. 当调用socket函数以后, 返回一个文件描述符,
             内核会提供 与 该文件描述符相对应的
             读和写缓冲区, 
             同时还有两个队列, 分别是请求连接队列和已连接队列
    
    */
    int fd = socket(AF_INET,SOCK_STREAM,0);//创建tcp套接字
    if (fd < 0) {
        exit(-1);
    }
/* struct sockaddr , struct sockaddr_in  struct sockaddr_in6 说明 
        //一半不用这个
                struct sockaddr {
                    unsigned  short  sa_family;     address family, AF_xxx 
                    char  sa_data[14];  14 bytes of protocol address 
                }; 
        //sockaddr_in : 可以和sockaddr相互转换
        //他俩大小一样,这个是ipv4版本
        struct sockaddr_in{
            sa_family_t     sin_family;   //地址族（Address Family），也就是地址类型
            uint16_t        sin_port;     //16位的端口号
            struct in_addr  sin_addr;     //32位IP地址
            char            sin_zero[8];  //不使用，一般用0填充
        };
        //ipv6版本
        struct sockaddr_in6 {
        sa_family_t sin6_family;  //(2)地址类型，取值为AF_INET6
        in_port_t sin6_port;  //(2)16位端口号
        uint32_t sin6_flowinfo;  //(4)IPv6流信息
        struct in6_addr sin6_addr;  //(4)具体的IPv6地址
        uint32_t sin6_scope_id;  //(4)接口范围ID
        };

*/
    struct sockaddr_in addr;//初始化一个 套接字
    addr.sin_family = AF_INET;//协议使用ipv4协议簇
    addr.sin_port = htons(port);//host to net short: 本机字节序转换为网络字节序 大端
    /* ip地址转换函数说明: 
    const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
    网络IP转换为字符串形式的点分十进制的IP
    int inet_pton(int af, const char *src, void *dst);
    将字符串形式的点分十进制IP转换为大端模式的网络IP(整形4字节数)
    */
    addr.sin_addr.s_addr = htonl(INADDR_ANY);//INADDR_ANY: 表示使用任意有效的可用IP


    int flag = 1;
    //>注意<: setsockopt()函数用于任意类型、任意状态套接口的设置选项值，其作用和使用说明如下：
    /*   原型: int setsockopt(int sock, int level, int optname, const void *optval, socklen_t optlen);
        参数说明: 
            1.sock: 描述符
            2.level: 选项所在的协议层级
                1)SOL_SOCKET:通用套接字选项.
                2)IPPROTO_IP:IP选项.
                3)IPPROTO_TCP:TCP选项.　
            3.optname:需设置的选项
                    SO_REUSEADDR	BOOL	允许套接字绑定到已在使用的地址
            4.optlevel:指向存放选项值的缓冲区。
            5.optlen: optval 缓冲区的长度。
        返回值: 成功: 0
            失败: -1 && errno被设为以下的某个值
                EBADF：sock不是有效的文件描述词
                EFAULT：optval指向的内存并非有效的进程空间
                EINVAL：在调用setsockopt()时，optlen无效
                ENOPROTOOPT：指定的协议层不能识别选项
                ENOTSOCK：sock描述的不是套接字
            
    */
    //>注意<: 该函数作用: 允许套接字 fd 绑定到已经在使用的地址, 缓冲区地址为 &flag 
    /*>重点<: setsockopt 再解: SO_REUSEADDR and SO_REUSEPORT
    ==========================================
    + 1. SO_REUSEADDR : 地址复用 ==>  服务器停止后立即重启,且继续使用同一个 ip+port
    + 2. SO_REUSEPORT : 端口复用 ==>  每一个线程拥有自己的 服务器套接字,且可以重复绑定,在套接字上避免了锁的竞争
    ==========================================
    ==> 1. 地址复用:  
            a. 地址复用的原理
                ----------------------------------------
                +这个套接字选项通知内核，如果端口忙，
                + 但TCP状态位于 TIME_WAIT（Linux下的TIME_WAIT大概是2分钟） ，
                + 可以重用端口。
                + 如果端口忙，而TCP状态位于其他状态，
                +重用端口时依旧得到一个错误信息，指明"地址已经使用中"。
                -------------------------------

                - 注意: 需要在bind()之前设置SO_REUSEADDR套接字选项。
            b. 地址复用的作用:
                -------------------------------------
                + 如果你的服务程序停止后想立即重启，
                + 而新套接字依旧使用同一端口，
                + 此时SO_REUSEADDR 选项非常有用。               
                -----------------------------------------
    ==> 2. 端口复用:
            此选项允许完全重复捆绑，
            但仅在想捆绑相同 IP地址和端口 的套接口都指定了此套接口选项才行
            ----------------------
            + 允许多个套接字 bind()/listen() 同一个TCP/UDP端口 
            + 每一个线程拥有自己的服务器套接字 
            + 在服务器套接字上没有了锁的竞争 
            + 内核层面实现负载均衡 安全层面，
            --------------------------------
    */
    //>提问<: 为什么要这样: 我的目的是为了地质复用 服务器挂了 立刻重启 继续使用这个地址
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
        LOG_ERROR("try set SO_REUSEADDR failed, msg=%s", strerror(errno));
        exit(-1);
    }

    
    /*fcntl系统调用 : 可以用来对已打开的文件描述符进行各种控制操作
                        来改变已打开文件的的各种属性
        原型: int fcntl(int fd, int cmd ,struct flock* lock);
        参数: 
                1. fd: 被打开的文件的文件描述符
                2. cmd:
                        ⚫ 复制文件描述符（cmd=F_DUPFD 或 cmd=F_DUPFD_CLOEXEC ）；
                        ⚫ 获取/设置文件描述符标志（cmd=F_GETFD 或 cmd=F_SETFD ）；
                        ⚫ 获取/设置文件状态标志（ cmd=F_GETFL 或 cmd=F_SETFL ）；
                        ⚫ 获取/设置异步 IO 所有权（ cmd=F_GETOWN 或 cmd=F_SETOWN ）；
                        ⚫ 获取/设置记录锁（ cmd=F_GETLK 或 cmd=F_SETLK ）；
                3. 设置为阻塞属性
    */
    // F_SETFL  设置给arg描述符状态标志,可以更改的几个标志是： O_APPEND， O_NONBLOCK，O_SYNC和O_ASYNC。
    // 暨 设置该文件描述符为 非阻塞
    //>提问<: 为什么要设置成非阻塞? 因为你的epoll是 ET 模式
    // ===> 争取 尽可能的一次性读完 ,别阻塞在这
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        LOG_ERROR("set set listen fd O_NONBLOCK failed, msg=%s", strerror(errno));
        exit(-1);
    }



    /*bind函数描述: 将socket文件描述符和IP，PORT绑定
    int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
        参数
            socket: socket套接字的描述符
            addr: socketaddr结构,包含了 IP地址 + 端口
            addrlen : addr结构的长度 
        返回值: 
            成功: 返回0
            失败: 返回-1, 并设置errno
        */
    if (bind(fd, (sockaddr *)&addr, sizeof(sockaddr_in)) < 0) {
        LOG_ERROR("try bind port [%d] failed, msg=%s", port, strerror(errno));
        exit(-1);
    }

    /*listen函数描述: 将套接字由主动态变为被动态, 暨 转换文侦听套接字
    int listen(int sockfd, int backlog);
    参数说明:
        sockfd: 调用socket函数返回的文件描述符,
       backlog: 未连接队列 的队列长度 
    返回值:
       成功: 返回0
       失败: 返回-1, 并设置errno      
    */
    if (listen(fd, 10) < 0) {
        LOG_ERROR("try listen port[%d] failed, msg=%s", port, strerror(errno));
        exit(-1);
    }

    Listener listener;
    listener.FromRawFd(fd);

    LOG_INFO("listen %d success...", port);
    XFiber::xfiber()->TakeOver(fd);
    return listener;
}

void Listener::FromRawFd(int fd) {
    this->fd_ = fd;
}

//>重点<: shared_ptr
//>提问<: 为什么要用share_ptr
std::shared_ptr<Connection> Listener::Accept() {
    XFiber *xfiber = XFiber::xfiber();//获取单例xfiber
    //死循环 一直accept
    while (true) {
        /*accept函数说明:获得一个连接, 若当前没有连接则 根据 套接字状态 进行判断
        int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);    
        函数参数:
            sockfd: 调用socket函数返回的文件描述符,一般也就是从侦听套接字哪里接受
            addr: 传出参数, 保存客户端的地址信息
            addrlen: 传入传出参数,  addr变量所占内存空间大小
        返回值:
            成功: 返回一个 `新的文件描述符`,用于和客户端通信
            失败: 返回-1, 并设置errno值.


        accept函数: 监听 阻塞套接字
           ===>
            是一个阻塞函数, 若没有新的连接请求, 则一直阻塞.
            若有: 则从已连接队列中获取一个新的连接,
            并获得一个新的文件描述符, 
            该文件描述符用于和客户端通信.  
            (内核会负责将请求队列中的连接拿到已连接队列中)
           ===> 监听非阻塞 套接字 : 不会阻塞

        >注意<: fd是非阻塞套接字,如果accept监听非阻塞套接字 ,则不会阻塞
        */
        int client_fd = accept(fd_, nullptr, nullptr);
        // 监听到了链接
        if (client_fd > 0) {

            //>提问<: 为什么这里需要fctl
            //>解答<: 链接建立后 返回的 套接字 是 默认阻塞的
            //      ==>而我们要使用的 所有的socket都是非阻塞的
            //>提示<: 为了epoll ,将套接字改为非阻塞
            if (fcntl(client_fd, F_SETFL, O_NONBLOCK) != 0) {
                perror("fcntl");
                exit(-1);
            }
/*setsockopt函数解释:
int setsockopt(int sockfd, int level, int optname,const void *optval, socklen_t optlen)
    参数1：sockfd  ------文件描述符
    参数2：level   ------三个选项：
                            1.SOL_SOCKET 通用套接字选项
                            2.IPPROTO_IP ip层选项
                            3.IPPROTO_TCP TCP层选项
    参数3: optname : 相关选项 TCP_NODELAY :禁用ngal算法
    参数4：*optval    ------指向存放选项值的缓冲区（）
    参数5：socklen_t optlen -缓冲区大小
*/
/* 什么是 nagle 算法? 为什么要关闭nagle算法? ==> 我们不蓄!
        Nagle算法通过将未确认的数据存入缓冲区直到蓄足一个包一起发送的方法，
        来减少主机发送的零碎小数据包的数目。
        但对于某些应用来说，这种算法将降低系统性能。
        所以TCP_NODELAY可用来将此算法关闭。
        应用程序编写者只有在确切了解它的效果并确实需要的情况下，
        才设置TCP_NODELAY选项，因为设置后对网络性能有明显的负面影响。
        
        TCP_NODELAY是唯一使用IPPROTO_TCP层的选项，
        其他所有选项都使用SOL_SOCKET层。

ngal算法开启/不开启
    ==============================================================
    +  禁用:  启动TCP_NODELAY，就意味着禁用了Nagle算法，
    +   允许小包的发送。对于延时敏感型，
    +   同时数据传输量比较小的应用，开启TCP_NODELAY选项无疑是一个正确的选择。
    ==============================================================
    +  使用:  如果开启了Nagle算法，就很可能出现频繁的延时，导致用户体验极差。当
    ==============================================================
>重点<:
    开启: 就很可能出现频繁的延时，导致用户体验极差。
        数据只有在写缓存中累积到一定量之后，
        才会被发送出去，这样明显提高了网络利用率
        (实际传输数据payload与协议头的比例大大提高）。但是这又不可避免地增加了延时

        连续进行多次对小数据包的写操作，然后进行读操作，
        本身就不是一个好的网络编程模式；在应用层就应该进行优化。


启发:   
        对于既要求低延时，又有大量小数据传输
        还同时想提高网络利用率的应用，
        大概只能用UDP自己在应用层来实现可靠性保证了。
        好像企鹅家就是这么干的。
        lol: ping高了 会丢包, 你操作会有延迟, 而不是直接断掉,只有你网络连接彻底断开时,才会掉线
*/
            int nodelay = 1;
            //>重点<: 禁用ngal算法,允许 小数据包的单独发送, 减少延迟!
            //>提示<: 这里很重要
            if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
                LOG_ERROR("try set TCP_NODELAY failed, msg=%s", strerror(errno));
                close(client_fd);
                client_fd = -1;
            }

            //fd是有效地 ,几把他注册到epoll中去
            xfiber->TakeOver(client_fd);
            return std::shared_ptr<Connection>(new Connection(client_fd));
        }
        else { //没有监听到链接
            // ET 模式下 数据彻底读写完成
            //>提示<: 数据彻底读写完成,也就证明 这个fiber的工作结束了


/* 非阻塞accept , 在没有连接可以健力时,linux下返回 EAGAIN ,windows 下返回10035
https://blog.csdn.net/weixin_45921256/article/details/104627030?ops_request_misc=&request_id=&biz_id=102&utm_term=%E9%9D%9E%E9%98%BB%E5%A1%9E%20accept%20eagain&utm_medium=distribute.pc_search_result.none-task-blog-2~all~sobaiduweb~default-2-104627030.142^v9^control,157^v4^new_style&spm=1018.2226.3001.4187
这是因为在windows中，非阻塞模式抛出的异常是WSAEWOULDBLOCK，在linux环境下抛出的异常才是EAGAIN
测试：同样一份代码，在linux中能够正确执行
（如果必要的话在开头加上#coding=utf-8）
*/
            if (errno == EAGAIN) {// tryagain   没有链接可以建立
                WaitingEvents events;
                events.waiting_fds_r_.push_back(fd_);
                //>重点<: 没有链接可以建立, 就会立马切出负责建立连接的fiber, 去处理其他的待处理的fiber
                //>提示<: 被动套接字 一般就只用来 读取数据,所以是读事件类型
                xfiber->RegisterWaitingEvents(events); // 单纯的就是为了注册侦听套接字描述符到xfiber中
                /*>提问<:为什么要 把 侦听被动套接字的(唯一一个侦听协程)切出?
                    ==> 退出该侦听fiber 是为了尽可能的去运行其他fiber
                    ==> 一旦 侦听套接字被触发
                    ==> epoll侦听被动套接字, 被动套接字接收到syn报文时,会触发 epoll
                    ==> 触发 epoll后, epoll 会去找到 对应的 文件描述符(侦听套接字)
                    ==> 文件描述符 和对应的fiber对象存储在 map中, 这样我们就可以直接获取到对应 fd应该执行的上下文信息了
                    ==> 这样 侦听套接字fiber就又可以回来继续运行了
                */
                /*>提问<: 此时侦听 协程退出之后,下一次进入时什么情景?
                    ==> 此时 切出cpu, 会保存当前上下文信息.
                    ==> 下一次 由于 被动套接字接受数据,从而触发epoll
                    ==> epoll被触发后,通过手动调度 上下文切换,
                    ==> 使 的 侦听协程会回到当前上下文.
                    ==> 由于是一个死循环, 所以 侦听协程, 会立即 回到循环最开始的地方, 进行 accept
                    ==> 此时 accept成功
                */
                /*>提问<: 为什么 accept是一个死循环? 
                    是为了 整个流程的再现!
                    xfiber->SwitchToScheduler 的上下文 信息 就是 while(true) {...}

                    下一次fiber协程切换上下文信息回来时 会继续 执行整个accept流程
                */
                xfiber->SwitchToScheduler();
            }
            else if (errno == EINTR) { //陷入了某个系统调用
                LOG_INFO("accept client connect return interrupt error, ignore and conitnue...");
            }
            else {
                perror("accept");
            }
/* errno说明
EAGAIN (Try again ) : 写操作返回EAGAIN就是没有空间可以写,读操作返:wq回EAGAIN 就是没有没有数据可以读
    在读数据的时候,没有数据在底层缓冲的时候会遇到.
    比如我们epoll使用ET的时候，是需要在每次事件轮回中处理所有的消息，因为不然就得等下一次事件轮回才能处理了，
    因此我们基本用ET跟非阻塞fd一起使用，然后通过判断返回值-1时，errno为EAGAIN来判断已经读完了
EINTR(Interrupted system call) : 一句话: 陷入了系统调用
    被其它的系统调用中断了, 对于fd进行操作比较容易出现,一般
裸用recv都是需要判断的, 处理也很简单, 再进行一次操作就可以了
EWOULDBLOCK (Operation would block) : 有的系统是EWOULDBLOCK，而不是EAGAIN
EPIPE(Broken pipe) : 一句话: 接收端关闭,但是发送方还在发送导致破裂
    接收端关闭(缓冲中没有多余的数据),但是发送端还在write.

ECONNRESET(Connection reset by peer) : 收到RST包可能是接收到数据后不进行读取或者没读取
完毕直接close,另一端再调用write或者read操作,另外使用了SO_LINGER设置发送RST直接断开连接后
close连接,另一端也会收到这个错误. 另外在epoll中一般也是可能返回EPOLLHUP事件。连接的时候也
可能出现这样的错误

ETIMEDOUT (Connection timed out) : 连接超时
ECONNREFUSED (Connection refused) : 拒绝连接, 一般在机器存在但是相应的端口上没有数据的
时候出现

ENETUNREACH (Network is unreachable) : 网络不可达，可能是由于路器的限制不能访问,需要
检查网络

EADDRNOTAVAIL (Cannot assign requested address) : 不能分配本地地址,一般在端口不够用
的时候会出现,很可能是短连接的TIME_WAIT问题造成

EADDRINUSE (Address already in use) : 地址已经被使用, 已经有相应的服务程序占用了这个
端口, 或者占用端口的程序退出了但没有设置端口复用

*/
        }
    }

    //>提问<: 这又是个啥?
    return std::shared_ptr<Connection>(new Connection(-1));
}


Connection::Connection() {
}

Connection::Connection(int fd) {
    fd_ = fd;
}

Connection::~Connection() {
    XFiber::xfiber()->UnregisterFd(fd_);
    LOG_INFO("close fd[%d]", fd_);
    close(fd_);
    fd_ = -1;
}














/*思考: 如何保证读取数据的完整性?
    epoll侦听套接字, 没当事件发生,我们就继续执行 读取,直到 彻底读完,这样就保证了读取的完整
*/
std::shared_ptr<Connection> Connection::ConnectTCP(const char *ipv4, uint16_t port) {
    int fd = socket(AF_INET,SOCK_STREAM, 0);
 
    struct sockaddr_in svr_addr;
    memset(&svr_addr, 0, sizeof(svr_addr));
    svr_addr.sin_family = AF_INET;
    svr_addr.sin_port = htons(port);  
    svr_addr.sin_addr.s_addr = inet_addr(ipv4);  
 
    //连接服务器，成功返回0，错误返回-1
    if (connect(fd, (struct sockaddr *)&svr_addr, sizeof(svr_addr)) < 0)
    {
        LOG_ERROR("try connect %s:%d failed, msg=%s", ipv4, port, strerror(errno));
        return std::shared_ptr<Connection>(new Connection(-1));
    }

    int nodelay = 1;
    //能用ngal算法
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
        LOG_ERROR("try set TCP_NODELAY failed, msg=%s", strerror(errno));
        close(fd);
        return std::shared_ptr<Connection>(new Connection(-1));
    }

    //客户端连接建立成功 ,修改描述符为 非阻塞
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        LOG_ERROR("set set fd[%d] O_NONBLOCK failed, msg=%s", fd, strerror(errno));
        close(fd);
        return std::shared_ptr<Connection>(new Connection(-1));
    }
    LOG_DEBUG("connect %s:%d success with fd[%d]", ipv4, port, fd);

    // 已建立的链接fd 注册到xfiber调度器中去
    XFiber::xfiber()->TakeOver(fd);

    return std::shared_ptr<Connection>(new Connection(fd));
}
/*思考: 本项目是ET模式下非阻塞的套接字,如何保证写入数据的完整性?
        ET模式的 write策略:  ===>  只要可写, 就一直写, 直到数据发送完, 或者 errno = EAGAIN

    当 第一次write写满时,epoll侦听的套接字 会从 writeable -> unwriteable; 
            此时,由于是非阻塞套接字 会返回 -1 和errno ==eagain
            此时 说明 该fiber对象工作还没有做完
            就把 当前工作的 fiber 对象 + 所操作的 套接字fd加入到map中去
            同时让epoll 侦听该套接字, 
            然后让出当前cpu.
            ====> 当 这个套接字内的缓冲区 有空间可以写时,暨 从unwriteable -> writeable状态
                    此时epoll被触发,epoll_wait + 出参 event + map
                    我们就可以明确地知道我们该调用哪个协程
                    ==> 这样就可以保证 没有写完的 协程,继续书写
*/
ssize_t Connection::Write(const char *buf, size_t sz, int timeout_ms) const {
    size_t write_bytes = 0;
    //获取线程内单例xfiber
    XFiber *xfiber = XFiber::xfiber();
    // 是否设置有超时时间: 有则计算出死线
    int64_t expire_at = timeout_ms > 0 ? util::NowMs() + timeout_ms : -1;

    while (write_bytes < sz) {
        /*write函数 解释:
            原型: ssize_t write(int fd, const void *buf, size_t nbytes);
                参数: 
                    1. fd :套接字fd
                    2. buf: 缓冲区写入位置
                    3. nbytes: 为要写入的数据的字节数。
                返回值:
                    成功: 写入的字节数
                    失败 -1
                    结束: 0
            阻塞套接子 write:   
                        如果缓冲区被写满,则一直阻塞,知道有新的位置可以
            非阻塞套接字write:
                        如果没有地方可以写 ,写入失败,返回EAGAIN
        */
        int n = write(fd_, buf + write_bytes, sz - write_bytes);
        //写入成功
        if (n > 0) {
            write_bytes += n;
            LOG_DEBUG("write to fd[%d] return %d, total send %ld bytes", fd_, n, write_bytes);
        }
        //写入完成
        else if (n == 0) {
            LOG_INFO("write to fd[%d] return 0 byte, peer has closed", fd_);
            return 0;
        }
        else {
            //有超时时间 , 并且当前时间 > 死线 暨已经超时
            if (expire_at > 0 && util::NowMs() >= expire_at) {
                LOG_WARNING("write to fd[%d] timeout after wait %dms", fd_, timeout_ms);
                return 0;
            }
            //写入出错: 没有陷入系统调用,缓冲区也没有写满,就是单纯的 系统调用
            if (errno != EAGAIN && errno != EINTR) {
                LOG_DEBUG("write to fd[%d] failed, msg=%s", fd_, strerror(errno));
                return -1;
            }
            // 缓冲区写入 满了
            else if (errno == EAGAIN) {
                LOG_DEBUG("write to fd[%d] return EAGIN, add fd into IO waiting events and switch to sched", fd_);
                WaitingEvents events;
                events.expire_at_ = expire_at;
                events.waiting_fds_w_.push_back(fd_);
                // 缓冲区写满了. 需要把当前这个fd重新注册回xfiber调度器
                xfiber->RegisterWaitingEvents(events);
                //缓冲区写满, 则切换当前fiber 出去 干别的事情, 等待缓冲区有空间里 在由epoll来进行通知
                //>提问<: ET模式下 如何通知第二次?  epoll注册后,会一直监听被注册的fd,直到你取消注册
                //>提问<: ET模式下 触发一次后,还需要重新注册到epoll中去吗? 不需要,除非你手动取消注册
                xfiber->SwitchToScheduler();
            }
            else {
                //pass
            }
        }
    }
    LOG_DEBUG("write to fd[%d] for %ld byte(s) success", fd_, sz);
    return sz;
}

ssize_t Connection::Read(char *buf, size_t sz, int timeout_ms) const {
    XFiber *xfiber = XFiber::xfiber();
    int64_t expire_at = timeout_ms > 0 ? util::NowMs() + timeout_ms : -1;

    while (true) {
        /*read函数解释:
            原型: ssize_t read(int fd, void *buf, size_t count);
            参数:
                1.fd :要读取的文件的描述符
                2.buf:要接收数据的 缓冲区地址
                3.count: 为要读取的数据的字节数。
            返回值:
                > 0 : 成功读取的 数据字节数
                == 0: 表示读取完毕
                < 0 : 表示失败,返回一个errno   
                        | eagain : 
                        | eintr : 陷入系统调用或者中断
            阻塞套接字fd 调用read:
                        如果没有数据可以读,就阻塞,等到新的数据到来.
                        新的数据到来,就开始读数据,然后 返回读的数据数量
            非阻塞套接子 调用read:
                        errno = EAGAIN    在当前缓冲区中无可读数据
        read/write 和recv/send的区别:
            recv/send和read/write功能都差不多，只是recv/send提供了第四个参数来控制读写操作.
                ===========================================
                +  int recv(int sockfd,void *buf,int len,int flags)
                +  int send(int sockfd,void *buf,int len,int flags
                ===========================================
                第四个参数 flags: 
                        1. 0 : 和read/write 没区别
                        2. MSG_DONTROUTE:  不查找路由表 
                        3. MSG_OOB :    接受或发送带外数据 
                        4. MSG_PEEK :   查看数据,并不从系统缓冲区移走数据 |
                        5. MSG_WAITALL: 等待任何数据 
        */
        // 注意 read和writ 使用的 套接子 都是 非阻塞套接字: 协程, 加快效率
        int n = read(fd_, buf, sz);
        LOG_DEBUG("read from fd[%d] reutrn %d bytes", fd_, n);
        if (n > 0) {
            return n;
        }
        else if (n == 0) {
            LOG_DEBUG("read from fd[%d] return 0 byte, peer has closed", fd_);
            return 0;
        }
        else {
            if (expire_at > 0 && util::NowMs() >= expire_at) {
                LOG_WARNING("read from fd[%d] timeout after wait %dms", fd_, timeout_ms);
                return 0;
            }
            if (errno != EAGAIN && errno != EINTR) {
                LOG_DEBUG("read from fd[%d] failed, msg=%s", fd_, strerror(errno))
                return -1;
            }
            else if (errno == EAGAIN) { // 数据读取完毕: ET模式下非阻塞套接字,没有比必要继续等,这段时间要充分利用
                LOG_DEBUG("read from fd[%d] return EAGIN, add into waiting/expire events with expire at %ld  and switch to sched", fd_, expire_at);
                WaitingEvents events;
                events.expire_at_ = expire_at;
                events.waiting_fds_r_.push_back(fd_);
                //把当前的fd注册到xfiber中去
                //>提问<: 仔细想想? 这里和上边的几个注册,都只是单纯的 注册fd吗?
                //>解答<: 不是! 绝对不是!
                /*>重点<: 
                    1. 所有的fiber被调度后,进行运行的外壳都是统一且共享的 XFIBER调度器外壳
                    2. 所以: 所有的fiber 被调度运行商xfiber时 都能够使用xfiber 的成员变量成员函数
                    3. 只看外部: 其实全局 有且只有一中对象在运行
                    =========================================================================================
                    >重点<: 全局在运行的 不论何时 都是 xfiber  区别就是 xfiber内部的上下文在不断切换 +
                    //本项目最重要的一点                                  +
                    =========================================================================================
                    4. 在xfiber调度外壳的调度下,当前fiber 是先将自己 注册到xfiber中去,
                    5. 然后再进行切换 ===>registerwaitingevents内部 会使用cur_dispatch_fiber 作为被存储的fiber对象
                */
                
                xfiber->RegisterWaitingEvents(events);
                xfiber->SwitchToScheduler();
            }
            else if (errno == EINTR) {
                //pass
            }
        }
    }
    return -1;
}
