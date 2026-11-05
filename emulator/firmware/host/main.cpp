/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
// Host platform shim for the Polyend Tracker CE core (probe).
// Implements the Platform interface with no hardware; display commands are
// recorded so the FT812 stream can be inspected.
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <cerrno>
#include <fcntl.h>
#include <ucontext.h>
#include <unistd.h>

#include "Platform.h"
#include "Tracker.h"
#include "DisplayDriver.h"
#include "ConfigManager.h"
#include "audio.h"
#include "eve.h"
#include "fs.h"
#include "midi.h"
#include "service.h"

static const char* sdRoot()
{
    const char* env = getenv("TRACKER_SD");
    return (env && *env) ? env : "sdcard";
}

// Set by the door; the process re-execs itself after the loop unwinds.
static volatile sig_atomic_t restartRequested = 0;

static uint64_t now_us()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

class HostTimer : public Timer
{
  public:
    uint32_t millis() override { return (uint32_t)(now_us() / 1000); }
    uint32_t micros() override { return (uint32_t)now_us(); }
    void delay(int ms) override { if (ms > 0) usleep((useconds_t)ms * 1000); }
    uint32_t clockCycles() override { return (uint32_t)now_us() * 180; }
    uint32_t clockCyclesPerUs() override { return 180; }
};

class HostLeds : public Leds
{
  public:
    void setLed(int idx, bool enabled, int brightness) override
    {
        if (idx >= 0 && idx < 16) printf("led %d %d %d\n", idx, enabled, brightness);
    }
};

class HostEncoder : public Encoder
{
  public:
    void setAcceleration(int) override {}
    void setResolution(int) override {}
};

class HostBattery : public Battery
{
  public:
    void enableMonitoring() override {}
    int getLevel() override { return 100; }
    float getVoltage() override { return 4.2f; }
    bool getCharging() override { return true; }
    int getTimeTo() override { return 0; }
    bool getState() override { return true; }
    bool isEmpty() override { return false; }
};

// A switch group driven by the door.  Events are queued by the service thread
// and dispatched on the main thread, so the firmware's callbacks run where its
// own scan would have called them.  Hold repeats follow the device's Keys
// driver: the first after holdTime, then faster.
class HostSwitchGroup : public SwitchGroup
{
  public:
    void setHoldTime(int timeMs) override
    {
        std::lock_guard<std::mutex> lock(mu);
        holdTimeMs = timeMs > 0 ? timeMs : 333;
    }

    bool isButtonPressed(int idx) override
    {
        std::lock_guard<std::mutex> lock(mu);
        return idx >= 0 && idx < kMaxButtons && pressed[idx];
    }

    void setActionCallback(ActionType action, void (*callback)(uint8_t)) override
    {
        std::lock_guard<std::mutex> lock(mu);
        int a = (int)action;
        if (a >= 0 && a < 3) cb[a] = callback;
    }

    void queue(int idx, bool down)
    {
        if (idx < 0 || idx >= kMaxButtons) return;
        std::lock_guard<std::mutex> lock(mu);
        events.push_back({(uint8_t)idx, down});
    }

    // A release for every switch: dispatch drops the ones already up, and a
    // release queued after a press clears both in the same pass.
    void releaseAll()
    {
        std::lock_guard<std::mutex> lock(mu);
        for (int i = 0; i < kMaxButtons; i++) events.push_back({(uint8_t)i, false});
    }

    void dispatch(uint32_t now)
    {
        std::vector<Event> fire;
        void (*callbacks[3])(uint8_t) = {};
        {
            std::lock_guard<std::mutex> lock(mu);
            std::vector<Event> out;
            out.swap(events);
            for (auto& ev : out)
            {
                if (ev.down == pressed[ev.idx]) continue;
                pressed[ev.idx] = ev.down;
                if (ev.down)
                {
                    holdAt[ev.idx] = now + (uint32_t)holdTimeMs;
                    holdFactor[ev.idx] = 0;
                }
                fire.push_back(ev);
            }
            for (int i = 0; i < kMaxButtons; i++)
            {
                if (!pressed[i] || now < holdAt[i]) continue;
                if (holdFactor[i] < 5) holdFactor[i]++;
                static const uint16_t steps[6] = {333, 250, 160, 120, 90, 70};
                int dt = steps[holdFactor[i]];
                holdAt[i] = now + (uint32_t)(dt - (dt / 6));
                fire.push_back({(uint8_t)i, true, true});
            }
            for (int i = 0; i < 3; i++) callbacks[i] = cb[i];
        }
        // Callbacks run without the lock: the firmware may call back in.
        for (auto& ev : fire)
        {
            void (*fn)(uint8_t) = callbacks[ev.hold ? 2 : (ev.down ? 0 : 1)];
            if (fn) fn(ev.idx);
        }
    }

