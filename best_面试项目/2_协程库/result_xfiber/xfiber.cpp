
#include <stdio.h>
#include <errno.h> //是C语言C标准函式库里的标头档，定义了通过错误码来回报错误资讯的宏。
#include <error.h>//error系列函数是Linux系统编程中，一种debug的方式，
#include <cstring>
#include <iostream>
#include <sys/epoll.h>//epoll
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <assert.h>//断言
#include "xfiber.h"

XFiber::XFiber() {
    //XFiber 调度器初始化时 没有任何fiber被调度
    this->cur_dispatch_fiber_ = nullptr;

    //>注意<: 这里是deque
    this->ready_fibers_ = std::deque<Fiber *>();
    this->running_fibers_ = std::deque<Fiber *>();
    // epoll 初始化
    //>注意<: epoll_create 和 epoll_create1
    this->ep_fd_ = epoll_create1(0);
    if (ep_fd_ < 0) {
        LOG_ERROR("epoll_create failed, msg=%s", strerror(errno));
        exit(-1);
    }


/*
    睁大眼睛: epoll_create和epoll_create1
        第一级调用: int epoll_create(int size); size参数 只是对内核的建议,没啥用
        第二级调用: int epoll_create1(int flags);
        第三级调用: ep_alloc()创建内部数据(eventpoll)
            -------------------------------
            + 1.初始化epoll文件等待队列（双向链表）
            + 2.初始化eventpoll文件唤醒队列（双向链表）
            + 3.初始化就绪队列（双向链表）
            -----------------------------------
        拓展: epoll_create1的参数flag
            flag == 0 : 和 epoll_create() 一样: 创建一个 epoll实例
            flag == EPOLL_CLOEXEC
                在文件描述符上面设置执行时关闭（FD_CLOEXEC）标志描述符。
*/
}
XFiber::~XFiber(){
    //关掉epoll
    close(this->ep_fd_);
}

//获取正在 xfiber上调度的fiber的ctx (fiber上了xfiber后 ctx替换)
XFiberCtx* XFiber::SchedCtx(){
    return &sched_ctx_;
}


//>注意<: 新建一个fiber, 只是简单的将他加入到ready队列中去
void XFiber::CreateFiber(std::function<void ()> run, size_t stack_size, std::string fiber_name) {
    if (stack_size == 0) {
        stack_size = 1024 * 1024;
    }
    Fiber *fiber = new Fiber(run, this, stack_size, fiber_name);
    ready_fibers_.push_back(fiber);
    LOG_DEBUG("create a new fiber with id[%lu]", fiber->Seq());
}










// 唤醒某一个协程,上调度器
// 唤醒时,需要处理: fiber协程对应的 可读可写事件 以及 超时事件
// 如何处理:  1. 该fiber加入就绪队列
//       2. 从 xfiber 的 等待队列中(map) 删除 该fiber 对应的 可读 可写 事件(fd)
//       3. 从 xfiber 的 超时队列 中 删除 该fiber 对应的 超时事件
//     =======> 理由 wakeup唤醒后 加入 dispatch ,按顺序上调度器, 上了调度器之后 会 处理fiber自身的读写超时事件
//      ===> 处理完 ,本来就应该删除,只不过这里是提前了一点点而已
void XFiber::WakeUpFiber(Fiber * fiber) {

    // DEBUG_ENABLE : 默认为0 LOG_DEBUG(fmt,...)
    // LOG_DEBUG("try wakeup fiber[%lu] %p", fiber->Seq(), fiber);
//第一步: 被唤醒的 fiber ,要先加入就绪队列
    this->ready_fibers_.push_back(fiber);
  LOG_INFO("try wakeup fiber[%lu] %p, %d fibers ready to run", fiber->Seq(), fiber, ready_fibers_.size());


//第二步: 从全局等待队列中删除该fiber对应的读事件,写事件
    //>提问<: 等待队列是哪个? running是运行 ,ready是就绪
    //>解答<:  等待队列 是 那个 map
        // 2.1 先获取 fiber的等待事件集合 ==> waiting_events
            // >重点<: 一个 fd只能交给 一个 协程(fiber)处理
            //      +_+ 但是 一个 fiber 可以处理多个fd
    

    //>重点<: 一个fiber 可以对应很多个 很多个 fd事件 每个fd事件交给一个 新的 fiber去处理
    //      ==> 所以这样就有了 waitingevent
    //每一个fiber 都有一个 waitingevent 里面包含了 这个fiber 负责的fd对象
    // 现在 这个fiber即将被 wakeup 所以也需要 将这些fd处理掉
    WaitingEvents & waiting_events = fiber->GetWaitingEvents();
        // 2.2 在 对 waiting_events 内 所有的 fd 进行处理: 可读fd 可写fd 超时队列
    for (size_t i = 0;i < waiting_events.waiting_fds_r_.size();i ++) {

        int fd = waiting_events.waiting_fds_r_[i];
        //>注意<: io_waiting_fibers_ 就是那个 map
        auto iter = io_waiting_fibers_.find(fd);
        if (iter != io_waiting_fibers_.end()) {
            io_waiting_fibers_.erase(iter);
        }
    }

    for (size_t i = 0;i < waiting_events.waiting_fds_w_.size();i ++) {
        int fd = waiting_events.waiting_fds_w_[i];
        auto iter = io_waiting_fibers_.find(fd);
        if (iter != io_waiting_fibers_.end()) {
            io_waiting_fibers_.erase(iter);
        }

    }
//第三步: 从 全局超时队列中 删除掉对应的fd事件
    //获取 fiber设置的超时时间
    int64_t expire_at = waiting_events.expire_at_;
    if (expire_at > 0) {
        //expire_at_ 默认为 -1;
        //expire_events_: xfiber的超时队列
        auto expired_iter = this->expire_events_.find(expire_at);

        //std::map<int64_t, std::set<Fiber *>> expire_events_; 
        if (expired_iter->second.find(fiber) == expired_iter->second.end()) {
            // key: expire_at val: set()(fiber ,fd)
            // 如果 set中 没有这个val 警告
            // 没有找到
            LOG_WARNING("not fiber [%lu] in expired events", fiber->Seq());
        }
        else {
            // fiber超时 已经设置了, fiber被调度上 xfiber,所以 带有超时时间的也去掉(在这段时间内一并处理掉)
            // 打印debug信息
            LOG_DEBUG("remove fiber [%lu] from expire events...", fiber->Seq());
            //从map中删除这个 val
            expired_iter->second.erase(fiber);
        }
    }
    LOG_DEBUG("fiber [%lu] %p has wakeup success, ready to run!", fiber->Seq(), fiber);
}   



