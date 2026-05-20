# huskyfe — build natively on the phone (host glibc).
#
# Push:   scp -r src Makefile root@phone:/root/huskyfe/
# Build:  cd /root/huskyfe && make
# Run:    pkill -9 -f 'wcomp|cage|phoc|phosh|weston|modetest' 2>/dev/null
#         chroot /var/lib/machines/halium /usr/local/bin/glproxy-srv > /tmp/glp.log 2>&1 &
#         ./huskyfe

CXX      := g++
CC       := gcc
WLPROTO  := /usr/share/wayland-protocols
WLSCAN   := wayland-scanner
PKGS     := wayland-server xkbcommon freetype2 dbus-1
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs   $(PKGS) 2>/dev/null)

INCS     := -I/usr/include/libdrm -Isrc/protocol $(PKG_CFLAGS)
CXXFLAGS := -O2 -g -std=c++20 -Wall -Wextra -pthread $(INCS)
CFLAGS   := -O2 -g -Wall -Wextra $(INCS)
GLPROXY_LIB ?= /usr/local/lib/glproxy
LDFLAGS  := -L$(GLPROXY_LIB) -Wl,-rpath,$(GLPROXY_LIB) -rdynamic -lGLESv2 -lEGL -ldrm $(PKG_LIBS)

GIT_SHA ?= $(shell git rev-parse HEAD 2>/dev/null || cat src/version.txt 2>/dev/null || echo unknown)
CXXFLAGS += -DHUSKYFE_GIT_SHA=\"$(GIT_SHA)\"

# Generated wayland-protocol files.
GEN_HDR := src/protocol/xdg-shell-server-protocol.h \
           src/protocol/linux-dmabuf-unstable-v1-server-protocol.h \
           src/protocol/viewporter-server-protocol.h \
           src/protocol/text-input-unstable-v3-server-protocol.h
GEN_SRC := src/protocol/xdg-shell-protocol.c \
           src/protocol/linux-dmabuf-unstable-v1-protocol.c \
           src/protocol/viewporter-protocol.c \
           src/protocol/text-input-unstable-v3-protocol.c

CPP_SRCS := src/main.cpp src/Input.cpp src/Status.cpp src/Apps.cpp \
            src/Icons.cpp src/Background.cpp \
            src/Wifi.cpp src/Bluetooth.cpp src/Keyboard.cpp src/Blur.cpp \
            src/WaylandHost.cpp src/Notifications.cpp src/Haptics.cpp \
            src/Flashlight.cpp src/Camera.cpp src/Updater.cpp \
            src/GL.cpp src/Renderer.cpp src/ImageRenderer.cpp src/Text.cpp
C_SRCS   := $(GEN_SRC)

CPP_OBJS := $(CPP_SRCS:.cpp=.o)
C_OBJS   := $(C_SRCS:.c=.o)

HDRS := src/Input.h src/GL.h src/Renderer.h src/Spring.h \
        src/Text.h  src/Status.h src/Apps.h \
        src/ImageRenderer.h src/Icons.h src/Background.h \
        src/Wifi.h src/Bluetooth.h src/Keyboard.h src/Blur.h \
        src/WaylandHost.h src/Notifications.h src/Haptics.h \
        src/Flashlight.h $(GEN_HDR) \
        src/third_party/stb_image.h \
        src/third_party/nanosvg.h src/third_party/nanosvgrast.h

huskyfe: $(CPP_OBJS) $(C_OBJS)
	$(CXX) -pthread -o $@ $(CPP_OBJS) $(C_OBJS) $(LDFLAGS)



%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c $(HDRS)
	$(CC)  $(CFLAGS)   -c -o $@ $<

# Generated wayland protocol bindings.
src/protocol/xdg-shell-server-protocol.h: $(WLPROTO)/stable/xdg-shell/xdg-shell.xml
	mkdir -p src/protocol
	$(WLSCAN) server-header $< $@

src/protocol/xdg-shell-protocol.c: $(WLPROTO)/stable/xdg-shell/xdg-shell.xml
	mkdir -p src/protocol
	$(WLSCAN) public-code $< $@

src/protocol/linux-dmabuf-unstable-v1-server-protocol.h: \
        $(WLPROTO)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
	mkdir -p src/protocol
	$(WLSCAN) server-header $< $@

src/protocol/linux-dmabuf-unstable-v1-protocol.c: \
        $(WLPROTO)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
	mkdir -p src/protocol
	$(WLSCAN) public-code $< $@

src/protocol/viewporter-server-protocol.h: \
        $(WLPROTO)/stable/viewporter/viewporter.xml
	mkdir -p src/protocol
	$(WLSCAN) server-header $< $@

src/protocol/viewporter-protocol.c: \
        $(WLPROTO)/stable/viewporter/viewporter.xml
	mkdir -p src/protocol
	$(WLSCAN) public-code $< $@

src/protocol/text-input-unstable-v3-server-protocol.h: \
        $(WLPROTO)/unstable/text-input/text-input-unstable-v3.xml
	mkdir -p src/protocol
	$(WLSCAN) server-header $< $@

src/protocol/text-input-unstable-v3-protocol.c: \
        $(WLPROTO)/unstable/text-input/text-input-unstable-v3.xml
	mkdir -p src/protocol
	$(WLSCAN) public-code $< $@

clean:
	rm -f huskyfe $(CPP_OBJS) $(C_OBJS)
	rm -rf src/protocol

.PHONY: clean
