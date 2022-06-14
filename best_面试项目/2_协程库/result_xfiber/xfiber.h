
#pragma once

#include <set>
#include <map>
#include <list>
#include <queue>
#include <vector>
#include <string>
#include <functional>  
#include <ucontext.h> //协程切换的几个函数

#include "log.h" //日志模块
#include "util.h"

typedef enum {
    INIT = 0,
    READYING = 1,
    WAITING = 2,
    FINISHED= 3
}FiberStatus;



// XfiberCtx 就是这个类型
typedef ucontext_t XFiberCtx;

//>注意<:交换上下文的本质 就是 交换ctx
#define SwitchCtx(from, to) swapcontext(from, to)

// 这里是信号量
struct Sem {

	//>注意<: thread_local: 确保每一个县城里面 只有一个sem_seq
    thread_local static int64_t sem_seq;

    int64_t seq_;

    Sem(int32_t value);

    bool Acquire(int32_t apply_value, int32_t timeout_ms=-1);

    void Release(int32_t acquired_value);

    bool operator < (const Sem &other) const {
        return seq_ < other.seq_;
    }
};


//>注意<: 这里是 fiber的事件集合
struct WaitingEvents {
    WaitingEvents() {
        expire_at_ = -1;
    }
    void Reset() {
        expire_at_ = -1;
        waiting_fds_r_.clear();
        waiting_fds_w_.clear();
    }

    // 一个协程中监听的fd不会太多，所以直接用数组



    //fiber的waitingevents 管理 这个fiber下 处理的读写fd 以及信号量

    // 被监听的 读事件 fd数组
    std::vector<int> waiting_fds_r_;
    // 被坚挺的 写事件 的 fd数组
    std::vector<int> waiting_fds_w_;
    // 被监听的 信号量数组
    std::vector<Sem> waiting_sems_;
    //fiber的超时时间
    
    // 超时时间 是针对整个 fiber的 
    //>重点<: 一旦超时, 整个fiber失效, 同时 信号量 fd也全都不用管了 
    int64_t expire_at_;
};


class Fiber;

//>注意<: XFIber 是: 单线程进程中的 主线程, 同时 是主协程
//      负责调度其他所有的协程, 暨协程调度器
class XFiber {
public:
    XFiber();
    ~XFiber();

    //>注意<:wakeupfiber 函数 解决了 两次唤醒的问题!
    //>提示<:wakeupfiber 函数 负责 唤醒ready队列中的所有协程 
    void WakeUpFiber(Fiber * fiber);

    //>注意<:createfiber 函数 负责 初始化一个 fiber对象,暨初始化一个协程
    //>提示<: 该函数 参数 run 是一个函数指针,暨run是 该协程(fiber对象)的 入口函数
    void CreateFiber(std::function<void ()> run, size_t stack_size = 0, std::string fiber_name = "");

    //>重点<: 主协程 通过 该方法 调度其他协程, 同时该方法 , 是主协程的 主要上下文
    //>提示<: 该方法 调度的 对象 是 fiber 对象,暨其他的协程
    void Dispatch();

    //>重点<: xfiber调度类 通过使用该函数 ,使正在运行的协程, 主动让出cpu
    //>解答<: 暨 当前fiber对象(协程)没有运行完毕,自己主动让出cpu
    //>注意<: yield 之后 正在运行的协程,让出cpu之后 ,`cpu会回到主调度协程, 暨xfiber调度器的 dispatch方法
    //>提示<: 如何实现回到xfiber.dispatch? 依靠ucontext函数簇, 保证每一个fiber对象(协程) 的后续上下文都是 xfiber对象
    void Yield();

    //>提问<: 此函数和Yield有何区别?
    //>解答<: Yield 函数, 主动让出cpu后,让出cpu的原主人, 还有存在意义, 主动切出后仍然是ready状态
    //      而 switchtoschedfiber 只是单纯的让出cpu,
    //>重点<: 当前正在运行的 协程(fiber对象), 让出cpu, 去执行xfiber调度器的 dispatch上下文
    //>重点<: Yield 的子调用
    void SwitchToScheduler();


