#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string>
#include <string_view>
#include <chrono>
#include <cerrno>
#include <fmt/format.h> // https://github.com/fmtlib/fmt

#include <liburing/io_service.hpp>

const auto MIN_CHUNK_SIZE = 16 * 1024 * 1024;
const auto BUFFER_SIZE = 4096;

uio::task<> empty() {
  co_return;
}

uio::task<> start_work(uio::io_service& service, const char* hostname, const char* path) {
 
    addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    }, *addrs;
    if (int ret = getaddrinfo(hostname, "http", &hints, &addrs); ret < 0) {
        fmt::print(stderr, "getaddrinfo({}): {}\n", hostname, gai_strerror(ret));
        throw std::runtime_error("getaddrinfo");
    }
    uio::on_scope_exit freeaddr([=]() { freeaddrinfo(addrs); });
    int    pipefd[2];
    pipe(pipefd) | uio::panic_on_err("pipe2", true);
    uio::on_scope_exit closepipe([=]() { close(pipefd[0]); close(pipefd[1]); });
 

    for (struct addrinfo *addr = addrs; addr; addr = addr->ai_next) {
        int clientfd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol) | uio::panic_on_err("socket creation", true);
        uio::on_scope_exit closesock([&]() { service.close(clientfd); });

        if (co_await service.connect(clientfd, addr->ai_addr, addr->ai_addrlen) < 0) continue;

        auto header = fmt::format("GET {} HTTP/1.0\r\nHost: {}\r\nAccept: */*\r\n\r\n", path, hostname);
        co_await service.send(clientfd, header.data(), header.size(), MSG_NOSIGNAL) | uio::panic_on_err("send", false);

        std::array<char, 1024> buffer;
        int res;

        do {
          res = co_await service.splice(clientfd, -1, pipefd[1], -1, 8 * 1024, SPLICE_F_MOVE) | uio::panic_on_err("splice1", false);
          if (res <= 0) break;
          co_await service.splice(pipefd[0], -1, STDOUT_FILENO, -1, 8 * 1024, SPLICE_F_MOVE) | uio::panic_on_err("splice2", false);
        } while (res > 0);
        co_return;
    }
    fmt::print(stdout, "ended!\n");

    throw std::runtime_error("Unable to connect any resolved server");
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fmt::print("Usage: {} <hostname> <path>\n", argv[0]);
        return 1;
    }

    uio::io_service service;

    // Start main coroutine ( for co_await )
    service.run(start_work(service, argv[1], argv[2]));
}
