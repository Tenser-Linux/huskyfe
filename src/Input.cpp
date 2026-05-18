#include "Input.h"

#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <cstdio>
#include <cstring>

namespace huskyfe {
namespace {

int g_keys_fd  = -1;
int g_touch_fd = -1;


struct TouchState {
    int  cur_slot   = 0;
    int  tracking[10] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
    int  x[10] = {0};
    int  y[10] = {0};
    bool dirty[10] = {false};
    bool down_emitted[10] = {false};
};
TouchState g_touch;

int open_evdev(const char* path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "huskyfe: open %s: %s\n", path, strerror(errno));
    }
    return fd;
}


inline bool bit_test(const unsigned long* bits, int code) {
    return (bits[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long)))) & 1ul;
}

constexpr size_t kBitsBufLongs =
    (KEY_MAX / (8 * sizeof(long))) + 1;

bool device_supports(int fd, int ev_type, int code) {
    unsigned long bits[kBitsBufLongs] = {0};
    if (ioctl(fd, EVIOCGBIT(ev_type, sizeof(bits)), bits) < 0)
        return false;
    return bit_test(bits, code);
}

bool device_has_ev_type(int fd, int ev_type) {
    unsigned long bits[kBitsBufLongs] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(bits)), bits) < 0)
        return false;
    return bit_test(bits, ev_type);
}


bool is_touchscreen(int fd) {
    return device_has_ev_type(fd, EV_ABS)
        && device_supports(fd, EV_ABS, ABS_MT_POSITION_X)
        && device_supports(fd, EV_ABS, ABS_MT_POSITION_Y);
}


bool is_keys_device(int fd) {
    if (!device_has_ev_type(fd, EV_KEY)) return false;
    return device_supports(fd, EV_KEY, KEY_POWER)
        || device_supports(fd, EV_KEY, KEY_PROG1)
        || device_supports(fd, EV_KEY, KEY_VOLUMEUP)
        || device_supports(fd, EV_KEY, KEY_VOLUMEDOWN);
}

}

bool input_open() {


    for (int i = 0; i < 32; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {


            if (errno == ENOENT) break;
            continue;
        }
        char name[128] = {0};
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        bool claimed = false;
        if (g_touch_fd < 0 && is_touchscreen(fd)) {
            g_touch_fd = fd;
            fprintf(stderr, "huskyfe: input touch  -> %s '%s'\n", path, name);
            claimed = true;
        } else if (g_keys_fd < 0 && is_keys_device(fd)) {
            g_keys_fd = fd;
            fprintf(stderr, "huskyfe: input keys   -> %s '%s'\n", path, name);
            claimed = true;
        }
        if (!claimed) close(fd);
    }
    if (g_keys_fd  < 0) fprintf(stderr, "huskyfe: no keys input device found\n");
    if (g_touch_fd < 0) fprintf(stderr, "huskyfe: no touch input device found\n");
    return g_keys_fd >= 0 && g_touch_fd >= 0;
}

int input_keys_fd()  { return g_keys_fd; }
int input_touch_fd() { return g_touch_fd; }

void input_close() {
    if (g_keys_fd  >= 0) { close(g_keys_fd);  g_keys_fd  = -1; }
    if (g_touch_fd >= 0) { close(g_touch_fd); g_touch_fd = -1; }
}

namespace detail {

bool poll_keys(int fd, InputEvent& out) {
    input_event ie{};
    ssize_t n = read(fd, &ie, sizeof(ie));
    if (n != (ssize_t)sizeof(ie)) return false;
    out = {};
    if (ie.type != EV_KEY) return true;
    bool pressed = ie.value != 0;
    switch (ie.code) {


        case KEY_PROG1:
        case KEY_POWER:    out.kind = pressed ? InputKind::PowerPressed
                                              : InputKind::PowerReleased; break;
        case KEY_VOLUMEUP:   if (pressed) out.kind = InputKind::VolUp; break;
        case KEY_VOLUMEDOWN: if (pressed) out.kind = InputKind::VolDown; break;
        default: break;
    }
    return true;
}


bool poll_touch(int fd, InputEvent& out) {
    static int  pending_slot = -1;
    static bool pending_is_down = false;
    static bool pending_is_up = false;

    out = {};


    if (pending_slot >= 0) {
        int s = pending_slot;
        if (pending_is_up) {
            out.kind = InputKind::TouchUp;
            out.slot = s;
            out.x = g_touch.x[s]; out.y = g_touch.y[s];
            g_touch.down_emitted[s] = false;
            pending_slot = -1; pending_is_up = false;
            return true;
        }
        if (pending_is_down) {
            out.kind = InputKind::TouchDown;
            g_touch.down_emitted[s] = true;
        } else {
            out.kind = InputKind::TouchMove;
        }
        out.slot = s;
        out.x = g_touch.x[s]; out.y = g_touch.y[s];
        pending_slot = -1; pending_is_down = false;
        return true;
    }

    input_event ie{};
    ssize_t n = read(fd, &ie, sizeof(ie));
    if (n != (ssize_t)sizeof(ie)) return false;

    if (ie.type == EV_ABS) {
        int s = g_touch.cur_slot;
        if (s < 0 || s >= 10) return true;
        switch (ie.code) {
            case ABS_MT_SLOT:
                g_touch.cur_slot = (ie.value < 10) ? ie.value : 0;
                break;
            case ABS_MT_TRACKING_ID:
                if (ie.value < 0) {
                    g_touch.tracking[s] = -1;
                    g_touch.dirty[s] = true;
                } else {
                    g_touch.tracking[s] = ie.value;
                    g_touch.dirty[s] = true;
                }
                break;
            case ABS_MT_POSITION_X:
                g_touch.x[s] = ie.value;
                g_touch.dirty[s] = true;
                break;
            case ABS_MT_POSITION_Y:
                g_touch.y[s] = ie.value;
                g_touch.dirty[s] = true;
                break;
            default: break;
        }
        return true;
    }

    if (ie.type == EV_SYN && ie.code == SYN_REPORT) {


        for (int s = 0; s < 10; s++) {
            if (!g_touch.dirty[s]) continue;
            g_touch.dirty[s] = false;
            bool currently_tracking = (g_touch.tracking[s] >= 0);
            if (currently_tracking && !g_touch.down_emitted[s]) {
                pending_slot = s; pending_is_down = true; pending_is_up = false;
            } else if (!currently_tracking && g_touch.down_emitted[s]) {
                pending_slot = s; pending_is_down = false; pending_is_up = true;
            } else if (currently_tracking) {
                pending_slot = s; pending_is_down = false; pending_is_up = false;
            }
            break;
        }
    }
    return true;
}

}
}