//>重点<: xfiber调度器 调度策略
void  XFiber::Dispatch() {
    //>注意<: 死循环! 也就是说 上下文一直都在这个循环内,无法跳出Dispatch
    //      ==> fiber上调度器 后也会一直在 这个 dispatch 内运行
    while(true) {
// 情况1: 就绪队列 不为空  ==> 处理就绪队列
        if (this->ready_fibers_.size() > 0 ) {
            // 完美转发: 实现==>万能引用 + 应用折叠
            // 功能: 将所有的 准备队列 全部 转换成就绪队列, 从而运行
            this->running_fibers_ = std::move(this->ready_fibers_);
            this->ready_fibers_.clear();
            LOG_DEBUG("there are %ld fiber(s) in ready list, ready to run...", running_fibers_.size());
        
            //>注意<: 现在需要处理所有的 running队列
            // 策略: 先来先服务
            // 遍历运行队列 ,处理每一个fiber
            for (auto iter = this->running_fibers_.begin();iter != this->running_fibers_.end();iter ++) {
                Fiber * fiber = * iter; // 获取即将上调度器的fiber
                this->cur_dispatch_fiber_ = fiber; //>注意<: 提前保存该对象,考虑到调度途中 万一需要让出cpu(暨YILED)
                LOG_DEBUG("switch from sched to fiber[%lu]", fiber->Seq());
                // assert(SwitchCtx(SchedCtx(), fiber->Ctx()) == 0);
                //#define SwitchCtx(from, to) swapcontext(from, to) 交换ctx
                //>重点<: XFiber 调度器 如何调度? swapcontext来进行CPU上下文切换
                /*
                    解释: 
                        1.当前cpu上运行的是XFIBER调度器,
                            ==> 所以当前cpu上下文也是 XFIBER的上下文
                        2.XFIBER即将调度fiber对象上cpu运行,
                            ==> 所以就需要保存 当前 XFIBER的 上下文信息, swapcontext(当前XFIBER的ctx,fiber的ctx)
                                    ==> 交换后: xfiber的ctx 就是 fiber的ctx ,暨相当于调度上了fiber
                                    ==> cur_distance_fiber 就是 fiber的ctx
                            ==> 并且 使用 cur_distance_fiber 的上下文信息来进行运行 fiber对象
                        3. 由于 交换后的 cur_dispacth_fib_.fib_ctx.uc_link == 最开始的xfiber.xfiber_ctx
                            ==> 暨: 交换后的 cur_dispatch_fib 运行完后 会上行 最开始的xfiber.xfiber_ctx  , 暨继续运行 xfiber调度器上次交换出去的地方
                            ==> 所以 在 FIBER对象(被调度协程)运行完毕后,会回归到 xfiber(调度协程)的上下文
                            ==> 所以 ,回去之后 的下一条命令 就是  交换后的cur_dispatch_fib_ = nullptr,暨表示 fiber已经运行完毕
                */
                assert(SwitchCtx(SchedCtx(),fiber->Get_Fiber_Ctx()) == 0);// 这条语句开始执行是: 是去运行fiber上下文 这条语句执行完毕时: 时从fiber回到xfiber
                //注意: 
                     //该调度对象(暨协程)已经彻底啊调度完成
                     // ===> 为什么说调度完成了?
                     //      因为他已经 从被调度的fiber对象的上下文中,再一次的返回到了 XFIBER的上下文
                     //      浏览 FIBER中 ctx的初始化
                     //      fiber->fiber_ctx_.uc_link = xfiber_ctx_
                     //      任何一个 fiber对象(暨协程),在调度后,都必须回到XFIBER调度器,从而继续调度运行队列里面的后续协程
                     // 该fiber对象调度完成,就就没啥用了,所以就不需要保存了
                this->cur_dispatch_fiber_ = nullptr; //fiber 运行完毕回到xfiber 所以 当前即将运行的ffiber 为null 

                if (fiber->IsFinished()) {
                    LOG_INFO("fiber[%lu] finished, free it!", fiber->Seq());
                    delete fiber;//彻底运行完毕 ,就没啥用了
                }
                
            }
            this->running_fibers_.clear();//运行队列中所有的被调度FIBER(暨被调度协程)已经调度完成
        }
    
// 情况2: 在全局超时队列中处理带有超时时间的事件
        int64_t now_ms = util::NowMs(); 
        //this->expire_events_.begin->first: 暨超时时间(dead_line) 是 超时时间expire_at_ + now_ms(设置时的事件戳) 计算而来
        // while (!this->expire_events_.empty() && this->expire_events_.begin()->first <= now_ms) {
        


        //>注意<: 这里特别 离谱!
        //>重点<: 这里超级离谱! 
        //>提问<: 这里为什么是这样写? 为什么不能写 >=? 
        //>解答<: 如果写 >= 的话 未超时 的fiber 会被频繁调用, 哪怕压根 没有被触发!
        //      ==> 写 <= 这又是为什么呢?
        //>解答<: 超时队列 只处理 超时的fiber 
        //>重点<: 1.如果最小的 超时时限都没有 超时, 那么全部都没有超时, 就没有必要触发! ==> 暨wakeup
        //     2. 如果 反写乘 >=  会导致所有未超时的fiber都会被再次上cpu,哪怕根本没有数据流入流出
        //     3. 如果 协程 >= 那么 超时时限 根本不会准确: 因为1多次 的上下cpu sleepms = 当前时间 + timeout
        //                      ==> 每一次上cpu timeout都会改变
        //     4. 为什么要写成这样? wakeup超时fiber 不是事与愿违吗? 超时的不是应该舍弃吗? wakeup之后 有注定会让 超时fiber上一次CPU? 这不是和初衷(超时舍弃)相违背吗
        //      ======> 超时的fiber唤醒, 唤醒后wakeup 
        //         ===> wakeup 先加入ready_对列 然后再从超时队列 等待集合 epoll中删除
        //         ==> 然后再次上cpu ,
        //>注意<:     此时: 是最后一次上cpu ,上cpu的目的是为了 //>提示<: 超时fiber进行自己的超时逻辑,退出!,再也找不到他了,彻底finished了
        //>重点<:   这样就保证了: 1. 超时队列里面全都是未超时的   2. 一旦fiber超时 就无效化(彻底finished了)(哪怕再次被触发也无效)
        //              3. 保证了 未超时fiber不会频繁上cpu 导致 超时仙剑动态变化 ===>从而影响 到固定的超时时限(死线不会后移了)
        while (!this->expire_events_.empty() && this->expire_events_.begin()->first <= now_ms) {
	           //std::map<int64_t, std::set<Fiber *>> expire_events_; 按照超时时间排序
               // this->expire_events_.begin()->first 红黑树,也就是 最小的那个超时时间
            std::set<Fiber *> &expired_fibers = expire_events_.begin()->second;

            // 依次处理超时队列
            while (!expired_fibers.empty()) 
            {
            
                std::set<Fiber *>::iterator expired_fiber = expired_fibers.begin();
                //唤醒超时队列里面的每一个
                WakeUpFiber(* expired_fiber);
            }
            expire_events_.erase(expire_events_.begin());
        }    







// 情况3: 处理epoll监听中被 触发的fd    ===> 特殊: epoll事件 被触发后 立即调用WakeUpFiber 将该fiber 加入就绪队列 删除 等待集合中的可读可写fiber,删除超时队列里的 fiber
        #define MAX_EVENT_COUNT 512

/*结构体细节: 事件+ 存储信息的联合体
    记住union的特性，它只会保存最后一个被赋值的成员，
    所以不要data.fd,data.ptr都赋值；通用的做法就是给ptr赋值，
    fd只是有些时候为了演示或怎么样罢了
>>>>>>>> 可见，这个 data成员还与具体的使用方式相关。 <<<<<<<<<

typedef union epoll_data 
{
  void *ptr;    : 感兴趣的数据
  int fd;   
  uint32_t u32;
  uint64_t u64;
} epoll_data_t;


struct epoll_event
{
  // epoll 注册的事件，比如EPOLLIN、EPOLLOUT等等，
  //这个参数在epoll_ctl注册事件时，可以明确告知注册事件的类型。
  uint32_t events; 

  epoll_data_t data;//保存触发事件的某个文件描述符相关的数据
} __EPOLL_PACKED;
*/
        struct epoll_event evs[MAX_EVENT_COUNT];

/*>注意<: epoll_wait
函数功能: 等待epoll事件的产生
 int epoll_wait(int epfd, struct epoll_event * events, int maxevents, int timeout);
参数:
    1. epfd: eopll结构句柄
    2. epoll_events: 从内核得到的 epoll事件集合,会拷贝一份到该处,注意是地址
    3. maxevents: 告之内核这个events有多大
                maxevents <= epoll_create()时的size，
    4. timeout : 超时时间
                ① 0:   立即返回
                ② -1: -1将不确定，也有说法说是永久阻塞
返回值:
      1. 成功 : > 0  返回需要处理的事件数目
      2. 失败 : -1: 
      epoll_wait处理完事件后, 会对fd上的 事件进行清空
      fd依旧存在,但不再关心这个事件

//。epoll_wait将会接收到消息，并且将数据拷贝到用户空间，清空链表。 
对于LT模式epoll_wait清空就绪链表之后会检查该文件描述符是哪一种模式，
如果为LT模式，且必须该节点确实有事件未处理，  =============> 继续关心 这个 ,通知多次
那么就会把该节点重新放回到 刚刚删除掉的且刚准备好的就绪链表，
epoll_wait马上返回。ＥＴ模式不会检查，只会调用一次      ==========> 不在加入到关心链表,只通知一次


*/

        // 设置超时时间2s
        int n = epoll_wait(this->ep_fd_, evs, MAX_EVENT_COUNT, 2);
        if (n < 0) {
            LOG_ERROR("epoll_wait error, msg=%s", strerror(errno));
            continue;
        }


        //处理触发事件
        for (int i = 0; i < n; i++) {
            //获取事件单个epoll事件
            struct epoll_event &ev = evs[i];
            //epoll 事件 文件描述符
            int fd = ev.data.fd;

            // map中 fd 对应的是 一个waitting_fiber : witing_fiber:中有两份 fiber* 
            auto fiber_iter = io_waiting_fibers_.find(fd); 
            //被通知的事件 ,存在于 等待队列中
            if (fiber_iter != io_waiting_fibers_.end()) {
                // 获取 等待集合中 的等待fiber
                WaitingFibers &waiting_fiber = fiber_iter->second;
                //可读事件
                if (ev.events & EPOLLIN) {
                    if (waiting_fiber.r_ == nullptr) {
                        LOG_WARNING("fd[%d] is readfd and this fd is null", fd);
                    }
                    else {
                        LOG_DEBUG("waiting fd[%d] has fired IN event, wake up pending fiber[%lu]", fd, waiting_fiber.r_->Seq());
                        WakeUpFiber(waiting_fiber.r_); //唤醒fd对应的 读事件处理协程
                    }

                }
                else if (ev.events & EPOLLOUT) {
                    if (waiting_fiber.w_ == nullptr) {
                        LOG_WARNING("fd[%d] has been fired OUT event, but not found any fiber to handle!", fd);
                    }
                    else {
                        LOG_DEBUG("waiting fd[%d] has fired OUT event, wake up pending fiber[%lu]", fd, waiting_fiber.w_->Seq());
                        WakeUpFiber(waiting_fiber.w_);
                    }
                }
            }
        }
    }
}