  private:
    static constexpr int kMaxButtons = 80;
    struct Event
    {
        uint8_t idx = 0;
        bool down = false;
        bool hold = false;
    };

    std::mutex mu;
    std::vector<Event> events;
    bool pressed[kMaxButtons] = {};
    uint32_t holdAt[kMaxButtons] = {};
    int holdFactor[kMaxButtons] = {};
    int holdTimeMs = 333;
    void (*cb[3])(uint8_t) = {};
};

class HostRadio : public Radio
{
  public:
    void enableLineInput(bool) override {}
    void enable(bool) override {}
    bool setFrequency(float) override { return false; }
    void setSeekCallback(void (*)()) override {}
    void seek(SeekDirection) override {}
    void updateSeek() override {}
    bool updateRDS() override { return false; }
    float getFrequency() override { return 0; }
    float getBaseFrequency() override { return 0; }
    float getSpacing() override { return 0; }
    bool isinitialized() override { return false; }
    void clearRDS() override {}
    void setRegion(Region) override {}
    bool isRegionChanged() override { return false; }
    char* getStationName() override { static char name[] = ""; return name; }
};

class HostCodec : public Sgtl5000
{
  public:
    void init() override {}
    void setInputType(InputType) override {}
    void setHeadphoneSource(HeadphoneSource) override {}
    void setLineInLevel(int) override {}
    void setMicGain(int) override {}
    void setHeadphoneMuted(bool) override {}
    void setMasterVolume(float) override {}
    void setAutoVolumeEnabled(bool) override {}
    void configureAutoVolume(AutoVolumeParams*) override {}
    void setEnhanceBassEnabled(bool) override {}
    void configureEnhanceBass(EnhanceBassParams*) override {}
    void setEqualizerType(EqualizerType) override {}
    void configureFiveBandEqualizer(float*) override {}
    void enableSurroundSound(bool) override {}
    void configureSurroundSound(SurroundSoundParams*) override {}
    void enableHardwareProcessor(bool) override {}
};

// The firmware passes channels 1..16, as the MK66 drivers do; raw MIDI bytes
// carry 0..15.
static inline uint8_t midiStatus(uint8_t base, uint8_t channel)
{
    return (uint8_t)(base | ((channel > 0 ? channel - 1 : 0) & 0x0F));
}

class HostMidi : public UsbMidi
{
  public:
    explicit HostMidi(MidiPort* port)
      : port_(port)
    {
    }

    bool isReadAvailable() override { return port_ != nullptr && port_->isReadAvailable(); }

    void read() override
    {
        uint8_t m[3] = {0, 0, 0};
        int n = port_ != nullptr ? port_->read(m) : 0;
        if (n <= 0 || rx_ == nullptr) return;
        uint8_t s = m[0];
        if (s >= 0xF8)
        {
            if (rx_->realTime) rx_->realTime(s);
            return;
        }
        if (s == 0xF2)
        {
            if (rx_->songPosition) rx_->songPosition((uint16_t)(m[1] | (m[2] << 7)));
            return;
        }
        uint8_t channel = (uint8_t)((s & 0x0F) + 1);
        switch (s & 0xF0)
        {
            case 0x90:
                if (m[2] == 0)
                {
                    if (rx_->noteOff) rx_->noteOff(channel, m[1], 0);
                }
                else if (rx_->noteOn)
                    rx_->noteOn(channel, m[1], m[2]);
                break;
            case 0x80:
                if (rx_->noteOff) rx_->noteOff(channel, m[1], m[2]);
                break;
            case 0xB0:
                if (rx_->controlChange) rx_->controlChange(channel, m[1], m[2]);
                break;
            default: break;
        }
    }

