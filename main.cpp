#include <algorithm>
#include <queue>
#include <fcntl.h>
#include <memory>
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
#include <filesystem>

#include <liburing/io_service.hpp>

const auto MIN_CHUNK_SIZE = 128 * 1024 * 1024;
const auto BUFFER_SIZE = 8192;

class HTTPConnection {
private:
public:
  int fd;
  const char* hostname;
  bool receiving = false;
  std::unique_ptr<std::string> buffer;
  // disallow copy, move
  HTTPConnection(const HTTPConnection&) = delete;
  HTTPConnection(HTTPConnection&&) = delete;
  HTTPConnection(int fd, const char* hostname): fd(fd), hostname(hostname) {};
  ~HTTPConnection() {
    close(fd);
  };
  uio::task<int> send(uio::io_service& service, const std::string_view str) {
    return service.send(fd, str.data(), str.length(), MSG_NOSIGNAL) | uio::panic_on_err("send", false);
  }
  uio::task<std::string> recv_headers(uio::io_service& service) {
    // TODO: wrap in some sort of async mutex thing
    // that way we can actually pipeline this stuff
    // see cppcoro
    if (receiving) throw std::runtime_error("already receiving");
    receiving = true;
    uio::on_scope_exit reset([&]() { receiving = false; });
    std::vector<char> buf(4096);
    unsigned int length = 0;
    if (buffer) {
      std::copy(buffer->begin(), buffer->end(), buf.begin());
      length = buffer->length();
      buffer.reset();
    }
    do {
      std::string_view buf_view(buf.data(), length);
      auto idx = buf_view.find("\r\n\r\n");
      if (idx != -1) {
        if (idx + 4 < length)
          buffer = std::make_unique<std::string>(buf_view.substr(idx + 4));

        co_return buf_view.substr(0, idx);
      }
      if (length >= 4096) {
        throw std::system_error(1, std::generic_category(), "header too long");
      }
      int res = co_await service.recv(fd, buf.data() + length, 4096 - length, 0) | uio::panic_on_err("recv", false);
      if (res < 1) throw std::system_error(1, std::generic_category(), "connection closed");
      length += res;
    } while(true);
  }
};

uio::task<unsigned int> get_content_length(uio::io_service& service, HTTPConnection& conn, const char* url) {
  fmt::print(stderr, "get_content_length start: {}\n", url);
  auto request = fmt::format("HEAD {} HTTP/1.1\r\nHost: {}\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n", url, conn.hostname);
  co_await conn.send(service, request);
  auto headers = co_await conn.recv_headers(service);
  auto pos = headers.find("\r\nContent-Length: ");
  if (pos < 1) throw std::system_error(1, std::generic_category(), "couldn't find content-length header");
  auto end = headers.find("\r\n", pos + 18);
  std::string content_length = headers.substr(pos + 18, end - pos - 18);
  unsigned int length = std::stoul(content_length);
  fmt::print(stderr, "content-length: {}\n", length);
  co_return length;
}

uio::sqe_awaitable uio_fallocate(uio::io_service& service, int fd, unsigned int length) {
  auto* sqe = service.io_uring_get_sqe_safe();
  io_uring_prep_fallocate(sqe, fd, 0, 0, length);
  io_uring_sqe_set_flags(sqe, 0);
  return uio::sqe_awaitable(sqe);
}

class File {
public:
  int fd;
  File(int fd): fd(fd) {};
  // disallow copy, move
  File(const File&) = delete;
  File(File&&) = delete;
  ~File() {
    close(fd);
  }
  static uio::task<std::shared_ptr<File>> create(uio::io_service& service, std::string filename, unsigned int length) {
    int fd = co_await service.openat(AT_FDCWD, filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644) | uio::panic_on_err("open", false);
    co_await uio_fallocate(service, fd, length);
    co_return std::make_shared<File>(fd);
  }
};

class FileSpec {
public:
  std::string dest;
  std::string src;
  std::vector<std::string> files;
  FileSpec(std::string dest, std::string src, std::vector<std::string> files): dest(dest), src(src), files(files) {};
};

std::vector<FileSpec> filespecs = {
  FileSpec("openai/clip-vit-large-patch14-336", "clip-vit-large-patch14-336/ce19dc912ca5cd21c8a653c79e251e808ccabcd1", {
    "config.json",
    "preprocessor_config.json",
    "pytorch_model.bin"
  }),
};

class Chunk {
public:
  std::shared_ptr<File> file;
  unsigned int offset;
  unsigned int length;
  std::string url;
  Chunk(std::shared_ptr<File> file, unsigned int offset, unsigned int length, std::string url): file(file), offset(offset), length(length), url(url) {};
};

