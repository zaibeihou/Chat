#include <stdio.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <locale.h>

#define MAX_EVENTS 1024
#define SERVER_PORT 8000

#define COLOR_RED    "\033[31m"      // 红色
#define COLOR_GREEN  "\033[32m"      // 绿色
#define STYLE_BOLD   "\033[1m"       // 粗体
#define COLOR_RESET  "\033[0m"       // 重置所有属性

//=============== 数据结构 ===============
struct user
{
   int fd;
   char name[32];
   char password[32];
};

struct client_node
{
   struct user user;                  
   struct client_node *next; 
};

struct my_events
{
   void *m_arg;                                     // 泛型参数，难点
   int m_event;                                     // 监听的事件
   int m_fd;                                        // 监听的文件描述符
   void (*call_back)(int fd, int event, void *arg); // 回调函数

   char m_buf[BUFSIZ];
   char m_id[32];
   int m_buf_len;
   int m_status;      // 是否在红黑树上, 1->在, 0->不在
   time_t m_lasttime; // 最后放入红黑树的时间
};

//=============== 全局变量 ===============
struct client_node *client_list;
int online_count = 0;
volatile sig_atomic_t server_running = 1;
int ep_fd;                              // 红黑树根（epoll_create返回的句柄）
struct my_events ep_events[MAX_EVENTS]; // 定义于任何函数体之外的变量被初始化为0（bss段）

// =============== 函数声明和定义 ===============
//链表操作函数
void init_list()
{
   client_list = (struct client_node *)malloc(sizeof(struct client_node));
   client_list->next = NULL; // 虚拟头节点的fd不需要赋值
}
void client_list_add(int fd,char *name)
{ // 头插法添加
   struct client_node *node = (struct client_node *)malloc(sizeof(struct client_node));
   node->user.fd = fd;
   strcpy(node->user.name, name);
   node->next = client_list->next;
   client_list->next = node;
}
void client_list_delete(int fd)
{
   struct client_node *curr = client_list;
   while (curr->next != NULL)
   {
      if (curr->next->user.fd == fd)
      {
         struct client_node *temp = curr->next;
         curr->next = curr->next->next;
         free(temp);
         return;
      }
      curr = curr->next;
   }
}
void cleanup_client_list() {
   struct client_node *curr = client_list;
   while (curr != NULL) {
      struct client_node *next = curr->next;
      free(curr);
      curr = next;
   }
}
//字符串处理
int login_str(const char *input ,char *name, char *password, size_t name_size, size_t password_size){
   char temp_username[64] = {0};  // 临时缓冲区设置大一点
   char temp_password[64] = {0};  // 临时缓冲区设置大一点
    
   // 读取到换行符之前的两个字符串
   int matched = sscanf(input, "%63s %63s", temp_username, temp_password);
    
   if (matched != 2) {
      return -1;  // 格式不正确
   }
    
   // 安全地复制到目标缓冲区
   strncpy(name, temp_username, name_size - 1);
   name[name_size - 1] = '\0';
    
   strncpy(password, temp_password, password_size - 1);
   password[password_size - 1] = '\0';
    
   return 0;
}

