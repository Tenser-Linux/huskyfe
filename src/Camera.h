#pragma once
#include <cstdint>
#include <string>

namespace huskyfe::camera {

struct ShmHeader {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixfmt;
    uint32_t frame_seq;
    uint64_t timestamp_ns;
};


bool init();
void shutdown();

bool        is_ready();
ShmHeader   header();
const uint8_t* frame_data();
uint32_t    last_seen_seq();
void        mark_seq(uint32_t s);


bool spawn_daemon(const std::string& tsv_path);
void kill_daemon();
bool daemon_alive();


bool is_busy();


void cleanup_orphan_daemon();

}
