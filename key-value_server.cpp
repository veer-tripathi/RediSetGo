// stdlib
#include <assert.h>
#include <cstdint>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

// system
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

// C++
#include <string>
#include <vector>
#include <map>

// ===================== UTIL =====================

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void msg_errno(const char *msg) {
    fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

static void die(const char *msg) {
    fprintf(stderr, "[%d] %s\n", errno, msg);
    abort();
}

static void fd_set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) die("fcntl get");

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        die("fcntl set");
    }
}

// ===================== CONNECTION =====================

const size_t k_max_msg = 32 << 20;

struct Conn {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;

    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
};

// ===================== BUFFER =====================

static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

static void buf_consume(std::vector<uint8_t> &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

// ===================== ACCEPT =====================

static Conn *handle_accept(int fd) {
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);

    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if (connfd < 0) {
        msg_errno("accept()");
        return NULL;
    }

    fd_set_nb(connfd);

    Conn *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    return conn;
}

// ===================== PARSING (NEW) =====================

const size_t k_max_args = 200000;

static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out) {
    // OLD helper (already simple, keep logic)
    if (cur + 4 > end) return false;
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

static bool read_str(const uint8_t *&cur, const uint8_t *end, size_t n, std::string &out) {
    // OLD helper
    if (cur + n > end) return false;
    out.assign((const char*)cur, n);
    cur += n;
    return true;
}

static int32_t parse_req(const uint8_t *data, size_t size, std::vector<std::string> &out) {
    // ===================== TODO =====================
    // Convert raw bytes → vector<string>

    // Protocol format:
    // [nstr][len][str1][len][str2]...

    // Steps:
    // 1. Read number of strings (nstr)
    // 2. Loop nstr times:
    //      - Read length (len)
    //      - Read string of size len
    // 3. Store strings in 'out'
    // 4. Ensure no extra garbage data remains

    // Return:
    // 0  → success
    // -1 → error
    const uint8_t *cur = data;
    const uint8_t *end = data + size;
    uint32_t nstr = 0;
    if (!read_u32(cur, end, nstr)) {
        return -1;
    }
    nstr = ntohl(nstr);
    if (nstr > k_max_args) {
        return -1;
    }
    out.resize(nstr);
    for (uint32_t i = 0; i < nstr; ++i) {
        uint32_t len = 0;
        if (!read_u32(cur, end, len)) {
            return -1;
        }
        len = ntohl(len);
        std::string s;
        if (!read_str(cur, end, (size_t)len, out[i])) {
            return -1;
        }
    }
    if (cur != end) {
        return -1;
    }
    return 0;
}

// ===================== RESPONSE =====================

enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2
};

struct Response {
    uint32_t status = RES_OK;
    std::vector<uint8_t> data;
};

static std::map<std::string, std::string> g_data;

// ===================== KV LOGIC (NEW) =====================

static void do_request(std::vector<std::string> &cmd, Response &out) {
    // ===================== TODO =====================
    // Process command from client

    // cmd example:
    // ["get", "key"]
    // ["set", "key", "value"]
    // ["del", "key"]

    // Steps:
    // 1. Check cmd[0] (command type)
    // 2. Perform operation on g_data:
    //      GET → find value
    //      SET → insert/update
    //      DEL → erase
    // 3. Fill Response:
    //      out.status
    //      out.data (if GET)

    // Error handling:
    // invalid command → RES_ERR
    if (cmd.size() == 2 && cmd[0] == "get") {
        auto it = g_data.find(cmd[1]);
        if (it == g_data.end()) {
            out.status = RES_NX;    
            return;
        }
        const std::string &val = it->second;
        out.data.assign(val.begin(), val.end());
    } 
    else if (cmd.size() == 3 && cmd[0] == "set") {
        g_data[cmd[1]].swap(cmd[2]);
    } 
    else if (cmd.size() == 2 && cmd[0] == "del") {
        g_data.erase(cmd[1]);
    } 
    else {
        out.status = RES_ERR;       
    }
}

// ===================== RESPONSE BUILD (NEW) =====================

