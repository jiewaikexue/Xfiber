#include <stdio.h>
#include <signal.h>
#include <iostream>
#include <string>
#include <cstring>
#include <memory>

#include "xfiber.h"
#include "xsocket.h"

using namespace std;

void sigint_action(int sig)
{
    std::cout << std::endl
              << "exit......" << std::endl;
    exit(0);
}

#define DPRINT(fmt, args...) fprintf(stderr, "[D][%s %d] " fmt "\n", __FILE__, __LINE__, ##args);
// #define USE_REDIS_BENCHMARK
// #define USE_TSET_MYSLEEP
// #define USE_IO_PRINTF
#define USE_SEM
int main()
{

    // 程序终止( interrupt )信号, 在用户键入 INTR 字符(通常是 Ctrl + C )时发出，用于通知前台进程组终止进程。
    // init信号注册的处理动作是该函数
    signal(SIGINT, sigint_action);
    XFiber *xfiber = XFiber::xfiber();
#ifdef USE_TEST_MYSLEEP
    xfiber->CreateFiber([xfiber]
                        {
        for (int i = 0; i < 10; i++) {
            cout << i << endl;
            xfiber->SleepMs(1000);
        } });
#endif
#ifdef USE_SEM
    std::list<int> buffer;
    Sem sem(4); // 当信号量为1 其实就是退化成了一把互斥锁
    xfiber->CreateFiber([&]
                        {
        int index = 0;
        while (true) {
            sem.Acquire(1);
            buffer.push_back(index++);
            LOG_INFO("produce %d", index-1);//生产者
                    std::cout << "--------------------------------------------------------------------------" << std::endl;
            std:: cout<<"produce: buffer size is " << buffer.size() << " at now" << std::endl;
                    std::cout << "--------------------------------------------------------------------------" << std::endl;
            sem.Release(1);
            xfiber->Yield();
        } });

    xfiber->CreateFiber([&]
                        {
        while (true) {
            if (sem.Acquire(4, 10000)) {
                if (buffer.size() >= 4 ) {
                    int val = buffer.front();
                    buffer.pop_front();
                    buffer.pop_front();
                    buffer.pop_front();
                    buffer.pop_front();
                    LOG_INFO("consumer %d", val);
                    std::cout << "--------------------------------------------------------------------------" << std::endl;
                    std::cout << "+ cosumer :buffer size is " << buffer.size() << " at now" << std::endl;
                    std::cout << "--------------------------------------------------------------------------" << std::endl;
                }
                sem.Release(4);
            }
            xfiber->Yield();
        } });
#endif

// redis benchmark 测试
#ifdef USE_REDIS_BENTCHMARK
    xfiber->CreateFiber([&]
                        {
       Listener listener = Listener::ListenTCP(6379);
       while (true) {
           shared_ptr<Connection> conn1 = listener.Accept();
           //shared_ptr<Connection> conn2 = Connection::ConnectTCP("127.0.0.1", 6379);

           xfiber->CreateFiber([conn1] {
               while (true) {
                   char recv_buf[512];
                   int n = conn1->Read(recv_buf, 512, 50000);
                   if (n <= 0) {
                       break;
                   }

#if 0
                   conn2->Write(recv_buf, n);
                   char rsp[1024];
                   int rsp_len = conn2->Read(rsp, 1024);
                   cout << "recv from remote: " << rsp << endl;
                   conn1->Write(rsp, rsp_len);
#else
                   if (conn1->Write("+OK\r\n", 5, 1000) <= 0) {
                       break;
                   }
#endif
               }
           }, 0, "server");
       } });
#endif
// 单个线程io测试
#ifdef USE_IO_PRINTF
    // 这个fiber对象 只负责监听调用
    xfiber->CreateFiber([&]
                        {
       Listener listener = Listener::ListenTCP(8888); //任何有效的地址 + 8888端口
       while (true) {
           shared_ptr<Connection> conn1 = listener.Accept();
           //shared_ptr<Connection> conn2 = Connection::ConnectTCP("127.0.0.1", 6379);

            //又在这里创建了一个新的fiber对象交给这个fiber对象来负责处理数据 
           xfiber->CreateFiber([conn1] {
               while (true) {
                   char recv_buf[512];
                //    int n = conn1->Read(recv_buf, 512, 50000);//超时时间:如果超时了怎么办?这个red是有时间限制的 超时之后,就不会再red了,cilent写入会失败退出
                   int n = conn1->Read(recv_buf, 512, -1);
                   if (n <= 0) {
                       break;
                   }
                   printf("                +----------------------------------------------------------------\n");
                   printf("                +receive: %s\n",recv_buf);
                   printf("                +----------------------------------------------------------------\n");
               }
           }, 0, "server_recv");
       } });
#endif
    xfiber->Dispatch();

    return 0;
}
