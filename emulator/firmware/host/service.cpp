/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
// The two doors of the firmware host: one poll() loop owns the sockets, one
// mutex-free read of the display model per frame, and request objects shared
// by the web face, the text face and line clients.
#include "service.h"

#include "eve.h"

extern "C"
{
#include "json.h"
}

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

// web/index.html, embedded by build.sh so a deployed binary carries its face.
// It is used when no page file is found next to the binary.
extern unsigned char kWebPage[];
extern unsigned int kWebPage_len;

namespace
{
constexpr int MAX_CLIENTS = 32;
constexpr int IN_MAX = 65536;
constexpr int OUT_MAX = 1 << 20;
constexpr int kWidth = EveDisplay::kWidth;
constexpr int kHeight = EveDisplay::kHeight;

enum Kind
{
    CL_LINE = 0,
    CL_HTTP,
    CL_SSE
};

struct Client
{
    int fd = -1;
    int kind = CL_HTTP;
    std::string in;
    std::string out;
    size_t sent = 0;
    bool closeAfter = false;
    bool headerDone = false;
    size_t bodyOff = 0;
    long bodyLen = 0;
    std::string method;
    std::string path;
    uint32_t lastSeq = 0;
    // A "watch" client gets a frame whenever the display changes, instead of
    // only after a request.
    bool watch = false;
};

EveDisplay* gDisplay = nullptr;
InputSink* gInput = nullptr;
std::string gPagePath;
int gLineFd = -1;
int gHttpFd = -1;
volatile sig_atomic_t gStop = 0;
std::thread gThread;
Client gClients[MAX_CLIENTS];

void json_escape(std::string& o, const std::string& s)
{
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                }
                else o += (char)c;
                break;
        }
    }
}

std::string hexColor(uint32_t c)
{
    char b[8];
    snprintf(b, sizeof b, "#%06x", c & 0xFFFFFF);
    return b;
}

std::string frame_json()
{
    std::string o;
    EveFrame f;
    if (gDisplay == nullptr || !gDisplay->copyFrame(f))
    {
        o = "{\"t\":\"frame\",\"seq\":0,\"w\":800,\"h\":480,\"ops\":[],\"sprites\":[]}";
        return o;
    }

    char b[256];
    snprintf(b, sizeof b, "{\"t\":\"frame\",\"seq\":%u,\"w\":%d,\"h\":%d,\"sprites\":[",
             f.seq, kWidth, kHeight);
    o += b;
    for (size_t i = 0; i < f.sprite_meta.size(); i++)
    {
        const EveSprite& sp = f.sprite_meta[i];
        snprintf(b, sizeof b, "%s{\"id\":%u,\"w\":%u,\"h\":%u,\"stride\":%u,\"bpp\":%u,\"fmt\":%u}",
                 i ? "," : "", sp.id, sp.w, sp.h, sp.stride, sp.bpp, sp.format);
        o += b;
    }
    o += "],\"ops\":[";

    for (size_t i = 0; i < f.ops.size(); i++)
    {
        const EveOp& op = f.ops[i];
        if (i) o += ',';
        switch (op.kind)
        {
            case EveOp::kClear:
                o += "{\"op\":\"clear\",\"color\":\"" + hexColor(op.color) + "\"";
                snprintf(b, sizeof b, ",\"a\":%u}", op.alpha);
                o += b;
                break;
            case EveOp::kScissor:
                snprintf(b, sizeof b,
                         "{\"op\":\"scissor\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", op.x, op.y,
                         op.w, op.h);
                o += b;
                break;
            case EveOp::kRect:
                snprintf(b, sizeof b, "{\"op\":\"rect\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                                      "\"color\":\"#%06x\",\"a\":%u,\"blend\":%u}",
                         op.x, op.y, op.w, op.h, op.color & 0xFFFFFF, op.alpha, op.blend);
                o += b;
                break;
            case EveOp::kLine:
                snprintf(b, sizeof b, "{\"op\":\"line\",\"x\":%d,\"y\":%d,\"x2\":%d,\"y2\":%d,"
                                      "\"color\":\"#%06x\",\"w\":%u,\"a\":%u,\"blend\":%u}",
                         op.x, op.y, op.w, op.h, op.color & 0xFFFFFF, op.size, op.alpha,
                         op.blend);
                o += b;
                break;
            case EveOp::kPoint:
                snprintf(b, sizeof b, "{\"op\":\"point\",\"x\":%d,\"y\":%d,\"color\":\"#%06x\","
                                      "\"d\":%u,\"a\":%u,\"blend\":%u}",
                         op.x, op.y, op.color & 0xFFFFFF, op.size, op.alpha, op.blend);
                o += b;
                break;
            case EveOp::kText:
            {
                snprintf(b, sizeof b,
                         "{\"op\":\"text\",\"x\":%d,\"y\":%d,\"px\":%u,\"color\":\"#%06x\","
                         "\"a\":%u,\"blend\":%u,\"s\":\"",
                         op.x, op.y, op.font_px, op.color & 0xFFFFFF, op.alpha, op.blend);
                o += b;
                json_escape(o, op.text);
                o += "\",\"xs\":[";
                for (size_t k = 0; k < op.xs.size(); k++)
                {
                    snprintf(b, sizeof b, "%s%u", k ? "," : "", op.xs[k]);
                    o += b;
                }
                o += "]}";
                break;
            }
            case EveOp::kIcon:
                snprintf(b, sizeof b, "{\"op\":\"icon\",\"x\":%d,\"y\":%d,\"sprite\":%u,"
                                      "\"color\":\"#%06x\",\"a\":%u,\"blend\":%u}",
                         op.x, op.y, op.sprite, op.color & 0xFFFFFF, op.alpha, op.blend);
                o += b;
                break;
            case EveOp::kGradient:
                snprintf(b, sizeof b, "{\"op\":\"gradient\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                                      "\"color\":\"#%06x\",\"color2\":\"#%06x\","
                                      "\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d,"
                                      "\"a\":%u,\"blend\":%u}",
                         op.x, op.y, op.w, op.h, op.color & 0xFFFFFF, op.color2 & 0xFFFFFF,
                         op.gx0, op.gy0, op.gx1, op.gy1, op.alpha, op.blend);
                o += b;
                break;
        }
    }
    o += "]}";
    return o;
}

