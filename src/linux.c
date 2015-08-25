#include "linux.h"
#include "buffer.h"


#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>


void
safe_close(int fd)
{
    while (1)
    {
        int error = close(fd);

        switch (error)
        {
        case EINTR:
            break;
        default:
            return;
        }
    }
}

void
safe_close_ref(int* fd)
{
    assert(fd);

    if (*fd >= 0)
    {
        safe_close(*fd);
        *fd = -1;
    }
}

#define MinBufferSize  4


static pthread_mutex_t s_Lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_t s_EPollThread;
static int s_Generation;
static int s_RefCount;
static int s_EPollFd = -1;
static epoll_callback_data* s_Callbacks;
static int s_CallbacksSize;


static
void*
EPollThreadMain(void* arg)
{
    int count = 0, bufferSize = 0;
    struct epoll_event* events = NULL;

    while (1)
    {
        pthread_mutex_lock(&s_Lock);
        if (!events)
        {
            bufferSize = s_CallbacksSize >= 4 ? s_CallbacksSize : MinBufferSize;
            events = (struct epoll_event*)malloc(bufferSize * sizeof(*events));
        }
        else if (bufferSize < s_CallbacksSize)
        {
            events = (struct epoll_event*)realloc(events, s_CallbacksSize * sizeof(*events));
            bufferSize = s_CallbacksSize;
        }
        pthread_mutex_unlock(&s_Lock);

        count = epoll_wait(s_EPollFd, events, bufferSize, -1);

        if (count < 0)
        {
            switch (errno)
            {
            case EINTR:
                break;
            case EBADF:
                goto Exit;
            default:
                fprintf(stderr, "epoll thread received errno %d: %s\n", errno, strerror(errno));
                goto Exit;
            }
        }
        else
        {
            int i;

            pthread_mutex_lock(&s_Lock);

            for (i = 0; i < count; ++i)
            {
                struct epoll_event* ev = &events[i];

                if (ev->data.fd < s_CallbacksSize)
                {
                    if (s_Callbacks[ev->data.fd].callback)
                    {
                        s_Callbacks[ev->data.fd].callback(s_Callbacks[ev->data.fd].ctx, ev);
                    }
                }
            }

            pthread_mutex_unlock(&s_Lock);
        }
    }

Exit:
    free(events);
    return NULL;
}

int
epoll_loop_create()
{
    int error = 0;

    pthread_mutex_lock(&s_Lock);

    if (++s_RefCount == 1)
    {
        ++s_Generation;

        if ((s_EPollFd = epoll_create1(O_CLOEXEC)) < 0)
        {
            goto Error;
        }

        if ((error = pthread_create(&s_EPollThread, NULL, EPollThreadMain, NULL)) < 0)
        {
            goto Error;
        }
    }

    error = s_EPollFd;

Exit:
    pthread_mutex_unlock(&s_Lock);
    return error;

Error:
    error = errno;
    safe_close_ref(&s_EPollFd);
    errno = error;
    error = -1;
    goto Exit;
}

void
epoll_loop_destroy()
{
    int generation;

    pthread_mutex_lock(&s_Lock);

    generation = ++s_Generation;

    if (--s_RefCount == 0)
    {
        safe_close_ref(&s_EPollFd);

        pthread_mutex_unlock(&s_Lock);

        pthread_join(s_EPollThread, NULL);

        pthread_mutex_lock(&s_Lock);

        if (generation == s_Generation)
        {
            free(s_Callbacks);
            s_Callbacks = NULL;
            s_CallbacksSize = 0;
        }
    }

    pthread_mutex_unlock(&s_Lock);
}

int
epoll_loop_set_callback(int handle, epoll_callback_data callback)
{
    if (handle < 0)
    {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&s_Lock);

    if (!s_Callbacks)
    {
        size_t bytes;
        s_CallbacksSize = MinBufferSize;

        if (handle + 1 > s_CallbacksSize)
        {
            s_CallbacksSize = handle + 1;
        }

        bytes = sizeof(*s_Callbacks) * (size_t)s_CallbacksSize;
        s_Callbacks = (epoll_callback_data*)malloc(bytes);
        memset(s_Callbacks, 0, bytes);
    }
    else if (handle >= s_CallbacksSize)
    {
        size_t bytes;
        int previousSize;

        previousSize = s_CallbacksSize;
        s_CallbacksSize = (s_CallbacksSize * 17) / 10; /* x 1.68 */
        if (handle + 1 > s_CallbacksSize)
        {
            s_CallbacksSize = handle + 1;
        }

        bytes = sizeof(*s_Callbacks) * (size_t)s_CallbacksSize;
        s_Callbacks = (epoll_callback_data*)realloc(s_Callbacks, bytes);

        memset(s_Callbacks + previousSize, 0, sizeof(*s_Callbacks) * (size_t)(s_CallbacksSize - previousSize));
    }

    s_Callbacks[handle] = callback;

    pthread_mutex_unlock(&s_Lock);

    return 0;
}

