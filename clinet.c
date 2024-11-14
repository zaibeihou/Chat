#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h> 
#include <arpa/inet.h>
#include <ctype.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <locale.h>

#define SERVER_PORT 8000

void sys_error(const char* str){
    perror(str);
    exit(-1);
}

// 添加一个线程函数来处理发送消息
void* send_msg(void* arg) {
    int cfd = *(int*)arg;
    char buf[BUFSIZ];
    while(1) {
        // 从标准输入读取消息
        int n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
            continue;
        }
        // 发送给服务器
        write(cfd, buf, n);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int cfd;
    struct sockaddr_in server_addr;
    char buf[BUFSIZ];
    
    // 创建socket
    if ((cfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        sys_error("socket error");
    }
    
    // 设置服务器地址结构
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr.s_addr);
    
    // 连接服务器
    if (connect(cfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        sys_error("connect error");
    }
    printf("您已经成功连接服务器\n");
    printf("请登录：");
    fflush(stdout);
    // 创建发送消息的线程
    pthread_t tid;
    pthread_create(&tid, NULL, send_msg, &cfd);
    
    // 主线程负责接收消息
    while(1) {
        int n = read(cfd, buf, sizeof(buf));
        if (n <= 0) {  // 服务器关闭或出错
            sys_error("server closed connection");
        }
        // 将接收到的消息写到标准输出
        write(STDOUT_FILENO, buf, n);
    }
    
    close(cfd);
    return 0;
}