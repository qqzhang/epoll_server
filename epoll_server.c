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

//�̳߳�������нṹ��
struct task{
    int fd;            //��Ҫ��д���ļ�������
    struct task *next; //��һ������
};
//���ڱ�����ͻ��˷���һ����Ϣ������������
struct user_data{
    int fd;
    unsigned int n_size;
    char line[MAXLINE];
};
//�̵߳�������
void * readtask(void *args);
void * writetask(void *args);

//����epoll_event�ṹ��ı���,ev����ע���¼�,�������ڻش�Ҫ������¼�
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
    
    //��ʼ�����ڶ��̳߳ص��̣߳����������߳���������������̻߳ụ��ط�����������
    pthread_create(&tid1, NULL, readtask, NULL);
    pthread_create(&tid2, NULL, readtask, NULL);

    struct sockaddr_in clientaddr;
    struct sockaddr_in serveraddr;
    
    listenfd = socket(AF_INET, SOCK_STREAM, 0);

    setnonblocking(listenfd);    //��socket����Ϊ��������ʽ

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
    //��ʼ����
    if(listen(listenfd, LISTENQ))
    {
        printf("Listen failed!\n");
        exit(-1);
    }

    //�������ڴ���accept��epollר�õ��ļ�������
    epfd = epoll_create(MAX_FDS);
    if(epfd==-1) {
        printf("epoll_create error %d\n",errno);
        exit(-1);
    }

    ev.data.fd = listenfd;
    ev.events = EPOLLIN | EPOLLET;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);

    while(1) {
        //�ȴ�epoll�¼��ķ���
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

        //�����������������¼�
        for(i=0; i < nfds; ++i)
        {
            //���ｫ�����׽���ͬ�����׽��ַ�����һ����������ÿ�ζ�Ҫ��ѯ���ܻ�ü����׽��֣�����ʹ��
            //accept���ñ�ɻ���µ������׽��֣�����ܽ����������ŵ�����һ���߳��оͺ���
            if(events[i].data.fd==listenfd)
            {
                connfd = accept(listenfd,(sockaddr *)&clientaddr, &clilen);
                if(connfd<0){
                    perror("connfd<0");
                    continue;
                }
                setnonblocking(connfd);

                //printf("Connect from %s:%d\n",inet_ntoa(clientaddr.sin_addr),htons(clientaddr.sin_port));

                //�������ڶ��������ļ�������
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

                //����µĶ�����
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
                pthread_cond_broadcast(&cond1);	//�������еȴ�cond1�������߳�
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

                //�������ڶ��������ļ�������
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
    //���ڰѶ����������ݴ��ݳ�ȥ
    struct user_data *data = NULL;

    while(1){
        //��������������
        pthread_mutex_lock(&mutex);
        //�ȴ�ֱ��������в�Ϊ��
        //���whileҪ�ر�˵��һ�£�����pthread_cond_wait���ܺ����ƣ�Ϊ������Ҫ��һ��while (head == NULL)�أ�
        //��Ϊpthread_cond_wait����߳̿��ܻᱻ���⻽�ѣ�������ʱ��head != NULL������������Ҫ�������
        //���ʱ��Ӧ�����̼߳�������pthread_cond_wait  
        while(readhead == NULL) {
            pthread_cond_wait(&cond1, &mutex); //�߳��������ͷŻ����������ȴ��������ȵ�����ʱ�������ٴλ�û�����
            // pthread_cond_wait���Ƚ��֮ǰ��pthread_mutex_lock������mtx��
            // Ȼ�������ڵȴ����������ߣ�ֱ���ٴα����ѣ������������ǵȴ������������������ѣ�
            // ���Ѻ󣬸ý��̻���������pthread_mutex_lock(&mtx);���ٶ�ȡ��Դ  
        }

        fd = readhead->fd;

        //���������ȡ��һ��������
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
            //������Ҫ���ݳ�ȥ������
            ev.data.ptr = data;
            ev.events = EPOLLOUT | EPOLLET;
            epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);//�޸�sockfd��Ҫ������¼�ΪEPOLLOUT���⽫�������ݱ����ؿͻ��ˣ��������forѭ���У�
        } else if (n == 0) {
            //�ͻ��˹ر��ˣ����Ӧ�������׽��ֿ���Ҳ�����ΪEPOLLIN��Ȼ�������ȥ������׽���
            //������ֶ�����������Ϊ0����֪���ͻ��˹ر��ˡ�
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

