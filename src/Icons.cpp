#include "Icons.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "third_party/stb_image.h"

#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvgrast.h"

#include <unistd.h>
#include <cstdio>
#include <cmath>
#include <cstring>

namespace huskyfe::icons {
namespace {


const char* kSizesXY[] = { "256x256", "192x192", "128x128", "96x96", "72x72", "64x64", "48x48", "32x32" };
const char* kSizesN[]  = { "256", "192", "128", "96", "64", "48", "32", "22", "16" };


const char* kThemes[] = {
    "Papirus", "Papirus-Dark", "Papirus-Light",
    "breeze",  "Breeze",
    "Adwaita", "gnome", "Yaru",
    "hicolor",
};
const char* kBases[]  = { "/usr/share/icons", "/usr/local/share/icons" };
const char* kPixmaps[]= { "/usr/share/pixmaps", "/usr/local/share/pixmaps" };

bool readable(const char* p) { return access(p, R_OK) == 0; }


void resize_rgba(const unsigned char* src, int sw, int sh,
                 unsigned char* dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        int   y0 = (int)std::floor(fy);
        int   y1 = y0 + 1;
        float wy = fy - (float)y0;
        if (y0 < 0)   y0 = 0;
        if (y0 >= sh) y0 = sh - 1;
        if (y1 < 0)   y1 = 0;
        if (y1 >= sh) y1 = sh - 1;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            int   x0 = (int)std::floor(fx);
            int   x1 = x0 + 1;
            float wx = fx - (float)x0;
            if (x0 < 0)   x0 = 0;
            if (x0 >= sw) x0 = sw - 1;
            if (x1 < 0)   x1 = 0;
            if (x1 >= sw) x1 = sw - 1;
            for (int c = 0; c < 4; c++) {
                float p00 = src[(y0 * sw + x0) * 4 + c];
                float p01 = src[(y0 * sw + x1) * 4 + c];
                float p10 = src[(y1 * sw + x0) * 4 + c];
                float p11 = src[(y1 * sw + x1) * 4 + c];
                float a = p00 * (1.0f - wx) + p01 * wx;
                float b = p10 * (1.0f - wx) + p11 * wx;
                dst[(y * dw + x) * 4 + c] = (unsigned char)(a * (1.0f - wy) + b * wy);
            }
        }
    }
}

}

std::string find_path(const std::string& name) {
    if (name.empty()) return {};
    if (name[0] == '/') return readable(name.c_str()) ? name : std::string{};


    std::string stem = name;
    auto dot = stem.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = stem.substr(dot);
        if (ext == ".png" || ext == ".svg" || ext == ".xpm") stem = stem.substr(0, dot);
    }

    char buf[1024];


    static const char* kCats[] = {
        "apps", "devices", "categories", "places", "status",
    };


    for (auto theme : kThemes) {
        for (auto base : kBases) {
            for (auto cat : kCats) {
                snprintf(buf, sizeof(buf), "%s/%s/scalable/%s/%s.svg",
                         base, theme, cat, stem.c_str());
                if (readable(buf)) return buf;
            }
        }
    }
    for (auto theme : kThemes) {
        for (auto base : kBases) {

            for (auto sz : kSizesXY) {
                for (auto cat : kCats) {
                    snprintf(buf, sizeof(buf), "%s/%s/%s/%s/%s.png",
                             base, theme, sz, cat, stem.c_str());
                    if (readable(buf)) return buf;
                    snprintf(buf, sizeof(buf), "%s/%s/%s/%s/%s.svg",
                             base, theme, sz, cat, stem.c_str());
                    if (readable(buf)) return buf;
                }
            }

            for (auto sz : kSizesN) {
                for (auto cat : kCats) {
                    snprintf(buf, sizeof(buf), "%s/%s/%s/%s/%s.png",
                             base, theme, cat, sz, stem.c_str());
                    if (readable(buf)) return buf;
                    snprintf(buf, sizeof(buf), "%s/%s/%s/%s/%s.svg",
                             base, theme, cat, sz, stem.c_str());
                    if (readable(buf)) return buf;
                }
            }
        }
    }
    for (auto pix : kPixmaps) {
        snprintf(buf, sizeof(buf), "%s/%s.png", pix, stem.c_str());
        if (readable(buf)) return buf;
        snprintf(buf, sizeof(buf), "%s/%s.svg", pix, stem.c_str());
        if (readable(buf)) return buf;
    }
    return {};
}