    //>注意<: 将当前 文件描述符fd 以及 当前的fiber对象(正在cpu上运行,也就是当前正在运行的协程本身的上下文信息)
    //      打包为一个整体,注册到xfiber调度器中
    //>提问<: 为什么要这样做? 
    //>解答<: 
    void RegisterWaitingEvents(WaitingEvents &events);
    
    //>注意<: 把fd从xfiber中移除 
    //>提问<: 为什么需要这样做?
    //>解答<:  一个套接字fd已经彻底关闭, 暨 不会再有数据的读写
    //     此时我们需要把 fiber对象以及该fd 从map中移除,并且fiber对象要删除
    bool UnregisterFdFromScheduler(int fd);







    //>注意<: 回顾上面的 xfiber构造函数, 就知道我们该结构体的作用
    //>重点<: thread_local and 单线程内 多携程的情况下实现 单例
    //>提问<: 为什么要这样做?
    //>解答<: 设计目的
        //1.使得 单线程内多携程的情况下, xfiber对象 暨协程调度器 是单例
        //2. 如果是多线程的环境下 , 每一个线程内部 可能有很多个协程, 
        //  每一个线程内 有且只有一个协程调度器,且协程调度器之间彼此互不影响

    //>重点<: thread_local
    /*
    知识点: thread_local 和 gcc的 __thread

        thread_local / ++thread: 每个线程都拥有该变量的一份拷贝，且互不干扰。
                    +++++++++++++++++++++++++++++++++++++++++++++++
                    + 线程局部存储中的变量将一直存在，直至线程终止，
                    + 当线程终止时会自动释放这一存储
                    +++++++++++++++++++++++++++++++++++++++++++++++
        __thread: 只支持POD类型

        thread_local: 基本上所有类型都可以用

    */ 
    static XFiber * xfiber()
    {
        static thread_local XFiber xf;
        return &xf;
    };
    




    //>注意<: 一定需要该函数, 我们需要获取到 xfiber调度类的上下文信息,来进行保存
    //>重点<: 有了存根, 就可以在子协程 回到调度协程时,才有家可归
    // ucontext_t * Get_X_Fiber_Ctx();
	XFiberCtx * SchedCtx();




    //>注意<:将某个fd注册到epoll中去
    void TakeOver(int fd);
    //>注意<: 将某个函数从epoll中取消注册
    bool UnregisterFd(int fd);

    //>注意<: 计算并设置超时时间(死线)
    void SleepMs(int ms);



	//>注意<: 信号量  注册
    void RegisterSem(Sem *sem, int32_t value);
    //sem_infos.insert(std::make_pair(*sem, SemInfo(value)));

	//>注意<: 信号量  取消注册
    void UnregisterSem(Sem *sem);

	//>注意<: 信号量 尝试获取
    bool TryAcquireSem(Sem *sem, int32_t value);

    // release和restore的区别
    // restore时是可申请的
    // release时已申请完成的
	//++++++++++++++++++++++++++++++
	//>注意<: 信号量  释放
    void ReleaseSem(Sem *sem, int32_t value);

	//>注意<: 信号量 恢复
    void RestoreSem(Sem *sem, int32_t value);












private:
    //>提示<: 该变量 表示 epoll初始化后的fd,epoll通过一个句柄来管理反应堆
    //>提示<: 原名 efd_;
    int ep_fd_;


    // 两个deque,存储fiber对象
    //>提问<: 为什么使用deque ? list 不行吗
    //>解答<: list可以 deque更好
    //  list 和deque 在扩容时 deque更好一点
    //>重点<:  待补充 
    std::deque<Fiber *> ready_fibers_;
    std::deque<Fiber *> running_fibers_;

