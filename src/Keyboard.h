#pragma once
#include "Spring.h"
#include "Renderer.h"
#include "Text.h"

#include <functional>
#include <string>
#include <vector>

namespace huskyfe {


class Keyboard {
public:
    enum class Result { ONGOING, ACCEPTED, CANCELLED };

    bool init(int screen_w, int screen_h);
    void shutdown();

    void show(const std::string& title,
              const std::string& subtitle = "",
              const std::string& initial = "",
              bool password_mask = false);
    void show_live(std::function<void(char)> on_char);
    void dismiss();
    bool visible() const     { return visible_; }
    bool is_live()  const    { return live_; }
    Result consume_result();
    const std::string& text() const { return buffer_; }

    void tick(float dt);
    void render(Renderer& r, TextRenderer& title_text,
                TextRenderer& body_text, TextRenderer& key_text);

    bool on_touch_down(int x, int y);
    bool on_touch_move(int x, int y);
    bool on_touch_up(int x, int y);

private:
    enum class KeyKind { CHAR, SHIFT, BACKSPACE, SPACE, DONE, CANCEL, LAYER };
    enum class Layer   { LETTERS, SYMBOLS };
    struct Key {
        float   x, y, w, h;
        KeyKind kind;
        char    ch;
    };

    void build_layout();
    int  hit(int x, int y) const;

    int    screen_w_ = 0;
    int    screen_h_ = 0;
    bool   visible_  = false;
    bool   shift_    = false;
    bool   mask_     = false;
    bool   live_     = false;
    std::function<void(char)> on_char_;
    Layer  layer_    = Layer::LETTERS;
    Result result_   = Result::ONGOING;

    Spring  anim_;
    int     pressed_   = -1;
    std::vector<Key> keys_;

    std::string title_;
    std::string subtitle_;
    std::string buffer_;
};

}