bool load_png(const std::string& path, LoadedImage& out) {
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!px) {
        fprintf(stderr, "huskyfe: stbi_load %s failed: %s\n",
                path.c_str(), stbi_failure_reason());
        return false;
    }
    out.w = w;
    out.h = h;
    out.rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

bool load_svg(const std::string& path, int target_size, LoadedImage& out) {
    NSVGimage* img = nsvgParseFromFile(path.c_str(), "px", 96.0f);
    if (!img || img->width <= 0.0f || img->height <= 0.0f) {
        if (img) nsvgDelete(img);
        fprintf(stderr, "huskyfe: nsvgParseFromFile %s failed\n", path.c_str());
        return false;
    }
    float longest = std::max(img->width, img->height);
    float scale   = (float)target_size / longest;
    int   w       = (int)(img->width  * scale + 0.5f);
    int   h       = (int)(img->height * scale + 0.5f);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    out.w = w;
    out.h = h;
    out.rgba.assign((size_t)w * h * 4, 0);

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(img); return false; }

    float tx = (target_size - w) * 0.5f;
    float ty = (target_size - h) * 0.5f;
    (void)tx; (void)ty;
    nsvgRasterize(rast, img, 0.0f, 0.0f, scale,
                  out.rgba.data(), w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    return true;
}

bool load_image(const std::string& path, int target_size, LoadedImage& out) {
    if (path.size() >= 4) {
        const char* tail = path.c_str() + path.size() - 4;
        if (strcasecmp(tail, ".svg") == 0) return load_svg(path, target_size, out);
    }
    return load_png(path, out);
}

bool Atlas::build(const std::vector<std::string>& names, int slot_size, int cols, int rows) {
    slot_size_ = slot_size;
    cols_      = cols;
    rows_      = rows;
    atlas_w_   = cols * slot_size;
    atlas_h_   = rows * slot_size;
    int total  = cols * rows;
    filled_.assign((size_t)total, false);

    std::vector<unsigned char> atlas((size_t)atlas_w_ * atlas_h_ * 4, 0);
    std::vector<unsigned char> resized((size_t)slot_size * slot_size * 4);

    int found = 0;
    for (int i = 0; i < (int)names.size() && i < total; i++) {
        std::string path = find_path(names[i]);
        if (path.empty()) continue;
        LoadedImage img;
        if (!load_image(path, slot_size, img)) continue;

        const unsigned char* src;
        int sw, sh;
        if (img.w == slot_size && img.h == slot_size) {
            src = img.rgba.data(); sw = img.w; sh = img.h;
        } else {
            resize_rgba(img.rgba.data(), img.w, img.h,
                        resized.data(),  slot_size, slot_size);
            src = resized.data(); sw = slot_size; sh = slot_size;
        }

        int slot_col = i % cols;
        int slot_row = i / cols;
        int dst_x    = slot_col * slot_size;
        int dst_y    = slot_row * slot_size;
        for (int y = 0; y < sh; y++) {
            unsigned char* d = &atlas[((dst_y + y) * atlas_w_ + dst_x) * 4];
            const unsigned char* s = &src[y * sw * 4];
            memcpy(d, s, (size_t)sw * 4);
        }
        filled_[i] = true;
        found++;
    }
    fprintf(stderr, "huskyfe: icon atlas %dx%d, %d/%d slots filled\n",
            atlas_w_, atlas_h_, found, (int)names.size());

    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas_w_, atlas_h_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, atlas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

    return gl::check_error("Atlas::build");
}

void Atlas::shutdown() {
    if (tex_) glDeleteTextures(1, &tex_);
    tex_ = 0;
}

void Atlas::uv_rect(int idx, float& u0, float& v0, float& u1, float& v1) const {
    int col = idx % cols_;
    int row = idx / cols_;
    u0 = (float)(col * slot_size_)         / (float)atlas_w_;
    v0 = (float)(row * slot_size_)         / (float)atlas_h_;
    u1 = (float)((col + 1) * slot_size_)   / (float)atlas_w_;
    v1 = (float)((row + 1) * slot_size_)   / (float)atlas_h_;
}

}