    //>重点<:当前正在运行的fiber对象
    //>提问<: 为什么需要保存这个fiber对象?
    //>解答<: xfiber调度器调度fiber对象后, 如果fiber对象,yield让出cpu,之后 xfiber调度器,回归后,还可以通过该变量
        //  来掌握 到底是谁让出cpu了
    //>注意<: 期待后续详细解释
    Fiber * cur_dispatch_fiber_;

    //>提示<: xfiber调度器当上下文信息
    ucontext_t sched_ctx_;


    //>注意<: 此结构体设计用意:
    //>重点<: 我们是使用epoll 来监听fd,被监听的fd和 fd对应的 协程 存在了map中
        // 一个 fd 可能对应 一个  可读事件和  一个可写事件
        // 暨 key1= pair(int fd, fiber * w_);
        //   key2= pair(int fd, fiber * r_);

	struct WaitingFibers {
        Fiber *r_, *w_;
        WaitingFibers(Fiber *r = nullptr, Fiber *w = nullptr) {
            r_ = r;
            w_ = w;
        }
    };


	//>注意<: 信号量结构体
	struct SemInfo {
        SemInfo(int32_t value = 0) {
            value_ = value;
            acquiring_fiber_ = nullptr;
            acquired_value_ = 0;
            fibers_.clear();
        }

        int32_t value_; 
        // 正在尝试获取的fiber
        Fiber *acquiring_fiber_;
        // 正在尝试获取的value_
        int32_t acquired_value_;
        std::set<Fiber *> fibers_;
    };

    
    //>提问<: 会不会出现一个fd的读/写被多个协程监听？协程只负责 处理,epoll负责监听
    //>解答<: 不会！为什么? epoll 监听到时见发生,会立即将他出队,然后交给对应协程处理
    //>注意<: 一个fiber也不可能会监听多个fd
    //>重点<: 一个连接由一个协程处理
    //>重点<: 但是一个 协程 可能会处理多个fd(链接)

	//>提问<: 为什么使用map?
	//>解答<: 1.fd是有序的,fd基本上是不可能重复的
	//	   2. 所以: 有序+无重复,基本上就是红黑树了
    std::map<int,WaitingFibers> io_waiting_fibers_;
	
	//===============================================================
	//>注意<: 超时队列 
    // 也用红黑树,因为后面需要begin
    //>重点<: 超时队列的作用:
    //>提示<: 超时队列内, 如果尚未超时,则dispatch 不会进行上cpu,如果已经超时,则会进行上cpu进行调用
    //>提问<: 超时的fiber 会被自动删除吗?  会! 超时了 就会交给dispatch去调度
        //==> this->expire_events_.begin()->first <= now_ms : 暨超时死线 < 当前时间 ===已经超时
            //==> 会一个一个的wakeup
    //>提问<: 为什么会超时后的fiber自动删除?
        //======================> 我任由他 卡在超时队列里面! 但是一旦我需要 唤醒这个fiber进行工作室 wakeupfiber 会吧 他唤醒加入到ready队列
        //======================> 保证了 功能的不丢失
	std::map<int64_t, std::set<Fiber *>> expire_events_;

    // 已经结束的 fiber数组
    std::vector<Fiber *> finished_fibers_;

    // 信号量集合
    std::map<Sem, SemInfo> sem_infos_;
	//===============================================================

};
    
    

class Fiber {
public:
    //>注意<: 这点我不懂
    Fiber(std::function<void ()> run,XFiber * xfiber,size_t stack_size,std::string fiber_name);
    
    ~Fiber();

    //源名 Ctx
    ucontext_t * Get_Fiber_Ctx();
    
    std::string Name();

    bool IsFinished();

    uint64_t Seq();

