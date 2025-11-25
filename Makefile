FLAGS = $(shell pkg-config --libs --cflags wayland-client pixman-1 fcft)

all: pipebar

pipebar: pipebar.c protocols/*.h protocols/*.c
	gcc -o pipebar pipebar.c protocols/*.c $(FLAGS)

pipebar-debug: pipebar.c protocols/*.h protocols/*.c
	gcc -g -o pipebar-debug pipebar.c protocols/*.c $(FLAGS)

protocols/*.h:
	wayland-scanner client-header protocols/xdg-shell-stable.xml protocols/xdg-shell.h
	wayland-scanner client-header protocols/wlr-layer-shell-unstable-v1.xml protocols/wlr-layer-shell.h

protocols/*.c:
	wayland-scanner private-code protocols/xdg-shell-stable.xml protocols/xdg-shell.c
	wayland-scanner private-code protocols/wlr-layer-shell-unstable-v1.xml protocols/wlr-layer-shell.c

clean:
	rm -f pipebar pipebar-debug protocols/*.h protocols/*.c