    void sendRealTime(MessageType t) override
    {
        uint8_t b = (uint8_t)t;
        send(&b, 1);
    }
    void sendControlChange(uint8_t index, uint8_t value, uint8_t channel) override
    {
        uint8_t m[3] = {midiStatus(0xB0, channel), index, value};
        send(m, 3);
    }
    void sendProgramChange(uint8_t program, uint8_t channel) override
    {
        uint8_t m[2] = {midiStatus(0xC0, channel), program};
        send(m, 2);
    }
    void sendAfterTouch(uint8_t value, uint8_t channel) override
    {
        uint8_t m[2] = {midiStatus(0xD0, channel), value};
        send(m, 2);
    }
    void sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel) override
    {
        uint8_t m[3] = {midiStatus(0xA0, channel), note, value};
        send(m, 3);
    }
    void sendPitchBend(int value, uint8_t channel) override
    {
        if (value < 0) value = 0;
        if (value > 16383) value = 16383;
        uint8_t m[3] = {midiStatus(0xE0, channel), (uint8_t)(value & 0x7F), (uint8_t)(value >> 7)};
        send(m, 3);
    }
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override
    {
        uint8_t m[3] = {midiStatus(0x90, channel), note, velocity};
        send(m, 3);
    }
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override
    {
        uint8_t m[3] = {midiStatus(0x80, channel), note, velocity};
        send(m, 3);
    }
    void setReceiveActions(MidiReceiveActions* actions) override { rx_ = actions; }

  private:
    void send(const uint8_t* data, int len)
    {
        if (port_ != nullptr) port_->send(data, len);
    }

    MidiPort* port_;
    MidiReceiveActions* rx_ = nullptr;
};

class HostUartMidi : public UartMidi
{
  public:
    explicit HostUartMidi(MidiPort* port)
      : port_(port)
    {
    }

    bool isReadAvailable() override { return port_ != nullptr && port_->isReadAvailable(); }

    void read() override
    {
        uint8_t m[3] = {0, 0, 0};
        int n = port_ != nullptr ? port_->read(m) : 0;
        if (n <= 0 || rx_ == nullptr) return;
        uint8_t s = m[0];
        if (s >= 0xF8)
        {
            switch (s)
            {
                case 0xF8: if (rx_->clock) rx_->clock(); break;
                case 0xFA:
                    if (rx_->start) rx_->start();
                    break;
                case 0xFB:
                    if (rx_->continueMessage) rx_->continueMessage();
                    break;
                case 0xFC:
                    if (rx_->stop) rx_->stop();
                    break;
                default: break;
            }
            return;
        }
        if (s == 0xF2)
        {
            if (rx_->songPosition) rx_->songPosition((uint16_t)(m[1] | (m[2] << 7)));
            return;
        }
        uint8_t channel = (uint8_t)((s & 0x0F) + 1);
        switch (s & 0xF0)
        {
            case 0x90:
                if (m[2] == 0)
                {
                    if (noteOn_) noteOn_(channel, m[1], 0);
                    else if (rx_->noteOff) rx_->noteOff(channel, m[1], 0);
                }
                else if (noteOn_)
                    noteOn_(channel, m[1], m[2]);
                else if (rx_->noteOn)
                    rx_->noteOn(channel, m[1], m[2]);
                break;
            case 0x80:
                if (rx_->noteOff) rx_->noteOff(channel, m[1], m[2]);
                break;
            case 0xB0:
                if (rx_->controlChange) rx_->controlChange(channel, m[1], m[2]);
                break;
            default: break;
        }
    }