    //>注意<: 静态函数
    //>提问<: 为什么这里是静态函数? 有什么好处?
    //>解答<:  1.如果是成员函数,那么 makecontext时 
    //      并不是很好绑定(如果刚开始类没有实例化时,start函数为null) 这样并不是很好
    //     2.静态函数,可以保证任何时候这个函数都是存在的
    //>提问<: 参数是什么? 参数就是fiber对象,fiber对象就是协程
    static void Start(Fiber * fiber);


	struct FdEvent
	{
		int fd_;
		int64_t expired_at_;
		FdEvent(int fd = -1,int64_t expired_at = -1) {
		    if (expired_at <= 0) {
                expired_at = -1;
            }
            fd_ = fd;
            expired_at_ = expired_at;
		}
	};
	//>注意<: 获取等待事件集合
	WaitingEvents &GetWaitingEvents() {
        return waiting_events_;
    }
	//>注意<: 设置等待事件集合
	void SetWaitingEvent(const WaitingEvents &events);

private:
    //>提问<: seq的作用是什么?为什么需要特地保留
    uint64_t seq_;

    XFiber *xfiber_;

    std::string fiber_name_;

    //>注意<: 协程状态 0 1 2 3 
    // 初始化 准备 等待 结束
    FiberStatus status_;
    
//=====================================
/*
    ucontext_t 系列参数
    uc_link : ucp结构可以形成一个链表
    uc_flage:

    uc_stack: ucp结构堆栈信息:
        uc_stack.ss_sp : 栈顶指针
        uc_stack.ss_size : 栈空间大小
        uc_stack.ss_flage : 
        
        协程分类,有栈协程和无栈协程
    

    uc_mcontext :存储当前上下文 ===>各种各样的寄存器信息
        注意: get/set_context修改的都是 这里面的
         makecontext: 将后续上下文入口函数地址,也是保存在 这些寄存器信息里面的
    uc_sigmask:信号屏蔽掩码
*/

    ucontext_t ctx_;
    uint8_t *stack_ptr_;
    size_t stack_size_;

//=====================================

    
    //>提示<: 原名 func_
    //该协程的入口函数 (makecontext会绑定协程的入口函数地址并且,进行上下文切换)
    std::function<void ()> entry_func_;

	//>注意<: 等待事件集合
    // 一个 fiber可以处理多个 fd 但是一个 fd只能交给一个fiber处理
    // 一个fiber处理多个fd时 就直接分发给多个fiber算了
	WaitingEvents waiting_events_;


};


