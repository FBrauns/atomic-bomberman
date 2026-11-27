// The sockets of the browser build (Emscripten). A page cannot open sockets, so UDP is
// carried by a small virtual network made in JavaScript (web/net.js): datagrams to this
// machine (127.x) go from one socket to another inside the page, datagrams to another
// player go over a WebRTC data channel. Other players get made-up addresses (10.x.y.z)
// when their channel opens; a game is reached by its room code, which resolveHostPort()
// turns into such an address. Server, Client and Link run unchanged on top of it.
// TCP is not available: a client whose datagrams get no answer reports that it cannot connect.
#include "net/socket.hpp"

#include <chrono>
#include <cstring>

#include <emscripten.h>

// --- the JavaScript side (web/net.js holds Module.abNet) ---------------------------

EM_JS(int, ab_net_udp_open, (int port, int shared), {
    return Module.abNet ? Module.abNet.open(port, shared !== 0) : -1;
});
EM_JS(int, ab_net_udp_port, (int handle), {
    return Module.abNet ? Module.abNet.portOf(handle) : 0;
});
EM_JS(void, ab_net_udp_close, (int handle), {
    if (Module.abNet) Module.abNet.close(handle);
});
EM_JS(int, ab_net_udp_send, (int handle, unsigned ip, int port, const unsigned char* data, int length), {
    if (!Module.abNet) return 0;
    return Module.abNet.send(handle, ip >>> 0, port, HEAPU8.slice(data, data + length)) ? 1 : 0;
});
// Length of the next datagram (copied to `data`, at most `capacity` bytes), or -1 when none waits.
EM_JS(int, ab_net_udp_receive, (int handle, unsigned char* data, int capacity, unsigned* ip, int* port), {
    if (!Module.abNet) return -1;
    const d = Module.abNet.receive(handle);
    if (!d) return -1;
    const n = Math.min(d.data.length, capacity);
    HEAPU8.set(d.data.subarray(0, n), data);
    HEAPU32[ip >> 2] = d.ip >>> 0;
    HEAP32[port >> 2] = d.port;
    return n;
});
// A room code to the address of its host (and starts connecting to it); 0 if it is not one.
EM_JS(unsigned, ab_net_join_room, (const char* code), {
    return Module.abNet ? (Module.abNet.join(UTF8ToString(code)) >>> 0) : 0;
});

namespace ab::net {

namespace {

// "a.b.c.d" to an address.
std::optional<std::uint32_t> parseIpv4(const std::string& text) {
    std::uint32_t value = 0;
    int parts = 0;
    std::size_t i = 0;
    while (parts < 4) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9') return std::nullopt;
        int part = 0, digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 4) part = part * 10 + (text[i++] - '0'), ++digits;
        if (part > 255) return std::nullopt;
        value = (value << 8) | static_cast<std::uint32_t>(part);
        ++parts;
        if (parts < 4) {
            if (i >= text.size() || text[i] != '.') return std::nullopt;
            ++i;
        }
    }
    if (i != text.size()) return std::nullopt;
    return value;
}
}  // namespace

bool startup() { return true; }
void setTestUdpLoss(int) {}
void setTestUdpDelay(int, int, std::function<std::uint64_t()>) {}

std::uint64_t clockMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool Address::loopback() const { return !v6 && (ip >> 24) == 127; }

std::string Address::text() const {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." +
           std::to_string(ip & 255) + ":" + std::to_string(port);
}

// "localhost", an IPv4 literal, or a room code.
std::optional<Address> resolve(const std::string& host, std::uint16_t port) {
    Address a;
    a.port = port;
    if (host == "localhost") {
        a.ip = kLoopback;
        return a;
    }
    if (const auto ip = parseIpv4(host)) {
        a.ip = *ip;
        return a;
    }
    const std::uint32_t room = ab_net_join_room(host.c_str());
    if (room == 0) return std::nullopt;
    a.ip = room;
    return a;
}

