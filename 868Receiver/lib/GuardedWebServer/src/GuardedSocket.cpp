/*
  GuardedSocket.cpp - socket write with a progress deadline (part of GuardedWebServer).
  See GuardedSocket.h. License: GNU Lesser General Public License 2.1 or later.
*/

#include "GuardedSocket.h"
#include <lwip/sockets.h>

// lwip/sockets.h maps these names to lwip_*() macros that would break the member calls below
// (the core's NetworkClient.cpp does the same)
#undef connect
#undef write
#undef read
#undef close

/** @return >0 writable (or failed: the next write reports it), 0 timeout, <0 select() error. */
static int waitWritable(int fd, uint32_t timeoutMs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(fd + 1, nullptr, &set, nullptr, &tv);
}

size_t gwsWriteBounded(NetworkClient& client, const uint8_t* data, size_t len, uint32_t stallMs, uint32_t totalMs) {
    size_t sent = 0;
    const unsigned long start = millis();
    unsigned long lastProgress = start;
    while (sent < len) {
        const int fd = client.fd();
        if (fd < 0 || !client.connected()) break;
        if (totalMs && millis() - start >= totalMs) {
            client.stop();
            break;
        }
        const int ready = waitWritable(fd, 100);
        if (ready < 0) break;
        if (ready > 0) {
            size_t n = len - sent;
            if (n > GWS_WRITE_PIECE) n = GWS_WRITE_PIECE;
            const size_t w = client.NetworkClient::write(data + sent, n);
            if (w == 0) break;                 // the core closed the socket after an error
            sent += w;
            lastProgress = millis();
        } else if (millis() - lastProgress >= stallMs) {
            client.stop();                     // the peer does not read: drop it
            break;
        }
        feedLoopWDT();                         // only reached while inside the stall and total limits
    }
    return sent;
}
