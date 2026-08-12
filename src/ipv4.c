/* spotc-ipv4.so: LD_PRELOAD shim for the librespot child only.
 * Forces getaddrinfo to AF_INET: routers that advertise IPv6 without working
 * upstream leave the dealer websocket with ENETUNREACH (AAAA is tried first),
 * which kills Spirc. Spotify is fully reachable over IPv4. */
#define _GNU_SOURCE
#include <netdb.h>
#include <dlfcn.h>
#include <sys/socket.h>

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    static int (*real)(const char *, const char *,
                       const struct addrinfo *, struct addrinfo **);
    if (!real) real = dlsym(RTLD_NEXT, "getaddrinfo");
    struct addrinfo h = {0};
    if (hints) h = *hints;
    if (h.ai_family == AF_UNSPEC) h.ai_family = AF_INET;
    return real(node, service, &h, res);
}