static void make_response(const Response &resp, std::vector<uint8_t> &out) {
    // ===================== TODO =====================
    // Convert Response → binary format

    // Format:
    // [len][status][data]

    // Steps:
    // 1. Compute total length = 4 + data.size()
    // 2. Append length (4 bytes)
    // 3. Append status (4 bytes)
    // 4. Append data
    uint32_t len = 4 + (uint32_t)resp.data.size();
    uint32_t nlen = htonl(len);
    uint32_t nstatus = htonl(resp.status);
    buf_append(out, (const uint8_t *)&nlen, 4);
    buf_append(out, (const uint8_t *)&nstatus, 4);
    buf_append(out, resp.data.data(), resp.data.size());
}

// ===================== PIPELINE (NEW CORE) =====================

static bool try_one_request(Conn *conn) {
    // ===================== TODO =====================
    // Process ONE request from buffer

    // Step 1: Check if at least 4 bytes exist (length header)
    // Step 2: Read message length
    // Step 3: If full message not received → return false
    // Step 4: Extract request pointer
    // Step 5: Parse request → vector<string>
    // Step 6: Call do_request()
    // Step 7: Call make_response()
    // Step 8: Remove processed bytes from buffer

    // Return:
    // true  → processed one request
    // false → need more data
    if(conn->incoming.size()<4) return false;
    uint32_t len;
    memcpy(&len,conn->incoming.data(), 4);
    len=ntohl(len);
    if(len>k_max_msg){
        conn->want_close=true;
        return false;
    }
    if(4+len>conn->incoming.size()){
        return false;
    }
    std::vector<std::string> cmd;
    int32_t rv=parse_req(conn->incoming.data()+4, len, cmd);
    if(rv<0){
        conn->want_close = true;
        return false;
    }
    Response resp;
    do_request(cmd, resp);
    make_response(resp, conn->outgoing);
    buf_consume(conn->incoming, 4+len);
    return true;
}

// ===================== IO =====================

static void handle_write(Conn *conn) {
    ssize_t n = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());

    if (n < 0) {
        if (errno == EAGAIN) return;
        conn->want_close = true;
        return;
    }

    buf_consume(conn->outgoing, n);

    if (conn->outgoing.empty()) {
        conn->want_read = true;
        conn->want_write = false;
    }
}

static void handle_read(Conn *conn) {
    uint8_t buf[65536];

    ssize_t n = read(conn->fd, buf, sizeof(buf));

    if (n < 0) {
        if (errno == EAGAIN) return;
        conn->want_close = true;
        return;
    }

    if (n == 0) {
        conn->want_close = true;
        return;
    }

    buf_append(conn->incoming, buf, n);

    // PIPELINING LOOP
    while (try_one_request(conn)) {}

    if (!conn->outgoing.empty()) {
        conn->want_read = false;
        conn->want_write = true;
        handle_write(conn);
    }
}

// ===================== MAIN =====================

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) die("bind");

    fd_set_nb(fd);

    if (listen(fd, SOMAXCONN) < 0) die("listen");

    std::vector<Conn*> fd2conn;

    while (true) {
        std::vector<pollfd> pfds;

        pfds.push_back({fd, POLLIN, 0});

        for (Conn *c : fd2conn) {
            if (!c) continue;

            short ev = POLLERR;
            if (c->want_read) ev |= POLLIN;
            if (c->want_write) ev |= POLLOUT;

            pfds.push_back({c->fd, ev, 0});
        }

        int rv = poll(pfds.data(), pfds.size(), -1);
        if (rv < 0 && errno == EINTR) continue;
        if (rv < 0) die("poll");

        if (pfds[0].revents) {
            Conn *c = handle_accept(fd);
            if (c) {
                if (fd2conn.size() <= (size_t)c->fd) {
                    fd2conn.resize(c->fd + 1);
                }
                fd2conn[c->fd] = c;
            }
        }

        for (size_t i = 1; i < pfds.size(); i++) {
            Conn *c = fd2conn[pfds[i].fd];
            if (!c) continue;

            if (pfds[i].revents & POLLIN) handle_read(c);
            if (pfds[i].revents & POLLOUT) handle_write(c);

            if ((pfds[i].revents & POLLERR) || c->want_close) {
                close(c->fd);
                fd2conn[c->fd] = NULL;
                delete c;
            }
        }
    }
}