//调用该函数的协程: 主动让出cpu
// 让出cpu的结果就是,会回到XFIBER调度器,然后调度器,去选择下一个调度对象
void XFiber::Yield() {
    // 当前被调度 的 fiber的 ctx 不为空
    assert(this->cur_dispatch_fiber_!= nullptr);// 说明当前调度器上的是fiber协程
    // 主动切出的后仍然是ready状态，等待下次调度
    ready_fibers_.push_back(this->cur_dispatch_fiber_);
    SwitchToScheduler();
}


//函数功能: 从当前的 正在执行的协程(fiber对象)切换出来, 转而去执行xfiber调度器
void XFiber::SwitchToScheduler() {
    assert(this->cur_dispatch_fiber_ != nullptr);
    LOG_DEBUG("switch to sched");
    // assert(SwitchCtx(cur_dispatch_fiber_, SchedCtx()) == 0);

    // 就是单纯的 swapcontext
    // std::cout<< cur_dispatch_fiber_->Get_Fiber_Ctx()<<std::endl;
    // std::cout<< SchedCtx()<<std::endl;

    assert(SwitchCtx(cur_dispatch_fiber_->Get_Fiber_Ctx(),SchedCtx()) == 0);
}













//设置超时时间
void XFiber::SleepMs(int ms) {
    if (ms < 0) {
        return;
    }
    //当前系统时间+ 超时时间 == 死线
    int64_t expired_at = util::NowMs() + ms;
    // WaitingEvents 是一个时间集合
        // 被监听的 读事件数组 写事件数组 信号量数组 整个数组通用的 超时时间
    WaitingEvents events;
    events.expire_at_ = expired_at;

    // 将这一整个 WaitingEvents事件集合 注册到xfiber调度器中
    RegisterWaitingEvents(events);
    //切换回调度器
    SwitchToScheduler();
}


