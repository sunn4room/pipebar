#include <fcft/fcft.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct bar {
    const char* const version;

    const char* bg;
    const char* fg;
    const char* font;
};

enum {
    NO_ERROR,
    INNER_ERROR,
    RUNTIME_ERROR,
};

static void quit(struct bar* bar, const int code, const char* restrict fmt, ...)
{
    if (fmt != NULL) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }

    fcft_fini();
    exit(code);
}

static void pipe_init(struct bar* bar)
{
    struct stat stdin_stat;
    fstat(STDIN_FILENO, &stdin_stat);
    if (!S_ISFIFO(stdin_stat.st_mode)) {
        quit(bar, NO_ERROR,
            "pbar is a wayland statusbar that renders plain text from stdin and prints mouse event action to stdout.\n"
            "pbar version: %s\n"
            "pbar usage: producer | pbar [options] | consumer\n"
            "\n"
            "options are:\n"
            "        -B rrggbb[aa]   set default background color (000000ff)\n"
            "        -F rrggbb[aa]   set default foreground color (ffffffff)\n"
            "        -T font[:k=v]   set default font (monospace)\n"
            "\n",
            bar->version);
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
}

static void init(struct bar* bar)
{
    pipe_init(bar);

    fcft_init(FCFT_LOG_COLORIZE_AUTO, false, FCFT_LOG_CLASS_ERROR);
    if (!(fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING)) {
        quit(bar, INNER_ERROR, "fcft does not support text-run shaping.\n");
    }
}

int main(int argc, char** argv)
{
    struct bar bar = {
        .version = "0.1",
        .bg = "000000ff",
        .fg = "ffffffff",
        .font = "monospace",
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-B") == 0) {
            if (++i < argc) bar.bg = argv[i];
        } else if (strcmp(argv[i], "-F") == 0) {
            if (++i < argc) bar.fg = argv[i];
        } else if (strcmp(argv[i], "-T") == 0) {
            if (++i < argc) bar.font = argv[i];
        }
    }

    init(&bar);

    return NO_ERROR;
}
