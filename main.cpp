#include <iostream>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/epoll.h>

using namespace std;

#define HOST "192.168.30.80"
#define LISTEN_PORT "5876"
#define MAXEVENTS 64
#define BUFSIZE 2048

enum event_type {
    TUN, LISTNER, PEER
};

typedef struct peer_t{
    int fd;
    struct sockaddr addr;
    socklen_t addrlen;
    long ts;
} peer_t;

typedef struct server_t{
    int fd;
    struct addrinfo inf_addr;
    long ts;
}server_t;

typedef struct configuration_t{
    int efd;
    server_t server;
    peer_t peer;
} configuration_t;

typedef struct context_t{
    int fd;
    event_type etype;
    void *ptr;
} context_t;

configuration_t conf = {};

/**
 * We will register SIGINT, since we cannot register SIGKILL and SIGSTOP
 */
void sig_handler(int signo) {
    if (signo == SIGINT)
        cout << "received SIGINT" << endl;
    if (signo == SIGABRT)
        cout << "received SIGABRT" << endl;
    if (signo == SIGSEGV)
        cout << "received SIGSEGV" << endl;

    cout << "Shutdown Initiating" << endl;

    cout << "Shutdown Complete" << endl;
    exit(1);
}

void register_signals() {
    std::cout << "Registering  Signals" << std::endl;
    if (signal(SIGINT, sig_handler) == SIG_ERR)
        cout << "Not able to register SIGNALS (SIGINT)" << endl;
    if (signal(SIGABRT, sig_handler) == SIG_ERR)
        cout << "Not able to register SIGNALS (SIGABRT)" << endl;
    if (signal(SIGSEGV, sig_handler) == SIG_ERR)
        cout << "Not able to register SIGNALS (SIGSEGV)" << endl;
}

static socklen_t get_address_len(const sockaddr* address) {
    if(address->sa_family == AF_INET) return sizeof(struct sockaddr_in);
    else if(address->sa_family == AF_INET6) return sizeof(struct sockaddr_in6);
    return 0;
}

static int make_socket_non_blocking (int sfd) {
    int flags, s;

    flags = fcntl (sfd, F_GETFL, 0);
    if (flags == -1)
    {
        perror ("fcntl");
        return -1;
    }

    flags |= O_NONBLOCK;
    s = fcntl (sfd, F_SETFL, flags);
    if (s == -1)
    {
        perror ("fcntl");
        return -1;
    }

    return 0;
}

static int get_socket_and_bind(const char *host, const char *port,
                               struct addrinfo *inf_addr){
    struct addrinfo hints = {};
    struct addrinfo *result, *rp;
    int s, sfd = -1;

    std::cout << "Creating and Binding Socket" << std::endl;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE; // use my IP address

    s = getaddrinfo (host, port, &hints, &result);
    if (s != 0)
    {
        fprintf (stderr, "Could not getaddrinfo: %s\n", gai_strerror (s));
        return -1;
    }

    for (rp = result; rp != nullptr; rp = rp->ai_next)
    {
        int on = 1;
        sfd = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) {
            perror("create socket()");
            continue;
        }

        if(make_socket_non_blocking(conf.server.fd) < 0){
            perror( "Could make socket non blocking");
        }

        if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on)) < 0) {
            perror("setsockopt");
            continue;
        }

        s = bind (sfd, rp->ai_addr, rp->ai_addrlen);
        if (s == 0)
        {
            /* We managed to bind successfully! */
            memcpy(inf_addr, rp, sizeof(struct addrinfo));
            break;
        }

        close (sfd);
    }

    if (rp == nullptr)
    {
        fprintf (stderr, "Could not bind\n");
        return -1;
    }

    freeaddrinfo (result);

    return sfd;
}

