#ifndef PROCESSPOOL_PROCESSPOOL_H
#define PROCESSPOOL_PROCESSPOOL_H
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

//子进程结构，用于存放与父进程通信的管道 和 pid
class process{
public:
    process():m_pid(-1) { }

public:
    pid_t m_pid;
    int m_pipefd[2];
};

template <typename T>
class processpool{
private:
    processpool(int listenfd, int process_number);


public:
    static processpool<T> *create (int listenfd, int process_number = 8){
        if ( !m_instance ){
            m_instance = new processpool<T>(listenfd, process_number);
        }
        return m_instance;
    }

    ~processpool(){
        delete []m_sub_process;
    }

    //启动进程池
    void run();

private:
    void setup_sig_pipe();
    void run_parent();
    void run_child();

private:
    //进程池允许的最大子进程数量
    static const int MAX_PROCESS_NUMBER = 16;

    //每个子进程最多可以处理的客户数量
    static const int USER_PER_PROCESS = 65536;

    //epoll 最多能处理的事件数
    static const int MAX_EVENT_NUMBER = 10000;

    //进程池中的进程总数
    int m_process_number;

    //子进程在池中的序号
    int m_idx;

    //每个进程中都有一个epoll内核事件表， 用m_epollfd 标识
    int m_epollfd;

    //监听socket
    int m_listenfd;

    //子进程通过m_stop 来决定是否停止运行
    int m_stop;

    //保存所有子进程的描述信息
    process *m_sub_process;

    //进程池静态实例
    static processpool<T> *m_instance;
};

template <typename T>
processpool<T> * processpool<T>::m_instance = NULL;

static int sig_pipefd[2];

