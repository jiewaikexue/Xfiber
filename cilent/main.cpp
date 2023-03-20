
#include<unistd.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<stdio.h>
#include<stdlib.h>
#include<fcntl.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/ioctl.h>
#include<string.h>
 
int main()
{
    //创建一个socket
    int clientSocket=socket(AF_INET,SOCK_STREAM,0);
    
    //配置ip port 协议
    struct sockaddr_in addrSrc;
    memset(&addrSrc,0,sizeof(struct sockaddr_in));
 
    addrSrc.sin_family=AF_INET;
    addrSrc.sin_port=htons(8888);
    addrSrc.sin_addr.s_addr=inet_addr("127.0.0.1");
 
    connect(clientSocket,(const struct sockaddr *)&addrSrc,sizeof(struct sockaddr_in));
    
    //接受信息    
    char recvBuf[1024]={0};
    // recv(clientSocket,recvBuf,sizeof(recvBuf)-1,0);
    // printf("recv from server is :%s\n",recvBuf);
    
    while(1)
    {    
        //发送信息
        char sendBuf[100]={0};
        scanf("%s",sendBuf);
        int n = send(clientSocket,sendBuf,strlen(sendBuf)+1,0);
        printf ("send res : %d\n",n);
    }
    
    //关闭套接字
    close(clientSocket);
    
    return 0;
}