// >重点<: 超时时间针对的是一整个fiber
// 注册时, 需要将 fiber | 超时界限 注册到 全局超时队列里面去
// 同时 需要 根据传进来的 waitingevent 来分别初始化 fiber的 读写事件
void XFiber::RegisterWaitingEvents(WaitingEvents &events) {
    // assert(curr_fiber_ != nullptr);
    assert(cur_dispatch_fiber_ != nullptr);
    
    
    
    //如果有超时时间设置: 将超时时间加入到超时队列并设置fiber的waitingevents
    if (events.expire_at_ > 0) {
        // expire_events_[events.expire_at_].insert(curr_fiber_);
        //超时事件集合中 加入 当前 已经设置里超时时间的 fiber

        // 调度器的全局超时队列中加入当前fiber
        expire_events_[events.expire_at_].insert(cur_dispatch_fiber_);

        // 初始化当前fiber的 waitingevents
        cur_dispatch_fiber_->SetWaitingEvent(events);
        // curr_fiber_->SetWaitingEvent(events);
        LOG_DEBUG("register fiber [%lu] with expire event at %ld", cur_dispatch_fiber_->Seq(), events.expire_at_);
    }

    //处理读事件: 将读事件 加入到等待集合 并设置fiber的waitingevents
    for (size_t i = 0; i < events.waiting_fds_r_.size(); i++) {
        int fd = events.waiting_fds_r_[i];
        auto iter = io_waiting_fibers_.find(fd);
        if (iter == io_waiting_fibers_.end()) {
            // 所有的 读事件 都加入到 等待集合map 中去
            io_waiting_fibers_.insert(std::make_pair(fd, WaitingFibers(cur_dispatch_fiber_, nullptr)));


            // 分别设置 当前fiber的 读事件
            cur_dispatch_fiber_->SetWaitingEvent(events);
        }
    }
    //处理写事件: 将写事件 加入到等待集合 并设置fiber的waitingevents
    for (size_t i = 0; i < events.waiting_fds_w_.size(); i++) {
        int fd = events.waiting_fds_w_[i];
        auto iter = io_waiting_fibers_.find(fd);
        if (iter == io_waiting_fibers_.end()) {
            // 所有的 写事件都加入到 等待集合map中去
            io_waiting_fibers_.insert(std::make_pair(fd, WaitingFibers(nullptr, cur_dispatch_fiber_)));


            //分别设置当前fiber自身的 waitingevent 中的写事件
            cur_dispatch_fiber_->SetWaitingEvent(events);
        }
    }
    //处理信号量: 将信号量加入到信号集合 waitingevents
    for (size_t i = 0; i < events.waiting_sems_.size(); i++) {
        Sem &sem = events.waiting_sems_[i];
        // 没有这个信号量存在,则新插入
        if (sem_infos_.find(sem) == sem_infos_.end()) {
            sem_infos_.insert(std::make_pair(sem, SemInfo()));
        }
        //>重点<: 为什么这里和生面的不一样?
        //>解答<: 
        sem_infos_[sem].fibers_.insert(cur_dispatch_fiber_);
    }
}