uint32_t current_seq();

// The text face as JSON, for line clients that want lines rather than a draw
// list: {"t":"text","seq":N,"cols":80,"rows":24,"lines":["...", ...]}.
std::string text_json(int cols, int rows)
{
    static char text[201 * 101];
    service_frame_text(gDisplay, text, sizeof text, cols, rows);
    char b[96];
    snprintf(b, sizeof b, "{\"t\":\"text\",\"seq\":%u,\"cols\":%d,\"rows\":%d,\"lines\":[",
             current_seq(), cols, rows);
    std::string o = b;
    const char* p = text;
    bool first = true;
    while (*p)
    {
        const char* e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (!first) o += ',';
        o += '"';
        json_escape(o, std::string(p, n));
        o += '"';
        first = false;
        if (!e) break;
        p = e + 1;
    }
    o += "]}";
    return o;
}

std::string sprite_blob(uint32_t id)
{
    EveSprite sp;
    if (gDisplay == nullptr || !gDisplay->getSprite(id, sp)) return std::string();
    std::string s;
    s.resize(8);
    s[0] = (char)(sp.w & 0xFF);
    s[1] = (char)(sp.w >> 8);
    s[2] = (char)(sp.h & 0xFF);
    s[3] = (char)(sp.h >> 8);
    s[4] = (char)(sp.stride & 0xFF);
    s[5] = (char)(sp.stride >> 8);
    s[6] = (char)sp.bpp;
    s[7] = (char)sp.format;
    s.append((const char*)sp.data.data(), sp.data.size());
    return s;
}

// --- clients --------------------------------------------------------------

void client_drop(Client& c)
{
    if (c.fd >= 0) close(c.fd);
    c.fd = -1;
    c.in.clear();
    c.out.clear();
    c.sent = 0;
    c.kind = CL_HTTP;
    c.closeAfter = false;
    c.headerDone = false;
    c.bodyOff = 0;
    c.bodyLen = 0;
    c.method.clear();
    c.path.clear();
    c.lastSeq = 0;
    c.watch = false;
}

bool push(Client& c, const std::string& s)
{
    if (c.out.size() + s.size() > (size_t)OUT_MAX) return false;
    c.out += s;
    return true;
}

