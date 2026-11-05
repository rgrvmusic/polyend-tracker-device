/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */

// The Android panel: SDL3 + Dear ImGui over the firmware child.
//
// The firmware emits its display as a command list, not pixels, so the panel
// reads that list from the line door and draws it here at native speed.
// Audio, MIDI and the frame stream all arrive over descriptors.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>

#include <GLES3/gl3.h>

#include <jni.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h"

#include "child.h"
#include "midi_bridge.h"
#include "panel.h"
#include "render.h"
#include "screen.h"

namespace
{
constexpr int kRate = 44100;
constexpr int kScreenW = 800;
constexpr int kScreenH = 480;

// UI scale in pixels per dp. SDL reports content scale 1.0 before the Android
// display density arrives, so Java passes the real density instead.
std::atomic<float> gUiScale{1.0f};

std::atomic<bool> gContextLost{false};

// Reads below a target and never drops samples, so the socket stays full and
// the firmware's blocking writes pace it.
class AudioPipe
{
  public:
    void start(int fd, int rate)
    {
        stop();
        fd_ = fd;
        rate_ = rate;
        cap_ = (size_t)rate * 4;          // one second, stereo S16
        target_ = (size_t)rate * 4 / 10;  // keep about a tenth of a second
        buf_.assign(cap_, 0);
        r_ = w_ = n_ = 0;
        run_ = true;
        th_ = std::thread([this] { reader(); });
    }

    void stop()
    {
        if (run_.exchange(false))
        {
            // The reader owns the descriptor; shutting it down wakes its read.
            if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
            cv_.notify_all();
            if (th_.joinable()) th_.join();
        }
        else if (th_.joinable())
        {
            th_.join();
        }
        fd_ = -1;
    }

    ~AudioPipe() { stop(); }

    // SDL's get-callback: hand the device what there is, and silence when the
    // child has not produced enough.  Never blocks.
    void pull(SDL_AudioStream* stream, int want)
    {
        if (want <= 0) return;
        if ((size_t)want > scratch_.size()) want = (int)scratch_.size();
        size_t got;
        {
            std::lock_guard<std::mutex> lock(mu_);
            got = take(scratch_.data(), (size_t)want);
            cv_.notify_one();
        }
        if (got < (size_t)want) memset(scratch_.data() + got, 0, (size_t)want - got);
        SDL_PutAudioStreamData(stream, scratch_.data(), want);
    }

