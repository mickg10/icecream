// Two-process C<->F transport round-trip test for the M1 capability harness.
// Forks: parent = C, child = F, connected by an AF_UNIX socketpair. Exercises the
// M1 dialogue Hello[generation] -> Root -> (F-generated) Need -> Fill -> Ack, and
// verifies the 128-bit generation latches and every frame is byte-exact.
#include "cap_transport.h"
#include "cap_identity.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <cstdio>
using namespace cap;

int main() {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 2; }

    SourceGeneration gen;                       // deterministic (no rng in a test)
    for (int i = 0; i < 16; ++i) gen[i] = uint8_t(0xA0 + i);
    std::vector<uint8_t> rootMsg(1000);         // stand-in ROOT payload
    for (size_t i = 0; i < rootMsg.size(); ++i) rootMsg[i] = uint8_t(i * 7 + 3);
    std::vector<uint8_t> needMsg = {1, 2, 3, 4, 5};
    std::vector<uint8_t> fillMsg(50000);        // stand-in FILL payload (>1 frame worth)
    for (size_t i = 0; i < fillMsg.size(); ++i) fillMsg[i] = uint8_t(i * 13 + 1);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }

    if (pid == 0) {                             // ===== F (child) =====
        close(sv[0]); int fd = sv[1];
        SourceGeneration g;
        if (!recv_hello(fd, g)) { fprintf(stderr, "F: hello failed\n"); _exit(2); }
        if (g != gen)           { fprintf(stderr, "F: generation mismatch\n"); _exit(2); }
        Frame t; std::vector<uint8_t> b;
        if (!recv_frame(fd, t, b) || t != Frame::Root || b != rootMsg) { fprintf(stderr, "F: root differs\n"); _exit(2); }
        if (!send_frame(fd, Frame::Need, needMsg)) _exit(2);          // F derives + sends NEED
        if (!recv_frame(fd, t, b) || t != Frame::Fill || b != fillMsg) { fprintf(stderr, "F: fill differs\n"); _exit(2); }
        if (!send_frame(fd, Frame::Ack, {})) _exit(2);
        _exit(0);
    }

    close(sv[1]); int fd = sv[0];               // ===== C (parent) =====
    bool ok = true;
    ok &= send_hello(fd, gen);
    ok &= send_frame(fd, Frame::Root, rootMsg);
    Frame t; std::vector<uint8_t> b;
    ok &= recv_frame(fd, t, b) && t == Frame::Need && b == needMsg;
    ok &= send_frame(fd, Frame::Fill, fillMsg);
    ok &= recv_frame(fd, t, b) && t == Frame::Ack;
    int st = 0; waitpid(pid, &st, 0);
    bool fok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("cap_transport 2-proc round-trip: C_ok=%d F_ok=%d  gen-latch + Root + F-Need + Fill(%zuB) + Ack  => %s\n",
           int(ok), int(fok), fillMsg.size(), (ok && fok) ? "PASS" : "FAIL");
    return (ok && fok) ? 0 : 1;
}