// map: fd waitingfibers 
// waitingfibers : fiber1* fiber2*
// waitingevents: 读数组 写数组 信号量数组 超时时间

// >重点<: 根据 传入的 waitingevents 来分别设置 fiber的 读写事件 (读事件 写事件分开))
void Fiber::SetWaitingEvent(const WaitingEvents &events) {

    // 
    for (size_t i = 0; i < events.waiting_fds_r_.size(); i++) {

        //fiber 的waitingevents
        this->waiting_events_.waiting_fds_r_.push_back(events.waiting_fds_r_[i]);
    }
    for (size_t i = 0; i < events.waiting_fds_w_.size(); i++) {
        this->waiting_events_.waiting_fds_w_.push_back(events.waiting_fds_w_[i]);
    }
    if (events.expire_at_ > 0) {
        this->waiting_events_.expire_at_ = events.expire_at_;
    }
}








// 在epoll中注册某个fd
void XFiber::TakeOver(int fd) {
    struct epoll_event ev;
    // 读写 ET
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = fd;// 联合体 union 之关心一个
/* epoll_ctl 函数说明: lepoll的事件注册函数
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
参数:
    epfd: epoll结构句柄
    op: epoll_ctl注册的动作
        1. EPOLL_CTL_ADD：注册新的fd到epfd中；
        2. EPOLL_CTL_MOD：修改已经注册的fd的监听事件；
        3. EPOLL_CTL_DEL：从epfd中删除一个fd；
    fd : 需要epoll结构 去侦听的 句柄
    event: 关心 被侦听fd 上发生的事件类型 (只关心这些类型,其他的不关心)
            EPOLLIN : 读事件
            EPOLLOUT: 写事件
            EPOLLPRI: 额外数据到来(紧急数据)
            EPOLLERR：发生错误
            EPOLLET： 表示使用ET模式
返回值:
    成功: 0
    失败: -1   
*/
    if (epoll_ctl(ep_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERROR("add fd [%d] into epoll failed, msg=%s", fd, strerror(errno));
        exit(-1);
    }
    LOG_DEBUG("add fd[%d] into epoll event success", fd);
}
//从epoll中取消注册某个fd
bool XFiber::UnregisterFd(int fd) {
    LOG_DEBUG("unregister fd[%d] from sheduler", fd);
    auto io_waiting_fibers_iter = io_waiting_fibers_.find(fd);
    // assert(io_waiting_fibers_iter != io_waiting_fibers_.end());

    //第一步: 从等待集合map中删除 该fd对应的 读写fibers ==> fd都不见听了 fiber也就没用了
    if (io_waiting_fibers_iter != io_waiting_fibers_.end()) {
        WaitingFibers &waiting_fibers = io_waiting_fibers_iter->second;
        if (waiting_fibers.r_ != nullptr) {
            WakeUpFiber(waiting_fibers.r_);
        }
        if (waiting_fibers.w_ != nullptr) {
            WakeUpFiber(waiting_fibers.w_);
        }

        io_waiting_fibers_.erase(io_waiting_fibers_iter);
    }

    struct epoll_event ev;
    //第二步: 从epoll中删除
    if (epoll_ctl(ep_fd_, EPOLL_CTL_DEL, fd, &ev) < 0) {
        LOG_ERROR("unregister fd[%d] from epoll efd[%d] failed, msg=%s", fd, ep_fd_, strerror(errno));
    }
    else {
        LOG_INFO("unregister fd[%d] from epoll efd[%d] success!", fd, ep_fd_);
    }
    //>提问<: 为什么 不去设置超时队列和信号量队列呢? 超时队列不用管,超时的fiber不会处理,并且超时队列最后会彻底clear
    //>提问<: 那么信号量队列呢? epoll主要关心的还是读写 
    return true;
}
















// 从调度器中取消注册的fd :
//>提示<: 只是取消fd fiber还是存在的 fiber其实就是个ctx上下文及一堆便利
bool XFiber::UnregisterFdFromScheduler(int fd) {
    LOG_DEBUG("unregister fd[%d] from sheduler", fd);
    auto io_waiting_fibers_iter = io_waiting_fibers_.find(fd);
    // assert(io_waiting_fibers_iter != io_waiting_fibers_.end());


// 首先 从等待集合 map 中删除
    if (io_waiting_fibers_iter != io_waiting_fibers_.end()) {
        WaitingFibers &waiting_fibers = io_waiting_fibers_iter->second;

        // 删除之前,需要先 将他所负责的事情 进行交接 该干的要干完
        if (waiting_fibers.r_ != nullptr) {
            WakeUpFiber(waiting_fibers.r_);
        }
        if (waiting_fibers.w_ != nullptr) {
            WakeUpFiber(waiting_fibers.w_);
        }

        //删除
        io_waiting_fibers_.erase(io_waiting_fibers_iter);
    }
//其次 从epoll监听中退出
    struct epoll_event ev;
    //EPOLL_CTL_DEL :删除 摸个fd
    if (epoll_ctl(ep_fd_, EPOLL_CTL_DEL, fd, &ev) < 0) {
        LOG_ERROR("unregister fd[%d] from epoll efd[%d] failed, msg=%s", fd, ep_fd_, strerror(errno));
    }
    else {
        LOG_INFO("unregister fd[%d] from epoll efd[%d] success!", fd, ep_fd_);
    }
    return true;
}




//>注意<: 这一居有什么用? 为什么这么写? 和SEQ有什么关系?
//>解答<: 可能和 网络通信阶段的 三握的seq油管
thread_local uint64_t fiber_seq = 0;

Fiber::Fiber(std::function<void ()> run, XFiber *xfiber, size_t stack_size, std::string fiber_name) {
    
    /*
    1. fiber对象(暨背调度的协程)的入口函数地址
    2. 该入口函数地址是保存在 uc_mcontext中的:
    3. uc_mcontext:保存所有上下文信息,寄存器信息,入口函数地址
    */
    this->entry_func_ = run;

    this->xfiber_ = xfiber;
    this->fiber_name_ = fiber_name;
    //有栈协程: 栈大小为 128kb
    this->stack_size_ = stack_size;
    //协程的栈起始位置
    this->stack_ptr_ = new uint8_t[stack_size_];

    //ucontext函数簇 ctx初始化
    getcontext(&this->ctx_);
    //==========三兄弟=========
    ctx_.uc_stack.ss_sp = stack_ptr_;
    ctx_.uc_stack.ss_size = stack_size_;

    //>注意<: 这里很重要
    //>重点<: 每一个 fiber的 ctx 都link上 xfiber的唯一的 ctx
    //>提问<: 为什么要这样做?
    //>解答<: 这样能够保证 任何一个 fiber 运行完毕后,都会自动的去切换到xfiber 然后继续有xfiber调度其他fiber
        //---> 这也是协程调度器能够持续运行的根本 
    ctx_.uc_link = xfiber->SchedCtx();
    //=========================
    //该协程的入口函数 (makecontext会绑定协程的入口函数地址并且,进行上下文切换)
    // 1 : 表示只有1个参数 this : 这个参数就是this
    // Statr 只是入口函数的一层封装
    makecontext(&this->ctx_, (void (*)())Fiber::Start, 1, this);



    seq_ = fiber_seq;
    fiber_seq++;
    //当前fiber的状态时 INIT
    status_ = FiberStatus::INIT;
}


Fiber::~Fiber() {
    // 删除 fiber的携程栈
    delete []stack_ptr_;
    stack_ptr_ = nullptr;
    stack_size_ = 0;
}

uint64_t Fiber::Seq() {
    return this->seq_;
}
XFiberCtx *Fiber::Get_Fiber_Ctx() {
    return &ctx_;
}

void Fiber::Start(Fiber * fiber) {
    // 在这里 执行 fiber真正的处理函数也就是入口函数
    fiber->entry_func_();
    // 处理完成 fiber彻底结束啦
    fiber->status_ = FiberStatus::FINISHED;
    LOG_DEBUG("fiber[%lu] finished...", fiber->Seq());
}

std::string Fiber::Name() {
    return fiber_name_;
}

bool Fiber::IsFinished() {
    return status_ == FiberStatus::FINISHED;
}












// 这里是信号量
// struct Sem {

 	//>注意<: thread_local: 确保每一个县城里面 只有一个sem_seq
//     thread_local static int64_t sem_seq;

//     int64_t seq_;

//     Sem(int32_t value);

//     bool Acquire(int32_t apply_value, int32_t timeout_ms=-1);

//     void Release(int32_t acquired_value);

//     bool operator < (const Sem &other) const {
//         return seq_ < other.seq_;
//     }
// };
    //>注意<: 信号量结构体
	// struct SemInfo {
    //     SemInfo(int32_t value = 0) {
    //         value_ = value;
    //         acquiring_fiber_ = nullptr;
    //         acquired_value_ = 0;
    //         fibers_.clear();
    //     }

    //     int32_t value_; 
    //     // 正在尝试获取的fiber
    //     Fiber *acquiring_fiber_;
    //     // 正在尝试获取的value_
    //     int32_t acquired_value_;
    //     std::set<Fiber *> fibers_;
    // };


// 信号量注册: 将信号量 加入到xfiber::信号量集合中去
void XFiber::RegisterSem(Sem *sem, int32_t value) {
    sem_infos_.insert(std::make_pair(*sem, SemInfo(value)));
}

void XFiber::UnregisterSem(Sem *sem) {
    //sem_info: xfiber内部的 信号量集合std::map<Sem, XFiber::SemInfo> XFiber::sem_infos_
    auto sem_infos_iter = sem_infos_.find(*sem);

    // 取消注册的前提是一定要注册过了
    assert(sem_infos_iter != sem_infos_.end());
    //sem_infos_iter->second === SemInfo sem_infos_iter->second.fibers_ === SemInfo.fibers_
    // 取消了信号量之后,要把这个 fiber唤醒, 

    //>注意<: 如果不唤醒,可能就永远也不会通知到了
    //>重点<: sem_infos_iter->second.fibers_的fibers_ 是一个set
    //     ==> 一个信号量可以 管理 多个fibers 

    for (auto fiber: sem_infos_iter->second.fibers_) {
        WakeUpFiber(fiber);
    }
    //唤醒完成之后 清除 set
    sem_infos_iter->second.fibers_.clear();

    // 现在SemInfo的set集合清除完毕,
    // 还需要在 xfiber的信号量集合中(key:sem value:Seminfo) 中彻底断绝关系
    sem_infos_.erase(sem_infos_iter);
}


//>注意<: 尝试获取信号量
//>重点<: 为什么叫做尝试? 如何尝试
bool XFiber::TryAcquireSem(Sem *sem, int32_t value) {
    // xfiber中 sem_info_s 中是否存在这个 已经注册过了的 信号量
    auto iter = sem_infos_.find(*sem);

    // 获取这个sem 对应的 SemInSo
    SemInfo &sem_info = iter->second;

    if (sem_info.acquiring_fiber_ == nullptr) {
        if (sem_info.value_ >= value) {
            sem_info.value_ -= value;
            return value;
        }
        int32_t done_value = sem_info.value_;
        sem_info.value_ = 0;
        sem_info.acquired_value_ = done_value;
        sem_info.acquiring_fiber_ = cur_dispatch_fiber_;
        // sem_info.fibers_.insert(cur_dispatch_fiber_);
        sem_info.fibers_.erase(cur_dispatch_fiber_);
        return done_value;
    }

    if (cur_dispatch_fiber_ != sem_info.acquiring_fiber_) {
        sem_info.fibers_.insert(cur_dispatch_fiber_);
        return 0;
    }



    sem_info.fibers_.erase(cur_dispatch_fiber_);
    if (sem_info.value_ >= value) {
        sem_info.value_ -= value;
        sem_info.acquired_value_ = 0;
        sem_info.acquiring_fiber_ = nullptr;
        // if (sem_info.fibers_.find(cur_dispatch_fiber_) != sem_info.fibers_.end()) {
        //     sem_info.fibers_.erase(cur_dispatch_fiber_);
        // }
        return value;
    }
    else {
        int32_t done_value = sem_info.value_;
        sem_info.value_ = 0;
        sem_info.acquired_value_ += value;
        sem_info.acquiring_fiber_ = cur_dispatch_fiber_;
        // sem_info.fibers_.insert(cur_dispatch_fiber_);
        return done_value;
    }
}


// 释放信号量
void XFiber::ReleaseSem(Sem *sem, int32_t value) {
    auto iter = sem_infos_.find(*sem);
    assert(iter != sem_infos_.end());

    SemInfo &sem_info = iter->second;
    sem_info.value_ += value;
    // if (sem_info.fibers_.find(cur_dispatch_fiber_) != sem_info.fibers_.end()) {
    //     sem_info.fibers_.erase(cur_dispatch_fiber_); 
    // }
    LOG_INFO("release sem will wakeup %lu fibers", sem_info.fibers_.size());
    for (auto fiber: sem_info.fibers_) {
        WakeUpFiber(fiber);
    }
    sem_info.fibers_.clear();
}



// 信号量恢复
void XFiber::RestoreSem(Sem *sem, int32_t value) {
    auto iter = sem_infos_.find(*sem);
    assert(iter != sem_infos_.end());

    SemInfo &sem_info = iter->second;
    sem_info.value_ += value;
    sem_info.acquiring_fiber_ = nullptr;
    sem_info.acquired_value_ = 0;
    // if (sem_info.fibers_.find(cur_dispatch_fiber_) != sem_info.fibers_.end()) {
    //     sem_info.fibers_.erase(cur_dispatch_fiber_);
    // }
    LOG_INFO("restore sem will wakeup %lu fibers", sem_info.fibers_.size());
    for (auto fiber: sem_info.fibers_) {
        WakeUpFiber(fiber);
    }
    sem_info.fibers_.clear();
}




















// 每一个线程内部只存在一个: 注意是线程 单进程 单线程 内 多协程情况下是 单例
thread_local int64_t Sem::sem_seq = 0;

//Sem 结构体初始化
//sem_seq 就是信号量的编号吧
Sem::Sem(int32_t value) {
    seq_ = sem_seq++;
    //>重点<: xfiber调度器 为什么在这里初始化?
    //>提问<: why???????????????????????????????????
    //>解答<: 这里其实不是初始化
    //>注意<: Xfiber::xfiber()是一个静态的 函数 内部有一个 静态的单例Xfiber xf
    //>重点<: 在同一个线程内  只会存在这一个xf 所以 xfiber 可以多次调用
    /*=========================================
    + static XFiber * xfiber()
    + {
    +     static thread_local XFiber xf;
    +     return &xf;
    + };
    =========================================*/
    // 获得xfiber调度器
    //>注意<: 这里是将线程单例xf 获取
    XFiber *xfiber = XFiber::xfiber();

    //xfiber 将 这个信号量 注册到xfiber::sem_infos_中去
    xfiber->RegisterSem(this, value);
}


// 获取信号量
// 参数解释: apply_value: 申请的信号量  timeout_ms: 超时时间
bool Sem::Acquire(int32_t apply_value, int32_t timeout_ms) {
    XFiber *xfiber = XFiber::xfiber(); //>注意<: 获取线程单例xf
    int64_t expire_at = timeout_ms < 0 ? -1 : util::NowMs() + timeout_ms;//设置超时时间
    int32_t done_value = 0;//已经使用的量目前为0
    while (done_value < apply_value){ 
        done_value += xfiber->TryAcquireSem(this, apply_value - done_value); // apply_value - done_value === 余下的信号量
        if (done_value == apply_value) { //所有的信号量彻底用完了
            return true;
        }

        // 已经超时
        //>提问<:为什么要 判断已经超时? 
        //>解答<: 已经超时==> 整个fiber就懒得动了 ,所以就无效了 expire_at(died_line<现在事件) === 超时
        
        if (expire_at >= 0 && expire_at <= util::NowMs()) { 
            LOG_INFO("acquire sem timeout, restore %d sems", done_value);
            xfiber->RestoreSem(this, done_value); //  恢复 已经分配的信号量 (因为超时,所以就别占着茅坑了)
            return false;
        }


        //>重点<: 尚未超时
        WaitingEvents ev; 
        // LOG_INFO("还没有超时");
        ev.expire_at_ = expire_at;//超时时间
        ev.waiting_sems_.push_back(*this);//
        //ev.waiting_fds_r == ev.waiting_fds_w == nullptr 压根不关心
        //>注意<: registerwaitingevents: 注册函数 干 4件事
        //      ==> 1. 注册超时事件(超时队列) 2.注册读写事件(等待队列map) 3.注册新号量
        //>重点<:

        /*================================
        + 此处waitingevents只处理 : 
        + 1.超时时间:注册到xfiber中 
        + 2.信号量:注册到xfiber中去
        + 3.读写队列懂事空的:registerwaitingevents 注册4步 直走两步
        =================================*/
        xfiber->RegisterWaitingEvents(ev);//将这个 sem 注册到xfiber中去 
        xfiber->SwitchToScheduler();
    }
    return false;
}

void Sem::Release(int32_t value) {
    XFiber *xfiber = XFiber::xfiber();
    xfiber->ReleaseSem(this, value);
}