  private:
    void reader()
    {
        const int fd = fd_;
        std::vector<uint8_t> tmp(8192);
        for (;;)
        {
            size_t want;
            {
                std::unique_lock<std::mutex> lock(mu_);
                // Full ring means stop reading, not make room: the socket then
                // fills and the firmware's blocking write paces its thread.
                while (run_ && n_ > target_)
                    cv_.wait_for(lock, std::chrono::milliseconds(5));
                if (!run_) break;
                want = ImMin(cap_ - n_, tmp.size());
            }
            const ssize_t got = ::read(fd, tmp.data(), want);
            if (got < 0)
            {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                break;
            }
            if (got == 0) break; // the child closed its end
            {
                std::lock_guard<std::mutex> lock(mu_);
                for (ssize_t i = 0; i < got; i++)
                {
                    buf_[w_] = tmp[(size_t)i];
                    w_ = (w_ + 1) % cap_;
                }
                n_ += (size_t)got;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            r_ = w_ = n_ = 0;
            cv_.notify_all();
        }
        // childStop() closes the descriptor; closing it here would trip fdsan.
    }

    size_t take(uint8_t* out, size_t want)
    {
        const size_t got = ImMin(want, n_);
        for (size_t i = 0; i < got; i++)
        {
            out[i] = buf_[r_];
            r_ = (r_ + 1) % cap_;
        }
        n_ -= got;
        return got;
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<uint8_t> buf_;
    size_t cap_ = 0, target_ = 0, r_ = 0, w_ = 0, n_ = 0;
    std::atomic<bool> run_{false};
    int fd_ = -1;
    int rate_ = kRate;
    std::thread th_;
    std::vector<uint8_t> scratch_ = std::vector<uint8_t>(65536);
};

struct App
{
    ControlLink link;
    Frame frame;
    bool haveFrame = false;
    Child child;
    AudioPipe audio;
    SpriteCache sprites;
    PanelLayout layout;
    bool haveLayout = false;
    float layoutW = 0, layoutH = 0, layoutTop = -1;
    ImFont* font = nullptr;
    float scale = 1.0f;       // the display's UI scale
    PanelRect bannerMark;     // the top bar's control, for hit testing
    bool bannerDown = false;
    std::unordered_map<SDL_FingerID, int> fingers;
    std::vector<int> held;
    int mouseButton = -1;
    bool dirty = true;
    bool fullRedraw = true;
    Uint64 nextConnect = 0;
    Uint64 nextWatchdog = 0;
    Uint64 nextRestart = 0;
    std::string binary, sd, log;
};

// -2, so it cannot collide with a firmware switch id; the top bar's control is
// not a button the firmware knows about.
constexpr int kBanner = -2;

void SDLCALL audioPull(void* userdata, SDL_AudioStream* stream, int additional, int total)
{
    (void)total;
    static_cast<AudioPipe*>(userdata)->pull(stream, additional);
}

bool lifecycleWatch(void*, SDL_Event* e)
{
    if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) gContextLost = true;
    return true;
}

// ---- buttons -------------------------------------------------------------

// The top bar's control asks Java for the UserFS screen. The call is by name
// and not through a bound method, so no Java object is kept across a rotation.
void openUserFs()
{
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr) return;
    jclass cls = env->FindClass("nl/rgrv/ptpocket/MainActivity");
    if (cls == nullptr)
    {
        env->ExceptionClear();
        return;
    }
    jmethodID open = env->GetStaticMethodID(cls, "openUserFsScreen", "()V");
    if (open != nullptr) env->CallStaticVoidMethod(cls, open);
    // A screen that cannot open must not take the frame loop down with it.
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
}

void sendDown(App& app, int id)
{
    for (int held : app.held)
        if (held == id) return;
    app.held.push_back(id);
    app.link.sendButton(id, true);
    app.dirty = true;
}

void releaseIfIdle(App& app, int id)
{
    for (const auto& f : app.fingers)
        if (f.second == id) return;
    if (app.mouseButton == id) return;
    for (size_t i = 0; i < app.held.size(); i++)
        if (app.held[i] == id)
        {
            app.held.erase(app.held.begin() + (long)i);
            app.link.sendButton(id, false);
            app.dirty = true;
            return;
        }
}

void releaseBanner(App& app)
{
    if (!app.bannerDown) return;
    app.bannerDown = false;
    app.dirty = true;
    openUserFs();
}

void fingerDown(App& app, SDL_FingerID finger, ImVec2 pos)
{
    if (app.bannerMark.contains(pos))
    {
        app.fingers[finger] = kBanner;
        app.bannerDown = true;
        app.dirty = true;
        return;
    }
    const int id = app.haveLayout ? hitTest(app.layout, pos) : -1;
    app.fingers[finger] = id;
    if (id >= 0) sendDown(app, id);
}

void fingerUp(App& app, SDL_FingerID finger)
{
    auto it = app.fingers.find(finger);
    if (it == app.fingers.end()) return;
    const int id = it->second;
    app.fingers.erase(it);
    if (id == kBanner) releaseBanner(app);
    else if (id >= 0) releaseIfIdle(app, id);
}

void releaseAll(App& app)
{
    for (int id : app.held) app.link.sendButton(id, false);
    app.held.clear();
    app.fingers.clear();
    app.mouseButton = -1;
    app.bannerDown = false;
    app.dirty = true;
}

// ---- child ---------------------------------------------------------------

void startChild(App& app)
{
    app.audio.stop();
    app.link.close();
    midiBridgeSetFd(-1);
    childStop(app.child);
    if (!childStart(app.binary, app.sd, app.log, app.child))
    {
        SDL_Log("cannot start the firmware child");
        return;
    }
    SDL_Log("firmware child pid %d (audio fd %d, midi fd %d)", (int)app.child.pid,
            app.child.audioFd, app.child.midiFd);
    app.audio.start(app.child.audioFd, kRate);
    midiBridgeSetFd(app.child.midiFd);
    app.link.close();
    app.haveFrame = false;
    app.nextConnect = SDL_GetTicks();
    app.nextRestart = SDL_GetTicks() + 1000;
    app.dirty = true;
    app.fullRedraw = true;
}

void ensureDoor(App& app)
{
    if (app.link.connected()) return;
    const Uint64 now = SDL_GetTicks();
    if (now < app.nextConnect) return;
    app.nextConnect = now + 500;
    if (app.link.connectDoor()) SDL_Log("control door connected");
}

// ---- drawing -------------------------------------------------------------

void drawScene(App& app, ImGuiIO& io)
{
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    app.scale = ImMax(1.0f, gUiScale.load());
    const float top = bannerHeight(app.scale);
    if (!app.haveLayout || app.layoutW != w || app.layoutH != h || app.layoutTop != top)
    {
        app.layout = layoutPanel(w, h, top);
        app.layoutW = w;
        app.layoutH = h;
        app.layoutTop = top;
        app.haveLayout = true;
        app.dirty = true;
    }

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    drawBanner(dl, app.font, w, app.scale, "PTPOCKET", &app.bannerMark, app.bannerDown);

    const PanelRect& s = app.layout.screen;
    dl->AddRectFilled(ImVec2(s.x0, s.y0), ImVec2(s.x1, s.y1), IM_COL32(4, 4, 6, 255), 6.0f);

    dl->PushClipRect(ImVec2(s.x0, s.y0), ImVec2(s.x1, s.y1), true);
    if (app.haveFrame && app.font != nullptr)
    {
        const float scale = ImMin(s.width() / (float)kScreenW, s.height() / (float)kScreenH);
        const ImVec2 origin(s.x0 + (s.width() - kScreenW * scale) * 0.5f,
                            s.y0 + (s.height() - kScreenH * scale) * 0.5f);
        drawFrame(dl, app.frame, app.font, scale, origin, app.sprites);
    }
    else if (app.font != nullptr)
    {
        const char* msg = "waiting for the firmware";
        const float size = ImMax(14.0f, h * 0.03f);
        const ImVec2 tw = app.font->CalcTextSizeA(size, FLT_MAX, 0.0f, msg);
        dl->AddText(app.font, size,
                    ImVec2((s.x0 + s.x1 - tw.x) * 0.5f, (s.y0 + s.y1 - tw.y) * 0.5f),
                    IM_COL32(150, 155, 165, 255), msg);
    }
    dl->PopClipRect();

    drawPanel(dl, app.layout, app.font, app.held);
}

void renderFrame(App& app, ImGuiIO& io, SDL_Window* window)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    drawScene(app, io);
    ImGui::Render();

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    glViewport(0, 0, pw, ph);
    glClearColor(0.055f, 0.06f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
}
} // namespace

