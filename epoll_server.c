#include <iostream>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAXLINE 1024
#define OPEN_MAX 100
#define LISTENQ 20

#define MAX_FDS 256
#define MAX_EVENTS 20

#define SERV_PORT 6800 
#define SERV_IP "10.1.10.102"

#define INFTIM 1000

//线程池任务队列结构体
struct task{
    int fd;            //需要读写的文件描述符
    struct task *next; //下一个任务
};
//用于保存向客户端发送一次消息所需的相关数据
struct user_data{
    int fd;
    unsigned int n_size;
    char line[MAXLINE];
};
//线程的任务函数
void * readtask(void *args);
void * writetask(void *args);

//声明epoll_event结构体的变量,ev用于注册事件,数组用于回传要处理的事件
struct epoll_event ev,events[MAX_EVENTS];
int epfd;
pthread_mutex_t mutex;
pthread_cond_t cond1;
struct task *readhead=NULL,*readtail=NULL,*writehead=NULL;

void setnonblocking(int sock)
{
    int opts;
    opts=fcntl(sock, F_GETFL);
    if(opts<0)
    {
        perror("fcntl(sock,GETFL)");
        exit(1);
    }
    opts = opts | O_NONBLOCK;
    if(fcntl(sock, F_SETFL, opts)<0)
    {
        perror("fcntl(sock,SETFL,opts)");
        exit(1);
    }
}

int main()
{
    int i, listenfd, connfd, sockfd, nfds;
    pthread_t tid1,tid2;
    struct task *new_task = NULL;
    struct user_data *rdata = NULL;
    socklen_t clilen;

    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond1, NULL);
    
    //初始化用于读线程池的线程，开启两个线程来完成任务，两个线程会互斥地访问任务链表
    pthread_create(&tid1, NULL, readtask, NULL);
    pthread_create(&tid2, NULL, readtask, NULL);

    struct sockaddr_in clientaddr;
    struct sockaddr_in serveraddr;
    
    listenfd = socket(AF_INET, SOCK_STREAM, 0);

    setnonblocking(listenfd);    //把socket设置为非阻塞方式

    bzero(&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    const char *local_addr = SERV_IP;
    inet_aton(local_addr, &(serveraddr.sin_addr)); //htons(SERV_PORT);
    serveraddr.sin_port=htons(SERV_PORT);

    if(bind(listenfd,(sockaddr *)&serveraddr, sizeof(serveraddr)))
    {
        printf("BIND failed!\n");
        exit(-1);
    }
    //开始监听
    if(listen(listenfd, LISTENQ))
    {
        printf("Listen failed!\n");
        exit(-1);
    }

    //生成用于处理accept的epoll专用的文件描述符
    epfd = epoll_create(MAX_FDS);
    if(epfd==-1) {
        printf("epoll_create error %d\n",errno);
        exit(-1);
    }

    ev.data.fd = listenfd;
    ev.events = EPOLLIN | EPOLLET;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);

    while(1) {
        //等待epoll事件的发生
        nfds=epoll_wait(epfd, events, MAX_EVENTS, 500);
        if(nfds == 0) 
		{
            continue;
        }
            
        if(nfds < 0) 
		{
            printf("epoll_wait error %d\n",errno);
            exit(-1);
		}

        //处理所发生的所有事件
        for(i=0; i < nfds; ++i)
        {
            //这里将监听套接字同连接套接字放在了一起，这样导致每次都要轮询才能获得监听套接字，对其使用
            //accept调用便可获得新的连接套接字，如果能将监听动作放到另外一个线程中就好了
            if(events[i].data.fd==listenfd)
            {
                connfd = accept(listenfd,(sockaddr *)&clientaddr, &clilen);
                if(connfd<0){
                    perror("connfd<0");
                    continue;
                }
                setnonblocking(connfd);

                //printf("Connect from %s:%d\n",inet_ntoa(clientaddr.sin_addr),htons(clientaddr.sin_port));

                //设置用于读操作的文件描述符
                ev.data.fd=connfd;
                ev.events=EPOLLIN | EPOLLET;
                epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &ev);
            }
            else if(events[i].events & EPOLLIN)
            {
                if ( (sockfd = events[i].data.fd) < 0) 
                    continue;

                new_task = (struct task*)malloc(sizeof(struct task));
                if(new_task==NULL){
                    printf("malloc error\n");
                    exit(-1);
                }

                new_task->fd =sockfd;
                new_task->next = NULL;

                //添加新的读任务
                pthread_mutex_lock(&mutex);
                if(readhead == NULL)
                {
                    readhead = new_task;
                    readtail = new_task;
                }
                else
                {
                    readtail->next = new_task;
                    readtail = new_task;
                }
                pthread_cond_broadcast(&cond1);	//唤醒所有等待cond1条件的线程
                pthread_mutex_unlock(&mutex);
            }
            else if(events[i].events & EPOLLOUT)
            {
                rdata=(struct user_data *)events[i].data.ptr;
                sockfd = rdata->fd;
                write(sockfd, rdata->line, rdata->n_size);

                if(rdata){
                    free(rdata);
                    rdata=NULL;
                }

                //设置用于读操作的文件描述符
                ev.data.fd=sockfd;
                ev.events=EPOLLIN | EPOLLET;
                epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, &ev);
            }
        }
    }
}

