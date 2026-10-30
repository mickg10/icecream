/*
   Environment-transfer driver for envstore-run.sh.

   Plays the remote-client side of an environment install exactly as
   client/remote.cpp does (EnvTransferMsg, FileChunkMsg stream, EndMsg,
   VerifyEnvMsg), but against one daemon port and with an optional pause in
   the middle of the stream so a test can overlap two installs.

   Usage:
     envstore_xfer xfer HOST PORT TARGET NAME FILE [PAUSE_AT_BYTES PAUSED_MARKER RESUME_FILE]
         prints "verify=ok", "verify=fail" or "verify=none"; with
         ENVSTORE_XFER_DISCONNECT=1 it drops the connection at the pause
         instead of resuming (prints "disconnected", exit 3);
         exit 0 / 1 / 2 respectively.
     envstore_xfer internals HOST PORT
         prints the daemon's dump_internals() text.
     envstore_xfer digest FILE
         prints the XXH3-128 digest the daemon names a store tree by.
     envstore_xfer control HOST PORT LINE
         sends one command to a scheduler's text port (scheduler port + 1)
         and prints the reply up to its "200 done".
*/

#include "comm.h"
#include "digest128.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;

static Msg *wait_for_type(MsgChannel *channel, Msg::Value wanted, int timeout_msec)
{
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        Msg *msg = channel->get_msg(1, true);
        if (msg) {
            if (*msg == wanted) {
                return msg;
            }
            delete msg;
        }
        if (channel->at_eof()) {
            return nullptr;
        }
    }
    return nullptr;
}

static int do_internals(const char *host, int port)
{
    MsgChannel *channel = Service::createChannel(host, port, 10);
    if (!channel) {
        fprintf(stderr, "envstore_xfer: connect %s:%d failed\n", host, port);
        return 2;
    }
    if (!channel->send_msg(GetInternalStatus())) {
        delete channel;
        return 2;
    }
    Msg *msg = wait_for_type(channel, Msg::STATUS_TEXT, 10000);
    StatusTextMsg *status = msg ? dynamic_cast<StatusTextMsg *>(msg) : nullptr;
    if (!status) {
        delete msg;
        delete channel;
        return 2;
    }
    fputs(status->text.c_str(), stdout);
    delete msg;
    delete channel;
    return 0;
}

static int do_digest(const char *file)
{
    const int fd = open(file, O_RDONLY);
    if (fd < 0) {
        perror("envstore_xfer: open");
        return 2;
    }
    icecc::Digest128Builder digest;
    static unsigned char buffer[65536];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        digest.append(std::span<const uint8_t>(buffer, static_cast<size_t>(n)));
    }
    close(fd);
    if (n < 0) {
        return 2;
    }
    printf("%s\n", icecc::digest128_hex(digest.finish()).c_str());
    return 0;
}

static int do_control(const char *host, int port, const std::string &line)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (fd < 0 || inet_pton(AF_INET, host, &addr.sin_addr) != 1
            || connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        fprintf(stderr, "envstore_xfer: control connect %s:%d failed\n", host, port);
        return 2;
    }
    const std::string request = line + "\n";
    if (write(fd, request.data(), request.size()) != static_cast<ssize_t>(request.size())) {
        close(fd);
        return 2;
    }
    std::string reply;
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(20);
    while (Clock::now() < deadline && reply.find("200 done") == std::string::npos) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, 200) <= 0) {
            continue;
        }
        char buf[4096];
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        reply.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    fputs(reply.c_str(), stdout);
    return reply.find("200 done") == std::string::npos ? 2 : 0;
}

static bool wait_for_file(const char *path, int timeout_msec)
{
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        if (access(path, F_OK) == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

static int do_xfer(int argc, char **argv)
{
    const char *host = argv[2];
    const int port = atoi(argv[3]);
    const std::string target = argv[4];
    const std::string name = argv[5];
    const char *file = argv[6];
    long long pause_at = -1;
    const char *paused_marker = nullptr;
    const char *resume_file = nullptr;
    if (argc >= 10) {
        pause_at = atoll(argv[7]);
        paused_marker = argv[8];
        resume_file = argv[9];
    }

    const int fd = open(file, O_RDONLY);
    if (fd < 0) {
        perror("envstore_xfer: open");
        return 2;
    }
    MsgChannel *channel = Service::createChannel(host, port, 10);
    if (!channel) {
        fprintf(stderr, "envstore_xfer: connect %s:%d failed\n", host, port);
        close(fd);
        return 2;
    }

    bool sent = channel->send_msg(EnvTransferMsg(target, name));
    long long total = 0;
    bool paused = false;
    static unsigned char buffer[100000];
    while (sent) {
        const ssize_t bytes = read(fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            perror("envstore_xfer: read");
            sent = false;
            break;
        }
        if (bytes == 0) {
            break;
        }
        FileChunkMsg chunk(buffer, static_cast<size_t>(bytes));
        sent = channel->send_msg(chunk);
        total += bytes;
        if (sent && !paused && pause_at >= 0 && total >= pause_at) {
            paused = true;
            const int marker = open(paused_marker, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (marker >= 0) {
                close(marker);
            }
            if (!wait_for_file(resume_file, 120000)) {
                fprintf(stderr, "envstore_xfer: no resume file %s\n", resume_file);
                sent = false;
            } else if (getenv("ENVSTORE_XFER_DISCONNECT")) {
                close(fd);
                delete channel;
                printf("disconnected\n");
                return 3;
            }
        }
    }
    close(fd);

    int rc = 2;
    if (sent && channel->send_msg(EndMsg())
            && channel->send_msg(VerifyEnvMsg(target, name))) {
        Msg *msg = wait_for_type(channel, Msg::VERIFY_ENV_RESULT, 60000);
        VerifyEnvResultMsg *result = msg ? dynamic_cast<VerifyEnvResultMsg *>(msg) : nullptr;
        if (result) {
            rc = result->ok ? 0 : 1;
        }
        delete msg;
    }
    printf("verify=%s\n", rc == 0 ? "ok" : rc == 1 ? "fail" : "none");
    delete channel;
    return rc;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "internals") == 0) {
        return do_internals(argv[2], atoi(argv[3]));
    }
    if (argc >= 3 && strcmp(argv[1], "digest") == 0) {
        return do_digest(argv[2]);
    }
    if (argc >= 5 && strcmp(argv[1], "control") == 0) {
        return do_control(argv[2], atoi(argv[3]), argv[4]);
    }
    if (argc >= 7 && strcmp(argv[1], "xfer") == 0) {
        return do_xfer(argc, argv);
    }
    fprintf(stderr, "usage: envstore_xfer xfer HOST PORT TARGET NAME FILE "
                    "[PAUSE_AT PAUSED_MARKER RESUME_FILE] | internals HOST PORT"
                    " | digest FILE | control HOST PORT LINE\n");
    return 2;
}