    void sendRealTime(MessageType t) override
    {
        uint8_t b = (uint8_t)t;
        send(&b, 1);
    }
    void sendControlChange(uint8_t index, uint8_t value, uint8_t channel) override
    {
        uint8_t m[3] = {midiStatus(0xB0, channel), index, value};
        send(m, 3);
    }
    void sendProgramChange(uint8_t program, uint8_t channel) override
    {
        uint8_t m[2] = {midiStatus(0xC0, channel), program};
        send(m, 2);
    }
    void sendAfterTouch(uint8_t value, uint8_t channel) override
    {
        uint8_t m[2] = {midiStatus(0xD0, channel), value};
        send(m, 2);
    }
    void sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel) override
    {
        uint8_t m[3] = {midiStatus(0xA0, channel), note, value};
        send(m, 3);
    }
    void sendPitchBend(int value, uint8_t channel) override
    {
        if (value < 0) value = 0;
        if (value > 16383) value = 16383;
        uint8_t m[3] = {midiStatus(0xE0, channel), (uint8_t)(value & 0x7F), (uint8_t)(value >> 7)};
        send(m, 3);
    }
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override
    {
        uint8_t m[3] = {midiStatus(0x90, channel), note, velocity};
        send(m, 3);
    }
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override
    {
        uint8_t m[3] = {midiStatus(0x80, channel), note, velocity};
        send(m, 3);
    }
    void init() override {}
    void setReceiveActions(MidiReceiveActions* actions) override { rx_ = actions; }
    void setReceiveNoteOnAction(void (*action)(uint8_t, uint8_t, uint8_t)) override
    {
        noteOn_ = action;
    }

  private:
    void send(const uint8_t* data, int len)
    {
        if (port_ != nullptr) port_->send(data, len);
    }

    MidiPort* port_;
    MidiReceiveActions* rx_ = nullptr;
    void (*noteOn_)(uint8_t, uint8_t, uint8_t) = nullptr;
};

class HostUsbAudio : public UsbAudio
{
  public:
    void beginFillData() override {}
    void fillChannelData(void*, int, int) override {}
    void finishFillData() override {}
};

class HostUsb : public Usb
{
  public:
    void init(void) override {}
    void reset(void) override {}
    void update(void) override {}
    void setClassState(ClassType, bool) override {}
    bool getClassState(ClassType) override { return false; }
    bool isAttached() override { return false; }
};

class HostCrc : public CrcCalculator
{
  public:
    uint32_t calculate32bitCrc(const uint8_t* data, const uint16_t len) override
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (uint16_t i = 0; i < len; i++)
        {
            crc ^= data[i];
            for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
        return ~crc;
    }
};

class HostPlatform : public Platform
{
  public:
    HostPlatform()
    {
        sys.buildId = "host";
    }
    void setPowerMode(bool) override {}
    void resetMCU() override { printf("resetMCU\n"); }
    void runBootloader(char*) override {}
    void setPowerButtonCallback(PowerButtonCallback cb) override { powerCb = cb; }
    bool getPowerButtonState() override { return false; }
    bool readForceStartUp() override { return false; }

    Sgtl5000* getCodecAudio() override { return &codec; }
    Radio* getRadio() override { return &radio; }
    SwitchGroup* getButtons() override { return &buttons; }
    SwitchGroup* getPads() override { return &pads; }
    Leds* getLeds() override { return &leds; }
    Battery* getBattery() override { return &battery; }
    Timer* getTimer() override { return &timer; }
    Encoder* getEncoder() override { return &encoder; }
    DisplayDriver* getDisplayDriver() override { return &display; }
    UsbAudio* getUsbAudio() override { return &usbAudio; }
    UartMidi* getUartMidi() override { return &uartMidi; }
    UsbMidi* getUsbMidi() override { return &usbMidi; }
    ConfigManager* getConfigManager() override { return &config; }
    FileSystem* getFileSystem() override { return &fs; }
    CrcCalculator* getCrcCalculator() override { return &crc; }
    SystemInfo* getSystemInfo() override { return &sys; }
    Usb* getUsb() override { return &usb; }

    void update() {}

    // TRACKER_MIDI names a rawmidi node ("auto" picks the first one that is
    // not the emulator panel's); TRACKER_MIDI_JACK opens a second port only
    // when it is set.
    void openMidi()
    {
        const char* usb = getenv("TRACKER_MIDI");
        midiPort.open(usb != nullptr ? usb : "auto");
        printf("midi usb %c %s\n", midiPort.isOpen() ? '=' : ':', midiPort.path());
        const char* jack = getenv("TRACKER_MIDI_JACK");
        if (jack != nullptr)
        {
            jackPort.open(jack);
            printf("midi jack %c %s\n", jackPort.isOpen() ? '=' : ':', jackPort.path());
        }
    }