bool push_line(Client& c, const std::string& s)
{
    if (!push(c, s)) return false;
    c.out += '\n';
    return true;
}

void reply(Client& c, const char* status, const char* ctype, const std::string& body)
{
    char head[512];
    snprintf(head, sizeof head,
             "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
             "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
             status, ctype, body.size());
    push(c, head);
    push(c, body);
    c.closeAfter = true;
}

void reply_file(Client& c, const char* path, const char* ctype)
{
    FILE* f = fopen(path, "rb");
    if (f == nullptr)
    {
        reply(c, "404 Not Found", "text/plain", "missing\n");
        return;
    }
    std::string body;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, n);
    fclose(f);
    reply(c, "200 OK", ctype, body);
}

void sse_start(Client& c)
{
    push(c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\nConnection: close\r\n\r\n");
    c.kind = CL_SSE;
    c.closeAfter = false;
    push(c, "data: " + frame_json() + "\n\n");
}

int query_int(const std::string& path, const char* key, int dflt)
{
    std::string needle = std::string(key) + "=";
    size_t p = path.find(needle);
    if (p == std::string::npos) return dflt;
    return atoi(path.c_str() + p + needle.size());
}

void handle_request(const std::string& body)
{
    struct json* j = json_parse(body.c_str());
    if (j == nullptr || gInput == nullptr)
    {
        if (j) json_free(j);
        return;
    }
    const char* t = json_str(j, "t", "");
    const char* action = json_str(j, "action", json_int(j, "down", 0) ? "press" : "release");
    bool down = !strcmp(action, "press") || !strcmp(action, "down");
    if (!strcmp(t, "button")) gInput->button(json_int(j, "button", json_int(j, "n", -1)), down);
    else if (!strcmp(t, "pad")) gInput->pad(json_int(j, "pad", json_int(j, "n", -1)), down);
    else if (!strcmp(t, "encoder")) gInput->encoder(json_int(j, "delta", json_int(j, "value", 0)));
    else if (!strcmp(t, "release-all")) gInput->releaseAll();
    else if (!strcmp(t, "restart")) gInput->restart();
    json_free(j);
}

void http_handle(Client& c)
{
    if (c.method == "GET" && (c.path == "/" || c.path == "/index.html"))
    {
        FILE* f = gPagePath.empty() ? nullptr : fopen(gPagePath.c_str(), "rb");
        if (f != nullptr) fclose(f);
        if (f == nullptr)
        {
            // Deployed alone (e.g. in /tmp): serve the embedded copy.
            reply(c, "200 OK", "text/html; charset=utf-8",
                  std::string((const char*)kWebPage, kWebPage_len));
            return;
        }
        reply_file(c, gPagePath.c_str(), "text/html; charset=utf-8");
        return;
    }
    if (c.method == "GET" && (c.path == "/state" || c.path == "/frame"))
    {
        reply(c, "200 OK", "application/json", frame_json());
        return;
    }
    if (c.method == "GET" && c.path.rfind("/sprite", 0) == 0)
    {
        int id = query_int(c.path, "id", 0);
        std::string blob = sprite_blob((uint32_t)id);
        if (blob.empty())
        {
            reply(c, "404 Not Found", "application/json", "{\"error\":\"no sprite\"}");
            return;
        }
        reply(c, "200 OK", "application/octet-stream", blob);
        return;
    }
    // Prefix, like /sprite and /input: the query string is still in c.path.
    if (c.method == "GET" && c.path.rfind("/text", 0) == 0)
    {
        // The face is a character grid over the 800x480 screen: 80x24 cells is
        // the firmware's 10 px glyph pitch and roughly its line count.
        int cols = query_int(c.path, "cols", 80);
        int rows = query_int(c.path, "rows", 24);
        if (cols < 1 || cols > 200) cols = 80;
        if (rows < 1 || rows > 100) rows = 24;
        static char text[201 * 101];
        service_frame_text(gDisplay, text, sizeof text, cols, rows);
        reply(c, "200 OK", "text/plain; charset=utf-8", text);
        return;
    }
    if (c.method == "GET" && c.path == "/health")
    {
        reply(c, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (c.method == "GET" && c.path.rfind("/input?", 0) == 0)
    {
        // Convenience for curl and the text face: /input?button=23&down=1
        int button = query_int(c.path, "button", -1);
        int pad = query_int(c.path, "pad", -1);
        int delta = query_int(c.path, "encoder", 0);
        int down = query_int(c.path, "down", 0);
        if (gInput)
        {
            if (button >= 0) gInput->button(button, down != 0);
            if (pad >= 0) gInput->pad(pad, down != 0);
            if (delta) gInput->encoder(delta);
        }
        reply(c, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (c.method == "GET" && c.path == "/events")
    {
        sse_start(c);
        return;
    }
    if (c.method == "POST" && c.path == "/req")
    {
        std::string body = c.in.substr(c.bodyOff, (size_t)c.bodyLen);
        handle_request(body);
        reply(c, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    reply(c, "404 Not Found", "application/json", "{\"error\":\"not found\"}");
}

bool header_value(const std::string& headers, const char* name, char* out, size_t outlen)
{
    size_t n = strlen(name);
    const char* p = headers.c_str();
    while (*p)
    {
        const char* e = strstr(p, "\r\n");
        if (!e) e = p + strlen(p);
        if ((size_t)(e - p) > n && !strncasecmp(p, name, n) && p[n] == ':')
        {
            const char* v = p + n + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t m = (size_t)(e - v);
            if (m >= outlen) m = outlen - 1;
            memcpy(out, v, m);
            out[m] = 0;
            return true;
        }
        if (!*e) break;
        p = e + 2;
    }
    return false;
}

void http_read(Client& c)
{
    if (!c.headerDone)
    {
        size_t end = c.in.find("\r\n\r\n");
        if (end == std::string::npos) return;
        c.headerDone = true;
        c.bodyOff = end + 4;
        // The request line is before the header terminator, not after it.
        size_t sp = c.in.find(' ');
        if (sp == std::string::npos || sp > end) return;
        c.method = c.in.substr(0, sp);
        size_t p2 = sp + 1;
        size_t sp2 = c.in.find(' ', p2);
        if (sp2 == std::string::npos || sp2 > end) return;
        c.path = c.in.substr(p2, sp2 - p2);
        char clen[32];
        c.bodyLen = 0;
        if (header_value(c.in.substr(0, end), "Content-Length", clen, sizeof clen))
            c.bodyLen = strtol(clen, nullptr, 10);
        if (c.bodyLen < 0) c.bodyLen = 0;
    }
    if (c.closeAfter || c.in.size() < c.bodyOff + (size_t)c.bodyLen) return;
    http_handle(c);
}

void line_read(Client& c)
{
    for (;;)
    {
        size_t nl = c.in.find('\n');
        if (nl == std::string::npos) break;
        std::string line = c.in.substr(0, nl);
        c.in.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        struct json* j = json_parse(line.c_str());
        const char* t = j ? json_str(j, "t", "") : "";
        if (!strcmp(t, "hello"))
        {
            char host[128] = "";
            gethostname(host, sizeof host);
            push_line(c, std::string("{\"t\":\"hello\",\"role\":\"firmware\",\"host\":\"") + host +
                           "\"}");
            push_line(c, frame_json());
        }
        else if (!strcmp(t, "text"))
        {
            // A line client that wants the text face rather than the draw
            // list; asking is cheaper than shipping 50 KB of ops per key.
            push_line(c, text_json(80, 24));
        }
        else if (!strcmp(t, "watch"))
        {
            // Keep the screen live: a native panel asks once and then only
            // listens, the way /events behaves for the web face.
            c.watch = json_int(j, "on", 1) != 0;
            if (c.watch) push_line(c, frame_json());
        }
        else
        {
            handle_request(line);
            // A watching client already receives the frame from the display
            // broadcast; only one-off clients need the answer inline.
            if (!c.watch) push_line(c, frame_json());
        }
        if (j) json_free(j);
    }
    if (c.in.size() > IN_MAX) c.in.clear();
}

int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    // TRACKER_BIND=127.0.0.1 keeps the doors off the network, which is what
    // the Android wrapper wants: only the app on the same device talks to them.
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    const char* bindAddr = getenv("TRACKER_BIND");
    if (bindAddr != nullptr && *bindAddr) inet_pton(AF_INET, bindAddr, &a.sin_addr);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&a, sizeof a) != 0 || listen(fd, 16) != 0)
    {
        close(fd);
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

void accept_client(int lfd, int kind)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int fd = accept(lfd, (struct sockaddr*)&a, &al);
    if (fd >= 0) fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
    if (fd < 0) return;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    for (auto& c : gClients)
    {
        if (c.fd >= 0) continue;
        client_drop(c);
        c.fd = fd;
        c.kind = kind;
        c.in.reserve(IN_MAX);
        c.out.reserve(OUT_MAX / 4);
        if (kind == CL_LINE)
        {
            char host[128] = "";
            gethostname(host, sizeof host);
            push_line(c, std::string("{\"t\":\"hello\",\"role\":\"firmware\",\"host\":\"") + host +
                           "\"}");
            push_line(c, frame_json());
        }
        return;
    }
    close(fd);
}

void client_flush(Client& c)
{
    while (c.sent < c.out.size())
    {
        ssize_t n = send(c.fd, c.out.data() + c.sent, c.out.size() - c.sent, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            client_drop(c);
            return;
        }
        if (n == 0) return;
        c.sent += (size_t)n;
    }
    c.out.clear();
    c.sent = 0;
    if (c.closeAfter && c.kind != CL_SSE) client_drop(c);
}

void broadcast_sse(uint32_t seq)
{
    std::string frame = frame_json();
    std::string watched;
    for (auto& c : gClients)
    {
        if (c.fd < 0) continue;
        if (c.kind == CL_SSE)
        {
            if (c.lastSeq == seq) continue;
            c.lastSeq = seq;
            if (!push(c, "data: " + frame + "\n\n")) client_drop(c);
        }
        else if (c.kind == CL_LINE && c.watch)
        {
            if (c.lastSeq == seq) continue;
            c.lastSeq = seq;
            if (watched.empty()) watched = frame;
            if (!push_line(c, watched)) client_drop(c);
        }
    }
}

uint32_t current_seq()
{
    EveFrame f;
    if (gDisplay == nullptr || !gDisplay->copyFrame(f)) return 0;
    return f.seq;
}

void on_signal(int)
{
    gStop = 1;
}

void server_main()
{
    signal(SIGPIPE, SIG_IGN);
    double nextBroadcast = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    auto now_s = [&]() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)(ts.tv_sec - start.tv_sec) + (ts.tv_nsec - start.tv_nsec) / 1e9;
    };

    while (!gStop)
    {
        struct pollfd pfd[MAX_CLIENTS + 2];
        int idx[MAX_CLIENTS];
        int n = 0;
        pfd[n].fd = gLineFd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        int li = n++;
        pfd[n].fd = gHttpFd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        int hi = n++;
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (gClients[i].fd < 0) continue;
            idx[n - 2] = i;
            pfd[n].fd = gClients[i].fd;
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            n++;
        }
        int pr = poll(pfd, (nfds_t)n, 5);
        if (pr > 0)
        {
            if (pfd[li].revents & POLLIN) accept_client(gLineFd, CL_LINE);
            if (pfd[hi].revents & POLLIN) accept_client(gHttpFd, CL_HTTP);
            for (int k = 2; k < n; k++)
            {
                Client& c = gClients[idx[k - 2]];
                if (c.fd < 0) continue;
                if (!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                char buf[8192];
                ssize_t r = recv(c.fd, buf, sizeof buf, 0);
                if (r <= 0)
                {
                    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    if (r == 0 && c.kind == CL_SSE) client_drop(c);
                    else if (r == 0) client_drop(c);
                    continue;
                }
                if (c.in.size() + (size_t)r > (size_t)IN_MAX) client_drop(c);
                else c.in.append(buf, (size_t)r);
            }
        }
        for (auto& c : gClients)
        {
            if (c.fd < 0) continue;
            if (c.kind == CL_LINE) line_read(c);
            else if (c.kind == CL_HTTP) http_read(c);
        }
        double t = now_s();
        if (t >= nextBroadcast)
        {
            nextBroadcast = t + 0.1;
            broadcast_sse(current_seq());
        }
        for (auto& c : gClients)
            if (c.fd >= 0) client_flush(c);
    }
    for (auto& c : gClients) client_drop(c);
}
} // namespace

bool service_start(EveDisplay* display, InputSink* input, const ServiceOptions& opts)
{
    gDisplay = display;
    gInput = input;
    gStop = 0;

    if (opts.page && *opts.page) gPagePath = opts.page;
    else if (const char* env = getenv("TRACKER_PAGE")) gPagePath = env;
    if (gPagePath.empty())
    {
        const char* candidates[] = {"../web/index.html", "web/index.html",
                                    "emulator/firmware/web/index.html"};
        for (const char* p : candidates)
        {
            struct stat st;
            if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
            {
                gPagePath = p;
                break;
            }
        }
    }

    for (auto& c : gClients) c.fd = -1;
    gLineFd = listen_on(opts.linePort);
    gHttpFd = listen_on(opts.httpPort);
    if (gLineFd < 0 || gHttpFd < 0)
    {
        fprintf(stderr, "firmware host: cannot bind ports %d/%d: %s\n", opts.httpPort,
                opts.linePort, strerror(errno));
        if (gLineFd >= 0) close(gLineFd);
        if (gHttpFd >= 0) close(gHttpFd);
        return false;
    }
    printf("panel   http://0.0.0.0:%d/\n", opts.httpPort);
    printf("door    0.0.0.0:%d\n", opts.linePort);
    printf("page    %s\n", gPagePath.c_str());
    fflush(stdout);
    gThread = std::thread(server_main);
    return true;
}

void service_stop()
{
    gStop = 1;
    if (gThread.joinable()) gThread.join();
    if (gLineFd >= 0) close(gLineFd);
    if (gHttpFd >= 0) close(gHttpFd);
    gLineFd = gHttpFd = -1;
}

void service_frame_text(EveDisplay* display, char* out, int outSize, int cols, int rows)
{
    if (out == nullptr || outSize <= 0) return;
    out[0] = 0;
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 24;
    EveFrame f;
    if (display == nullptr || !display->copyFrame(f)) return;

    // Group the text runs into the firmware's visual lines.  Runs are placed
    // by their start, not per glyph, so words stay intact despite the two
    // glyph pitches.  Draw order inside a line is kept, so a label drawn over
    // placeholder cells wins.
    struct Line
    {
        int y;
        std::vector<size_t> ops;
    };
    std::vector<Line> lines;
    for (size_t i = 0; i < f.ops.size(); i++)
    {
        const EveOp& op = f.ops[i];
        if (op.kind != EveOp::kText || op.text.empty()) continue;
        Line* line = nullptr;
        for (auto& ln : lines)
        {
            // A digit can sit two pixels below the run beside it; 6 px merges
            // those while keeping the 26 px row pitch apart.
            int d = op.y - ln.y;
            if (d <= 6 && d >= -6)
            {
                line = &ln;
                break;
            }
        }
        if (line == nullptr)
        {
            lines.push_back({op.y, {}});
            line = &lines.back();
        }
        line->ops.push_back(i);
    }
    std::sort(lines.begin(), lines.end(),
              [](const Line& a, const Line& b) { return a.y < b.y; });

    const float cell = (float)kWidth / (float)cols;
    std::vector<std::string> outRows((size_t)rows);
    for (size_t li = 0; li < lines.size(); li++)
    {
        std::string row((size_t)cols, ' ');
        for (size_t oi : lines[li].ops)
        {
            const EveOp& op = f.ops[oi];
            int start = (int)(op.x / cell + 0.5f);
            for (size_t i = 0; i < op.text.size(); i++)
            {
                int x = start + (int)i;
                if (x < 0 || x >= cols) continue;
                // The firmware uses two private glyph codes for the clock
                // separators; a text face has no room for them.
                unsigned char ch = (unsigned char)op.text[i];
                row[(size_t)x] = (ch < 32 || ch > 126) ? ' ' : (char)ch;
            }
        }
        while (!row.empty() && row.back() == ' ') row.pop_back();
        size_t r = lines.size() <= (size_t)rows ? li : li * (size_t)rows / lines.size();
        outRows[r] = row;
    }

    size_t last = outRows.size();
    while (last > 0 && outRows[last - 1].empty()) last--;
    size_t p = 0;
    for (size_t y = 0; y < last; y++)
    {
        for (char ch : outRows[y])
        {
            if (p + 2 >= (size_t)outSize) break;
            out[p++] = ch;
        }
        if (p + 2 >= (size_t)outSize) break;
        out[p++] = '\n';
    }
    out[p] = 0;
}