std::optional<Address> resolveHostPort(const std::string& text, std::uint16_t defaultPort) {
    std::string host = text;
    while (!host.empty() && (host.back() == ' ' || host.back() == '\t')) host.pop_back();
    while (!host.empty() && (host.front() == ' ' || host.front() == '\t')) host.erase(host.begin());
    std::uint16_t port = defaultPort;
    if (const auto colon = host.rfind(':'); colon != std::string::npos) {
        const std::string digits = host.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        const int value = std::stoi(digits);
        if (value < 1 || value > 65535) return std::nullopt;
        port = static_cast<std::uint16_t>(value);
        host.resize(colon);
    }
    if (host.empty()) return std::nullopt;
    return resolve(host, port);
}

std::vector<std::string> multicastAsk(const std::string&, std::uint16_t, const std::string&, int) { return {}; }
std::optional<std::string> blockingExchange(const std::string&, int, const std::string&, int, std::string*) { return std::nullopt; }

// --- TCP: not available in a page ---------------------------------------------------

TcpSocket::~TcpSocket() { close(); }
TcpSocket::TcpSocket(TcpSocket&& o) noexcept { *this = std::move(o); }
TcpSocket& TcpSocket::operator=(TcpSocket&& o) noexcept {
    if (this != &o) {
        fd_ = o.fd_;
        connected_ = o.connected_;
        peer_ = o.peer_;
        in_ = std::move(o.in_);
        out_ = std::move(o.out_);
        encrypted_ = o.encrypted_;
        sendKey_ = o.sendKey_;
        receiveKey_ = o.receiveKey_;
        sent_ = o.sent_;
        received_ = o.received_;
        o.fd_ = kNone;
        o.connected_ = false;
    }
    return *this;
}
void TcpSocket::close() {
    fd_ = kNone;
    connected_ = false;
    in_.clear();
    out_.clear();
}
void TcpSocket::setKeys(const Key& sendKey, const Key& receiveKey) {
    sendKey_ = sendKey;
    receiveKey_ = receiveKey;
    encrypted_ = true;
}
bool TcpSocket::connect(const Address& to) {
    peer_ = to;
    return false;
}
void TcpSocket::send(std::uint8_t, const std::vector<std::uint8_t>&) {}
bool TcpSocket::pump() { return false; }
bool TcpSocket::receive(std::uint8_t&, std::vector<std::uint8_t>&) { return false; }

TcpListener::~TcpListener() { close(); }
void TcpListener::close() {
    fd_ = -1;
    port_ = 0;
}
// Pretends to listen, so that a server starts; nobody ever connects.
bool TcpListener::listen(std::uint16_t port) {
    fd_ = 0;
    port_ = port != 0 ? port : 27410;
    return true;
}
std::optional<TcpSocket> TcpListener::accept() { return std::nullopt; }

// --- UDP over the virtual network -----------------------------------------------------

UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() {
    if (fd_ != -1) ab_net_udp_close(static_cast<int>(fd_));
    fd_ = -1;
    port_ = 0;
}

bool UdpSocket::open(std::uint16_t port, bool, bool shared, bool) {
    close();
    const int handle = ab_net_udp_open(port, shared ? 1 : 0);
    if (handle < 0) return false;
    fd_ = handle;
    port_ = static_cast<std::uint16_t>(ab_net_udp_port(handle));
    v6_ = false;
    return true;
}

bool UdpSocket::sendTo(const Address& to, const std::vector<std::uint8_t>& data) { return sendNow(to, data); }

void UdpSocket::sendHeld() {}

bool UdpSocket::sendNow(const Address& to, const std::vector<std::uint8_t>& data) {
    if (fd_ == -1 || to.v6) return false;
    return ab_net_udp_send(static_cast<int>(fd_), to.ip, to.port, data.data(), static_cast<int>(data.size())) != 0;
}

bool UdpSocket::receiveFrom(Address& from, std::vector<std::uint8_t>& data) {
    if (fd_ == -1) return false;
    data.resize(2048);
    unsigned ip = 0;
    int port = 0;
    const int n = ab_net_udp_receive(static_cast<int>(fd_), data.data(), static_cast<int>(data.size()), &ip, &port);
    if (n < 0) {
        data.clear();
        return false;
    }
    data.resize(static_cast<std::size_t>(n));
    from = Address{};
    from.ip = ip;
    from.port = static_cast<std::uint16_t>(port);
    return true;
}

}  // namespace ab::net
