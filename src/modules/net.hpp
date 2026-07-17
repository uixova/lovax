#ifndef LOVAX_MODULE_NET_HPP
#define LOVAX_MODULE_NET_HPP

#include "common.hpp"

// Sockets for the `net` module. OS syscalls only — still zero third-party deps.
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <sys/time.h>
#endif

namespace Lovax {
namespace StdLib {

// ===== net module — blocking TCP/UDP sockets (OS syscalls, zero deps) =====
// A socket is an int handle. Every function validates its arguments and returns
// a catchable error object on any failure, so a misused socket never crashes the
// VM. Blocking calls honor net.set_timeout so a server loop can't hang forever.
#ifdef _WIN32
  inline void netCloseFd(long long fd) { closesocket((SOCKET)fd); }
  inline void netInit() {
      static bool done = false;
      if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = true; }
  }
#else
  inline void netCloseFd(long long fd) { ::close((int)fd); }
  inline void netInit() {}
#endif

inline ObjPtr netErr(const std::string& what, int line) {
    return makeError("net." + what + " failed: " + std::string(std::strerror(errno)), line);
}
inline bool netIsInt(const ObjPtr& o) { return o->type() == ObjectType::INTEGER; }
inline long long netInt(const ObjPtr& o) { return static_cast<IntegerObject*>(o.get())->value; }
inline bool netIsStr(const ObjPtr& o) { return o->type() == ObjectType::STRING; }
inline const std::string& netStr(const ObjPtr& o) { return static_cast<StringObject*>(o.get())->value; }

inline ObjPtr makeNetModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;
    netInit();
    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    // tcp_listen(port) -> server socket handle
    def("tcp_listen", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 1 || !netIsInt(a[0]))
            return makeError("net.tcp_listen(port) expects an integer port", line);
        long long port = netInt(a[0]);
        if (port < 1 || port > 65535) return makeError("net.tcp_listen: port out of range (1-65535)", line);
        int fd = (int)::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return netErr("tcp_listen", line);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t)port);
        if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { auto e = netErr("tcp_listen (bind)", line); netCloseFd(fd); return e; }
        if (::listen(fd, 16) < 0) { auto e = netErr("tcp_listen (listen)", line); netCloseFd(fd); return e; }
        return makeObj<IntegerObject>(fd);
    });

    // tcp_accept(server) -> client socket handle (blocks until a client connects)
    def("tcp_accept", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 1 || !netIsInt(a[0]))
            return makeError("net.tcp_accept(server) expects a socket handle", line);
        int c = (int)::accept((int)netInt(a[0]), nullptr, nullptr);
        if (c < 0) return netErr("tcp_accept", line);
        return makeObj<IntegerObject>(c);
    });

    // tcp_connect(host, port) -> client socket handle
    def("tcp_connect", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 2 || !netIsStr(a[0]) || !netIsInt(a[1]))
            return makeError("net.tcp_connect(host, port) expects (string, int)", line);
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        std::string portStr = std::to_string(netInt(a[1]));
        if (::getaddrinfo(netStr(a[0]).c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
            return makeError("net.tcp_connect: cannot resolve host '" + netStr(a[0]) + "'", line);
        int fd = (int)::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) { freeaddrinfo(res); return netErr("tcp_connect", line); }
        if (::connect(fd, res->ai_addr, (int)res->ai_addrlen) < 0) {
            auto e = netErr("tcp_connect", line); freeaddrinfo(res); netCloseFd(fd); return e;
        }
        freeaddrinfo(res);
        return makeObj<IntegerObject>(fd);
    });

    // send(sock, text) -> bytes sent
    def("send", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 2 || !netIsInt(a[0]) ||
            (!netIsStr(a[1]) && a[1]->type() != ObjectType::BYTES))
            return makeError("net.send(sock, text) expects (socket, string)", line);
        const std::string& s = a[1]->type() == ObjectType::BYTES
            ? static_cast<BytesObject*>(a[1].get())->data
            : netStr(a[1]);
        long long n = ::send((int)netInt(a[0]), s.data(), (int)s.size(), 0);
        if (n < 0) return netErr("send", line);
        return makeObj<IntegerObject>(n);
    });

    // recv(sock, [maxbytes=4096]) -> string ("" when the peer closed)
    def("recv", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.empty() || a.size() > 2 || !netIsInt(a[0]))
            return makeError("net.recv(sock, [maxbytes]) expects a socket handle", line);
        long long maxb = (a.size() == 2 && netIsInt(a[1])) ? netInt(a[1]) : 4096;
        if (maxb < 1 || maxb > 16 * 1024 * 1024) maxb = 4096;
        std::string buf((size_t)maxb, '\0');
        long long n = ::recv((int)netInt(a[0]), &buf[0], (int)maxb, 0);
        if (n < 0) return netErr("recv", line);
        buf.resize((size_t)n);
        return makeObj<StringObject>(buf);
    });

    // set_timeout(sock, seconds) -> null. 0 = blocking forever. Prevents hangs.
    def("set_timeout", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 2 || !netIsInt(a[0]) || (a[1]->type() != ObjectType::FLOAT && a[1]->type() != ObjectType::INTEGER))
            return makeError("net.set_timeout(sock, seconds) expects (socket, number)", line);
        double sec = a[1]->type() == ObjectType::FLOAT ? static_cast<FloatObject*>(a[1].get())->value
                                                       : (double)netInt(a[1]);
        int fd = (int)netInt(a[0]);