//
/*初始化监听socket*/
void initlistensocket(int ep_fd, unsigned short port);
/*将结构体成员变量初始化*/
void eventset(struct my_events *my_ev, int fd, void (*call_back)(int fd, int event, void *arg), void *event_arg);
/*向红黑树添加 文件描述符和对应的结构体*/
void eventadd(int ep_fd, int event, struct my_events *my_ev);
/*从红黑树上删除 文件描述符和对应的结构体*/
void eventdel(int ep_fd, struct my_events *ev);
/*发送数据*/
void senddata(int client_fd, int event, void *arg);
/*接收数据*/
void recvdata(int client_fd, int event, void *arg);
/*回调函数: 接收连接*/
void acceptconnect(int listen_fd, int event, void *arg);
//信号捕捉函数
void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\nReceived signal %d, shutting down server...\n", sig);
        server_running = 0;
    }
}
//清理资源的函数
void cleanup_resources() {
   printf("Starting cleanup...\n");
    
   if (client_list == NULL) {
      printf("Client list is already NULL\n");
      return;
   }
    
   // 1. 先关闭所有客户端连接
   struct client_node *curr = client_list->next;
   while (curr != NULL) {
      // 保存下一个节点的指针，因为当前节点即将被关闭
      struct client_node *next = curr->next;
        
      // 给客户端发送服务器关闭消息
      char shutdown_msg[] = "Server is shutting down. Goodbye!\n";
      send(curr->user.fd, shutdown_msg, strlen(shutdown_msg), 0);
        
      // 关闭socket
      close(curr->user.fd);
        
      // 从epoll中移除
      for (int i = 0; i < MAX_EVENTS; i++) {
            if (ep_events[i].m_status == 1 && ep_events[i].m_fd == curr->user.fd) {
               eventdel(ep_fd, &ep_events[i]);
               break;
            }
         }
        curr = next;
      }
    
    // 2. 然后释放链表内存
    cleanup_client_list();
    client_list = NULL;  // 防止重复释放
    
    // 3. 最后关闭epoll描述符
    if (ep_fd > 0) {
        close(ep_fd);
        ep_fd = -1;  // 防止重复关闭
    }
    
    printf("Cleanup complete.\n");
}
//广播函数
void broadcast(struct my_events *ev, char *buf)
{ 
   struct client_node *curr = client_list->next;
   int len;
   while (curr != NULL)
   {
      /*if (curr->fd == ev->m_fd){
         curr = curr->next;
         continue;
      }*/
      len = send(curr->user.fd, buf, strlen(buf), 0); // 回写
      printf("send success\n");
      if (len < 0)
      {
         printf("\n send[fd=%d] to %d error \n", ev->m_fd, curr->user.fd);
      }
      curr = curr->next;
   }
}
//检测重复名函数
int id_exists(const char *name){
   int is_exist = 0;
   struct client_node *cur = client_list->next;
   while(cur != NULL){
      if(strcmp(cur->user.name,name) == 0){
         is_exist = 1;
         break;
      }
      cur = cur->next;
   }
   return is_exist;
}
//用户名 密码核对
int verify_user(char *user_name, char *user_password){
   FILE *fp = fopen("user.txt", "r");
   if(fp == NULL){
      perror("open user.txt error");
      return 0;
   }
                  
   char line[64];
   char file_username[32];
   char file_password[32];

   while(fgets(line,sizeof(line),fp)){
      if(sscanf(line,"%s %s",file_username, file_password) == 2){
         if (strcmp(user_name, file_username) == 0 && strcmp(user_password, file_password) == 0) {
            fclose(fp);
            return 1; // 验证成功
         } 
      }
   }
   fclose(fp);
   return 0;
}

void list_online(int cfd){
   struct client_node *cur = client_list->next;
   char buf[BUFSIZ] = {0};

   sprintf(buf,"%s%s\n当前在线人数: %d 人\n在线列表：", COLOR_GREEN,STYLE_BOLD,online_count);
   while(cur != NULL){
      char user[36];
      sprintf(user,"\n - %s",cur->user.name);
      strcat(buf,user);
      cur = cur->next;
   }
   strcat(buf,"\n—————————————————————————————————————————————\n");
   strcat(buf,COLOR_RESET);
   send(cfd, buf, strlen(buf),0);
}

