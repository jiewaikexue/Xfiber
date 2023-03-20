
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
    this->ep_fd_ = epoll_create1(0);//
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
//>注意<: fiber 上了xfiber调度后 xfiber 的sched_ctx_ 存储的 是fiber的上下文信息
XFiberCtx* XFiber::SchedCtx(){
    return &sched_ctx_;
}


//>注意<: 新建一个fiber, 并且只是简单的将他加入到ready队列中去
//>注意<: std::function<void ()> run 是为了适应仿函数
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
//       2. 从 xfiber 的 io_waitiing_events中 删除 该fiber 负责的 可读可写事件 的fd 的映射关系
//             |==> 为什么要这么做: 一个套接字fd 被epoll侦听,该fd 对应的fiber 只有一个
//             |==> ==> 满足了 一个fd 只能被一个fiber处理的 情况
//             |==> 但是 ! 一个fiber 也可能同时 负责处理很多个fd
//             |==> ==> 所以 当该fiber 被任何一个epoll侦听到的 fd 唤醒后, 需要先 将本身负责的所有的读写fd ,在io_waiting_events 映射中删除
//             |==> ==> ==> 一个fd 只能被一个fiber处理,一个fiber可以处理多个fd. 我一旦被触发,多余的重复映射就删除吊
//             |==> ==> ==> 我先起床,我先把我能吃的早餐 全部吃掉  
//       3. 从 xfiber 的 超时队列 中 删除 该fiber 对应的 超时事件
//       4. 从epoll中踢出: 防止二次唤醒
//     =======> 理由 wakeup唤醒后 加入 dispatch ,按顺序上调度器, 上了调度器之后 会 处理fiber自身的读写超时事件
//      ===> 处理完 ,本来就应该删除,只不过这里是提前了一点点而已
void XFiber::WakeUpFiber(Fiber * fiber) {

    // DEBUG_ENABLE : 默认为0 LOG_DEBUG(fmt,...)
    // LOG_DEBUG("try wakeup fiber[%lu] %p", fiber->Seq(), fiber);
//第一步: 被唤醒的 fiber ,要先加入就绪队列
    this->ready_fibers_.push_back(fiber);
    LOG_INFO("try wakeup fiber[%lu] %p, %d fibers ready to run", fiber->Seq(), fiber, ready_fibers_.size());

    

//第二步: 从io_waiting_events中删除该fiber负责 的所有的 多个的 读写fd 和其他fiber的映射关系
    //>提问<:  等待队列是哪个? running是运行 ,ready是就绪
    //>解答<:  等待队列 是 epoll 监听fd 和对应fiber对象的map 即io_waiting_events
    //      io_waiting_events: 里面存储的是所有的 fd和fiber的映射关系
    //      epoll触发fd之后,从该map中寻找对应的 fiber
    
        // 2.1 先获取 fiber的等待事件集合 ==> waiting_events
            // >重点<: 一个 fd一般只能交给 一个 协程(fiber)处理
            //      +_+ 但是 一个 fiber 可以处理多个fd ==> 所以 我们需要去除多余的 重复的映射关系
    

        //>重点<: 一个fiber 可以对应很多个 很多个 fd事件 每个fd事件交给一个 新的 fiber去处理
        //      ==> 所以这样就有了 waitingevents
        //每一个fiber 都有一个 waitingevents 里面包含了 这个fiber 负责的fd对象
        // 现在 这个fiber即将被 wakeup 所以也需要 将这些fd处理掉
    auto & waiting_events = fiber->GetWaitingEvents();
        // 2.2 在 对 waiting_events 内 所有的 fd 进行处理: 可读fd 可写fd 超时队列


    // events:事件集合包含了 读事件 写事件 超时事件, 需要分别删除对应的 映射关系
    // 删除 读fd的映射关系   
    for (size_t i = 0;i < waiting_events.waiting_fds_r_.size();i ++) {
        int fd = waiting_events.waiting_fds_r_[i];

        //>注意<: io_waiting_fibers_ 就是那个 map 就是全局等待队列
        auto iter = io_waiting_fibers_.find(fd);
        //如果对应fd 存在 等待队列中, 就把该fd 删除
        if (iter != io_waiting_fibers_.end()) {
            io_waiting_fibers_.erase(iter);
        }

       
    }
    // 处理写事件的映射关系
    for (size_t i = 0;i < waiting_events.waiting_fds_w_.size();i ++) {
        int fd = waiting_events.waiting_fds_r_[i];
        auto iter = io_waiting_fibers_.find(fd);
        if (iter != io_waiting_fibers_.end()) {
            io_waiting_fibers_.erase(iter);
           
        }

    }

    //第三步: 从 全局超时队列expire_events_(超时时间,fiber)组成的map  中 删除掉对应的fd事件
    //获取 fiber的事件集合设置的超时时间  ==> 一旦到达该时间 整个fiber超时作废
    //>提问<: 为什么要在这里处理超时问题?
    //>解答<: 超时机制本质: 时间戳 和当前时间做对比: 超时:则整个超时队列会wakeup
    //>重点<: 部分超时的 会走自身内部 超时逻辑, 未超时的 相当于提前唤醒
    //>重点<: 一个fiber 可能会负责 多个fd, 超时属性是fd 和fiber的, 一旦当前fd 的负责人和本fiber 匹配,则fd的超时时间,也就是和本fiber匹配,则去掉本fiber的超市属性
    
    WaitingEvents &evs = fiber->GetWaitingEvents(); // 获取读事件fd,检测 是否设置有超时时间 , 然后去超时队列中, 寻找,当前fiber 是否有超时属性
            /* 二者合二为一
            for (size_t i = 0; i < evs.waiting_fds_r_.size(); i++) {
                auto expired_at = evs.expire_at_; 
                if (expired_at > 0) {
                    auto expired_iter = expire_events_.find(expired_at);
                    //如果 该fiber 没有超时属性
                    if (expired_iter->second.find(fiber) == expired_iter->second.end()) {
                        LOG_ERROR("not fiber [%lu] in expired events", fiber->Seq());
                    }
                    else {
                        // 如果该fiber 有超时属性, 则 因为被提前唤醒,而失去超时属性 ===> 解除超时映射
                        expired_iter->second.erase(fiber);
                    }
                }
            }

            for (size_t i = 0; i < evs.waiting_fds_w_.size(); i++) {
                int64_t expired_at = evs.expire_at_;
                if (expired_at > 0) {
                    auto expired_iter = expire_events_.find(expired_at);
                    if (expired_iter->second.find(fiber) == expired_iter->second.end()) {
                        LOG_ERROR("not fiber [%lu] in expired events", fiber->Seq());
                    }
                    else {
                        expired_iter->second.erase(fiber);
                    }
                }
            }
            */
    
    //>重点<: 从这里可以看出 超时时间 是 fiber的属性,
    //>注意<: 这里 比较的是当前fiber的超时时间,和超时队列里 其余fiber的超时时间
    int64_t expire_at = waiting_events.expire_at_;
    if (expire_at > 0) {
        // 这一步: 有没有和我具有相同超时时间的其他fiber对象呢?
        auto expired_iter = expire_events_.find(expire_at);
        if (expired_iter->second.find(fiber) == expired_iter->second.end()) {
            LOG_WARNING("not fiber [%lu] in expired events", fiber->Seq());
        }
        else {
            LOG_DEBUG("remove fiber [%lu] from expire events...", fiber->Seq());
            expired_iter->second.erase(fiber);
        }
    }

            // 时间: 2023年3月7日20:22:28 不需要这些
            // >重点<: 从 epoll 中也需要踢出
            // >解答<: 其实也不用从epoll中踢出,这个要看你的写法吧
                    // >解答< : 如果不踢出 的话, 就在初始化的时候 ,使用epoll_ctl_mod
                                // 因为 epoll_ctl_add 如果epoll内部该fd被触发后 fd还存在,但是事件集合被清空了,所以add会失败  
            // epoll 照着set ,把里面的fd踢出
            // >提问<: 为什么要从epoll中踢出啊?
            /* >解答<: epoll踢出: 
                >重点<: epoll_wait工作原理: 这里的epoll_ctl 并没有使用epollshort  所以不需要重新注册
                等侍注册在epfd上的socket fd的事件的发生，
                如果发生则将 `发生的sokct fd`和 `事 件类型放入到events数组中`
                `并 且将注册在epfd上的socket fd的事件类型给清空`，
                所以如果下一个循环你还要关注这个socket fd的话，则需要用epoll_ctl(epfd,EPOLL_CTL_MOD,listenfd,&ev)来重新设置socket fd的事件类型。
                `这时不用EPOLL_CTL_ADD,因为socket fd并未清空`，只是 `事件类型清空`。这一步非常重要。
                所以: wakeup之后 et 模式 + 死循环, 保证了 事件平稳的处理完成
                        处理完成之后,fd仍然存在,但是 fd的事件 会被清空, 
                    >重点<:    ====> 下一次相同fd 初始化时 epoll_ctl_add 会失败
                        ====>  如果没有相同的 fd继续来, 那么epoll中的 fd也太多了 ==> 红黑树虽然没啥影响
            */
        /*
            for (auto iter : waiting_fds) {
                if (epoll_ctl(this->ep_fd_, EPOLL_CTL_DEL, iter, nullptr) < 0) {
                perror("epoll_ctl");
                }
            }
            */

    // 4. 删除信号量
    for (auto sem : waiting_events.waiting_sems_) {
        auto iter = sem_infos_.find(sem);
        if (iter != sem_infos_.end()) {
            iter->second.fibers_.erase(fiber);
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
                this->cur_dispatch_fiber_ = nullptr; //fiber 运行完毕回到xfiber 所以 当前即将运行的fiber 为null 

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
        //>解答<: 部分超时 and 全部超时 
        //     -->  begin <= 当前时间 部分超时:  部分超时后,处理整个超时队列
        //                          --> 超时的fiber 进行唤醒,上cpu 执行内部自己的超时逻辑
        //                          --> 未超时的fiber: 就相当于 是提前唤醒
        //     --> begin >= 当前时间 全部都没有超时: 则彻底忽略整个 超时队列


        //>解答<: 超时队列 内部 未超时的 fiber 会被提前唤醒,失去超时属性, 一旦全部超时,则根本不进行处理.
        //      ================> 不进行处理的 后果:
        //      ================> 举例: 一个fiber 负责的fd 有超时时间, 但是已经超时很久了, 一直在超市队列里面等着
        //      ================> 直到 超时队列.begin <= 当前时间  ==> 视为 超时 ,然后 被唤醒 去执行自己的超时逻辑 之后推出


        //>重点<: :1. 如果最小的 超时时限都没有 超时, 那么全部都没有超时
        //     2. 如果 反写乘 >=  会导致所有未超时的fiber都会被再次上cpu,哪怕根本没有数据流入流出
        //     3. 如果 协程 >= 那么 超时时限 根本不会准确: 因为1多次 的上下cpu sleepms = 当前时间 + timeout
        //                      ==> 每一次上cpu timeout都会改变
        //     4. 为什么要写成这样? wakeup超时fiber 不是事与愿违吗? 超时的不是应该舍弃吗? wakeup之后 有注定会让 超时fiber上一次CPU? 这不是和初衷(超时舍弃)相违背吗
        //      ======> 超时的fiber唤醒, 唤醒后wakeup 
        //         ===> wakeup 先加入ready_对列 然后再从超时队列 等待集合 epoll中删除
        //         ==> 然后再次上cpu ,
        //>注意<:     此时: 是最后一次上cpu ,上cpu的目的是为了 //>提示<: 超时fiber进行自己的超时逻辑,退出!,再也找不到他了,彻底finished了
        //>重点<:   这样就保证了: 1. 超时队列里面全都是未超时的   
        //              2. 一旦fiber超时 就无效化(彻底finished了)(哪怕再次被触发也无效)
        //              3. 保证了 未超时fiber不会频繁上cpu 导致 超时时间线动态变化 ===>从而影响 到固定的超时时限(死线不会后移了)
        //              4. 一旦超时fiber 被最后一次上cpu 会自己去执行自己的内部的超时逻辑
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
            expire_events_.erase(expire_events_.begin());//1个大类的超时时间线fiber被全部唤醒完成
        }    







// 情况3: 处理epoll监听中被 触发的fd   
        // ===> 特殊: epoll事件 被触发后 立即调用WakeUpFiber 将该fiber 加入就绪队列 删除 等待集合中的可读可写fiber,
        //    删除超时队列里的 fiber
        #define MAX_EVENT_COUNT 512

            /*结构体细节: 事件+ 存储信息的联合体
                记住union的特性，它只会保存最后一个被赋值的成员，
                所以不要data.fd,data.ptr都赋值；
                通用的做法就是给ptr赋值，
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
            continue; //整个朱调度器 是一个死循环
        }


        //处理触发事件
        for (int i = 0; i < n; i++) {
            //获取事件单个epoll事件
            struct epoll_event &ev = evs[i];//evs 是epoll获取到数据 从rdlist 拷贝到此处的
            //epoll 事件 文件描述符
            int fd = ev.data.fd;


            // map中 fd 对应的是 一个waitting_fiber : witing_fiber:中有两份 fiber* : 读fiber对象 写fiber对象
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
//>注意<: Yiled 和switchtoscheduler 的区别: 到底是是否加入到就绪队列?
void XFiber::Yield() {
    // 当前被调度 的 fiber的 ctx 不为空
    assert(this->cur_dispatch_fiber_!= nullptr);// 说明当前调度器上的是fiber协程
    LOG_INFO("will Yield , ready_fibers_.size + 1");
    // 主动切出的后仍然是ready状态，等待下次调度
    ready_fibers_.push_back(this->cur_dispatch_fiber_);
    SwitchToScheduler();
}


//函数功能: 从当前的 正在执行的协程(fiber对象)切换出来, 转而去执行xfiber调度器
// 单纯的调用该函数, 不会讲ctx加入到就绪队列 ==> 一旦调用该函数, 就说明 该fiber 已经不再上cpu了
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
    
    // 为什么使用-1? 因为别的fd不可能是负数 为什么是false: sleepms单纯的就是为了 休眠,不参与任何读写
    // RegisterFdWithCurrFiber(-1,expired_at,false);
    WaitingEvents events;
    events.expire_at_ = expired_at;
    RegisterWaitingEvents(events);
    //切换回调度器
    SwitchToScheduler();
}



//>提示<: 第一版 :不能解决信号量的问题
//功能: 将目标fd 和当前fiber相互绑定
// fd 绑定 fiber: <map>io_waiting_ : fd 映射读写fiber
// fiber 绑定 fd: fiber 内部的 读写fd数组 中药新增该fd
//  超时时间: 将 超时时间 和当前fiber的关系 进行绑定 即expire_events
/*
bool XFiber::RegisterFdWithCurrFiber(int fd, int64_t expired_at, bool is_write) {
    auto iter = io_waiting_fibers_.find(fd);

    // 当前fd 尚未出现在map中
    if (iter == io_waiting_fibers_.end()) {
        WaitingFibers wb;
        if (is_write == false) {
            // 说明这是一个 专门写入数据的fiber对象 (暨协程)
            // wb是一个keyvalue结构, 保存的 是 fiber对象, fiber对象里面有 ctx上下文,ctx上下文继续去执行
            wb.r_ = this->cur_dispatch_fiber_;
            wb.w_ = nullptr;
            //初始化 
            this->cur_dispatch_fiber_->SetReadEvent(Fiber::FdEvent(fd));  
        } else {
            wb.w_ = this->cur_dispatch_fiber_;
            wb.r_ = nullptr;
            this->cur_dispatch_fiber_->SetWriteEvent(Fiber::FdEvent(fd));  
        }
        
    } else {
        // fd已经存在于 map中了 ,该fiber再一次的被调用之后, 上下文信息会发生改变,所以我们需要更新上下文信息
        if (is_write == false) {
            iter->second.r_ = this->cur_dispatch_fiber_;
            this->cur_dispatch_fiber_->SetReadEvent(Fiber::FdEvent(fd));  
        } else {
            iter->second.w_ = this->cur_dispatch_fiber_;
            this->cur_dispatch_fiber_->SetWriteEvent(Fiber::FdEvent(fd));  
        }
    }
    return true;
}
*/
//>提示<: 第二版 : 解决了信号量注册的问题
void XFiber::RegisterWaitingEvents(WaitingEvents &events) {
    assert(cur_dispatch_fiber_ != nullptr);
    if (events.expire_at_ > 0) {
        expire_events_[events.expire_at_].insert(cur_dispatch_fiber_);
        cur_dispatch_fiber_->SetExpireEvent(events);
        LOG_DEBUG("register fiber [%lu] with expire event at %ld", cur_dispatch_fiber_->Seq(), events.expire_at_);
    }

    for (size_t i = 0; i < events.waiting_fds_r_.size(); i++) {
        int fd = events.waiting_fds_r_[i];
        auto iter = io_waiting_fibers_.find(fd);
        if (iter == io_waiting_fibers_.end()) {
            io_waiting_fibers_.insert(std::make_pair(fd, WaitingFibers(cur_dispatch_fiber_, nullptr)));
            cur_dispatch_fiber_->SetReadEvent(events);
        }
    }

    for (size_t i = 0; i < events.waiting_fds_w_.size(); i++) {
        int fd = events.waiting_fds_w_[i];
        auto iter = io_waiting_fibers_.find(fd);
        if (iter == io_waiting_fibers_.end()) {
            io_waiting_fibers_.insert(std::make_pair(fd, WaitingFibers(nullptr, cur_dispatch_fiber_)));
            cur_dispatch_fiber_->SetWriteEvent(events);
        }
    }

    for (size_t i = 0; i < events.waiting_sems_.size(); i++) {
        Sem &sem = events.waiting_sems_[i];
        if (sem_infos_.find(sem) == sem_infos_.end()) {
            sem_infos_.insert(std::make_pair(sem, SemInfo()));
        }
        sem_infos_[sem].fibers_.insert(cur_dispatch_fiber_);
    }
}


//>提示<: 第一版 性能大概在3w~ 但是 会进行收尾工作. 只要是手动析构的,会进行最后一步收尾工作, 逻辑上更好!
// 将fd 取消注册
//>重点<: 知识取消了epoll注册,和各种的映射关系, 还需要最后一次上cpu 执行自己最后的处理逻辑
/*
bool XFiber::UnregisterFd(int fd) {
    auto iter = io_waiting_fibers_.find(fd);
    // 如果存在于 io_waitiing_fibers_ 映射中
    if (iter != io_waiting_fibers_.end()) {
        auto &waiting_fibers = iter->second;
        Fiber *fiber_r = waiting_fibers.r_;
        Fiber *fiber_w = waiting_fibers.w_;

        // 如果读 fd映射的负责读fiber不为空
        if (fiber_r != nullptr) {
           WaitingEvents &evs_r = fiber_r->GetWaitingEvents();
            for (size_t i = 0; i < evs_r.waiting_fds_r_.size(); i++) {

                if (evs_r.waiting_fds_r_[i] == fd) {
                    int64_t expired_at = evs_r.expire_at_; // 获取超时时间
                    if (expired_at > 0) { //存在超时时间
                        auto expired_iter = expire_events_.find(expired_at); 
                        // 断开超时队列 映射关系
                        if (expired_iter->second.find(fiber_r) == expired_iter->second.end()) {
                            LOG_ERROR("not fiber [%lu] in expired events", fiber_r->Seq());
                        }
                        else {
                            // 断开超时映射关系
                            expired_iter->second.erase(fiber_r);
                        }
                    }
                }
            }
        }
        // 读事件 超时队列断开映射
        if (fiber_w != nullptr) {
            WaitingEvents &evs_w = fiber_w->GetWaitingEvents();
            for (size_t i = 0; i < evs_w.waiting_fds_w_.size(); i++) {
                if (evs_w.waiting_fds_w_[i] == fd) {
                    int64_t expired_at = evs_w.expire_at_;
                    if (expired_at > 0) {
                        auto expired_iter = expire_events_.find(expired_at);
                        if (expired_iter->second.find(fiber_w) == expired_iter->second.end()) {
                            LOG_ERROR("not fiber [%lu] in expired events", fiber_w->Seq());
                        }
                        else {
                            expired_iter->second.erase(fiber_r);
                        }
                    }
                }
            }
        }
        io_waiting_fibers_.erase(iter);
    }
    else {
        LOG_INFO("fd[%d] not register into sched", fd);
    }

    // 从epoll中取消注册
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = fd;

    if (epoll_ctl(ep_fd_, EPOLL_CTL_DEL, fd, &ev) < 0) {
        LOG_ERROR("unregister fd[%d] from epoll efd[%d] failed, msg=%s", fd, ep_fd_, strerror(errno));
    }
    else {
        LOG_INFO("unregister fd[%d] from epoll efd[%d] success!", fd, ep_fd_);
    }
    return true;
}
*/
//>第二版<:  会进行最后一次wakeup 收尾 更好 性能 3w+ 
bool XFiber::UnregisterFd(int fd) {
    LOG_DEBUG("unregister fd[%d] from sheduler", fd);
    auto io_waiting_fibers_iter = io_waiting_fibers_.find(fd);
    // assert(io_waiting_fibers_iter != io_waiting_fibers_.end());

    if (io_waiting_fibers_iter != io_waiting_fibers_.end()) {
        WaitingFibers &waiting_fibers = io_waiting_fibers_iter->second;
        if (waiting_fibers.r_ != nullptr) {
            //为什么这里是唤醒?
            //>重点<:   wakeup 内部 会自动的断开其余的多余映射 and 处理 超时队列 (提前唤醒)
            WakeUpFiber(waiting_fibers.r_);
        }
        if (waiting_fibers.w_ != nullptr) {
            WakeUpFiber(waiting_fibers.w_);
        }

        io_waiting_fibers_.erase(io_waiting_fibers_iter);
    }

    struct epoll_event ev;
    if (epoll_ctl(ep_fd_, EPOLL_CTL_DEL, fd, &ev) < 0) {
        LOG_ERROR("unregister fd[%d] from epoll efd[%d] failed, msg=%s", fd, ep_fd_, strerror(errno));
    }
    else {
        LOG_INFO("unregister fd[%d] from epoll efd[%d] success!", fd, ep_fd_);
    }
    return true;
}




//功能: 将事件集合 注册给对应fiber对象
// >重点<: 超时时间针对的是一整个fiber
//>提示<: 超时时间,是整个fiber协程对象的生存线
// 注册时, 需要将 fiber | 超时界限 注册到 全局超时队列里面去
// 同时 需要 根据传进来的 waitingevent 来分别初始化 fiber的 读写事件
// void XFiber::RegisterWaitingEvents(WaitingEvents &events) {
//     // assert(curr_fiber_ != nullptr);
//     assert(cur_dispatch_fiber_ != nullptr);
    
    
    
//     //如果有超时时间设置: 将超时时间加入到超时队列并设置fiber的waitingevents
//     // 超时队列: 超时时间,<fiber*,fiber*> 映射
//     if (events.expire_at_ > 0) {
//         // expire_events_[events.expire_at_].insert(curr_fiber_);
//         //超时事件集合中 加入 当前 已经设置里超时时间的 fiber

//         // 调度器的全局超时队列中加入当前正在运行的fiber
//         expire_events_[events.expire_at_].insert(cur_dispatch_fiber_);

//         // 初始化当前fiber的 waitingevents
//         cur_dispatch_fiber_->SetWaitingEvent(events);
//         // curr_fiber_->SetWaitingEvent(events);
//         LOG_DEBUG("register fiber [%lu] with expire event at %ld", cur_dispatch_fiber_->Seq(), events.expire_at_);
//     }

//     //处理读事件: 将读事件 加入到等待集合 并设置fiber的waitingevents
//     for (size_t i = 0; i < events.waiting_fds_r_.size(); i++) {
//         int fd = events.waiting_fds_r_[i];
//         auto iter = io_waiting_fibers_.find(fd);
//         if (iter == io_waiting_fibers_.end()) {
//             // 所有的 读事件 都加入到 等待集合epoll 监听映射的map 中去
//             // pair: <fd,(r,w)> : 即 当前监听目标fd 和 当前fiber())
//             io_waiting_fibers_.insert(std::make_pair(fd, WaitingFibers(cur_dispatch_fiber_, nullptr)));


//             // 分别设置 当前fiber的 读事件
//             cur_dispatch_fiber_->SetWaitingEvent(events);
//         }
//     }
//     //处理写事件: 将写事件 加入到等待集合 并设置fiber的waitingevents
//     for (size_t i = 0; i < events.waiting_fds_w_.size(); i++) {
//         int fd = events.waiting_fds_w_[i];
//         auto iter = io_waiting_fibers_.find(fd);
//         if (iter == io_waiting_fibers_.end()) {
//             // 所有的 写事件都加入到 等待集合map中去
//             io_waiting_fibers_.insert(std::make_pair(fd, WaitingFibers(nullptr, cur_dispatch_fiber_)));


//             //分别设置当前fiber自身的 waitingevent 中的写事件
//             cur_dispatch_fiber_->SetWaitingEvent(events);
//         }
//     }
//     //处理信号量: 将信号量加入到信号集合 waitingevents
//     for (size_t i = 0; i < events.waiting_sems_.size(); i++) {
//         Sem &sem = events.waiting_sems_[i];
//         // 没有这个信号量存在,则新插入
//         if (sem_infos_.find(sem) == sem_infos_.end()) {
//             sem_infos_.insert(std::make_pair(sem, SemInfo()));
//         }
//         //>重点<: 为什么这里和生面的不一样?
//         //>解答<: 
//         sem_infos_[sem].fibers_.insert(cur_dispatch_fiber_);
//     }
// }


// map: fd waitingfibers 
// waitingfibers : fiber1* fiber2*
// waitingevents: 读数组 写数组 信号量数组 超时时间
// >重点<: 根据 传入的 waitingevents 来分别设置 fiber的 读写事件 (读事件 写事件分开))
// >提示<: 传入的events 是临时变量 ,fiber内部也有一个和这个一样结构的events事件集合
// 功能: 将events内部的事件 注册到fd中 (超时事件,r_fd,w_fd))
// void Fiber::SetWaitingEvent(const WaitingEvents &events) {

//     //注册读事件 
//     for (size_t i = 0; i < events.waiting_fds_r_.size(); i++) {

//         //fiber 的waitingevents
//         this->waiting_events_.waiting_fds_r_.push_back(events.waiting_fds_r_[i]);
//     }
//     // 注册写事件
//     for (size_t i = 0; i < events.waiting_fds_w_.size(); i++) {
//         this->waiting_events_.waiting_fds_w_.push_back(events.waiting_fds_w_[i]);
//     }
//     // 设置超时时间: 就是整个fiber对象的 生存时间线
//     if (events.expire_at_ > 0) {
//         this->waiting_events_.expire_at_ = events.expire_at_;
//     }
// }








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


/*
//从epoll中取消注册某个fd
//>提问<: 为什么不在超时队列里面也删除?
//>解答<: 从epoll中删除 就不会被触发了 等到超时,他自己就会wakeup 之后自己自觉注销
bool XFiber::UnregisterFd(int fd) {
    LOG_DEBUG("unregister fd[%d] from sheduler", fd);
    auto io_waiting_fibers_iter = io_waiting_fibers_.find(fd);
    // assert(io_waiting_fibers_iter != io_waiting_fibers_.end());

    //第一步: 从等待集合map中删除 该fd对应的 读写fibers ==> fd都不见听了 fiber也就没用了
    if (io_waiting_fibers_iter != io_waiting_fibers_.end()) {
        WaitingFibers &waiting_fibers = io_waiting_fibers_iter->second;
        // 删除之前,需要先 将他所负责的事情 进行交接 该干的要干完
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

*/




//>注意<: 这一居有什么用? 为什么这么写? 和SEQ有什么关系?
//>解答<: 可能和 网络通信阶段的 三握的seq油管
thread_local uint64_t fiber_seq = 0;

/* 函数参数解析
1. fiber对象(暨背调度的协程)的入口函数地址
2. 该入口函数地址是保存在 uc_mcontext中的:
3. uc_mcontext:保存所有上下文信息,寄存器信息,入口函数地址
4. stack_size:制定站空间多钱啊小
5. fiber_name: 当前协程 name
6. xfiber: 调度器
*/
Fiber::Fiber(std::function<void ()> run, XFiber *xfiber, size_t stack_size, std::string fiber_name) {
    
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

    /*
        makecontext参数解析:
        1. &this->ctx_: 当前的上下文信息
        2. Start: 协程入口函数地址
        3. "1": 后续参数个数
        4. "this": this指针
    */
    makecontext(&this->ctx_, (void (*)())Fiber::Start, 1, this);



    seq_ = fiber_seq;
    fiber_seq++;



    //makecontext成功之后, xfiber 单线程主协程被替换成为一个崭新的协程
    // so 当前fiber的状态时 INIT
    status_ = FiberStatus::INIT;
}

//>注意<: 销毁一个协程
//需要销毁一个协程的栈空间
Fiber::~Fiber() {
    // 删除 fiber的携程栈
    delete []stack_ptr_;
    stack_ptr_ = nullptr;
    stack_size_ = 0;
}


uint64_t Fiber::Seq() {
    return this->seq_;
}
// 获取 当前fiber 的上下文信息
XFiberCtx *Fiber::Get_Fiber_Ctx() {
    return &ctx_;
}

void Fiber::Start(Fiber * fiber) {
    // 在这里 执行 fiber真正的处理函数也就是入口函数
    fiber->entry_func_();  // 在这里之后 陷入执行逻辑


    // 处理完成 fiber彻底结束 ,切换状态
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
    //         UCBA_fiber_ = nullptr;
    //         UCBA_value_ = 0;
    //         fibers_.clear();
    //     }

    //     int32_t value_; 
    //     // 正在尝试获取的fiber
    //     Fiber *UCBA_fiber_;
    //     // 正在尝试获取的value_
    //     int32_t UCBA_value_;
    //     std::set<Fiber *> fibers_;
    // };




















// 信号量注册: 将信号量 加入到xfiber::信号量集合中去
// valer 表示信号量的多少
void XFiber::RegisterSem(Sem *sem, int32_t value) {
    sem_infos_.insert(std::make_pair(*sem, SemInfo(value)));
}

// 信号量的注销
void XFiber::UnregisterSem(Sem *sem) {
    //sem_info: xfiber内部的 信号量集合std::map<Sem, XFiber::SemInfo> XFiber::sem_infos_
    auto sem_infos_iter = sem_infos_.find(*sem);

    // 取消注册的前提是一定要注册过了
    assert(sem_infos_iter != sem_infos_.end());
    //sem_infos_iter->second === SemInfo sem_infos_iter->second.fibers_ === SemInfo.fibers_ // 就是当前申请信号量的fiber
    
    // 取消了信号量之后,要把这个SemInfo 内部 所有的 等待fibers 进行唤醒


    //>注意<: 如果不唤醒,可能就永远也不会通知到了
    //>重点<: sem_infos_iter->second.fibers_的fibers_ 是一个set
    //     ==> 一个信号量可以 管理 多个fibers 

    for (auto fiber: sem_infos_iter->second.fibers_) {
        WakeUpFiber(fiber);
    }
    //唤醒完成之后 清除 SemInfo内部等待该信号量的fiber(协程们)
    sem_infos_iter->second.fibers_.clear();

    // 现在SemInfo的set集合清除完毕,
    // 还需要在 xfiber的信号量集合中(key:sem value:Seminfo) 中彻底断绝关系
    sem_infos_.erase(sem_infos_iter);
}


int32_t XFiber::TryAcquireSem(Sem *sem, int32_t value) {
    // xfiber中 sem_info_s 中是否存在这个 已经注册过了的 信号量
    auto iter = sem_infos_.find(*sem);

    // 获取这个sem 对应的 SemInfo
    SemInfo &sem_info = iter->second;

    // 如果当前sem信号量,还没有欠款 即 没有上门要债的大债主 : 就直接满足申请者 
    if (sem_info.UCBA_fiber_ == nullptr) {
        // 当前可以使用的信号量个数 > 目标fiber需要申请的信号量个数
        if (sem_info.value_ >= value) {
            sem_info.value_ -= value; //更新当前sem持有资产
            return value;//返回成功申请的个数
        }
        // 当前Sem 不能够一次性满足, 就先净身出户, 然后记下 上门要债的大债主的名字
        int32_t done_value = sem_info.value_;
        sem_info.value_ = 0;
        sem_info.UCBA_value_ = done_value;
        sem_info.UCBA_fiber_ = cur_dispatch_fiber_;// 记录上门要债的大债主的名字
        // sem_info.fibers_.insert(cur_dispatch_fiber_);
        sem_info.fibers_.erase(cur_dispatch_fiber_); //Sem最多有且只能有一个大债主,大债主 一旦拿上资产了,就算做是上门要债,
        return done_value;
    }

    // 上门要债的大债主,不是当前申请者, 不能给钱, 要先给债主还钱,不然要出大问题!
    if (cur_dispatch_fiber_ != sem_info.UCBA_fiber_) {
        sem_info.fibers_.insert(cur_dispatch_fiber_); //我知道你也要申请,但是,大债主已经上门要债了, 你们没有上门要债的,先排队
        return 0;
    }


    // 当前的申请者 刚刚好是上门要债的大债主: 大债主两次上门了: 赶快给人家把钱算清了

    sem_info.fibers_.erase(cur_dispatch_fiber_);
    // 如果我现在的资产,可以还清
    if (sem_info.value_ >= value) {
        sem_info.value_ -= value; //把钱给人家
        // 上门债主拿够了:0 没有够: 就是他已经拿到的
        sem_info.UCBA_value_ = 0;   //把所有的债都给上门债主了, 所以现在
        sem_info.UCBA_fiber_ = nullptr; //
        // if (sem_info.fibers_.find(cur_dispatch_fiber_) != sem_info.fibers_.end()) {
        //     sem_info.fibers_.erase(cur_dispatch_fiber_);
        // }
        return value;
    }
    else {
        // 现在的资产无法还清, 继续全部资产给上门债主
        int32_t done_value = sem_info.value_;
        sem_info.value_ = 0;
        sem_info.UCBA_value_ += value;
        sem_info.UCBA_fiber_ = cur_dispatch_fiber_;
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
    // 一个信号量释放时,需要把 所有的等待的fiber唤醒
    for (auto fiber: sem_info.fibers_) {
        WakeUpFiber(fiber);
    }
    sem_info.fibers_.clear();
}



// 信号量恢复:将当前的已经申请到的信号量 释放
// 就是上门债主,主动放弃要债, 把到手的全部都给你,他不再想你讨债了
void XFiber::RestoreSem(Sem *sem, int32_t value) {
    auto iter = sem_infos_.find(*sem);
    assert(iter != sem_infos_.end());

    SemInfo &sem_info = iter->second;
    sem_info.value_ += value;
    sem_info.UCBA_fiber_ = nullptr;
    sem_info.UCBA_value_ = 0;
    // if (sem_info.fibers_.find(cur_dispatch_fiber_) != sem_info.fibers_.end()) {
    //     sem_info.fibers_.erase(cur_dispatch_fiber_);
    // }
    LOG_INFO("restore sem will wakeup %lu fibers", sem_info.fibers_.size());
    // 依次循环处理其他排队的债主
    for (auto fiber: sem_info.fibers_) {
        WakeUpFiber(fiber);// 上门债主已经回家了,下一个排队的债主 继续来要债吧
    }
    sem_info.fibers_.clear();
}




















// 每一个线程内部只存在一个: 注意是线程 单进程 单线程 内 多协程情况下是 单例
// >提示<: 用来表示 全局总共创建了多少个信号量
thread_local int64_t Sem::sem_seq = 0;

//Sem 结构体初始化
//sem_seq 就是信号量的编号吧
// 传入的value ,是使用register 注册时,来填充信号量的多少
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
        //尝试申请信号量
        done_value += xfiber->TryAcquireSem(this, apply_value - done_value); // apply_value - done_value === 余下的信号量

        if (done_value == apply_value) { //已经申请到了 可以满足的信号量
            return true;
        }

        // 已经超时
        //>提问<:为什么要 判断已经超时? 
        //>解答<: 已经超时==> 整个fiber就懒得动了 ,所以就无效了 expire_at(died_line<现在事件) === 超时
        //如果已经超时,那么该fiber内部逻辑就已经不重要了,就应该 释放所有申请到的信号量,
        if (expire_at >= 0 && expire_at <= util::NowMs()) { 
            LOG_INFO("acquire sem timeout, restore %d sems", done_value);
            xfiber->RestoreSem(this, done_value); //  恢复 已经分配的信号量 (因为超时,所以就别占着茅坑了)
            return false;
        }


        //>重点<: 尚未超时
        WaitingEvents ev; 
        // LOG_INFO("还没有超时");
        ev.expire_at_ = expire_at;//超时时间

        //>提示<: y由于fiber对象申请到了信号量,所以就需要在events 内部维护
        
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