int
epoll_loop_get_fd()
{
    return s_EPollFd;
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
static int s_SocketFd = -1;
static int s_DebuggerFd = -1;
//static buffer* s_ReceivedBuffer = NULL;
//static buffer* s_SendBuffer = NULL;
//static size_t s_SendBufferOffset = 0;
static void* s_Ctx;
static net_callback s_Callback;

//static
//ssize_t Send(int fd)
//{
//    ssize_t s = 0;
//    size_t bytes;

//    if (s_SendBuffer &&
//        (bytes = buf_size(s_SendBuffer) - s_SendBufferOffset) > 0)
//    {
//Send:
//        s = write(fd, s_SendBuffer->beg + s_SendBufferOffset, bytes);

//        if (s < 0)
//        {
//            switch (errno)
//            {
//            case EAGAIN:
//            case EINTR:
//                goto Send;
//            default:
//                break;
//            }
//        }

//        if (s > 0 && s_Callback)
//        {
//            s_Callback(s_Ctx, NET_EVENT_SEND, NULL, s);
//        }
//    }

//    return s;
//}

//static
//ssize_t Receive(int fd)
//{
//    ssize_t r = 0;

//    for (r = 1; s_ReceivedBuffer && r > 0; )
//    {
//        if (!buf_left(s_ReceivedBuffer))
//        {
//            if (!buf_resize(s_ReceivedBuffer, buf_size(s_ReceivedBuffer) * 2))
//            {
//                errno = ENOMEM;
//                return -1;
//            }
//        }

//        r = read(fd, s_ReceivedBuffer->end, buf_left(s_ReceivedBuffer));

//        if (r < 0)
//        {
//            switch (errno)
//            {
//            case EAGAIN:
//            case EINTR:
//            case EBADF:
//                r = 0;
//                break;
//            default:
//                return -1;
//            }
//        }

//        if (r > 0)
//        {
//            s_ReceivedBuffer->end += r;

//            if (s_Callback)
//            {
//                s_Callback(s_Ctx, NET_EVENT_RECEIVE);
//            }
//        }
//    }

//    return r;
//}

static
void
EPollConnectionDataHandler(void *ctx, struct epoll_event *ev)
{
    assert(ev);

    if (ev->events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
    {
        epoll_callback_data ecd;
Close:
        memset(&ecd, 0, sizeof(ecd));
        epoll_loop_set_callback(ev->data.fd, ecd);

        safe_close(ev->data.fd);
        s_DebuggerFd = -1;

        // no mpre oin
//        ev->data.fd = s_SocketFd;
//        ev->events = EPOLLET | EPOLLIN;
//        epoll_ctl(epoll_loop_get_fd(), EPOLL_CTL_MOD, s_SocketFd, ev);

        if (s_Callback)
        {
            s_Callback(s_Ctx, NET_EVENT_HANGUP);
        }
    }
    else
    {
        int flags = 0;
        if (ev->events & EPOLLIN)
        {
//            ssize_t r = Receive(ev->data.fd);

//            if (r < 0)
//            {
//                goto Close;
//            }
            flags |= NET_EVENT_RECEIVE;
        }

        if (ev->events & EPOLLOUT)
        {
//            ssize_t s = Send(ev->data.fd);

//            if (s < 0)
//            {
//                goto Close;
//            }
            flags |= NET_EVENT_SEND;
        }

        if (flags && s_Callback) {
            s_Callback(s_Ctx, flags);
        }
    }
}


static
void
EPollAcceptHandler(void *ctx, struct epoll_event* ev)
{
    assert(ev);

    if (ev->events & (EPOLLHUP | EPOLLERR))
    {
        epoll_callback_data ecd;
        memset(&ecd, 0, sizeof(ecd));
        epoll_loop_set_callback(ev->data.fd, ecd);

        // HANDLE THIS
    }
    else if (ev->events & EPOLLIN)
    {
        int fd = accept4(ev->data.fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (fd >= 0)
        {
            if (s_DebuggerFd >= 0)
            {
                safe_close(fd);
            }
            else
            {
                int i = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *) &i, sizeof(i));

                ev->data.fd = fd;
                ev->events = EPOLLIN | EPOLLOUT | EPOLLET;

                if (epoll_ctl(epoll_loop_get_fd(), EPOLL_CTL_ADD, ev->data.fd, ev) == 0)
                {
                    epoll_callback_data ecd;
                    memset(&ecd, 0, sizeof(ecd));
                    ecd.callback = EPollConnectionDataHandler;
                    epoll_loop_set_callback(fd, ecd);

//                    // no mpre oin
//                    ev->data.fd = s_SocketFd;
//                    ev->events = EPOLLET;
//                    epoll_ctl(epoll_loop_get_fd(), EPOLL_CTL_MOD, s_SocketFd, ev);

                    s_DebuggerFd = fd;

                    if (s_Callback) {
                        s_Callback(s_Ctx, NET_EVENT_CONNECT);
                    }
                }
                else
                {
                    safe_close(fd);
                }
            }
        }
    }
}


int
net_listen(int p, int flags)
{
    int error = 0;
    int Limit = 10;
    int fd = -1;
    int on = 1;
    int i = 0;
    char port[6];
    struct addrinfo hints;
    struct addrinfo * info = NULL;
    struct epoll_event ev;
    epoll_callback_data ecd;

    memset(&hints, 0, sizeof(hints));
    memset(&ev, 0, sizeof(ev));
    memset(&ecd, 0, sizeof(ecd));

    error = epoll_loop_create();
    if (error < 0)
    {
        goto Out;
    }

    snprintf(port, sizeof(port), "%d", p);

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = IPPROTO_TCP;

    for (i = 0; i < Limit; ++i)
    {
        error = getaddrinfo("0.0.0.0", port, &hints, &info);

        switch (error)
        {
        case 0:
            i = Limit;
            break;
        case EAI_AGAIN:
            info = NULL;
            usleep(100000);
            break;
        case EAI_NODATA: // this happens if DNS can't resolve the host name
        case EAI_SERVICE: // happens if there is no DNS server entry in resolv.conf
        default:
            error = -1;
            errno = ENETDOWN;
            goto Exit;
        }
    }


    fd = socket(info->ai_family, info->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, info->ai_protocol);
    if (fd == -1)
    {
        error = -1;
        goto Exit;
    }

    // Allow address resuse
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    error = bind(fd, info->ai_addr, info->ai_addrlen);
    if (error == -1)
    {
        goto Exit;
    }

    if (listen(fd, SOMAXCONN) < 0)
    {
        error = -1;
        goto Exit;
    }


    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epoll_loop_get_fd(), EPOLL_CTL_ADD, ev.data.fd, &ev) < 0)
    {
        error = -1;
        goto Exit;
    }

    ecd.callback = EPollAcceptHandler;
    if (epoll_loop_set_callback(fd, ecd) < 0)
    {
        error = -1;
        goto Exit;
    }

    s_SocketFd = fd;

    goto Out;