#ifdef _WIN32
        DWORD ms = (DWORD)(sec * 1000.0);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
#else
        timeval tv{}; tv.tv_sec = (long)sec; tv.tv_usec = (long)((sec - (long)sec) * 1e6);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        return NULL_OBJ_;
    });

    // close(sock) -> null
    def("close", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 1 || !netIsInt(a[0]))
            return makeError("net.close(sock) expects a socket handle", line);
        netCloseFd(netInt(a[0]));
        return NULL_OBJ_;
    });

    // udp_socket() -> handle
    def("udp_socket", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (!a.empty()) return argCountError("udp_socket", "0", a.size(), line);
        int fd = (int)::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return netErr("udp_socket", line);
        return makeObj<IntegerObject>(fd);
    });
    // udp_bind(sock, port) -> null
    def("udp_bind", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 2 || !netIsInt(a[0]) || !netIsInt(a[1]))
            return makeError("net.udp_bind(sock, port) expects (socket, int)", line);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t)netInt(a[1]));
        if (::bind((int)netInt(a[0]), (sockaddr*)&addr, sizeof(addr)) < 0) return netErr("udp_bind", line);
        return NULL_OBJ_;
    });
    // udp_send(sock, host, port, text) -> bytes sent
    def("udp_send", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.size() != 4 || !netIsInt(a[0]) || !netIsStr(a[1]) || !netIsInt(a[2]) || !netIsStr(a[3]))
            return makeError("net.udp_send(sock, host, port, text) expects (socket, string, int, string)", line);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)netInt(a[2]));
        if (::inet_pton(AF_INET, netStr(a[1]).c_str(), &addr.sin_addr) != 1)
            return makeError("net.udp_send: invalid host '" + netStr(a[1]) + "'", line);
        const std::string& s = netStr(a[3]);
        long long n = ::sendto((int)netInt(a[0]), s.data(), (int)s.size(), 0, (sockaddr*)&addr, sizeof(addr));
        if (n < 0) return netErr("udp_send", line);
        return makeObj<IntegerObject>(n);
    });
    // udp_recv(sock, [maxbytes]) -> string
    def("udp_recv", [](const Args& a, int line, const CallFn&) -> ObjPtr {
        if (a.empty() || a.size() > 2 || !netIsInt(a[0]))
            return makeError("net.udp_recv(sock, [maxbytes]) expects a socket handle", line);
        long long maxb = (a.size() == 2 && netIsInt(a[1])) ? netInt(a[1]) : 4096;
        if (maxb < 1 || maxb > 16 * 1024 * 1024) maxb = 4096;
        std::string buf((size_t)maxb, '\0');
        long long n = ::recvfrom((int)netInt(a[0]), &buf[0], (int)maxb, 0, nullptr, nullptr);
        if (n < 0) return netErr("udp_recv", line);
        buf.resize((size_t)n);
        return makeObj<StringObject>(buf);
    });

    mod->frozen = true;
    mod->moduleName = "net";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}


} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_NET_HPP
