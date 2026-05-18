#pragma once
#include "GL.h"

#include <string>
#include <vector>

namespace huskyfe::icons {


std::string find_path(const std::string& name);


struct LoadedImage {
    std::vector<unsigned char> rgba;
    int w = 0;
    int h = 0;
};
bool load_png(const std::string& path, LoadedImage& out);
bool load_svg(const std::string& path, int target_size, LoadedImage& out);

bool load_image(const std::string& path, int target_size, LoadedImage& out);


class Atlas {
public:
    bool build(const std::vector<std::string>& icon_names_or_paths,
               int slot_size, int cols, int rows);
    void shutdown();
    GLuint texture()   const { return tex_; }
    int    slot_size() const { return slot_size_; }
    int    cols()      const { return cols_; }
    int    rows()      const { return rows_; }
    void   uv_rect(int idx, float& u0, float& v0, float& u1, float& v1) const;
    bool   has_slot(int idx) const { return idx >= 0 && idx < (int)filled_.size() && filled_[idx]; }

private:
    GLuint tex_       = 0;
    int    slot_size_ = 0;
    int    cols_      = 0;
    int    rows_      = 0;
    int    atlas_w_   = 0;
    int    atlas_h_   = 0;
    std::vector<bool> filled_;
};

}
