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
#include "processpool.h"

class cgi_conn{
public:
    cgi_conn(){ }
    ~cgi_conn(){ }

    void init(int epollfd, int sockfd, const sockaddr_in & client_addr){

        m_epollfd = epollfd;
        m_sockfd = sockfd;
        m_address = client_addr;
        memset(m_buf, '\0', BUFFER_SIZE);
        m_read_idx = 0;
    }

    void process(){

        int idx = 0;
        int ret = -1;
        while(true){
            idx = m_read_idx;
            ret = recv(m_sockfd, m_buf + idx, BUFFER_SIZE - 1 - idx, 0);
            if ( ret < 0){
                if ( errno != EAGAIN){
                    removefd(m_epollfd, m_sockfd);
                }
                break;
            }
            else if ( ret == 0){
                removefd(m_epollfd, m_sockfd);
                break;
            }
            else{
                m_read_idx += ret;
                printf("user content is :%s , length = %d,\n", m_buf, ret);
                for(; idx < m_read_idx; ++ idx){
                    if ( (idx >= 1) && (m_buf[idx - 1] == '\r') && m_buf[idx] =='\n'){
                        break;
                    }
                }

                if ( idx == m_read_idx){
                    continue;
                }
                m_buf[idx - 1] = '\0';
                char *file_name = m_buf;
                if (access(file_name, F_OK) == -1){
                    removefd(m_epollfd, m_sockfd);
                    break;
                }

                ret = fork();
                if ( ret == -1){
                    removefd(m_epollfd, m_sockfd);
                    break;
                }
                else if ( ret > 0){
                    removefd(m_epollfd, m_sockfd);
                    break;
                }
                else{
                    close(STDOUT_FILENO);
                    dup(m_sockfd);
                    execl(m_buf, m_buf, 0);
                    printf("回收cgi进程\n");
                    exit(0);
                }

            }
        }
    }

private:
    static const int BUFFER_SIZE = 1024;
    static int m_epollfd;
    int m_sockfd;
    sockaddr_in m_address;
    char m_buf[BUFFER_SIZE];
    int m_read_idx;
};
int cgi_conn::m_epollfd = -1;

int getListenSockfd(char * argv[]){
    const char *ip = argv[1];
    int port = atoi(argv[2]);
    struct sockaddr_in address;

    //设定address的地址信息
    bzero( & address, sizeof (address)); //清空地址信息
    address.sin_family = AF_INET;//协议族
    inet_pton(AF_INET, ip, & address.sin_addr); //填入网络序IP
    address.sin_port = htons(port); //填入网络序 port

    //初始化监听socket
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if ( -1 == listenfd){
        close(listenfd);
        perror("socket");
        return -1;
    }

    int ret = bind(listenfd, (struct sockaddr *) &address, sizeof (address));
    if ( -1 == ret ){
        close(listenfd);
        perror("bind"); return -1;
    }
    ret = listen(listenfd, 5); //监听队列 backlog 为 5，实际队列可能允许比这个值大一点
    if ( -1 == ret){
        close(listenfd);
        perror("listen"); return -1;
    }
    return listenfd;
}

int main(int argc ,char *argv[]) {

    if ( argc <= 2){
        perror("main");
        return 1;
    }

    int listenfd = getListenSockfd(argv);
    if (-1 == listenfd){
        perror("getListenSockfd");
        return 1;
    }

    processpool<cgi_conn> * pool = processpool<cgi_conn>::create(listenfd);
    if ( pool){
        pool->run();
        delete pool;
    }
    close(listenfd);
    return 0;
}