  private:
    HostTimer timer;
    HostLeds leds;
    HostEncoder encoder;
    HostBattery battery;
    HostSwitchGroup buttons, pads;
    HostRadio radio;
    HostCodec codec;
    MidiPort midiPort, jackPort;
    HostMidi usbMidi{&midiPort};
    HostUartMidi uartMidi{&jackPort};
    HostUsbAudio usbAudio;
    HostUsb usb;
    HostConfigManager config{sdRoot()};
    HostCrc crc;
    HostFileSystem fs{sdRoot()};
    EveDisplay display;
    SystemInfo sys;
    PowerButtonCallback powerCb = nullptr;
};

// The door's events, delivered on the main thread to the firmware's own
// handlers (the same functions the MK66 platform's hardware loop calls).
extern void onButtonPush(uint8_t);
extern void onButtonRelease(uint8_t);
extern void onButtonHold(uint8_t);
extern void onPadPush(uint8_t);
extern void onPadRelease(uint8_t);
extern void onPadHold(uint8_t);
extern void onPotChange(uint8_t, int16_t);

class HostInput : public InputSink
{
  public:
    HostInput(HostSwitchGroup& buttons, HostSwitchGroup& pads)
      : buttons_(buttons)
      , pads_(pads)
    {
    }

    void button(int idx, bool down) override { buttons_.queue(idx, down); }
    void pad(int idx, bool down) override { pads_.queue(idx, down); }

    void releaseAll() override
    {
        buttons_.releaseAll();
        pads_.releaseAll();
    }

    void restart() override { restartRequested = 1; }

    void encoder(int delta) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        encoder_ += delta;
    }

    int takeEncoder()
    {
        std::lock_guard<std::mutex> lock(mu_);
        int d = encoder_;
        encoder_ = 0;
        return d;
    }

  private:
    HostSwitchGroup& buttons_;
    HostSwitchGroup& pads_;
    std::mutex mu_;
    int encoder_ = 0;
};

