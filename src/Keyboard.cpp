#include "Keyboard.h"

#include "Renderer.h"
#include "Text.h"

#include <algorithm>
#include <cmath>

namespace huskyfe {

namespace {


constexpr float MODAL_Y          = 1700.0f;
constexpr float TITLE_Y          = MODAL_Y +  90.0f;
constexpr float SUBTITLE_Y       = MODAL_Y + 160.0f;
constexpr float FIELD_Y          = MODAL_Y + 230.0f;
constexpr float FIELD_H          = 110.0f;
constexpr float FIELD_PAD_X      = 80.0f;

constexpr float KEY_AREA_Y       = MODAL_Y + 410.0f;
constexpr float KEY_H            = 130.0f;
constexpr float ROW_GAP          = 12.0f;
constexpr float SIDE_PAD         = 30.0f;
constexpr float KEY_GAP          = 8.0f;

constexpr int   ROWS             = 5;

const char* ROW_DIGITS  = "1234567890";


const char* ROW_TOP_L   = "qwertyuiop";
const char* ROW_MID_L   = "asdfghjkl";
const char* ROW_BOT_L   = "zxcvbnm";


const char* ROW_TOP_S   = "-/:;()$&@\"";
const char* ROW_MID_S   = ".,?!'`~_+";
const char* ROW_BOT_S   = "=[]{}<>";

}

bool Keyboard::init(int screen_w, int screen_h) {
    screen_w_ = screen_w;
    screen_h_ = screen_h;
    anim_.snap_to(0.0f);
    build_layout();
    return true;
}

void Keyboard::shutdown() {
    keys_.clear();
}

void Keyboard::build_layout() {
    keys_.clear();
    const float row_w   = (float)screen_w_ - 2.0f * SIDE_PAD;

    auto place_row = [&](const char* row, int row_idx, int slot_count) {
        float total = slot_count * 121.0f + (slot_count - 1) * KEY_GAP;
        float lx    = SIDE_PAD + (row_w - total) * 0.5f;
        float ry    = KEY_AREA_Y + row_idx * (KEY_H + ROW_GAP);
        for (int i = 0; row[i]; i++) {
            Key k{};
            k.x = lx + i * (121.0f + KEY_GAP);
            k.y = ry;
            k.w = 121.0f;
            k.h = KEY_H;
            k.kind = KeyKind::CHAR;
            k.ch   = row[i];
            keys_.push_back(k);
        }
    };

    const char* r1 = (layer_ == Layer::SYMBOLS) ? ROW_TOP_S : ROW_TOP_L;
    const char* r2 = (layer_ == Layer::SYMBOLS) ? ROW_MID_S : ROW_MID_L;
    const char* r3 = (layer_ == Layer::SYMBOLS) ? ROW_BOT_S : ROW_BOT_L;

    place_row(ROW_DIGITS, 0, 10);
    place_row(r1,         1, 10);
    place_row(r2,         2, 9);


    {
        float total = 181.0f + 7 * 121.0f + 181.0f + 8 * KEY_GAP;
        float lx    = SIDE_PAD + (row_w - total) * 0.5f;
        float ry    = KEY_AREA_Y + 3 * (KEY_H + ROW_GAP);

        Key m{}; m.x = lx; m.y = ry; m.w = 181.0f; m.h = KEY_H;
        m.kind = (layer_ == Layer::SYMBOLS) ? KeyKind::LAYER : KeyKind::SHIFT;
        m.ch = 0;
        keys_.push_back(m);
        float cx = lx + 181.0f + KEY_GAP;
        for (int i = 0; r3[i]; i++) {
            Key k{};
            k.x = cx + i * (121.0f + KEY_GAP);
            k.y = ry;
            k.w = 121.0f;
            k.h = KEY_H;
            k.kind = KeyKind::CHAR;
            k.ch   = r3[i];
            keys_.push_back(k);
        }
        float bx = cx + 7 * (121.0f + KEY_GAP);
        Key bs{}; bs.x = bx; bs.y = ry; bs.w = 181.0f; bs.h = KEY_H;
        bs.kind = KeyKind::BACKSPACE; bs.ch = 0;
        keys_.push_back(bs);
    }


    {
        constexpr float TW = 200.0f;
        constexpr float CW = 220.0f;
        constexpr float SW = 470.0f;
        constexpr float DW = 220.0f;
        float total = TW + CW + SW + DW + 3 * KEY_GAP;
        float lx    = SIDE_PAD + (row_w - total) * 0.5f;
        float ry    = KEY_AREA_Y + 4 * (KEY_H + ROW_GAP);

        Key t{}; t.x = lx; t.y = ry; t.w = TW; t.h = KEY_H;
        t.kind = KeyKind::LAYER; t.ch = 0;
        keys_.push_back(t);
        Key c{};  c.x  = lx + TW + KEY_GAP;                       c.y  = ry; c.w  = CW; c.h  = KEY_H; c.kind  = KeyKind::CANCEL; c.ch  = 0; keys_.push_back(c);
        Key sp{}; sp.x = lx + TW + KEY_GAP + CW + KEY_GAP;        sp.y = ry; sp.w = SW; sp.h = KEY_H; sp.kind = KeyKind::SPACE;  sp.ch = 0; keys_.push_back(sp);
        Key d{};  d.x  = lx + TW + KEY_GAP + CW + KEY_GAP + SW + KEY_GAP; d.y = ry; d.w = DW; d.h = KEY_H; d.kind = KeyKind::DONE; d.ch = 0; keys_.push_back(d);
    }
}

int Keyboard::hit(int x, int y) const {

    float dy = (1.0f - std::clamp(anim_.value, 0.0f, 1.0f)) * (float)screen_h_;
    for (size_t i = 0; i < keys_.size(); i++) {
        const Key& k = keys_[i];
        if ((float)x >= k.x && (float)x < k.x + k.w
         && (float)y >= k.y + dy && (float)y < k.y + dy + k.h)
            return (int)i;
    }
    return -1;
}

void Keyboard::show(const std::string& title, const std::string& subtitle,
                    const std::string& initial, bool password_mask) {
    title_    = title;
    subtitle_ = subtitle;
    buffer_   = initial;
    mask_     = password_mask;
    shift_    = false;
    live_     = false;
    on_char_  = nullptr;
    layer_    = Layer::LETTERS;
    result_   = Result::ONGOING;
    pressed_  = -1;
    visible_  = true;
    build_layout();
    anim_.stiffness = 280.0f;
    anim_.damping   =  34.0f;
    anim_.set(1.0f);
}

void Keyboard::show_live(std::function<void(char)> on_char) {
    title_    = "";
    subtitle_ = "";
    buffer_   = "";
    mask_     = false;
    live_     = true;
    on_char_  = std::move(on_char);
    shift_    = false;
    layer_    = Layer::LETTERS;
    result_   = Result::ONGOING;
    pressed_  = -1;
    visible_  = true;
    build_layout();
    anim_.stiffness = 280.0f;
    anim_.damping   =  34.0f;
    anim_.set(1.0f);
}

void Keyboard::dismiss() {
    anim_.stiffness = 280.0f;
    anim_.damping   =  34.0f;
    anim_.set(0.0f);

}

Keyboard::Result Keyboard::consume_result() {
    Result r = result_;
    if (r != Result::ONGOING) result_ = Result::ONGOING;
    return r;
}

void Keyboard::tick(float dt) {
    anim_.tick(dt);


    if (visible_ && anim_.target == 0.0f && anim_.value < 0.005f) {
        visible_ = false;
    }
}

void Keyboard::render(Renderer& r, TextRenderer& title_text,
                      TextRenderer& body_text, TextRenderer& key_text) {
    if (anim_.value <= 0.005f) return;

    const float a       = std::clamp(anim_.value, 0.0f, 1.0f);
    const float dy      = (1.0f - a) * (float)screen_h_;
    const float opacity = a;
    const float sw      = (float)screen_w_;
    const float sh      = (float)screen_h_;

    r.begin_pass();
    const float field_x = FIELD_PAD_X;
    const float field_w = sw - 2.0f * FIELD_PAD_X;


    if (!live_) {

        r.draw_rect(0.0f, 0.0f, sw, sh, { 0.0f, 0.0f, 0.0f, 0.55f * opacity }, 0.0f);


        r.draw_rect(0.0f, MODAL_Y + dy, sw, sh - MODAL_Y,
                    { 0.07f, 0.07f, 0.10f, opacity }, 0.0f);


        r.draw_rect(field_x, FIELD_Y + dy, field_w, FIELD_H,
                    { 0.20f, 0.20f, 0.24f, opacity }, FIELD_H * 0.5f);
    }


    for (size_t i = 0; i < keys_.size(); i++) {
        const Key& k = keys_[i];
        huskyfe::Color top_c{ 0.22f, 0.22f, 0.26f, opacity };
        if (k.kind == KeyKind::SHIFT && shift_) top_c = { 0.30f, 0.60f, 0.95f, opacity };
        if (k.kind == KeyKind::DONE)            top_c = { 0.30f, 0.65f, 0.40f, opacity };
        if (k.kind == KeyKind::CANCEL)          top_c = { 0.50f, 0.30f, 0.30f, opacity };
        if ((int)i == pressed_) {
            top_c.r *= 0.78f; top_c.g *= 0.78f; top_c.b *= 0.78f;
        }
        huskyfe::Color bot_c{ top_c.r * 0.55f, top_c.g * 0.55f, top_c.b * 0.55f, top_c.a };
        r.draw_rect_gradient(k.x, k.y + dy, k.w, k.h, top_c, bot_c, 18.0f);
    }
    r.flush();


    const huskyfe::Color title_fg{ 1.0f, 1.0f, 1.0f, opacity };
    const huskyfe::Color dim_fg  { 0.7f, 0.7f, 0.78f, opacity };

    title_text.begin(r.xform_data());
    if (!title_.empty()) {
        float w = title_text.measure_width(title_.c_str());
        title_text.draw((sw - w) * 0.5f, TITLE_Y + dy, title_.c_str(), title_fg);
    }
    title_text.end();

    if (!subtitle_.empty()) {
        body_text.begin(r.xform_data());
        float w = body_text.measure_width(subtitle_.c_str());
        body_text.draw((sw - w) * 0.5f, SUBTITLE_Y + dy, subtitle_.c_str(), dim_fg);
        body_text.end();
    }


    title_text.begin(r.xform_data());
    if (!buffer_.empty()) {
        std::string display;
        if (mask_) display.assign(buffer_.size(), '*');
        else       display = buffer_;
        title_text.draw(field_x + 32.0f,
                        FIELD_Y + dy + FIELD_H * 0.5f + title_text.ascent() * 0.32f,
                        display.c_str(), title_fg);
    }
    title_text.end();


    key_text.begin(r.xform_data());
    char label[8];
    for (size_t i = 0; i < keys_.size(); i++) {
        const Key& k = keys_[i];
        switch (k.kind) {
            case KeyKind::CHAR:
                label[0] = (shift_ && k.ch >= 'a' && k.ch <= 'z')
                                ? (char)(k.ch - 'a' + 'A') : k.ch;
                label[1] = 0;
                break;
            case KeyKind::SHIFT:     std::snprintf(label, sizeof(label), "Shift");   break;
            case KeyKind::BACKSPACE: std::snprintf(label, sizeof(label), "<X");      break;
            case KeyKind::SPACE:     std::snprintf(label, sizeof(label), "space");   break;
            case KeyKind::DONE:      std::snprintf(label, sizeof(label), "Done");    break;
            case KeyKind::CANCEL:    std::snprintf(label, sizeof(label), "Cancel");  break;
            case KeyKind::LAYER:
                std::snprintf(label, sizeof(label),
                              layer_ == Layer::LETTERS ? "#+=" : "ABC");
                break;
        }
        float lw = key_text.measure_width(label);
        float lx = k.x + (k.w - lw) * 0.5f;
        float ly = k.y + dy + k.h * 0.5f + key_text.ascent() * 0.32f;
        key_text.draw(lx, ly, label, title_fg);
    }
    key_text.end();
}

bool Keyboard::on_touch_down(int x, int y) {
    if (!visible_) return false;
    pressed_ = hit(x, y);
    return true;
}

bool Keyboard::on_touch_move(int , int ) {
    return visible_;
}

bool Keyboard::on_touch_up(int x, int y) {
    if (!visible_) return false;
    int idx = hit(x, y);
    if (idx >= 0 && idx == pressed_) {
        const Key& k = keys_[(size_t)idx];
        switch (k.kind) {
            case KeyKind::CHAR: {
                char c = (shift_ && k.ch >= 'a' && k.ch <= 'z')
                            ? (char)(k.ch - 'a' + 'A') : k.ch;
                if (live_) { if (on_char_) on_char_(c); }
                else       { buffer_.push_back(c); }
                shift_ = false;
                break;
            }
            case KeyKind::SHIFT:
                shift_ = !shift_;
                break;
            case KeyKind::BACKSPACE:
                if (live_) { if (on_char_) on_char_('\b'); }
                else if (!buffer_.empty()) buffer_.pop_back();
                break;
            case KeyKind::SPACE:
                if (live_) { if (on_char_) on_char_(' '); }
                else       buffer_.push_back(' ');
                break;
            case KeyKind::DONE:
                if (live_) { if (on_char_) on_char_('\n'); }
                else { result_ = Result::ACCEPTED; dismiss(); }
                break;
            case KeyKind::CANCEL:
                if (live_) { dismiss(); }
                else { result_ = Result::CANCELLED; dismiss(); }
                break;
            case KeyKind::LAYER:
                layer_ = (layer_ == Layer::LETTERS) ? Layer::SYMBOLS : Layer::LETTERS;
                shift_ = false;
                build_layout();
                break;
        }
    }
    pressed_ = -1;
    return true;
}

}
