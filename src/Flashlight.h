#pragma once

namespace huskyfe::flashlight {


bool init();


void shutdown();

bool ok();
bool is_on();


bool set(bool on, int level = 0x40);


bool set_level(int level);

}
