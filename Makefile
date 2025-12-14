all: pipebar

pipebar: pipebar.c protocols/*.h protocols/*.c
	gcc -o pipebar pipebar.c protocols/*.c `pkg-config --libs --cflags wayland-client pixman-1 fcft`

pipebar-debug: pipebar.c protocols/*.h protocols/*.c
	gcc -g -o pipebar-debug pipebar.c protocols/*.c `pkg-config --libs --cflags wayland-client pixman-1 fcft`

protocols/*.h: protocols/*.xml
	wayland-scanner client-header protocols/xdg-shell-stable.xml protocols/xdg-shell.h
	wayland-scanner client-header protocols/wlr-layer-shell-unstable-v1.xml protocols/wlr-layer-shell.h
	wayland-scanner client-header protocols/viewporter-stable.xml protocols/viewporter.h
	wayland-scanner client-header protocols/fractional-scale-staging-v1.xml protocols/fractional-scale.h

protocols/*.c: protocols/*.xml
	wayland-scanner private-code protocols/xdg-shell-stable.xml protocols/xdg-shell.c
	wayland-scanner private-code protocols/wlr-layer-shell-unstable-v1.xml protocols/wlr-layer-shell.c
	wayland-scanner private-code protocols/viewporter-stable.xml protocols/viewporter.c
	wayland-scanner private-code protocols/fractional-scale-staging-v1.xml protocols/fractional-scale.c

clean:
	rm -f pipebar pipebar-debug protocols/*.h protocols/*.c
