#pragma once

namespace huskyfe::haptics {

bool init(const char* dev = "/dev/input/event2");
void shutdown();
bool ok();


bool play(int duration_ms, int strength, int period_ms = 20,
          int attack_ms = 0, int attack_level = 0);


bool play_tone(int freq_hz, int duration_ms, int strength = 24000);

}