// The firmware loads .ptfx plugins into the first 32 KB of the scratch buffer
// and jumps to them.  On the device that RAM is executable; on Linux it needs
// PROT_EXEC.  The rest of the buffer stays writable scratch.
static void makePtfxBufferExecutable()
{
    extern uint8_t externalRamScratchBuff[];
    const long page = sysconf(_SC_PAGESIZE);
    const uintptr_t base = (uintptr_t)externalRamScratchBuff;
    const uintptr_t start = base & ~(uintptr_t)(page - 1);
    const uintptr_t end = (base + 768 * 1024 + (uintptr_t)page - 1) & ~(uintptr_t)(page - 1);
    if (mprotect((void*)start, (size_t)(end - start), PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        perror("mprotect(scratch)");
}

static volatile sig_atomic_t stopRequested = 0;

static void onSignal(int)
{
    stopRequested = 1;
}

// A fault or a hang (update() not advancing, seen from the audio thread)
// re-executes the process, so the doors come back. Bounded runs (--frames)
// keep the fault so a test fails; TRACKER_RESTART=0 disables both.
static char** g_argv = nullptr;
static std::atomic<long> g_mainBeat{0};
static volatile sig_atomic_t g_restartOnFatal = 0;

static void hardRestart(const char* why)
{
    // A signal handler may only touch async-signal-safe calls.
    write(2, "host: ", 6);
    write(2, why, strlen(why));
    write(2, "; restarting\n", 13);
    execv("/proc/self/exe", g_argv);
    _exit(1);
}

// A hex number, written with write(2) alone: this runs in a signal handler.
static void writeHex(const char* label, unsigned long v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[2 + 2 * sizeof(unsigned long)];
    size_t n = 0;
    write(2, label, strlen(label));
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int shift = (int)sizeof(unsigned long) * 8 - 4; shift >= 0; shift -= 4)
    {
        buf[n++] = digits[(v >> shift) & 0xf];
    }
    write(2, buf, n);
}

static void onFatalSignal(int sig, siginfo_t* info, void* context)
{
    const char* name = "fatal signal";
    switch (sig)
    {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGBUS: name = "SIGBUS"; break;
        case SIGILL: name = "SIGILL"; break;
        case SIGFPE: name = "SIGFPE"; break;
        case SIGABRT: name = "SIGABRT"; break;
    }
    if (!g_restartOnFatal)
    {
        write(2, "host: ", 6);
        write(2, name, strlen(name));
        // si_code tells a bad address from a bad opcode.
        if (info != nullptr)
        {
            writeHex(" addr=", (unsigned long)info->si_addr);
            writeHex(" code=", (unsigned long)info->si_code);
        }
#if defined(__arm__)
        if (context != nullptr)
        {
            const ucontext_t* uc = static_cast<const ucontext_t*>(context);
            writeHex(" pc=", uc->uc_mcontext.arm_pc);
            writeHex(" lr=", uc->uc_mcontext.arm_lr);
            writeHex(" sp=", uc->uc_mcontext.arm_sp);
        }
#endif
        write(2, "\n", 1);
        // Maps turn the addresses into a module and an offset.
        write(2, "host: maps\n", 11);
        const int fd = open("/proc/self/maps", O_RDONLY);
        if (fd >= 0)
        {
            char buf[4096];
            ssize_t n;
            while ((n = read(fd, buf, sizeof buf)) > 0) write(2, buf, (size_t)n);
            close(fd);
        }
        _exit(128 + sig);
    }
    hardRestart(name);
}

static void installContainment()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = onFatalSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

// Android's seccomp filter answers unknown syscalls with SIGSYS
// (SECCOMP_RET_TRAP), not ENOSYS, and glibc then dies instead of using its
// fallback. This static binary catches the signal itself. The legacy file
// syscalls glibc 2.24 still issues (rmdir, chmod, link, symlink, statfs) are
// carried out with their *at forms and the result returned in r0; anything
// else gets ENOSYS.
#if defined(__arm__)
// linux/seccomp.h value; the kernel reports it in siginfo_t.si_code.
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif

// Syscall numbers are the ARM EABI ones the toolchain's asm/unistd-eabi.h
// carries; they are not the asm-generic numbering.
static constexpr int kSysAccept4 = 366;
static constexpr int kSysSendto = 290;
static constexpr int kSysRecvfrom = 292;
static constexpr int kSysUnlinkat = 328;
static constexpr int kSysLinkat = 330;
static constexpr int kSysSymlinkat = 331;
static constexpr int kSysFchmodat = 333;
static constexpr long kAtFdcwd = -100;
static constexpr long kAtRemoveDir = 0x200;

// Inline svc, as libc's own stubs use.  libc's syscall() is not safe from a
// signal handler and its errno handling is unwanted: the result goes straight
// back to the trapping stub.  Six argument registers cover the socket calls.
static inline long rawSyscall(int nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
    register long r0 __asm__("r0") = a0;
    register long r1 __asm__("r1") = a1;
    register long r2 __asm__("r2") = a2;
    register long r3 __asm__("r3") = a3;
    register long r4 __asm__("r4") = a4;
    register long r5 __asm__("r5") = a5;
    register long r7 __asm__("r7") = nr;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
                     : "memory");
    return r0;
}

