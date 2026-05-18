

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include "wayland-util.h"

extern const struct wl_interface wl_seat_interface;
extern const struct wl_interface wl_surface_interface;
extern const struct wl_interface zwp_text_input_v3_interface;

static const struct wl_interface *text_input_unstable_v3_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	&wl_surface_interface,
	&wl_surface_interface,
	&zwp_text_input_v3_interface,
	&wl_seat_interface,
};

static const struct wl_message zwp_text_input_v3_requests[] = {
	{ "destroy", "", text_input_unstable_v3_types + 0 },
	{ "enable", "", text_input_unstable_v3_types + 0 },
	{ "disable", "", text_input_unstable_v3_types + 0 },
	{ "set_surrounding_text", "sii", text_input_unstable_v3_types + 0 },
	{ "set_text_change_cause", "u", text_input_unstable_v3_types + 0 },
	{ "set_content_type", "uu", text_input_unstable_v3_types + 0 },
	{ "set_cursor_rectangle", "iiii", text_input_unstable_v3_types + 0 },
	{ "commit", "", text_input_unstable_v3_types + 0 },
};

static const struct wl_message zwp_text_input_v3_events[] = {
	{ "enter", "o", text_input_unstable_v3_types + 4 },
	{ "leave", "o", text_input_unstable_v3_types + 5 },
	{ "preedit_string", "?sii", text_input_unstable_v3_types + 0 },
	{ "commit_string", "?s", text_input_unstable_v3_types + 0 },
	{ "delete_surrounding_text", "uu", text_input_unstable_v3_types + 0 },
	{ "done", "u", text_input_unstable_v3_types + 0 },
};

WL_EXPORT const struct wl_interface zwp_text_input_v3_interface = {
	"zwp_text_input_v3", 1,
	8, zwp_text_input_v3_requests,
	6, zwp_text_input_v3_events,
};

static const struct wl_message zwp_text_input_manager_v3_requests[] = {
	{ "destroy", "", text_input_unstable_v3_types + 0 },
	{ "get_text_input", "no", text_input_unstable_v3_types + 6 },
};

WL_EXPORT const struct wl_interface zwp_text_input_manager_v3_interface = {
	"zwp_text_input_manager_v3", 1,
	2, zwp_text_input_manager_v3_requests,
	0, NULL,
};