Exit:
    epoll_loop_destroy();
    if (fd >= 0) safe_close(fd);
Out:
    if (info) freeaddrinfo(info);
    return error;
}

void
net_set_callback(void* ctx, net_callback callback)
{
    s_Ctx = ctx;
    s_Callback = callback;
}

void
net_close()
{
    if (s_DebuggerFd >= 0)
    {
        shutdown(s_DebuggerFd, O_RDWR);
    }
}

int
net_hangup()
{
    epoll_callback_data ecd;
    memset(&ecd, 0, sizeof(ecd));

    if (s_SocketFd >= 0)
    {
        epoll_loop_set_callback(s_SocketFd, ecd);
        safe_close_ref(&s_SocketFd);

        net_close();

        epoll_loop_destroy();
    }

    return 0;
}

int
net_send(const char* buffer, int bytes)
{
    ssize_t s;
Send:
    s = write(s_DebuggerFd, buffer, bytes);

    if (s < 0)
    {
        switch (errno)
        {
        case EAGAIN:
        case EINTR:
            goto Send;
        default:
            return -1;
        }
    }


    return (int)s;
}


int
net_receive(char* buffer, int bytes) {
    int re = 0;

    while (re < bytes) {
        ssize_t r = read(s_DebuggerFd, buffer, bytes);

        if (r < 0) {
            switch (errno)
            {
            case EINTR:
                break;
            case EAGAIN:
                return re;
            default:
                return -1;
            }
        } else if (r > 0) {
            buffer += r;
            re += r;
        } else {
            break;
        }
    }

    return re;
}

