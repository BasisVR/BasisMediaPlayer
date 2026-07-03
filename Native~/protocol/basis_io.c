#include "basis_io.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define BASIS_INVALID_SOCK INVALID_SOCKET
  #define closesock closesocket
  #define sock_errno WSAGetLastError()
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <sys/time.h>
  typedef int sock_t;
  #define BASIS_INVALID_SOCK (-1)
  #define closesock close
  #define sock_errno errno
#endif

struct basis_io {
    sock_t fd;
};

void basis_io_global_init(void) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

void basis_io_global_shutdown(void) {
#if defined(_WIN32)
    WSACleanup();
#endif
}

static void set_blocking(sock_t fd, int blocking) {
#if defined(_WIN32)
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    fcntl(fd, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

void basis_io_set_read_timeout(basis_io_t* io, int timeout_ms) {
    if (!io || io->fd == BASIS_INVALID_SOCK) return;
#if defined(_WIN32)
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(io->fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(io->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

static int local_allowed(void) {
    const char* v = getenv("BASIS_MEDIA_ALLOW_LOCAL");
    return v && v[0];
}

static int ipv4_octets_blocked(uint8_t b0, uint8_t b1) {
    if (b0 == 0) return 1;                              /* 0/8 unspecified */
    if (b0 == 10) return 1;                             /* 10/8 */
    if (b0 == 127) return 1;                            /* 127/8 loopback */
    if (b0 == 100 && (b1 & 0xC0) == 64) return 1;       /* 100.64/10 CGNAT */
    if (b0 == 169 && b1 == 254) return 1;               /* 169.254/16 link-local (metadata) */
    if (b0 == 172 && b1 >= 16 && b1 <= 31) return 1;    /* 172.16/12 */
    if (b0 == 192 && b1 == 168) return 1;               /* 192.168/16 */
    if (b0 >= 224) return 1;                            /* multicast/reserved */
    return 0;
}

/* SSRF guard: reject connecting to a non-global-unicast target. Checks the ACTUAL
 * resolved address, so a public name pointed at a private IP (and DNS rebinding) is
 * caught here at connect time, not just at the URL string. */
static int sockaddr_is_blocked(const struct sockaddr* sa) {
    if (!sa) return 1;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in* s = (const struct sockaddr_in*)sa;
        const uint8_t* b = (const uint8_t*)&s->sin_addr.s_addr; /* network order = octets in order */
        return ipv4_octets_blocked(b[0], b[1]);
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6* s = (const struct sockaddr_in6*)sa;
        const uint8_t* b = (const uint8_t*)s->sin6_addr.s6_addr;
        int i, nz = 0, loop = (b[15] == 1);
        for (i = 0; i < 16; i++) if (b[i]) { nz = 1; break; }
        if (!nz) return 1;                                 /* :: unspecified */
        for (i = 0; i < 15; i++) if (b[i]) { loop = 0; break; }
        if (loop) return 1;                                /* ::1 loopback */
        if ((b[0] & 0xFE) == 0xFC) return 1;               /* fc00::/7 ULA */
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return 1; /* fe80::/10 link-local */
        if (b[0] == 0xFF) return 1;                        /* ff00::/8 multicast */
        {
            int mapped = 1;
            for (i = 0; i < 10; i++) if (b[i]) { mapped = 0; break; }
            if (mapped && b[10] == 0xFF && b[11] == 0xFF)  /* ::ffff:a.b.c.d */
                return ipv4_octets_blocked(b[12], b[13]);
        }
        if (b[0] == 0x20 && b[1] == 0x02)                  /* 2002::/16 6to4 */
            return ipv4_octets_blocked(b[2], b[3]);
        return 0;
    }
    return 1;
}

basis_io_t* basis_io_connect(const char* host, int port, int timeout_ms) {
    if (!host || port <= 0) return NULL;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return NULL;

    sock_t fd = BASIS_INVALID_SOCK;
    int allow_local = local_allowed();
    for (ai = res; ai; ai = ai->ai_next) {
        if (!allow_local && sockaddr_is_blocked(ai->ai_addr)) continue;
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == BASIS_INVALID_SOCK) continue;

        /* non-blocking connect with select() timeout */
        set_blocking(fd, 0);
        int rc = connect(fd, ai->ai_addr, (int)ai->ai_addrlen);
        int inprogress = 0;
#if defined(_WIN32)
        inprogress = (rc != 0 && sock_errno == WSAEWOULDBLOCK);
#else
        inprogress = (rc != 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK));
#endif
        if (rc == 0) {
            set_blocking(fd, 1);
            break;
        }
        if (inprogress) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(fd, &wf);
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            int sel = select((int)fd + 1, NULL, &wf, NULL, timeout_ms > 0 ? &tv : NULL);
            if (sel > 0) {
                int err = 0;
                socklen_t elen = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
                if (err == 0) {
                    set_blocking(fd, 1);
                    break;
                }
            }
        }
        closesock(fd);
        fd = BASIS_INVALID_SOCK;
    }
    freeaddrinfo(res);

    if (fd == BASIS_INVALID_SOCK) return NULL;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));

    basis_io_t* io = (basis_io_t*)calloc(1, sizeof(*io));
    if (!io) { closesock(fd); return NULL; }
    io->fd = fd;
    basis_io_set_read_timeout(io, timeout_ms > 0 ? timeout_ms : 15000);
    return io;
}

int basis_io_read(basis_io_t* io, uint8_t* buf, int len) {
    if (!io || io->fd == BASIS_INVALID_SOCK || !buf || len <= 0) return -1;
    int n = (int)recv(io->fd, (char*)buf, len, 0);
    return n;
}

int basis_io_read_full(basis_io_t* io, uint8_t* buf, int len) {
    int got = 0;
    while (got < len) {
        int n = basis_io_read(io, buf + got, len - got);
        if (n <= 0) return got;
        got += n;
    }
    return got;
}

int basis_io_write_full(basis_io_t* io, const uint8_t* buf, int len) {
    if (!io || io->fd == BASIS_INVALID_SOCK || !buf) return -1;
    int sent = 0;
    while (sent < len) {
        int n = (int)send(io->fd, (const char*)buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

void basis_io_close(basis_io_t* io) {
    if (!io) return;
    if (io->fd != BASIS_INVALID_SOCK) closesock(io->fd);
    free(io);
}