int main(void)
{
   unsigned short port = SERVER_PORT;
   setlocale(LC_ALL, "zh_CN.UTF-8");
   struct sigaction sa;
   sa.sa_handler = handle_signal;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = 0;
    
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        exit(1);
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction SIGTERM");
        exit(1);
    }

   ep_fd = epoll_create(MAX_EVENTS); // 创建红黑树,返回给全局变量ep_fd;
   if (ep_fd <= 0)
   {
      perror("epoll_create error");
      exit(-1);
   }
   /*初始化监听socket*/
   initlistensocket(ep_fd, port);
   init_list();
   int checkpos = 0;
   int i;
   struct epoll_event events[MAX_EVENTS]; // epoll_wait的传出参数(数组：保存就绪事件的文件描述符)
   while (server_running)
   {
      /*超时验证,每次测试100个连接,60s内没有和服务器通信则关闭客户端连接*/
      long now = time(NULL);                // 当前时间
      for (i = 0; i < 100; i++, checkpos++) // 一次循环检测100个，使用checkpos控制检测对象
      {
         if (checkpos == MAX_EVENTS - 1)
            checkpos = 0;
         if (ep_events[i].m_status != 1) // 不在红黑树上
            continue;

         long spell_time = now - ep_events[i].m_lasttime; // 客户端不活跃的时间
         if (spell_time >= 600)                           // 如果时间超过60s
         {
            printf("[fd= %d] timeout \n", ep_events[i].m_fd);
            close(ep_events[i].m_fd);       // 关闭与客户端连接
            eventdel(ep_fd, &ep_events[i]); // 将客户端从红黑树摘下
         }
      }

      /*监听红黑树,将满足条件的文件描述符加至ep_events数组*/
      int n_ready = epoll_wait(ep_fd, events, MAX_EVENTS, 0); // 1秒没事件满足则返回0
      if (n_ready < 0 && errno != EINTR)                      // EINTR：interrupted system call
      {
         perror("epoll_wait");
         break;
      }

      for (i = 0; i < n_ready; i++)
      {
         // 将传出参数events[i].data的ptr赋值给"自定义结构体ev指针"
         struct my_events *ev = (struct my_events *)(events[i].data.ptr);
         if ((events[i].events & EPOLLIN) && (ev->m_event & EPOLLIN)) // 读就绪事件
            ev->call_back(ev->m_fd, events[i].events, ev->m_arg);
         if ((events[i].events & EPOLLOUT) && (ev->m_event & EPOLLOUT)) // 写就绪事件
            ev->call_back(ev->m_fd, events[i].events, ev->m_arg);
      }
   }
   printf("Server shutdown running.\n");
   cleanup_resources();
   printf("Server shutdown complete.\n");
   
   return 0;
}

/*初始化监听socket*/
void initlistensocket(int ep_fd, unsigned short port)
{
   int listen_fd;
   struct sockaddr_in listen_socket_addr;

   // printf("\n initlistensocket() \n");

   /*申请一个socket*/
   listen_fd = socket(AF_INET, SOCK_STREAM, 0);
   fcntl(listen_fd, F_SETFL, O_NONBLOCK); // 将socket设置为非阻塞模式
   /*使I/O变成非阻塞模式(non-blocking)，在读取不到数据或是写入缓冲区已满会马上return，而不会阻塞等待。*/
   int opt = 1;
   setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 设置为可以端口复用
   /*SO_REUSEADDR：（这通常是重启监听服务器时出现，若不设置此选项，则bind时将出错。）
     SOL_SOCKET：To manipulate options at the sockets API level, level is  specified  as  SOL_SOCKET.*/

   /*绑定前初始化*/
   bzero(&listen_socket_addr, sizeof(listen_socket_addr));
   listen_socket_addr.sin_family = AF_INET;
   listen_socket_addr.sin_port = htons(port);
   listen_socket_addr.sin_addr.s_addr = inet_addr("0.0.0.0");
   /*绑定*/
   bind(listen_fd, (struct sockaddr *)&listen_socket_addr, sizeof(listen_socket_addr));
   /*设置监听上限*/
   listen(listen_fd, 128);

   /*将listen_fd初始化*/
   eventset(&ep_events[MAX_EVENTS - 1], listen_fd, acceptconnect, &ep_events[MAX_EVENTS - 1]);
   /*将listen_fd挂上红黑树*/
   eventadd(ep_fd, EPOLLIN, &ep_events[MAX_EVENTS - 1]);

   return;
}

/*将结构体成员变量初始化*/
void eventset(struct my_events *my_ev, int fd, void (*call_back)(int, int, void *), void *event_arg)
{
   my_ev->m_fd = fd;
   my_ev->m_event = 0;       // 开始不知道关注的是什么事件，因此设置为0
   my_ev->m_arg = event_arg; // 指向自己
   my_ev->call_back = call_back;

   if (my_ev->m_id[0] == '\0') {
      strcpy(my_ev->m_id, "NULL");
   }
   my_ev->m_status = 0;            // 0表示没有在红黑树上
   my_ev->m_lasttime = time(NULL); // 调用eventset函数的绝对时间
   return;
}