static void onSyscallTrap(int signo, siginfo_t* info, void* context)
{
    // Only a seccomp trap is ours; any other SIGSYS takes its default action.
    if (info == nullptr || info->si_code != SYS_SECCOMP)
    {
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        sigaction(signo, &dfl, nullptr);
        raise(signo);
        return;
    }
    ucontext_t* uc = static_cast<ucontext_t*>(context);
    // A 64-bit kernel reports the pc after the svc, a 32-bit kernel on it.
    // Step only when the instruction there is an svc: skipping the epilogue
    // would leave the stack one word out.
    const unsigned long pc = uc->uc_mcontext.arm_pc;
    if (uc->uc_mcontext.arm_cpsr & 0x20)
    {
        // Thumb "svc #0" is one 16-bit encoding.
        if ((*(const unsigned short*)pc & 0xff00) == 0xdf00)
            uc->uc_mcontext.arm_pc = pc + 2;
    }
    else if ((*(const unsigned int*)pc & 0x0f000000u) == 0x0f000000u)
    {
        // ARM "svc #0" is one 32-bit word.
        uc->uc_mcontext.arm_pc = pc + 4;
    }

    const long a0 = (long)uc->uc_mcontext.arm_r0;
    const long a1 = (long)uc->uc_mcontext.arm_r1;
    const long a2 = (long)uc->uc_mcontext.arm_r2;
    const long a3 = (long)uc->uc_mcontext.arm_r3;
    long result = -ENOSYS;
    switch (uc->uc_mcontext.arm_r7)
    {
        case 40: // rmdir(path)
            result = rawSyscall(kSysUnlinkat, kAtFdcwd, a0, kAtRemoveDir, 0, 0, 0);
            break;
        case 15: // chmod(path, mode)
            result = rawSyscall(kSysFchmodat, kAtFdcwd, a0, a1, 0, 0, 0);
            break;
        case 9: // link(old, new)
            result = rawSyscall(kSysLinkat, kAtFdcwd, a0, kAtFdcwd, a1, 0, 0);
            break;
        case 83: // symlink(target, path)
            result = rawSyscall(kSysSymlinkat, a0, kAtFdcwd, a1, 0, 0, 0);
            break;
        case 285: // accept(fd, addr, len); accept4 does the same with flags 0
            result = rawSyscall(kSysAccept4, a0, a1, a2, 0, 0, 0);
            break;
        case 289: // send(fd, buf, len, flags); sendto with no destination
            result = rawSyscall(kSysSendto, a0, a1, a2, a3, 0, 0);
            break;
        case 291: // recv(fd, buf, len, flags); recvfrom with no source
            result = rawSyscall(kSysRecvfrom, a0, a1, a2, a3, 0, 0);
            break;
        default:
            break;
    }
    uc->uc_mcontext.arm_r0 = (unsigned long)result;
}
#endif

static void installSeccompSkip()
{
#if defined(__arm__)
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = onSyscallTrap;
    sigemptyset(&sa.sa_mask);
    // No SA_RESETHAND: every thread that traps needs the handler.
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, nullptr);
#endif
}

#if defined(__arm__)
// set_robust_list is issued before main and traps too, so the handler is
// installed by wrapping __libc_start_main.
extern "C" int __real___libc_start_main(int (*fn)(int, char**, char**), int argc, char** argv,
                                        void (*init)(void), void (*fini)(void),
                                        void (*rtld_fini)(void), void* stack_end);

#if defined(TRACKER_GLIBC224)
// The toolchain's crt1.o passes no init function (glibc 2.34 runs .init_array
// itself), but glibc 2.24 only calls constructors when init is non-NULL.
// Without this every global object would keep a null vptr.
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

static void runInitArray(int argc, char** argv, char** envp)
{
    (void)argc;
    (void)argv;
    (void)envp;
    for (void (**p)(void) = __init_array_start; p != __init_array_end; p++)
    {
        (*p)();
    }
}
#endif

extern "C" int __wrap___libc_start_main(int (*fn)(int, char**, char**), int argc, char** argv,
                                        void (*init)(void), void (*fini)(void),
                                        void (*rtld_fini)(void), void* stack_end)
{
    installSeccompSkip();
#if defined(TRACKER_GLIBC224)
    if (init == nullptr) init = runInitArray;
#endif
    return __real___libc_start_main(fn, argc, argv, init, fini, rtld_fini, stack_end);
}
#endif

