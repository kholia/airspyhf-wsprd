#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "service_notify.h"

static void send_notification(const char *message)
{
    const char *socket_name = getenv("NOTIFY_SOCKET");
    struct sockaddr_un address = {0};
    socklen_t address_length;
    int socket_fd;
    size_t name_length;

    if (socket_name == NULL || socket_name[0] == '\0' || message == NULL) {
        return;
    }
    name_length = strlen(socket_name);
    if (name_length >= sizeof(address.sun_path)) {
        return;
    }
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_name, name_length + 1U);
    if (address.sun_path[0] == '@') {
        address.sun_path[0] = '\0';
    }
    address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                 name_length + 1U);
    socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        return;
    }
    (void)sendto(socket_fd, message, strlen(message), MSG_NOSIGNAL,
                 (const struct sockaddr *)&address, address_length);
    close(socket_fd);
}

void service_notify_ready(void)
{
    send_notification("READY=1\nSTATUS=Receiving and decoding WSPR");
}

void service_notify_status(const char *status)
{
    char message[256];

    if (status != NULL && snprintf(message, sizeof(message), "STATUS=%s", status) > 0) {
        send_notification(message);
    }
}

void service_watchdog_ping(void)
{
    static struct timespec previous;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (previous.tv_sec == 0 || now.tv_sec - previous.tv_sec >= 10) {
        send_notification("WATCHDOG=1");
        previous = now;
    }
}