/*向红黑树添加文件描述符和对应的结构体*/
void eventadd(int ep_fd, int event, struct my_events *my_ev)
{
   int op;
   struct epoll_event epv;
   epv.data.ptr = my_ev;                // 让events[i].data.ptr指向我们初始化后的my_events，在注册的时候决定的，等到事件为激活态的时候，再将ptr的内容取出，进行比较，回调即可。
   epv.events = my_ev->m_event = event; // EPOLLIN或EPOLLOUT，默认LT 此处修改成ET

   if (my_ev->m_status == 0)
   {
      op = EPOLL_CTL_ADD;
   }
   else
   {
      printf("\n add error: already on tree \n");
      return;
   }

   if (epoll_ctl(ep_fd, op, my_ev->m_fd, &epv) < 0) // 实际添加/修改
   {
      perror("epoll_ctl error");
      // printf("\n event add/mod false [fd= %d] [events= %d] \n", my_ev->m_fd, my_ev->m_event);
   }
   else
   {
      my_ev->m_status = 1;
      // printf("\n event add ok [fd= %d] [events= %d] \n", my_ev->m_fd, my_ev->m_event);
   }

   return;
}
/*从红黑树上删除 文件描述符和对应的结构体*/
void eventdel(int ep_fd, struct my_events *ev)
{
   if (ev->m_status != 1)
      return;

   epoll_ctl(ep_fd, EPOLL_CTL_DEL, ev->m_fd, NULL);
   ev->m_status = 0;

   return;
}

/*回调函数: 接收连接*/
void acceptconnect(int listen_fd, int event, void *arg)
{
   int connect_fd;
   int i; // 标识ep_events数组下标
   int flag = 0;
   char client_ip[32];
   struct sockaddr_in connect_socket_addr;
   socklen_t connect_socket_len; // a value-result argument
   /*the caller must initialize it  to  contain the  size (in bytes) of the structure pointed to by addr;
    on return it will contain the actual size of the peer address.*/
   connect_socket_len = sizeof(connect_socket_addr);
   if ((connect_fd = accept(listen_fd, (struct sockaddr *)&connect_socket_addr, &connect_socket_len)) < 0)
   /*addrlen不能用&+sizeof来获取，sizeof表达式的结果是一个unsigned的纯右值，无法对其引用。
   主要原因__addr_len是一个传入传出参数*/
   {
      if (errno != EAGAIN && errno != EINTR)
      { /*暂时不处理*/
      }

      return;
   }
   do
   {
      for (i = 0; i < MAX_EVENTS; i++) // 从全局数组ep_events中找一个空闲位置i(类似于select中找值为-1的位置)
         if (ep_events[i].m_status == 0)
            break;
      if (i >= MAX_EVENTS)
      {
         printf("\n %s : max connect [%d] \n", __func__, MAX_EVENTS);
         break;
      }

      /* 设置非阻塞 */
      if ((flag = fcntl(connect_fd, F_SETFL, O_NONBLOCK)) < 0)
      {
         perror("fcntl NONBLOCK error");
         client_list_delete(connect_fd); // 需要删除已添加的客户端节点
         online_count--;
         close(connect_fd); // 关闭连接
         break;
      }

      eventset(&ep_events[i], connect_fd, recvdata, &ep_events[i]);
      eventadd(ep_fd, EPOLLIN | EPOLLET, &ep_events[i]);

   } while (0);

   printf("client connected ip: %s port:%d\n", inet_ntop(AF_INET, (const void *)&connect_socket_addr.sin_addr.s_addr, client_ip, sizeof(client_ip)), ntohs(connect_socket_addr.sin_port));

   return;
}