int main(int argc, char** argv)
{
    // Line-buffered: a start-up failure must reach the log before a kill.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    static HostPlatform platform;
    long frameLimit = -1;
    long dumpEvery = 0;
    bool dumpAtEnd = false;
    bool serve = true;
    ServiceOptions opts;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frameLimit = atol(argv[++i]);
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) dumpEvery = atol(argv[++i]);
        else if (!strcmp(argv[i], "--dump")) dumpAtEnd = true;
        else if (!strcmp(argv[i], "--http") && i + 1 < argc) opts.httpPort = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--line") && i + 1 < argc) opts.linePort = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) opts.page = argv[++i];
        else if (!strcmp(argv[i], "--no-serve")) serve = false;
        else
        {
            fprintf(stderr,
                    "usage: %s [--http PORT] [--line PORT] [--page FILE] [--no-serve]\n"
                    "          [--frames N] [--dump] [--dump-every N]\n"
                    "  runs until SIGINT/SIGTERM unless --frames is given\n",
                    argv[0]);
            return 2;
        }
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    g_argv = argv;
    g_restartOnFatal = frameLimit < 0;
    {
        const char* rs = getenv("TRACKER_RESTART");
        if (rs != nullptr && !strcmp(rs, "0")) g_restartOnFatal = 0;
    }
    installContainment();
    makePtfxBufferExecutable();
    Tracker* tracker = Tracker::getInstance();
    printf("start display\n");
    tracker->getStartDisplayFunc()(&platform);
    printf("tracker init\n");
    tracker->init(&platform);
    printf("tracker init done\n");

    HostSwitchGroup* buttons = static_cast<HostSwitchGroup*>(platform.getButtons());
    HostSwitchGroup* pads = static_cast<HostSwitchGroup*>(platform.getPads());
    buttons->setActionCallback(SwitchGroup::kPress, onButtonPush);
    buttons->setActionCallback(SwitchGroup::kRelease, onButtonRelease);
    buttons->setActionCallback(SwitchGroup::kHold, onButtonHold);
    pads->setActionCallback(SwitchGroup::kPress, onPadPush);
    pads->setActionCallback(SwitchGroup::kRelease, onPadRelease);
    pads->setActionCallback(SwitchGroup::kHold, onPadHold);
    HostInput input(*buttons, *pads);
    platform.openMidi();

    // Audio thread: 128 sample blocks at the MK66 I2S rate.  With a device
    // sink the blocking write paces this thread; without one it sleeps.
    AudioOut audioOut;
    bool audioSink = audioOut.open(44117.6470588);
    printf("audio %c %s\n", audioSink ? '=' : ':', audioOut.describe());
    static int16_t left[128], right[128];
    std::thread audio([&]() {
        const double rate = 44117.6470588;
        const auto period = std::chrono::nanoseconds((long long)(128.0 / rate * 1e9));
        auto next = std::chrono::steady_clock::now();
        long beat = g_mainBeat.load();
        auto beatTime = std::chrono::steady_clock::now();
        while (!stopRequested && !restartRequested)
        {
            tracker->updateAudioBuffers(left, right, 128);
            if (audioSink) audioOut.feed(left, right, 128);
            if (!audioOut.paces())
            {
                next += period;
                std::this_thread::sleep_until(next);
            }
            if (g_restartOnFatal)
            {
                auto now = std::chrono::steady_clock::now();
                long b = g_mainBeat.load(std::memory_order_relaxed);
                if (b != beat)
                {
                    beat = b;
                    beatTime = now;
                }
                else if (now - beatTime > std::chrono::seconds(10))
                {
                    hardRestart("watchdog: firmware update stalled");
                }
            }
        }
    });
    EveDisplay* display = static_cast<EveDisplay*>(platform.getDisplayDriver());
    if (serve && !service_start(display, &input, opts))
    {
        fprintf(stderr, "door ports %d/%d unavailable\n", opts.httpPort, opts.linePort);
        serve = false;
    }
    long frames = 0;
    while (!stopRequested && !restartRequested && (frameLimit < 0 || frames < frameLimit))
    {
        platform.update();
        tracker->update();
        // Deliver door input on this thread, where the device's scan runs.
        uint32_t ms = platform.getTimer()->millis();
        buttons->dispatch(ms);
        pads->dispatch(ms);
        int delta = input.takeEncoder();
        if (delta != 0) onPotChange(0, (int16_t)delta);
        frames++;
        g_mainBeat.fetch_add(1, std::memory_order_relaxed);
        if (dumpEvery > 0 && frames % dumpEvery == 0) display->dumpFrameText(stdout);
        usleep(2000);
    }
    if (dumpAtEnd) display->dumpFrameText(stdout);
    printf("host run finished after %ld frames\n", frames);
    stopRequested = 1;
    if (serve) service_stop();
    audio.join();
    audioOut.close();
    std::fflush(nullptr);
    if (restartRequested)
    {
        // Same binary, same arguments, fresh firmware state; the sockets carry
        // SO_REUSEADDR so the doors come back at once.
        execv("/proc/self/exe", argv);
        perror("execv");
        return 1;
    }
    return 0;
}
