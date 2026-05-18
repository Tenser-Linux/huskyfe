#include "Haptics.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

namespace huskyfe::haptics {
namespace {
int g_fd = -1;
int g_effect_id = -1;
std::mutex g_mu;


bool icase_substr(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++) {
        if (strncasecmp(p, needle, nl) == 0) return true;
    }
    return false;
}


bool try_open_haptic(const char* path, int* out_fd) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    char name[128] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
        close(fd);
        return false;
    }


    const char* patterns[] = { "cs40l26", "vibrator", "haptic" };
    for (const char* pat : patterns) {
        if (icase_substr(name, pat)) {
            fprintf(stderr, "haptics: matched %s (name=\"%s\") on pattern \"%s\"\n",
                    path, name, pat);
            *out_fd = fd;
            return true;
        }
    }
    close(fd);
    return false;
}
}

bool init(const char* dev) {
    if (g_fd >= 0) return true;


    if (DIR* d = opendir("/dev/input")) {
        while (dirent* e = readdir(d)) {
            if (strncmp(e->d_name, "event", 5) != 0) continue;
            std::string p = std::string("/dev/input/") + e->d_name;
            int fd = -1;
            if (try_open_haptic(p.c_str(), &fd)) {
                g_fd = fd;
                closedir(d);
                return true;
            }
        }
        closedir(d);
    }


    g_fd = open(dev, O_RDWR | O_CLOEXEC);
    if (g_fd < 0) {
        fprintf(stderr, "haptics: no haptic node matched and open %s: %s\n",
                dev, strerror(errno));
        return false;
    }
    fprintf(stderr, "haptics: fallback opened %s\n", dev);
    return true;
}

void shutdown() {
    if (g_fd < 0) return;
    if (g_effect_id >= 0)
        ioctl(g_fd, EVIOCRMFF, (void*)(long)g_effect_id);
    close(g_fd);
    g_fd = -1;
    g_effect_id = -1;
}

bool ok() { return g_fd >= 0; }

bool play(int duration_ms, int strength, int period_ms,
          int attack_ms, int attack_level) {
    if (g_fd < 0)               return false;
    if (duration_ms <= 0)       return false;
    if (strength <= 0)          return false;
    if (strength > 32767)       strength = 32767;
    if (attack_level < 0)       attack_level = 0;
    if (attack_level > 32767)   attack_level = 32767;
    if (attack_ms < 0)          attack_ms = 0;
    if (attack_ms > duration_ms) attack_ms = duration_ms;
    if (period_ms < 4)          period_ms = 4;

    std::lock_guard<std::mutex> lk(g_mu);


    struct ff_effect e{};
    e.type = FF_PERIODIC;
    e.id   = g_effect_id;
    e.u.periodic.waveform               = FF_SINE;
    e.u.periodic.period                 = (uint16_t)period_ms;
    e.u.periodic.magnitude              = (int16_t)32767;
    e.u.periodic.offset                 = 0;
    e.u.periodic.phase                  = 0;
    e.u.periodic.envelope.attack_length = (uint16_t)attack_ms;
    e.u.periodic.envelope.attack_level  = (uint16_t)attack_level;
    e.u.periodic.envelope.fade_length   = 0;
    e.u.periodic.envelope.fade_level    = 0;
    e.replay.length = (uint16_t)duration_ms;
    e.replay.delay  = 0;

    if (ioctl(g_fd, EVIOCSFF, &e) < 0) {
        fprintf(stderr, "haptics: EVIOCSFF: %s\n", strerror(errno));
        return false;
    }
    g_effect_id = e.id;


    {
        struct input_event gain{};
        gain.type  = EV_FF;
        gain.code  = FF_GAIN;
        gain.value = (int)(((unsigned)strength * 0xFFFFu) / 32767u);
        if (write(g_fd, &gain, sizeof(gain)) != (ssize_t)sizeof(gain)) {
            fprintf(stderr, "haptics: gain write: %s\n", strerror(errno));

        }
    }

    struct input_event ev{};
    ev.type  = EV_FF;
    ev.code  = (uint16_t)g_effect_id;
    ev.value = 1;
    if (write(g_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        fprintf(stderr, "haptics: play write: %s\n", strerror(errno));
        return false;
    }
    return true;
}

bool play_tone(int freq_hz, int duration_ms, int strength) {
    if (freq_hz < 1) return false;


    int period_ms = 1000 / freq_hz;
    if (period_ms < 4) period_ms = 4;
    return play(duration_ms, strength, period_ms, 0,
                0);
}

}