/*接收数据*/
void recvdata(int client_fd, int event, void *arg)
{
   struct my_events *ev = (struct my_events *)arg;
   // 先从树上摘下
   eventdel(ep_fd, ev);
   int total_read = 0;

   while (1)
   {
      // 确保不会缓冲区溢出
      int remaining = sizeof(ev->m_buf) - total_read - 1; // 预留一个字节给'\0'
      if (remaining <= 0)
      {
         printf("Buffer full, total_read: %d\n", total_read);
         break;
      }

      int len = recv(client_fd, ev->m_buf + total_read, remaining, 0);
      if (len == 0) // 对端关闭连接
      {
         char leave_msg[BUFSIZ];
         
         if(strcmp(ev->m_id,"NULL") != 0){
            online_count--;
            client_list_delete(client_fd);
            snprintf(leave_msg, sizeof(leave_msg),
                  "%s%s============= %s 离开了聊天室 ============= [在线人数: %d]%s\n",
                  COLOR_GREEN,STYLE_BOLD,
                  ev->m_id, online_count, COLOR_RESET);
            broadcast(ev, leave_msg);
            strcpy(ev->m_id, "NULL");
            printf("Client[%d] closed connection\n", client_fd);
         }

         close(client_fd);
         return;
      }
      else if (len < 0)
      {
         if (errno == EAGAIN || errno == EWOULDBLOCK)
         {
            // 数据读取完毕（非阻塞模式下暂时没有更多数据可读）
            if (total_read > 0)
            {
               // 处理接收到的数据
               ev->m_buf[total_read] = '\0';
               ev->m_buf_len = total_read;
               printf("Received from client[%d]: %s", client_fd, ev->m_buf);

               // 检查是否是第一条消息（设置ID）
               if (strcmp(ev->m_id, "NULL") == 0)
               {
                  char login_name[32];
                  char login_password[32];

                  // 去掉消息中的换行符
                  ev->m_buf[strcspn(ev->m_buf, "\n")] = '\0';
                  //ev->m_buf[sizeof(ev->m_id) - 1] = '\0';

                  int ret = login_str(ev->m_buf, login_name, login_password, sizeof(login_name), sizeof(login_password));
                  if(ret == -1){
                     char error_msg[128];
                     snprintf(error_msg, sizeof(error_msg),
                        "%s错误: 登录格式不正确%s%s\n",COLOR_GREEN,STYLE_BOLD,COLOR_RESET);
                     send(client_fd, error_msg, strlen(error_msg), 0);
                     // 继续监听新的ID输入
                     eventset(ev, client_fd, recvdata, ev);
                     eventadd(ep_fd, EPOLLIN, ev);
                     return;
                  }

                  if(verify_user(login_name, login_password) == 0){
                     char error_msg[64];
                     snprintf(error_msg, sizeof(error_msg),
                        "%s错误: 用户不存在或密码错误%s\n",COLOR_GREEN,STYLE_BOLD);
                     send(client_fd, error_msg, strlen(error_msg), 0);//ID已经存在
                     eventset(ev, client_fd, recvdata, ev);
                     eventadd(ep_fd, EPOLLIN, ev);
                     return;   
                  }
                  
                  if(id_exists(ev->m_buf)){
                     char error_msg[64];
                     snprintf(error_msg, sizeof(error_msg),
                        "%s错误: 请勿重复登录%s\n",COLOR_GREEN,STYLE_BOLD);
                     send(client_fd, error_msg, strlen(error_msg), 0);//ID已经存在
                     // 继续监听新的ID输入
                     eventset(ev, client_fd, recvdata, ev);
                     eventadd(ep_fd, EPOLLIN, ev);
                     return;
                  }

                  // 设置ID
                  strncpy(ev->m_id,login_name, sizeof(login_name) - 1);
                  ev->m_id[sizeof(ev->m_id) - 1] = '\0';

                  // 广播加入消息
                  char join_msg[BUFSIZ];
                  online_count++;
                  client_list_add(client_fd,ev->m_id);
                  snprintf(join_msg, sizeof(join_msg),
                     "%s%s============= %s 加入聊天室 ============= [在线人数: %d]%s\n",
                        COLOR_GREEN,  STYLE_BOLD,  ev->m_id, online_count,COLOR_RESET);
                  broadcast(ev, join_msg);

                  // 重新设置为接收模式
                  eventset(ev, client_fd, recvdata, ev);
                  eventadd(ep_fd, EPOLLIN, ev);
               }
               else if(strncmp(ev->m_buf, "@",1) == 0){//处理私聊消息
                  char send_name[32];
                  int send_fd = -1;
                  char *msg_content = NULL;
                  int found_user = 0;
                  int i;
                  for(i = 1; ev->m_buf[i] != ' ' && i < 32 && i < strlen(ev->m_buf); i++) {
                     send_name[i-1] = ev->m_buf[i];
                  }
                  send_name[i-1] = '\0';  

                  //获取消息内容
                  msg_content = strchr(ev->m_buf +1 ,' ');
                  if(msg_content == NULL){// 

                  }
                  msg_content++; // 跳过空格
                  msg_content[strcspn(msg_content, "\n")] = 0;
                  for(int i = 0; i < MAX_EVENTS; i++){
                     if(ep_events[i].m_fd > 0 &&  (strcmp(send_name, ep_events[i].m_id) == 0)){
                        send_fd = ep_events[i].m_fd;
                        found_user = 1;
                        break;
                     }
                  }

                  if(!found_user || send_fd < 0){
                     
                     char error_msg[64] = {0};
                     snprintf(error_msg, sizeof(error_msg), "用户 %s 不存在或已离线\n", send_name);
                     send(ev->m_fd, error_msg, strlen(error_msg), 0);
                     eventset(ev, client_fd, recvdata, ev);
                     eventadd(ep_fd, EPOLLIN, ev);
                     break;
                  }

                  // 5. 发送私信
                  char private_message[BUFSIZ] = {0};
                  char confirm_message[BUFSIZ] = {0};
    
                  // 给接收者的消息
                  snprintf(private_message, sizeof(private_message),
                        "\033[35m%s 悄悄地对你说: %s\033[0m\n", 
                           ev->m_id, msg_content);
    
                  // 给发送者的确认消息
                  snprintf(confirm_message, sizeof(confirm_message),
                        "\033[35m你悄悄地对 %s 说: %s\033[0m\n", 
                           send_name, msg_content);
    
                  // 发送消息
                  if(send(send_fd, private_message, strlen(private_message), 0) < 0) {
                     char error_msg[64] = {0};
                     snprintf(error_msg, sizeof(error_msg), "发送失败，对方可能已离线\n");
                     send(ev->m_fd, error_msg, strlen(error_msg), 0);
                  } else {
                  // 发送成功确认给发送者
                  send(ev->m_fd, confirm_message, strlen(confirm_message), 0);
                  }
                  // 重新设置为接收模式
                  eventset(ev, client_fd, recvdata, ev);
                  eventadd(ep_fd, EPOLLIN, ev);

               }else if(strncmp(ev->m_buf, "/list",5) == 0){
                  list_online(client_fd);
                  eventset(ev, client_fd, recvdata, ev);
                  eventadd(ep_fd, EPOLLIN, ev);
               }
               else{
                  // 处理普通消息
                  char broadcast_buf[BUFSIZ+100];
                  snprintf(broadcast_buf, sizeof(broadcast_buf),
                           "%s: %s", ev->m_id, ev->m_buf);

                  strncpy(ev->m_buf, broadcast_buf, sizeof(ev->m_buf) - 1);
                  ev->m_buf[sizeof(ev->m_buf) - 1] = '\0';
                  ev->m_buf_len = strlen(ev->m_buf);

                  // 切换到发送模式
                  eventset(ev, client_fd, senddata, ev);
                  eventadd(ep_fd, EPOLLOUT, ev);
               }
            }
            else
            {
               // 没有读到数据，继续监听读事件
               eventset(ev, client_fd, recvdata, ev);
               eventadd(ep_fd, EPOLLIN, ev);
            }
            break; // 退出读取循环
         }
         else if (errno == EINTR)
         {
            // 被信号中断，继续读取
            continue;
         }
         else
         {
            // 其他错误
            printf("recv error on fd[%d]: %s\n", client_fd, strerror(errno));
            client_list_delete(client_fd);
            online_count--;
            close(client_fd);
            return;
         }
      }
      else
      { // len > 0
         total_read += len;
         // 如果缓冲区已满，停止读取
         if (total_read >= sizeof(ev->m_buf) - 1)
         {
            printf("Buffer full, total_read: %d\n", total_read);
            break;
         }
      }
   }
}

/*发送数据*/
void senddata(int client_fd, int event, void *arg)
{
   struct my_events *ev = (struct my_events *)arg;
   broadcast(ev, ev->m_buf);
   // printf("-----\n");
   eventdel(ep_fd, ev);                   // 1.将ev对应的文件描述符和结构体从红黑树拿下
   eventset(ev, client_fd, recvdata, ev); // 2.设置client_fd对应的回调函数为recvdata
   eventadd(ep_fd, EPOLLIN, ev);          // 3.将ev对应的文件描述符和结构体放上红黑树,监听读事件EPOLLIN
   // printf("-----\n");

   return;
}