void* readtask(void *args)
{
    int fd=-1;
    unsigned int n;
    //用于把读出来的数据传递出去
    struct user_data *data = NULL;

    while(1){
        //互斥访问任务队列
        pthread_mutex_lock(&mutex);
        //等待直到任务队列不为空
        //这个while要特别说明一下，单个pthread_cond_wait功能很完善，为何这里要有一个while (head == NULL)呢？
        //因为pthread_cond_wait里的线程可能会被意外唤醒，如果这个时候head != NULL，则不是我们想要的情况。
        //这个时候，应该让线程继续进入pthread_cond_wait  
        while(readhead == NULL) {
            pthread_cond_wait(&cond1, &mutex); //线程阻塞，释放互斥锁，当等待的条件等到满足时，它会再次获得互斥锁
            // pthread_cond_wait会先解除之前的pthread_mutex_lock锁定的mtx，
            // 然后阻塞在等待对列里休眠，直到再次被唤醒（大多数情况下是等待的条件成立而被唤醒，
            // 唤醒后，该进程会先锁定先pthread_mutex_lock(&mtx);，再读取资源  
        }

        fd = readhead->fd;

        //从任务队列取出一个读任务
        struct task *tmp = readhead;
        readhead = readhead->next;
        if(tmp){
            free(tmp);
            tmp=NULL;
        }
        pthread_mutex_unlock(&mutex);

        data = (struct user_data *)malloc(sizeof(struct user_data));
        if(data == NULL) {
            printf("malloc error\n");
            exit(-1);
        }

        data->fd=fd;
        if ( (n = read(fd, data->line, MAXLINE)) > 0) {

            data->n_size = n;
            //设置需要传递出去的数据
            ev.data.ptr = data;
            ev.events = EPOLLOUT | EPOLLET;
            epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);//修改sockfd上要处理的事件为EPOLLOUT，这将导致数据被发回客户端（在上面的for循环中）
        } else if (n == 0) {
            //客户端关闭了，其对应的连接套接字可能也被标记为EPOLLIN，然后服务器去读这个套接字
            //结果发现读出来的内容为0，就知道客户端关闭了。
            close(fd);
            printf("Client close connect!\n");

            if(data != NULL) {
                free(data);
                data = NULL;
            }
        } else {
            if (errno == ECONNRESET)
                close(fd);
            else
                printf("readline error %d\n",errno);

            if(data != NULL) {
                free(data);
                data = NULL;
            }
        }
    }
}