int main() {
    int s, nevent;
    struct epoll_event event = {};
    struct epoll_event *events;

    std::cout << "Initializing Server" << std::endl;

    register_signals();

    conf.server.fd = get_socket_and_bind(
            HOST, LISTEN_PORT,
            &conf.server.inf_addr
    );
    if(conf.server.fd < 0){
        perror("Could create socket");
    }
    cout << "socket " << conf.server.fd <<" bind at "
                      << inet_ntoa(((struct sockaddr_in*)conf.server.inf_addr.ai_addr)->sin_addr) << endl;

    conf.efd = epoll_create1 (0);
    if (conf.efd == -1){
        perror ("epoll_create");
        abort ();
    }


    context_t server_ctx = {};
    server_ctx.etype = LISTNER;
    server_ctx.fd = conf.server.fd;
    server_ctx.ptr = &conf.server;

    event.data.ptr = &server_ctx;
    event.events = EPOLLIN | EPOLLET;
    s = epoll_ctl (conf.efd, EPOLL_CTL_ADD, conf.server.fd, &event);
    if (s == -1){
        perror ("epoll_ctl");
        abort ();
    }

    /* Buffer where events are returned */
    events = (struct epoll_event *) calloc (MAXEVENTS, sizeof(epoll_event));

    while ((nevent = epoll_wait (conf.efd, events, MAXEVENTS, -1)) > 0){
        int i;

        for (i = 0; i < nevent; i++){
            auto *ctx = static_cast<context_t *>(events[i].data.ptr);

            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN))){
                /* An error has occured on this fd, or the socket is not
                   ready for reading (why were we notified then?) */
                perror("epoll error");
                if(ctx->etype == LISTNER) {
                    break;
                }else{
                    close(ctx->fd);
                }
                continue;
            }

            if((events[i].events & EPOLLIN)){
                if(ctx->etype == LISTNER){
                    int optval = 1;
                    char buffer[BUFSIZE];
                    struct sockaddr local_addr, peer_addr;
                    socklen_t peer_addrLen, local_addrLen;

                    getsockname(ctx->fd, &local_addr, &local_addrLen);
                    peer_addr.sa_family = local_addr.sa_family;
                    peer_addrLen = get_address_len(&peer_addr);

                    int r = (int) (recvfrom(ctx->fd, buffer, BUFSIZE,
                                                       MSG_DONTWAIT, &peer_addr, &peer_addrLen));

                    cout << "received " << r << " bytes from listener" << endl;
                    if(r > 0){

                        conf.peer.fd = socket(local_addr.sa_family, SOCK_DGRAM, 0);
                        if(conf.peer.fd < 0){
                            perror("peer socket()");
                            continue;
                        }

                        make_socket_non_blocking(conf.peer.fd);

                        if (setsockopt(conf.peer.fd, SOL_SOCKET, SO_REUSEADDR, (char *) &optval, sizeof(optval)) < 0) {
                            perror("setsockopt() set SO_REUSEADDR");
                            close(conf.peer.fd);
                            continue;
                        }

                        if(bind(conf.peer.fd, &local_addr, local_addrLen) < 0){
                            perror("peer bind()");
                            close(conf.peer.fd);
                            continue;
                        }

                        if(connect(conf.peer.fd, &peer_addr, peer_addrLen) < 0){
                            perror("peer connect()");
                            close(conf.peer.fd);
                            continue;
                        }

                        conf.peer.addr.sa_family = peer_addr.sa_family;
                        memcpy(&conf.peer.addr, &peer_addr, peer_addrLen);
                        conf.peer.addrlen = peer_addrLen;

                        context_t peer_ctx = {};
                        peer_ctx.etype = PEER;
                        peer_ctx.fd = conf.peer.fd;
                        peer_ctx.ptr = &conf.peer;

                        event.data.ptr = &peer_ctx;
                        event.events = EPOLLIN | EPOLLET;

                        if(epoll_ctl (conf.efd, EPOLL_CTL_ADD, conf.peer.fd, &event) < 0){
                            perror ("peer epoll_ctl()");
                            close(conf.peer.fd);
                        }

                        cout << "new connection from "
                                << inet_ntoa(((struct sockaddr_in*)&conf.peer.addr)->sin_addr)
                                << ":" << ntohs(((struct sockaddr_in*)&conf.peer.addr)->sin_port)
                                << endl;
                    }
                }

                if(ctx->etype == PEER){
                    char buffer[BUFSIZE];
                    auto * peer = static_cast<peer_t *>(ctx->ptr);
                    auto r = (int) recv(peer->fd, buffer, BUFSIZE, 0);
                    cout << "received " << r << " bytes from "
                            << inet_ntoa(((struct sockaddr_in*)&peer->addr)->sin_addr)
                            << ":" << ntohs(((struct sockaddr_in*)&peer->addr)->sin_port)
                            << endl;
                }
            }
        }
    }

    return 0;
}