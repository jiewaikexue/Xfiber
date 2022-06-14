
#include <stdio.h>
#include <signal.h>
#include <iostream>
#include <string>
#include <cstring>
#include <memory>

#include "xfiber.h"
#include "xsocket.h"

using namespace std;


void sigint_action(int sig) {
    std::cout << "exit..." << std::endl;
    exit(0);    
}

// #define DEBUG_ENABLE
#define DPRINT(fmt, args...) fprintf(stderr, "[D][%s %d] " fmt"\n", __FILE__, __LINE__, ##args);


int main() {
    signal(SIGINT, sigint_action);

    XFiber *xfiber = XFiber::xfiber();
    /*xfiber->AddTask([&]() {
        cout << "hello world 11" << endl;
        xfiber->Yield();
        cout << "hello world 12" << endl;
         xfiber->Yield();
        cout << "hello world 13" << endl;
        xfiber->Yield();
        cout << "hello world 14" << endl;
        cout << "hello world 15" << endl;

    }, 0, "f1");

    xfiber->AddTask([]() {
        cout << "hello world 2" << endl;
    }, 0, "f2");
    */

    // xfiber->CreateFiber([xfiber]{
    //     for (int i = 0; i < 10; i++) {
    //         cout << i << endl;
    //         xfiber->SleepMs(1000);
    //     }
    // });

    // std::list<int> buffer;
    // Sem sem(1);
    // xfiber->CreateFiber([&]{
    //     int index = 0;
    //     while (true) {
    //         sem.Acquire(1);// 默认抄手事件是-1 该fiber 第一次运行 就已经超时了 
    //                 //因为两行分开的 nowms,所以注定第一次调用就returnfalse
    //         buffer.push_back(index++); //之后 继续运行 直到yield
    //         LOG_INFO("produce %d", index-1);
    //         sem.Release(1);
    //         xfiber->Yield();//yield之后 加入ready队列 之后进行第二个fiber调用
    //     }
    // });

    // xfiber->CreateFiber([&]{ //这个fiber第一次调用上cpu之后 会运行一次,运行完,之后会注册到超时队列,之后 ready队列为0 超时队列为1 但未超时,所以一直是xfiber调度器在运行,直到超时后,wakeup将这个 释放
    //     while (true) {
    //         if (sem.Acquire(2, 10000)) { //在这里就已经切出了 acquire设置超时的话 会自动注册并且切换
    //             if (buffer.size() > 0) {
    //                 int val = buffer.front();
    //                 buffer.pop_front();
    //                 LOG_INFO("consumer %d", val);
    //             }
    //             sem.Release(1);
    //         }
    //         xfiber->Yield();
    //     }
    // });

    xfiber->CreateFiber([&]{
       Listener listener = Listener::ListenTCP(7000);
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
       }
    });
    
    xfiber->Dispatch();

    return 0;
}
