

#ifndef VIEWPORTER_SERVER_PROTOCOL_H
#define VIEWPORTER_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;


struct wl_surface;
struct wp_viewport;
struct wp_viewporter;

#ifndef WP_VIEWPORTER_INTERFACE
#define WP_VIEWPORTER_INTERFACE


extern const struct wl_interface wp_viewporter_interface;
#endif
#ifndef WP_VIEWPORT_INTERFACE
#define WP_VIEWPORT_INTERFACE


extern const struct wl_interface wp_viewport_interface;
#endif

#ifndef WP_VIEWPORTER_ERROR_ENUM
#define WP_VIEWPORTER_ERROR_ENUM
enum wp_viewporter_error {


	WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS = 0,
};
#endif


struct wp_viewporter_interface {


	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);


	void (*get_viewport)(struct wl_client *client,
			     struct wl_resource *resource,
			     uint32_t id,
			     struct wl_resource *surface);
};


#define WP_VIEWPORTER_DESTROY_SINCE_VERSION 1


#define WP_VIEWPORTER_GET_VIEWPORT_SINCE_VERSION 1

#ifndef WP_VIEWPORT_ERROR_ENUM
#define WP_VIEWPORT_ERROR_ENUM
enum wp_viewport_error {


	WP_VIEWPORT_ERROR_BAD_VALUE = 0,


	WP_VIEWPORT_ERROR_BAD_SIZE = 1,


	WP_VIEWPORT_ERROR_OUT_OF_BUFFER = 2,


	WP_VIEWPORT_ERROR_NO_SURFACE = 3,
};
#endif


struct wp_viewport_interface {


	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);


	void (*set_source)(struct wl_client *client,
			   struct wl_resource *resource,
			   wl_fixed_t x,
			   wl_fixed_t y,
			   wl_fixed_t width,
			   wl_fixed_t height);


	void (*set_destination)(struct wl_client *client,
				struct wl_resource *resource,
				int32_t width,
				int32_t height);
};


#define WP_VIEWPORT_DESTROY_SINCE_VERSION 1


#define WP_VIEWPORT_SET_SOURCE_SINCE_VERSION 1


#define WP_VIEWPORT_SET_DESTINATION_SINCE_VERSION 1

#ifdef  __cplusplus
}
#endif

#endif
