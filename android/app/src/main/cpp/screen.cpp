/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */

#include "screen.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C"
{
// The panel's JSON reader: the door protocol has nested objects and a field
// scan would read the wrong one.
#include "json.h"
}

namespace
{
constexpr int kDoorPort = 8895;
constexpr int kHttpPort = 8894;
constexpr size_t kMaxLine = 1 << 20;

uint32_t parseColor(const char* s)
{
    if (s == nullptr || *s != '#') return 0xFFFFFF;
    return (uint32_t)strtoul(s + 1, nullptr, 16) & 0xFFFFFF;
}

FrameOpKind kindOf(const char* op)
{
    if (!strcmp(op, "clear")) return OP_CLEAR;
    if (!strcmp(op, "scissor")) return OP_SCISSOR;
    if (!strcmp(op, "rect")) return OP_RECT;
    if (!strcmp(op, "line")) return OP_LINE;
    if (!strcmp(op, "point")) return OP_POINT;
    if (!strcmp(op, "text")) return OP_TEXT;
    if (!strcmp(op, "icon")) return OP_ICON;
    return OP_GRADIENT;
}

void parseFrame(const struct json* j, Frame& f)
{
    f.ops.clear();
    f.sprites.clear();
    f.seq = (uint32_t)json_int(j, "seq", 0);

    const struct json* sprites = json_get(j, "sprites");
    for (size_t i = 0; i < json_count(sprites); i++)
    {
        const struct json* s = json_at(sprites, i);
        SpriteMeta m;
        m.id = json_int(s, "id", 0);
        m.w = json_int(s, "w", 0);
        m.h = json_int(s, "h", 0);
        m.stride = json_int(s, "stride", 0);
        m.bpp = json_int(s, "bpp", 0);
        m.fmt = json_int(s, "fmt", 0);
        f.sprites.push_back(m);
    }

    const struct json* ops = json_get(j, "ops");
    for (size_t i = 0; i < json_count(ops); i++)
    {
        const struct json* o = json_at(ops, i);
        FrameOp op;
        op.kind = kindOf(json_str(o, "op", ""));
        op.x = json_int(o, "x", 0);
        op.y = json_int(o, "y", 0);
        op.w = json_int(o, "w", 0);
        op.h = json_int(o, "h", 0);
        op.x2 = json_int(o, "x2", 0);
        op.y2 = json_int(o, "y2", 0);
        op.px = json_int(o, "px", 16);
        op.sprite = json_int(o, "sprite", 0);
        op.size = json_int(o, "d", json_int(o, "w", 1));
        op.gx0 = json_int(o, "x0", 0);
        op.gy0 = json_int(o, "y0", 0);
        op.gx1 = json_int(o, "x1", 0);
        op.gy1 = json_int(o, "y1", 0);
        op.color = parseColor(json_str(o, "color", "#ffffff"));
        op.color2 = parseColor(json_str(o, "color2", "#ffffff"));
        op.alpha = (unsigned char)json_int(o, "a", 255);
        op.blend = (unsigned char)json_int(o, "blend", 0);
        if (op.kind == OP_TEXT)
        {
            op.text = json_str(o, "s", "");
            int xs[1024];
            int n = json_numbers(o, "xs", xs, 1024);
            op.xs.assign(xs, xs + n);
        }
        f.ops.push_back(std::move(op));
    }
}
} // namespace

bool fetchSprite(int id, SpriteImage& out)
{
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(kHttpPort);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr*)&a, sizeof a) != 0)
    {
        close(fd);
        return false;
    }
    char req[128];
    int n = snprintf(req, sizeof req, "GET /sprite?id=%d HTTP/1.0\r\n\r\n", id);
    if (write(fd, req, (size_t)n) != n)
    {
        close(fd);
        return false;
    }
    std::string body;
    char buf[8192];
    for (;;)
    {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r <= 0) break;
        body.append(buf, (size_t)r);
        if (body.size() > (1u << 20)) break;
    }
    close(fd);

    size_t hdr = body.find("\r\n\r\n");
    if (hdr == std::string::npos) return false;
    if (body.compare(0, 12, "HTTP/1.1 200") != 0) return false;
    const unsigned char* p = (const unsigned char*)body.data() + hdr + 4;
    size_t len = body.size() - hdr - 4;
    if (len < 8) return false;
    out.w = p[0] | (p[1] << 8);
    out.h = p[2] | (p[3] << 8);
    out.stride = p[4] | (p[5] << 8);
    out.bpp = p[6];
    out.fmt = p[7];
    out.data.assign(p + 8, p + len);
    return true;
}

bool ControlLink::connectDoor()
{
    close();
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(kDoorPort);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr*)&a, sizeof a) != 0)
    {
        ::close(fd);
        return false;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fd_ = fd;
    in_.clear();
    // One request: from here on the door pushes a frame whenever the display
    // changes, which is what keeps the panel live without polling.
    static const char kWatcher[] = "{\"t\":\"watch\",\"on\":1}\n";
    (void)write(fd_, kWatcher, sizeof kWatcher - 1);
    return true;
}

void ControlLink::close()
{
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    in_.clear();
    lastSeq_ = 0;
    haveSeq_ = false;
}

bool ControlLink::pollFrame(Frame& out)
{
    if (fd_ < 0) return false;
    char buf[65536];
    for (;;)
    {
        ssize_t r = ::read(fd_, buf, sizeof buf);
        if (r > 0)
        {
            in_.append(buf, (size_t)r);
            if (in_.size() > kMaxLine) in_.clear();
            continue;
        }
        // The child went away: drop the connection so the panel reconnects
        // when the next child's door is up.  EAGAIN is just "nothing more".
        if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) close();
        break;
    }

    bool got = false;
    for (;;)
    {
        size_t nl = in_.find('\n');
        if (nl == std::string::npos) break;
        std::string line = in_.substr(0, nl);
        in_.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        struct json* j = json_parse(line.c_str());
        if (j == nullptr) continue;
        if (!strcmp(json_str(j, "t", ""), "frame"))
        {
            Frame f;
            parseFrame(j, f);
            // A frame is only interesting when it is newer than the one on
            // screen; the door may repeat one after a request.
            if (!haveSeq_ || f.seq != lastSeq_)
            {
                lastSeq_ = f.seq;
                haveSeq_ = true;
                out = std::move(f);
                got = true;
            }
        }
        json_free(j);
    }
    return got;
}

void ControlLink::sendButton(int id, bool down)
{
    if (fd_ < 0) return;
    char line[96];
    int n = snprintf(line, sizeof line, "{\"t\":\"button\",\"button\":%d,\"down\":%d}\n", id,
                     down ? 1 : 0);
    ssize_t w = ::write(fd_, line, (size_t)n);
    (void)w; // a dropped press while the door is busy is not worth a queue
}
