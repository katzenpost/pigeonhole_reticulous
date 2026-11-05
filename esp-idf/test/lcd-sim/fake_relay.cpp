// SPDX-License-Identifier: AGPL-3.0-only
// relay.py's socket with the host tests' in-memory replica behind it, for runs with no Reticulum:
// every sim connected shares the one replica.
//   build/fake-relay <socket>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <vector>

#include "fake_replica.h"

struct Conn {
    int fd;
    Bytes in;
    wire::Reassembler reasm;
};

static void put(int fd, const Bytes& f) {
    uint8_t h[2] = {(uint8_t)(f.size() >> 8), (uint8_t)f.size()};
    if (write(fd, h, 2) != 2 || write(fd, f.data(), f.size()) != (ssize_t)f.size()) perror("fake-relay: write");
}

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: fake-relay <socket>\n"); return 2; }
    unlink(argv[1]);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", argv[1]);
    if (bind(srv, (sockaddr*)&a, sizeof a) < 0 || listen(srv, 4) < 0) { perror("fake-relay"); return 1; }
    FakeReplica rep;
    std::vector<Conn> conns;
    for (;;) {
        std::vector<pollfd> p{{srv, POLLIN, 0}};
        for (auto& c : conns) p.push_back({c.fd, POLLIN, 0});
        if (poll(p.data(), p.size(), -1) < 0) return 1;
        for (size_t i = conns.size(); i-- > 0;) {
            if (!p[i + 1].revents) continue;
            Conn& c = conns[i];
            uint8_t tmp[4096];
            ssize_t n = read(c.fd, tmp, sizeof tmp);
            if (n <= 0) {
                close(c.fd);
                conns.erase(conns.begin() + i);
                fprintf(stderr, "fake-relay: a sim left; %zu boxes\n", rep.boxes.size());
                continue;
            }
            c.in.insert(c.in.end(), tmp, tmp + n);
            while (c.in.size() >= 2 && c.in.size() >= 2 + (size_t)(c.in[0] << 8 | c.in[1])) {
                size_t len = (size_t)(c.in[0] << 8 | c.in[1]);
                Bytes f(c.in.begin() + 2, c.in.begin() + 2 + len);
                c.in.erase(c.in.begin(), c.in.begin() + 2 + len);
                std::swap(rep.reasm, c.reasm);
                rep.on_frame(f);
                std::swap(rep.reasm, c.reasm);
                for (auto& o : rep.outbox) put(c.fd, o);
                rep.outbox.clear();
            }
        }
        if (p[0].revents & POLLIN) {
            int fd = accept(srv, nullptr, nullptr);
            if (fd < 0) continue;
            put(fd, {});  // the Channel is up
            conns.push_back({fd, {}, {}});
            fprintf(stderr, "fake-relay: a sim connected\n");
        }
    }
}