//非阻塞IO
static int setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//把fd 注册到 epollfd的内核事件表上
static void addfd(int epollfd, int fd){
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//把fd 从 epollfd内核事件表 移除
static void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 信号处理函数
static void sig_handler(int sig){
    int save_errno = errno;
    int msg = sig;
    send(sig_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//为信号设置信号处理函数
static void addsig(int sig, void (handler)(int), bool restart = true){
    struct sigaction sa;
    memset(&sa, '\0', sizeof (sa));
    sa.sa_handler = handler;
    if ( restart ){
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert( sigaction(sig, &sa, NULL) != -1);
}

template <typename T>
processpool<T> ::processpool(int listenfd, int process_number):
m_listenfd(listenfd), m_process_number(process_number),m_idx(-1),m_stop(false){
    assert( (process_number > 0) && (process_number <= MAX_PROCESS_NUMBER));
    m_sub_process = new process[process_number];
    for(int i = 0; i < process_number; i ++){
        int ret = socketpair(PF_UNIX, SOCK_STREAM,0, m_sub_process[i].m_pipefd);
        assert( ret == 0);
        m_sub_process[i].m_pid = fork();
        assert(m_sub_process[i].m_pid >= 0);

        //父进程关闭pipe[1]，由此可见， 父进程用m_pipefd[0]来和子进程通信。
        if ( m_sub_process[i].m_pid > 0){
            close(m_sub_process[i].m_pipefd[1]);
            continue;
        }else{
            //这里为什么是break 呢，为了防止子进程继续创建子进程。
            close(m_sub_process[i].m_pipefd[0]);
            m_idx = i;
            break;
        }
    }
}

//统一事件源
template <typename T>
void processpool<T>::setup_sig_pipe() {
    //创建epoll内核事件表 和 信号管道
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
    assert( ret != -1);
    setnonblocking(sig_pipefd[1]);
    addfd(m_epollfd, sig_pipefd[0]);

    //设置信号处理函数
    addsig(SIGCHLD, sig_handler);
    addsig(SIGTERM, sig_handler);
    addsig(SIGINT, sig_handler);
    addsig(SIGPIPE, SIG_IGN); //这是什么东西？
}

//如果 m_idx = -1 代表这个子进程没有被开启。
//为什么这个run 可以 启动所有的进程呢？
/*
 * pid = fork();
 * if ( pid > 0){
 *
 * }else if ( pid == 0){
 *
 * }else{
 *
 * }
 * run();
 * 就相当于这种情况， run不管是父进程还是子进程，他们都是公用的。
 * */
template<typename T>
void processpool<T>:: run(){
    //只有父进程的 pid = -1。
    if ( m_idx != -1){
        run_child();
        return;
    }
    run_parent();
}

template <typename T>
void processpool<T>::run_child() {
    setup_sig_pipe();

    //每个子进程要通过进程池中的序列号m_idx找到和父进程通信的管道。
    //子进程采用m_pipefd[1]和 父进程通信
    int pipefd = m_sub_process[m_idx].m_pipefd[1];

    addfd(m_epollfd, pipefd);
    epoll_event events[MAX_EVENT_NUMBER];
    T *users = new T[USER_PER_PROCESS];
    assert(users != nullptr);
    int number = 0;
    int ret = -1;
    while( ! m_stop){
        number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if ( (number < 0) && (errno != EINTR)){
            printf("epoll failure\n");
            break;
        }

        for(int i = 0; i < number; i ++){
            int sockfd = events[i].data.fd;
            //如果管道可读，说明必然是父进程通知的。
            if ( (sockfd == pipefd) && (events[i].events & EPOLLIN)){
                int client = 0;
                ret = recv(sockfd, (char *)&client, sizeof(client), 0);

                //如果 ret < 0 或者 ret = 0，应该是关闭 或者出错了。
                if ( ((ret < 0 ) && (errno != EAGAIN)) || ret == 0){
                    continue;
                }

                else{
                    struct sockaddr_in client_address;
                    socklen_t client_addrlength = sizeof(client_address);
                    int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if ( connfd < 0){
                        printf("errno is %d\n", errno);
                        continue;
                    }
                    addfd(m_epollfd, connfd);
                    users[connfd].init(m_epollfd, connfd, client_address);
                }
            }

            //如果子进程收到了 信号，进行处理 （信号是父进程通过管道发送的， 也有可能收到子进程的子进程退出的信号，这里是统一事件源）
            else if ( (sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN)){
                int sig;
                char signals[1024];
                //读取信号值
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if ( ret <= 0) continue;
                 for(int i = 0; i < ret; i ++){
                     switch (signals[i]) {

                         //回收子进程 的子进程
                         case SIGCHLD:{
                             pid_t pid;
                             int stat;
                             while( (pid = waitpid(-1, &stat, WNOHANG)) > 0){
                                 continue;
                             }
                             break;
                         }

                         case SIGTERM:
                         case SIGINT:{
                             m_stop = true;
                             break;
                         }
                         default:{
                             break;
                         }
                     }
                 }
            }

            //如果是其他可读数据，那么客户请求到来，调用逻辑处理对象的process方法进行处理
            else if ( events[i].events & EPOLLIN){
                users[sockfd].process();
            }
            else{
                continue;
            }
        }
    }

    delete [] users;
    users = NULL;
    close(pipefd);
    close(m_listenfd);//这个 不能执行，m_listenfd 由谁创建，就应该由谁来销毁。
    close(m_epollfd);
}

template <typename T>
void processpool<T>::run_parent() {
    setup_sig_pipe();
    addfd(m_epollfd, m_listenfd);
    epoll_event events[MAX_EVENT_NUMBER];
    int sub_process_counter = 0;
    int new_conn= 1;
    int number = 0;
    int ret = -1;

    while( !m_stop){
        number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR)){
            printf("epoll failure\n");
            break;
        }
        for(int i = 0; i < number; i ++){
            int sockfd = events[i].data.fd;
            if ( sockfd == m_listenfd){
                int j = sub_process_counter; //当前指向第几个进程

                //找一圈，找一个可以使用的进程
                do{
                    if (m_sub_process[j].m_pid != -1){
                        break;
                    }
                    j = (j + 1) % m_process_number;
                }while(j != sub_process_counter);

                //如果 = -1，代表进程池没有开启。。
                if (m_sub_process[j].m_pid == -1){
                    m_stop = true;
                    break;
                }

                //找到空闲进程
                sub_process_counter = (j + 1) % m_process_number;
                send(m_sub_process[j].m_pipefd[0], (char *) &new_conn, sizeof(new_conn), 0);
                printf("send request to child %d\n",j);
            }

            else if ( (sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN)){
                int sig;
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if ( ret <= 0){
                    continue;
                }
                for(int j = 0; j < ret; j++){
                    switch (signals[j]) {
                        case SIGCHLD:{
                            pid_t pid;
                            int stat;

                            //如果某个子进程退出， 回收相应资源。
                            while( (pid = waitpid(-1, &stat, WNOHANG)) > 0){
                                //循环找出是哪个子进程退出
                                for(int k = 0; k < m_process_number; k ++){
                                    if (m_sub_process[k].m_pid == pid){
                                        printf("child%d join\n", k);
                                        close(m_sub_process[k].m_pipefd[0]);
                                        m_sub_process[k].m_pid = -1;
                                    }
                                }
                            }
                            //如果所有的子进程退出， 父进程也退出
                            m_stop = true;
                            for(int k = 0; k < m_process_number; k ++){
                                if ( m_sub_process[k].m_pid != -1){
                                    m_stop = false;
                                }
                            }
                            break;
                        }

                        case SIGTERM:
                        case SIGINT:{
                            printf("kill all the child now\n");
                            for(int k = 0; k < m_process_number; k ++){
                                pid_t pid = m_sub_process[i].m_pid;
                                if ( pid != -1){
                                    kill(pid, SIGTERM);
                                }
                            }
                            break;
                        }

                        default:{
                            break;
                        }
                    }
                }
            }
            else {
                continue;
            }
        }
    }
    close(m_epollfd);
}

#endif //PROCESSPOOL_PROCESSPOOL_H