// The display density, from MainActivity. This is a level, not an edge: the
// panel reads it between frames and recomputes its layout when it changes.
extern "C" void Java_nl_rgrv_ptpocket_MainActivity_nativeUiScale(JNIEnv*, jclass,
                                                                  jfloat density)
{
    gUiScale.store(density > 0.0f ? (float)density : 1.0f);
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    signal(SIGPIPE, SIG_IGN);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
    {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_AddEventWatch(lifecycleWatch, nullptr);

    static App app;
    const char* filesDir = SDL_GetAndroidInternalStoragePath();
    const std::string files = filesDir != nullptr ? filesDir : ".";
    app.binary = files + "/runtime/tracker-firmware-mini";
    app.sd = files + "/sdcard";
    app.log = files + "/tracker.log";
    const std::string fontPath = files + "/fonts/NotoSansMono.ttf";

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow("Tracker Mini", 0, 0,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr)
    {
        SDL_Log("SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    // Fallback for the density Java sends: SDL reports 1.0 until the Android
    // display's density arrives, so this covers only the earliest frames.
    const float contentScale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (contentScale > 1.0f) gUiScale.store(contentScale);
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (gl == nullptr)
    {
        SDL_Log("SDL_GL_CreateContext: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    app.font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 24.0f);
    if (app.font == nullptr)
    {
        SDL_Log("no font at %s; using the built-in one", fontPath.c_str());
        app.font = io.Fonts->AddFontDefault();
    }

    ImGui_ImplSDL3_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = kRate;
    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audioPull, &app.audio);
    if (stream != nullptr)
    {
        const SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(stream);
        SDL_AudioSpec devSpec, inSpec, outSpec;
        int frames = 0;
        SDL_GetAudioDeviceFormat(dev, &devSpec, &frames);
        SDL_GetAudioStreamFormat(stream, &inSpec, &outSpec);
        SDL_Log("audio: device %u Hz %u ch fmt %u %d frames; stream %u Hz %u ch fmt %u -> %u Hz %u ch fmt %u",
                devSpec.freq, devSpec.channels, (unsigned)devSpec.format, frames,
                inSpec.freq, inSpec.channels, (unsigned)inSpec.format,
                outSpec.freq, outSpec.channels, (unsigned)outSpec.format);
        SDL_ResumeAudioDevice(dev);
    }
    else
    {
        SDL_Log("no audio device: %s", SDL_GetError());
    }

    startChild(app);

    while (!stopRequested())
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL3_ProcessEvent(&e);
            switch (e.type)
            {
                case SDL_EVENT_QUIT:
                    requestStop();
                    break;
                case SDL_EVENT_FINGER_DOWN:
                case SDL_EVENT_FINGER_MOTION:
                {
                    const float x = e.tfinger.x * io.DisplaySize.x;
                    const float y = e.tfinger.y * io.DisplaySize.y;
                    if (e.type == SDL_EVENT_FINGER_DOWN) fingerDown(app, e.tfinger.fingerID,
                                                                   ImVec2(x, y));
                    break;
                }
                case SDL_EVENT_FINGER_UP:
                case SDL_EVENT_FINGER_CANCELED:
                    fingerUp(app, e.tfinger.fingerID);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (e.button.button == SDL_BUTTON_LEFT)
                    {
                        if (app.bannerMark.contains(ImVec2(e.button.x, e.button.y)))
                        {
                            app.mouseButton = kBanner;
                            app.bannerDown = true;
                            app.dirty = true;
                            break;
                        }
                        const int id = app.haveLayout
                                           ? hitTest(app.layout, ImVec2(e.button.x, e.button.y))
                                           : -1;
                        app.mouseButton = id;
                        if (id >= 0) sendDown(app, id);
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (e.button.button == SDL_BUTTON_LEFT && app.mouseButton != -1)
                    {
                        const int id = app.mouseButton;
                        app.mouseButton = -1;
                        if (id == kBanner) releaseBanner(app);
                        else if (id >= 0) releaseIfIdle(app, id);
                    }
                    break;
                case SDL_EVENT_DID_ENTER_FOREGROUND:
                    gContextLost = true;
                    break;
                case SDL_EVENT_WILL_ENTER_BACKGROUND:
                    releaseAll(app);
                    break;
                case SDL_EVENT_WINDOW_RESIZED:
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    app.dirty = true;
                    break;
                default:
                    break;
            }
        }

        ensureDoor(app);
        if (app.link.pollFrame(app.frame))
        {
            app.haveFrame = true;
            app.dirty = true;
        }

        const Uint64 now = SDL_GetTicks();
        if (now >= app.nextWatchdog)
        {
            app.nextWatchdog = now + 500;
            // Only the transition matters: asking again about a pid that is
            // already gone would keep pushing the restart out forever.
            if (app.child.running() && childExited(app.child))
            {
                if (WIFSIGNALED(app.child.status))
                    SDL_Log("firmware child killed by signal %d", WTERMSIG(app.child.status));
                else
                    SDL_Log("firmware child exited with %d", WEXITSTATUS(app.child.status));
                app.haveFrame = false;
                app.audio.stop();
                midiBridgeSetFd(-1);
                app.link.close();
                app.dirty = true;
                app.nextRestart = now + 1000;
            }
        }
        if (!app.child.running() && now >= app.nextRestart)
        {
            SDL_Log("restarting the firmware child");
            startChild(app);
        }

        if (gContextLost.exchange(false))
        {
            // The activity was paused; the EGL context (and with it every
            // texture ImGui made) is gone.
            app.sprites.release();
            ImGui_ImplOpenGL3_DestroyDeviceObjects();
            app.dirty = true;
        }

        if (!app.dirty)
        {
            SDL_Delay(4);
            continue;
        }
        renderFrame(app, io, window);
        app.dirty = false;
    }

    releaseAll(app);
    if (stream != nullptr)
    {
        SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(stream));
        SDL_DestroyAudioStream(stream);
    }
    app.audio.stop();
    midiBridgeSetFd(-1);
    app.link.close();
    childStop(app.child);
    app.sprites.release();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