/* epoll 10大相关问题
        问题1: 为什么要使用epoll
        问题2: epoll有什么好处? 为什么不适用select?或者是poll?
        问题3: 你这是水平触发还是边缘触发? ET \ LT ?
        问题4: ET LT和socket的阻塞关系?
        问题5: 为什么epoll可以支持百万级别的访问?
        问题6: epoll时完美的吗? 他无法解决什么问题? 该问题又该如何解决
                 答: epoll无法 解决 每一个 socket各自的timeout问题
                   int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout); 
                   多个 ep_fd_ 公用一个 timeout
                     解决方法: libevent库      
        问题7: LT / ET 模式下 accept的问题 and ET模式下的 如何确保读写完整数据:(阻塞socket和非阻塞socket) ===> 重点
        问题8: ET模式为什么非要设置在 非阻塞套接字? (阻塞套接字 会导致无法调用epoll_wait而是其他就绪套接字饿死)
        问题9: ET/LT 适用场景? 为什么?
        问题10 : epoll事件有哪些?
*/
/* epoll问题解答:  
        问题1 2 解答:
                select poll epoll三者的区别 暨各自的优缺点
                    select、poll、epoll之间的区别(搜狗面试):https://www.cnblogs.com/aspirant/p/9166944.html

        问题三3解答:
            LT \ ET:可以得出这样的结论:
                  ET模式:   
                        读写触发情况:
                                        1.读操作:
                                                a. 当缓冲区由unreadable ==> readable，即缓冲区由空变为不空的时候。
                                                b. 当有 新数据到达时 ，即缓冲区新增数据时
                                                c. 当缓冲区有数据可读，且epoll_fd被修改EPOLLIN事件时。(updateevents(efd,fd,epoll_in,epoll_cmd_ctl))
                                                        ===> 看后面的问题,et代码
                                        2.写操作:
                                                a. 当缓冲区有 unwrite ==> write时, 暨缓冲区 由不可写,变为可写
                                                b. 当有 旧数据被发送走，即缓冲区中的数据减少
                                                c. 当缓冲区有空间可写，且应用进程对相应的描述符进行EPOLL_CTL_MOD 修改EPOLLOUT事件时。
                                        
                        通知次数:  只会通知一次,
                        数据丢弃:  没有读写完的数据会全部丢弃,下一次通知时,就是读写另一段数据( 除非有新的数据再次到达)
                                ==> 
                                  如果这次没有把数据全部读写完(如读写缓冲区太小)，
                                  那么下次调用epoll_wait()时，它不会通知你
                                ==> 如果read一次没有读尽buffer中的数据，那么下次将得不到读就绪的通知，造成buffer中已有的数据无机会读出，除非有新的数据再次到达
                          -------------------------------------------------
                          | 这里所谓的状态的变化并不包括缓冲区中还有未处理的数据,
                          | 也就是说,如果要采用ET模式,需要一直read/write直到出错为止,
                          | 很多人反映为什么采用ET模式只接收了一部分数据就再也得不到通知了,
                          | 大多因为这样;
                          ---------------------------------------------------
                  LT模式: 只要缓冲区有数据就会一直触发(准确:只要某个 socket 处于 readable/writable 状态，无论什么时候进行 epoll_wait 都会返回该 socket)
                        读写触发情况:
                                1. 读操作:      
                                        a. 新到数据: 暨 从 unreadable -> readable;
                                        b. 还有数据:   处于EPOLLIN状态
                                                因为: LT内部逻辑: 没有发完,就继续保持EPOLLIN
                                                if(一次性发完) updateevents(ep_fd,fd,EPOLLOUT,EPOLL_CMD_CTL);
                                                else    updateevents(ep_fd,fd,EPOLLOUT | EPOLLIN,EPOLL_CMD_CTL); 
                
                                2. 写操作:      
                                        a. 从 unwriteable -> writeable 
                                        b. 没有一次写完,暨 任何处于writeable状态时
                                            同上
                        通知次数: 会通知你很多次
                        数据丢弃: 不会丢弃数据: 下一次通知会继续读写该数据
                                  ==> 一段数据没有读完,下次调用epollwait时,会继续读写该段数据
                        适用场景:
                        --------------------------------------------------
                        | 其中LT就是与select和poll类似
                        | 当被监控的文件描述符上有可读写事件发生时，(处于read/write状态)
                        | epoll_wait()会通知处理程序去读写。
                        | 如果这次没有把数据一次性全部读写完(如读写缓冲区太小)，
                        | 那么下次调用 epoll_wait()时，
                        | 它还会通知你在上次没读写完的文件描述符上继续读
                        --------------------------------------------------
                ET \ LT: 结论
                         ET模式仅当读写状态发生变化的时候才获得通知,
                         这里所谓的状态的变化并不包括缓冲区中还有未处理的数据,也就是说,
                         如果要采用ET模式,
                         需要一直read/write直到出错为止,
                         很多人反映为什么采用ET模式只接收了一部分数据就再也得不到通知了,
                         大多因为这样;
                         而LT模式是只要有数据没有处理就会一直通知下去的.
                            ===================================================
                            |二者的差异在于 
                            | level-trigger 模式下只要某个 socket 处于 readable/writable 状态，
                            | 无论什么时候进行 epoll_wait 都会返回该 socket；
                            | ++++++++++++++++++++++++++++++++++++++++++++++++
                            | 而 edge-trigger 模式下只有某个 socket
                            | 从 unreadable 变为 readable，或从unwritable 变为writable时，(暨读写状态改变)
                            | epoll_wait 才会返回该 socket。
                            =======================================================
                ET和LT如何选择?
                        ET : 
                            适用场景: 实时传输.  处理大数据使用non-block模式的socket。
                            优点:   每次内核只会通知一次(fd状态变为 可读 / 可写时)，
                                    大大减少了内核资源的浪费，提高效率。
                            缺点:   步保证数据的完整性
                                    (只通知一次, 每次一直读写,知道读写出错, 后续的数据会丢掉)
                            
                        LT :
                            适用场景: 吞吐量大,多路复用
                            优点:   保证数据完整性
                                    进行IO操作的时候，如果还有数据，就会一直的通知你。
                            缺点:   效率低下
                                    ==================================================
                                    +   由于只要还有数据，内核就会不停的从内核空间转到用户空间，
                                    +   所有占用了大量内核资源，
                                    +   试想一下当有大量数据到来的时候，每次读取一个字节，
                                    +   这样就会不停的进行切换。内核资源的浪费严重。效率来讲也是很低的。
                                    ==================================================
                ET / LT 谁的性能更优? : 具体情况具体分析
                        http://www.cppblog.com/Leaf/archive/2013/02/25/198061.html
                        • epoll 的 ET 和 LT 的 模式处理 逻辑差异极小，
                            性能测试结果表明常规应用场景 中二者性能差异可以忽略。
                        • ET 的 使用逻辑复杂，出错概率更高。
                        • ET 和 LT  性能差异主要在于 epoll_wait 系统调用的处理速度，
                            是否是 user app 的性能瓶颈需要视应用场景而定，不可一概而论。
                ET 为什么更加高效?: 建立在 你不关心 数据完整性的基础上
                        
                        当系统中有大量 就绪 fd,并且 你不关心数据完整性时
                        选择 ET,会让你 每一个fd 只读写1遍
                        选择 LT,会让你每一个fd, 读写很多遍, 暨 调用多次 epoll_wait
                        所以 当你不关系数据完整性时,ET更优
                ET 模式下 数据处理问题:
                        数据丢弃: 的前提是,后续 对同一个epollfd,没有再次触发
                                如果再次触发, 则 不会产生数据丢弃
                        et保证了 每一次被触发后,一口气,读写 ,直到出错,,出错后未处理的数据数据 不做处理
                                但是同一个fd可能会被多次触发, 上一次为被处理的数据,会再次处理
                
        问题4解答:  ET LT和socket的阻塞关系?
                1. 非阻塞socket
                        LT模式: 对于select和epoll的LT模式，这种读写方式是没有问题的
                                LT模式: 非阻塞,读写时,没有数据会返回 EAGAIG(E again)
                                读： 忽略掉errno = EAGAIN的错误，下次继续读；
                                写： 忽略掉errno = EAGAIN的错误，下次继续写。
                        ET模式: 
                                读:只要可读,就一直读，直到返回0，或者 errno = EAGAIN
                                写:就一直写,直到数据发送完，或者 errno = EAGAIN
                2. 阻塞 socket:ET会造成饥饿
                        LT 模式:   阻塞时 会返回EAGAIG
                                   读/写: 等待下次调用,然后继续读写该数据
                                
                        ET 模式: ET模式要求使用 `非阻塞套接字`: 阻塞的套接字会造成饥饿
                                 ET是高速工作方式，只支持no_block socket,它效率要比LT更高
                                阻塞时,会 返回 EAGEIN
                                读/写: 由于epoll,et 只会读写同一段数据 一次, 所以 后续数据会丢弃
                           

        问题5解答: 为什么epoll可以支持百万级别的连接？
                关键点: 反应堆结构:
                        ==> 1. 红黑树: 红黑树以fd为排序依据, 是在红黑树上查找
                            2. epollepoll通过注册fd上的回调函数，回调函数监控到有事件发生，就放入就绪链表

                        =====================================================   
                        |   在server的处理过程中，大家可以看到其中重要的操作是，
                        |   使用epoll_ctl修改clientfd在epoll中注册的epoll_event, 
                        |   这个操作首先在红黑树中找到fd对应的epoll_event, 然后进行修改，
                        |   红黑树是典型的二叉平衡树，其时间复杂度是log2(n), 1百万的文件句柄，
                        |   只需要16次左右的查找，速度是非常快的，支持百万级别毫无压力
                        |   另外，epoll通过注册fd上的回调函数，回调函数监控到有事件发生，
                        |   则准备好相关的数据放到到就绪链表里面去，这个动作非常快，成本也非常小
                        =====================================================   
        问题8 解答: ET 为什么要使用非阻塞套接字?    别面阻塞在文件内,而无法调用epoll_wait,从而导致的其他ep_fd的饿死
                ==============================================
                +   因为ET模式下的读写需要一直读或写直到出错
                +   （对于读，当读到的实际字节数 < 请求字节数时就可以停止），
                +   而如果你的文件描述符如果 是 阻塞的 BLOCK
                +   那这个  一直读或一直写  势必会在  最后一次阻塞。(文件描述符数学)
                +   这样就不能在阻塞在epoll_wait上了，(epoll_wait不会执行)
                +   造成其他文件描述符的任务饿死。
                ======================================================
        问题9解答: ET/LT 适用场景 暨解答 : et lt 代码: https://blog.csdn.net/qq_29257201/article/details/114263770?spm=1001.2101.3001.6650.5&utm_medium=distribute.pc_relevant.none-task-blog-2%7Edefault%7EBlogCommendFromBaidu%7Edefault-5.pc_relevant_default&depth_1-utm_source=distribute.pc_relevant.none-task-blog-2%7Edefault%7EBlogCommendFromBaidu%7Edefault-5.pc_relevant_default&utm_relevant_index=10
                ET模式: 
                        1. 高流量情景:  比如nginx就是使用et模式，
                        2. 大文件(大数据):  比如发送大文件et模式较为使用。
                LT模式: 一般情况
                        比如redis就是LT模式。LT: 适用场景: 普遍情况,
                为什么? 从代码逻辑分析
                    LT代码逻辑:
                        ==================================================
                        +   1. 当epoll实例监听套接字有读事件触发时，将新产生的 连接套接字注册到epoll实例中，事件类型为
                                    EPOLLIN。
                        +   2. 当连接套接字有请求时，激发读事件，此时我们读取套接字内容并进行处理，接着发送一个reponsd
                        + ================================================
                        +   3. 区分条件: 是否一次性发完
                        +       if  一次性发完:      updateEvents(efd, fd, EPOLLIN, EPOLL_CTL_MOD)
                        +                            //一次性触发完成后,epollout没有了,就意味着从write -> unwrite,          
                        +       else 没有一次性发完: updateEvents(efd, fd, EPOLLIN | EPOLLOUT, EPOLL_CTL_MOD);
                        +                             // 没有一次发完, 就继续发,继续保持epollout
                        ===================================================
                    ET 代码逻辑:
                        ==================================================
                        +   1. 当epoll实例监听套接字有读事件触发时，将新产生的 连接套接字注册到epoll实例中.事件类型为
                                    EPOLLIN | EPOLLOUT | EPOLLRT。
                        +   2. 当连接套接字有请求时，激发读事件，此时我们读取套接字内容并进行处理，接着发送一个reponsd
                        + ================================================
                        +   3. epoll一定会一次性发完: 
                        +           updateEvents(efd, fd, EPOLLIN, EPOLL_CTL_MOD);
                        +               // 如何确保一次发完, 
                                        // 管你发完没有发完, 立即将epollout状态去掉,保证不会因为epollout状态被触发
                                        //只有下一次 epollout状态回归才能被再次触发
                        ===================================================
                        ====> 大数据,大文件的情况下,LT会多次 改变套接字的事件类型 (updateevent改变套接字事件类型)
                                而ET 不会多次改变套接字事件类型
        问题10 解答: epoll事件大全
                1.  EPOLLIN       连接到达；有数据来临；
                2.  EPOLLOUT      有数据要写
                ==============================================
                3.  EPOLLRDHUP    这个好像有些系统检测不到，可以使用EPOLLIN，read返回0，删除掉事件，关闭close(fd);
                4.  EPOLLPRI      外带数据
                5.  EPOLLERR      只有采取动作时，才能知道是否对方异常。即对方突然断掉，是不可能
                                  EPOLLERR 是服务器这边出错（自己出错当然能检测到，对方出错你咋能知道啊）
        问题7: 重点解答:
                7.1 如何保证在ET模式下的数据读写完整性?
                        方法策略:
                                对于读: 只要可读, 就一直读, 直到返回 0, 或者 errno = EAGAIN
                                对于写: 只要buffer还有空间  && 用户请求写的数据还未写完
                                        ==> 就一直写
                        前提: 使用非阻塞套接字(目的: 避免 阻塞在套接字内,无法调用epoll_wait从而使其他套接字饿死)
                                
                        伪代码: 
                                读操作:只要可读, 就一直读, 直到返回 0, 或者 errno = EAGAIN
                                        ==>     read返回0: 没有数据可以读了
                                                EAGGIN: 没有数据可以读了
                                                read返回 -1 && errno!= EAGAIN :反正 没有读完, 就perror("readerror")
                                        ==> 最后记得 更改ep_fd的状态: 变成 EPOLL_OUT去掉
                                                =============================
                                                //   ev.data.fd = fd;
                                                //   ev.events = events[i].events | EPOLLOUT; 改变状态 epollin不见了
                                                //   epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
                                                ================================
                                写操作:
                                        默认前提: 使用非阻塞socket
                                                重点: 注意 usleep()
                                                        ===> 如何解决 缓冲区写满,但是 数据并没有彻底写完的情况?
                                                        ===> usleep一会,等待处理完成后在继续写
                                                        ===> 为什么可以这样? 因为这是非阻塞套接字,他不会阻塞在文件哪里,也不会导致饿死
                                                                但是你需要保证他的 扯扯弟弟的写完
                                                伪代码:
                                                        tmp = write(sockfd, p, total);
                                                        if(tmp < 0) && if(errno == EINTR)// 陷入了某一个系统调用而导致的写入失败,
                                                        if(tmp < 0) && if(errno == EAGAIN)// 写入缓冲区满了,而导致的temp < 0
                                                                  usleep(1000);continue; //说不定后面还有数据呢,我们等缓冲区消化完毕再继续
                                                        if(temp>= 0) //写入成功, 返回写入字节数
                                        不听话: 使用阻塞的socket:
                                                而如果你的文件描述符如果不是非阻塞的
                                                那这个一直读或一直写势必会在最后一次阻塞。
                                                这样就不能在阻塞在epoll_wait上了，
                                                造成其他文件描述符的任务饿死。
                7.2 ET模式下 ACCEPT的实现:
                        考虑这种情况：多个连接同时到达，服务器的 TCP 就绪队列瞬间积累多个就绪链接
                                由于是边缘触发模式，epoll 只会通知一次，accept 只处理一个连接，导致 TCP 就绪队列中剩下的连接都得不到处理。
                        解决办法: 
                                ===================================
                                + 用 while 循环抱住 accept 调用，
                                + 处理完 TCP 就绪队列中的所有连接后再退出循环。
                                + 如何知道是否处理完就绪队列中的所有连接呢？ 
                                + accept 返回 -1 并且 errno 设置为 EAGAIN 就表示所有连接都处理完。
                                ===================================
*/