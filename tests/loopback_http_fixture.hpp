#pragma once

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "knowledge.hpp"

namespace axiom::test {

struct HttpRequest {
    std::string method, target;
    knowledge::Bytes body;
};
struct HttpResponse {
    knowledge::Bytes body;
    int status = 200;
};
class HttpFixture {
public:
    explicit HttpFixture(std::function<HttpResponse(const HttpRequest&)> handler)
        : handler_(std::move(handler)), listener_(socket(AF_INET, SOCK_STREAM, 0)) {
        require(listener_.value >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener_.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        require(listen(listener_.value, 16) == 0);
        socklen_t length = sizeof(address);
        require(getsockname(listener_.value, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] {
            try { serve(); }
            catch (...) { std::lock_guard lock(mutex_); failure_ = std::current_exception(); }
        });
    }
    ~HttpFixture() { stop(); }
    HttpFixture(const HttpFixture&) = delete;
    HttpFixture& operator=(const HttpFixture&) = delete;
    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }
    void finish() {
        stop();
        std::lock_guard lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
    }
    std::vector<HttpRequest> requests() {
        std::lock_guard lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
        return requests_;
    }
private:
    struct Socket {
        int value;
        explicit Socket(int descriptor) : value(descriptor) {}
        ~Socket() { if (value >= 0) close(value); }
        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;
    };
    static void require(bool condition) {
        if (!condition) throw std::runtime_error("Loopback HTTP fixture failed");
    }
    static void send_bytes(int socket, std::span<const std::uint8_t> bytes) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto size = send(socket, bytes.data() + offset, bytes.size() - offset, flags);
            if (size <= 0) return; // Client cancellation is exercised deliberately.
            offset += static_cast<std::size_t>(size);
        }
    }
    void stop() noexcept {
        if (!thread_.joinable()) return;
        stopping_.store(true);
        Socket wake(socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port_);
        if (wake.value < 0 || connect(wake.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            std::terminate();
        }
        thread_.join();
    }
    void serve() {
        for (;;) {
            Socket client(accept(listener_.value, nullptr, nullptr));
            require(client.value >= 0);
            if (stopping_.load()) return;
            const timeval timeout{5, 0};
            require(setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
            require(setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
#ifdef SO_NOSIGPIPE
            const int enabled = 1;
            require(setsockopt(client.value, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0);
#endif
            std::string input;
            while (input.find("\r\n\r\n") == std::string::npos) {
                char bytes[1024];
                const auto size = recv(client.value, bytes, sizeof(bytes), 0);
                require(size > 0 && input.size() + static_cast<std::size_t>(size) <= 16384);
                input.append(bytes, static_cast<std::size_t>(size));
            }
            const auto body_start = input.find("\r\n\r\n") + 4;
            const auto headers = input.substr(0, body_start);
            const auto first_space = headers.find(' '), second_space = headers.find(' ', first_space + 1);
            require(first_space != std::string::npos && second_space != std::string::npos);
            require(headers.find("Authorization:") == std::string::npos && headers.find("Cookie:") == std::string::npos);
            HttpRequest request{headers.substr(0, first_space),
                headers.substr(first_space + 1, second_space - first_space - 1),
                {input.begin() + static_cast<std::ptrdiff_t>(body_start), input.end()}};
            const auto length_at = headers.find("Content-Length: ");
            const auto length = length_at == std::string::npos ? 0ULL : std::stoull(headers.substr(length_at + 16));
            require(length <= 2 * knowledge::maximum_text_bytes + 256 && request.body.size() <= length);
            while (request.body.size() < length) {
                std::uint8_t bytes[1024];
                const auto size = recv(client.value, bytes, std::min<std::size_t>(sizeof(bytes), length - request.body.size()), 0);
                require(size > 0);
                request.body.insert(request.body.end(), bytes, bytes + size);
            }
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(request);
            }
            const auto response = handler_(request);
            const auto header = "HTTP/1.1 " + std::to_string(response.status) + " Fixture\r\n"
                "Content-Type: application/octet-stream\r\nConnection: close\r\nContent-Length: "
                + std::to_string(response.body.size()) + "\r\n\r\n";
            send_bytes(client.value, {reinterpret_cast<const std::uint8_t*>(header.data()), header.size()});
            send_bytes(client.value, response.body);
        }
    }
    std::function<HttpResponse(const HttpRequest&)> handler_;
    Socket listener_;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::exception_ptr failure_;
    std::vector<HttpRequest> requests_;
    std::thread thread_;
};

} // namespace axiom::test