uio::task<> download_chunk(uio::io_service& service, HTTPConnection& conn, Chunk& chunk) {
  fmt::print(stderr, "download_chunk start: {}\n", chunk.url);
  // time it with wall clock, then calculate speed
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  auto request = fmt::format("GET {} HTTP/1.1\r\nHost: {}\r\nAccept: */*\r\nConnection: keep-alive\r\nRange: bytes={}-{}\r\n\r\n", chunk.url, conn.hostname, chunk.offset, chunk.offset + chunk.length - 1);
  co_await conn.send(service, request);
  auto headers = co_await conn.recv_headers(service);
  
  unsigned int done = 0;
  if (conn.buffer) {
    co_await service.write(chunk.file->fd, conn.buffer->data(), conn.buffer->length(), done) | uio::panic_on_err("write", false);
    done += conn.buffer->length();
    conn.buffer.reset();
  }
  std::array<char, BUFFER_SIZE> buf;
  // todo: fixed size reads, enqueue & link
  // or, splice
  while (done < chunk.length) {
    int read = co_await service.recv(conn.fd, buf.data(), std::min<int>(chunk.length - done, BUFFER_SIZE), 0) | uio::panic_on_err("recv", false);
    if (read < 1) throw std::system_error(1, std::generic_category(), "connection closed");
    co_await service.write(chunk.file->fd, buf.data(), read, done) | uio::panic_on_err("write", false);
    done += read;
  }
  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  fmt::print(stderr, "download_chunk speed: {} MB/s\n", (chunk.length / 1024.0 / 1024.0 * 8.0) / (duration / 1000.0));
}

uio::task<> download_filespec(uio::io_service& service, std::vector<std::unique_ptr<HTTPConnection>>& conn, FileSpec& filespec) {
  // fmt::print(stderr, "download_filespec start: {}\n", filespec.dest);
  std::queue<Chunk> chunks;
  std::filesystem::create_directories(filespec.dest);
  uint64_t total_length = 0;
  for (auto& file_ : filespec.files) {
    auto url = fmt::format("/replicate-weights/{}/{}", filespec.src, file_);
    uint64_t length = co_await get_content_length(service, *conn[0], url.c_str());
    total_length += length;
    // todo create base dir
    auto file = co_await File::create(service, filespec.dest + "/" + file_, length);
    // enqueue chunks
    auto offset = 0;
    do {
      chunks.emplace(file, offset, std::min<unsigned int>(length - offset, MIN_CHUNK_SIZE), url);
      offset += MIN_CHUNK_SIZE;
    } while (offset < length);
  }
  fmt::print(stderr, "download_filespec: {} chunks\n", chunks.size());
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  std::vector<uio::task<>> tasks;
  auto fn = [&](auto& conn_) -> uio::task<> {
    while (!chunks.empty()) {
      auto chunk = chunks.front();
      chunks.pop();
      co_await download_chunk(service, *conn_, chunk);
    }
  };
  for (auto& conn_ : conn) {
    tasks.emplace_back(fn(conn_));
  }
  for (auto& task : tasks) {
    co_await task;
  }
  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  fmt::print(stderr, "download_filespec speed: {} MB/s\n", (total_length / 1024.0 / 1024.0 * 8.0) / (duration / 1000.0));
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
        std::vector<std::unique_ptr<HTTPConnection>> connections;
        connections.emplace_back(std::make_unique<HTTPConnection>(clientfd, hostname));
        for (int i = 0; i < 4; i++) {
          int clientfd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol) | uio::panic_on_err("socket creation", true);
          //uio::on_scope_exit closesock([&]() { service.close(clientfd); });
          if (co_await service.connect(clientfd, addr->ai_addr, addr->ai_addrlen) < 0) throw std::runtime_error("Unable to connect any resolved server");
          connections.emplace_back(std::make_unique<HTTPConnection>(clientfd, hostname));
        }
        fmt::print(stdout, "opened {} connections\n", connections.size());

        co_await download_filespec(service, connections, filespecs[0]);

        // int res;
        // do {
        //   res = co_await service.splice(clientfd, -1, pipefd[1], -1, 8 * 1024, SPLICE_F_MOVE) | uio::panic_on_err("splice1", false);
        //   if (res <= 0) break;
        //   co_await service.splice(pipefd[0], -1, STDOUT_FILENO, -1, 8 * 1024, SPLICE_F_MOVE) | uio::panic_on_err("splice2", false);
        // } while (res > 0);
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
