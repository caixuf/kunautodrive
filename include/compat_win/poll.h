#ifndef FLOWENGINE_COMPAT_WIN_POLL_H
#define FLOWENGINE_COMPAT_WIN_POLL_H

#include <winsock2.h>

#ifndef POLLIN
#define POLLIN  0x0001
#endif
#ifndef POLLOUT
#define POLLOUT 0x0004
#endif

/* select has been part of Winsock since v1 and is sufficient for
 * FlowEngine's small descriptor sets.  Older MinGW winsock2.h lacks the
 * poll family entirely; current WinLibs provides POLLRDNORM with pollfd. */
#ifndef POLLRDNORM
struct pollfd {
    int   fd;
    short events;
    short revents;
};
#endif

static inline int flow_win_poll(struct pollfd* fds, unsigned long nfds, int timeout) {
    if (!fds || nfds == 0) return 0;

    fd_set readfds, writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    SOCKET maxfd = 0;
    for (unsigned long i = 0; i < nfds; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        SOCKET s = (SOCKET)fds[i].fd;
        if (fds[i].events & POLLIN) FD_SET(s, &readfds);
        if (fds[i].events & POLLOUT) FD_SET(s, &writefds);
        if (s > maxfd) maxfd = s;
    }

    struct timeval tv;
    struct timeval* tvp = NULL;
    if (timeout >= 0) {
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        tvp = &tv;
    }
    int rc = select((int)maxfd + 1, &readfds, &writefds, NULL, tvp);
    if (rc <= 0) return rc;
    int ready = 0;
    for (unsigned long i = 0; i < nfds; ++i) {
        if (fds[i].fd < 0) continue;
        SOCKET s = (SOCKET)fds[i].fd;
        if ((fds[i].events & POLLIN) && FD_ISSET(s, &readfds)) fds[i].revents |= POLLIN;
        if ((fds[i].events & POLLOUT) && FD_ISSET(s, &writefds)) fds[i].revents |= POLLOUT;
        if (fds[i].revents) ++ready;
    }
    return ready;
}

#define poll flow_win_poll

#endif /* FLOWENGINE_COMPAT_WIN_POLL_H */
