#pragma once
#include <cstdint>

namespace huskyfe {

enum class InputKind : uint8_t {
    None,
    PowerPressed,
    PowerReleased,
    VolUp,
    VolDown,
    TouchDown,
    TouchMove,
    TouchUp,
};

struct InputEvent {
    InputKind kind = InputKind::None;
    int32_t   x = 0;
    int32_t   y = 0;
    int32_t   slot = 0;
};


bool input_open();


int  input_keys_fd();
int  input_touch_fd();


template <typename Fn>
void input_drain(int fd, Fn&& cb);

void input_close();


namespace detail {
bool poll_keys(int fd, InputEvent& out);
bool poll_touch(int fd, InputEvent& out);
}

template <typename Fn>
void input_drain(int fd, Fn&& cb) {
    if (fd < 0) return;
    InputEvent ev;
    bool is_keys = (fd == input_keys_fd());
    while (true) {
        bool got = is_keys ? detail::poll_keys(fd, ev) : detail::poll_touch(fd, ev);
        if (!got) break;
        if (ev.kind != InputKind::None) cb(ev);
    }
}

}
