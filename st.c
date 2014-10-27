/* See LICENSE for licence details. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <wchar.h>

#include "helpers.h"
#include "libsuckterm.h"

char* argv0;

#if defined(__linux)

# include <pty.h>

#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif


/* Arbitrary sizes */
#define ESC_BUF_SIZ   (128*UTF_SIZ)
#define ESC_ARG_SIZ   16
#define STR_BUF_SIZ   ESC_BUF_SIZ
#define STR_ARG_SIZ   ESC_ARG_SIZ


/* macros */
#define VT102ID "\033[?6c"

enum cursor_movement {
    CURSOR_SAVE,
    CURSOR_LOAD
};

enum cursor_state {
    CURSOR_DEFAULT = 0,
    CURSOR_WRAPNEXT = 1,
    CURSOR_ORIGIN = 2
};

enum charset {
    CS_GRAPHIC0,
    CS_GRAPHIC1,
    CS_UK,
    CS_USA,
    CS_MULTI,
    CS_GER,
    CS_FIN
};

enum escape_state {
    ESC_START = 1,
    ESC_CSI = 2,
    ESC_STR = 4, /* DSC, OSC, PM, APC */
            ESC_ALTCHARSET = 8,
    ESC_STR_END = 16, /* a final string was encountered */
            ESC_TEST = 32, /* Enter in test mode */
};

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode>] */
typedef struct {
    char buf[ESC_BUF_SIZ];
    /* raw string */
    int len;
    /* raw string length */
    char priv;
    int arg[ESC_ARG_SIZ];
    int narg;
    /* nb of args */
    char mode;
} CSIEscape;

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct {
    char type;
    /* ESC type ... */
    char buf[STR_BUF_SIZ];
    /* raw string */
    int len;
    /* raw string length */
    char* args[STR_ARG_SIZ];
    int narg;              /* nb of args */
} STREscape;

// pty.c

static void csihandle(void);
static void csiparse(void);
static void csireset(void);
static void strhandle(void);
static void strparse(void);
static void strreset(void);

static void tclearregion(int, int, int, int);
static void tcursor(int);
static void tdeletechar(int);
static void tdeleteline(int);
static void tinsertblank(int);
static void tinsertblankline(int);
static void tmoveto(int, int);
static void tmoveato(int x, int y);
static void tnewline(int);
static void tputtab(bool);
static void tputc(char*, int);
static void treset();
static int tresize(int, int);
static void tscrollup(int, int);
static void tscrolldown(int, int);
static void tsetchar(char*, Cell*, int, int);
static void tsetscroll(int, int);
static void tswapscreen(void);
static void tsetdirt(int, int);
static void tsetmode(bool, bool, int*, int);
static void techo(char*, int);
static long tdefcolor(int*, int*, int);
static void tselcs(void);
static void tdeftran(char);

/* Globals */
Term term;
static CSIEscape csiescseq;
static STREscape strescseq;
int cmdfd;
/* button event on startup: 3 = release */

static void csidump(void) {
    int i;
    uint c;

    fprintf(stderr, "ESC[");
    for (i = 0; i < csiescseq.len; i++) {
        c = csiescseq.buf[i] & 0xff;
        if (isprint(c)) {
            fputc(c, stderr);
        } else if (c == '\n') {
            fprintf(stderr, "(\\n)");
        } else if (c == '\r') {
            fprintf(stderr, "(\\r)");
        } else if (c == 0x1b) {
            fprintf(stderr, "(\\e)");
        } else {
            fprintf(stderr, "(%02x)", c);
        }
    }
    fputc('\n', stderr);
}

static void strdump(void) {
    int i;
    uint c;

    fprintf(stderr, "ESC%c", strescseq.type);
    for (i = 0; i < strescseq.len; i++) {
        c = strescseq.buf[i] & 0xff;
        if (c == '\0') {
            return;
        } else if (isprint(c)) {
            fputc(c, stderr);
        } else if (c == '\n') {
            fprintf(stderr, "(\\n)");
        } else if (c == '\r') {
            fprintf(stderr, "(\\r)");
        } else if (c == 0x1b) {
            fprintf(stderr, "(\\e)");
        } else {
            fprintf(stderr, "(%02x)", c);
        }
    }
    fprintf(stderr, "ESC\\\n");
}

#include "ptyutils.h"

void ttyread(void) {
    static char buf[BUFSIZ];
    static int buflen = 0;
    char* ptr;
    char s[UTF_SIZ];
    int charsize; /* size of utf8 char in bytes */
    long utf8c;
    int ret;

    /* append read bytes to unprocessed bytes */
    if ((ret = read(cmdfd, buf + buflen, LEN(buf) - buflen)) < 0) {
        die("Couldn't read from shell: %s\n", SERRNO);
    }

    /* process every complete utf8 char */
    buflen += ret;
    ptr = buf;
    while (buflen >= UTF_SIZ || isfullutf8(ptr, buflen)) {
        charsize = utf8decode(ptr, &utf8c);
        utf8encode(&utf8c, s);
        tputc(s, charsize);
        ptr += charsize;
        buflen -= charsize;
    }

    /* keep any uncomplete utf8 char for the next call */
    memmove(buf, ptr, buflen);
}

void ttywrite(const char* s, size_t n) {
    if (xwrite(cmdfd, s, n) == -1) {
        die("write error on tty: %s\n", SERRNO);
    }
}

void ttysend(char* s, size_t n) {
    ttywrite(s, n);
    if (IS_SET(MODE_ECHO)) {
        techo(s, n);
    }
}

void ttyresize(void) {
    struct winsize w;

    w.ws_row = term.row;
    w.ws_col = term.col;
    w.ws_xpixel = term.tw;
    w.ws_ypixel = term.th;
    if (ioctl(cmdfd, TIOCSWINSZ, &w) < 0) {
        fprintf(stderr, "Couldn't set window size: %s\n", SERRNO);
    }
}

void tsetdirt(int top, int bot) {
    int i;

    LIMIT(top, 0, term.row - 1);
    LIMIT(bot, 0, term.row - 1);

    for (i = top; i <= bot; i++) {
        term.dirty[i] = 1;
    }
}

void tfulldirt(void) {
    tsetdirt(0, term.row - 1);
}

void tcursor(int mode) {
    static TCursor c[2];
    bool alt = IS_SET(MODE_ALTSCREEN);

    if (mode == CURSOR_SAVE) {
        c[alt] = term.c;
    } else if (mode == CURSOR_LOAD) {
        term.c = c[alt];
        tmoveto(c[alt].x, c[alt].y);
    }
}

void treset() {
    uint i;

    term.c = (TCursor){ {
            .mode = ATTR_NULL,
            .fg = term.defaultfg,
            .bg = term.defaultbg,
    }, .x = 0, .y = 0, .state = CURSOR_DEFAULT };

    memset(term.tabs, 0, term.col * sizeof(*term.tabs));
    for (i = term.tabspaces; i < term.col; i += term.tabspaces) {
        term.tabs[i] = 1;
    }
    term.top = 0;
    term.bot = term.row - 1;
    term.mode = MODE_WRAP;
    memset(term.trantbl, sizeof(term.trantbl), CS_USA);
    term.charset = 0;

    tclearregion(0, 0, term.col - 1, term.row - 1);
    tmoveto(0, 0);
    tcursor(CURSOR_SAVE);
}

void tnew(int col, int row, unsigned int defaultfg, unsigned int defaultbg, unsigned int tabspaces) {
    term = (Term){
            .defaultfg = defaultfg,
            .defaultbg = defaultbg,
            .tabspaces = tabspaces,
            .c = { .attr = { .fg = defaultfg, .bg = defaultbg, }, },
    };
    tresize(col, row);
    treset();
}

void tswapscreen(void) {
    Line* tmp = term.line;

    term.line = term.alt;
    term.alt = tmp;
    term.mode ^= MODE_ALTSCREEN;
    tfulldirt();
}

void tscrolldown(int orig, int n) {
    int i;
    Line temp;

    LIMIT(n, 0, term.bot - orig + 1);

    tclearregion(0, term.bot - n + 1, term.col - 1, term.bot);

    for (i = term.bot; i >= orig + n; i--) {
        temp = term.line[i];
        term.line[i] = term.line[i - n];
        term.line[i - n] = temp;

        term.dirty[i] = 1;
        term.dirty[i - n] = 1;
    }
}

void tscrollup(int orig, int n) {
    int i;
    Line temp;
    LIMIT(n, 0, term.bot - orig + 1);

    tclearregion(0, orig, term.col - 1, orig + n - 1);

    for (i = orig; i <= term.bot - n; i++) {
        temp = term.line[i];
        term.line[i] = term.line[i + n];
        term.line[i + n] = temp;

        term.dirty[i] = 1;
        term.dirty[i + n] = 1;
    }
}

void tnewline(int first_col) {
    int y = term.c.y;

    if (y == term.bot) {
        tscrollup(term.top, 1);
    } else {
        y++;
    }
    tmoveto(first_col ? 0 : term.c.x, y);
}

void csiparse(void) {
    char* p = csiescseq.buf, * np;
    long int v;

    csiescseq.narg = 0;
    if (*p == '?') {
        csiescseq.priv = 1;
        p++;
    }

    csiescseq.buf[csiescseq.len] = '\0';
    while (p < csiescseq.buf + csiescseq.len) {
        np = NULL;
        v = strtol(p, &np, 10);
        if (np == p) {
            v = 0;
        }
        if (v == LONG_MAX || v == LONG_MIN) {
            v = -1;
        }
        csiescseq.arg[csiescseq.narg++] = v;
        p = np;
        if (*p != ';' || csiescseq.narg == ESC_ARG_SIZ) {
            break;
        }
        p++;
    }
    csiescseq.mode = *p;
}

/* for absolute user moves, when decom is set */
void tmoveato(int x, int y) {
    tmoveto(x, y + ((term.c.state & CURSOR_ORIGIN) ? term.top : 0));
}

void tmoveto(int x, int y) {
    int miny, maxy;

    if (term.c.state & CURSOR_ORIGIN) {
        miny = term.top;
        maxy = term.bot;
    } else {
        miny = 0;
        maxy = term.row - 1;
    }
    LIMIT(x, 0, term.col - 1);
    LIMIT(y, miny, maxy);
    term.c.state &= ~CURSOR_WRAPNEXT;
    term.c.x = x;
    term.c.y = y;
}

void tsetchar(char* c, Cell* attr, int x, int y) {
    static char* vt100_0[62] = { /* 0x41 - 0x7e */
            "↑", "↓", "→", "←", "█", "▚", "☃", /* A - G */
            0, 0, 0, 0, 0, 0, 0, 0, /* H - O */
            0, 0, 0, 0, 0, 0, 0, 0, /* P - W */
            0, 0, 0, 0, 0, 0, 0, " ", /* X - _ */
            "◆", "▒", "␉", "␌", "␍", "␊", "°", "±", /* ` - g */
            "␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺", /* h - o */
            "⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", /* p - w */
            "│", "≤", "≥", "π", "≠", "£", "·", /* x - ~ */
    };

    /*
     * The table is proudly stolen from rxvt.
     */
    if (attr->mode & ATTR_GFX) {
        if (c[0] >= 0x41 && c[0] <= 0x7e
                && vt100_0[c[0] - 0x41]) {
            c = vt100_0[c[0] - 0x41];
        }
    }

    if (term.line[y][x].mode & ATTR_WIDE) {
        if (x + 1 < term.col) {
            term.line[y][x + 1].c[0] = ' ';
            term.line[y][x + 1].mode &= ~ATTR_WDUMMY;
        }
    } else if (term.line[y][x].mode & ATTR_WDUMMY) {
        term.line[y][x - 1].c[0] = ' ';
        term.line[y][x - 1].mode &= ~ATTR_WIDE;
    }

    term.dirty[y] = 1;
    term.line[y][x] = *attr;
    memcpy(term.line[y][x].c, c, UTF_SIZ);
}

void tclearregion(int x1, int y1, int x2, int y2) {
    int x, y, temp;

    if (x1 > x2) {
        temp = x1, x1 = x2, x2 = temp;
    }
    if (y1 > y2) {
        temp = y1, y1 = y2, y2 = temp;
    }

    LIMIT(x1, 0, term.col - 1);
    LIMIT(x2, 0, term.col - 1);
    LIMIT(y1, 0, term.row - 1);
    LIMIT(y2, 0, term.row - 1);

    for (y = y1; y <= y2; y++) {
        term.dirty[y] = 1;
        for (x = x1; x <= x2; x++) {
            term.line[y][x] = term.c.attr;
            memcpy(term.line[y][x].c, " ", 2);
        }
    }
}

void tdeletechar(int n) {
    int src = term.c.x + n;
    int dst = term.c.x;
    int size = term.col - src;

    term.dirty[term.c.y] = 1;

    if (src >= term.col) {
        tclearregion(term.c.x, term.c.y, term.col - 1, term.c.y);
        return;
    }

    memmove(&term.line[term.c.y][dst], &term.line[term.c.y][src],
            size * sizeof(Cell));
    tclearregion(term.col - n, term.c.y, term.col - 1, term.c.y);
}

void tinsertblank(int n) {
    int src = term.c.x;
    int dst = src + n;
    int size = term.col - dst;

    term.dirty[term.c.y] = 1;

    if (dst >= term.col) {
        tclearregion(term.c.x, term.c.y, term.col - 1, term.c.y);
        return;
    }

    memmove(&term.line[term.c.y][dst], &term.line[term.c.y][src],
            size * sizeof(Cell));
    tclearregion(src, term.c.y, dst - 1, term.c.y);
}

void tinsertblankline(int n) {
    if (term.c.y < term.top || term.c.y > term.bot) {
        return;
    }

    tscrolldown(term.c.y, n);
}

void tdeleteline(int n) {
    if (term.c.y < term.top || term.c.y > term.bot) {
        return;
    }

    tscrollup(term.c.y, n);
}

long tdefcolor(int* attr, int* npar, int l) {
    long idx = -1;
    uint r, g, b;

    switch (attr[*npar + 1]) {
        case 2: /* direct colour in RGB space */
            if (*npar + 4 >= l) {
                fprintf(stderr,
                        "erresc(38): Incorrect number of parameters (%d)\n",
                        *npar);
                break;
            }
            r = attr[*npar + 2];
            g = attr[*npar + 3];
            b = attr[*npar + 4];
            *npar += 4;
            if (!BETWEEN(r, 0, 255) || !BETWEEN(g, 0, 255) || !BETWEEN(b, 0, 255)) {
                fprintf(stderr, "erresc: bad rgb color (%d,%d,%d)\n",
                        r, g, b);
            } else {
                idx = TRUECOLOR(r, g, b);
            }
            break;
        case 5: /* indexed colour */
            if (*npar + 2 >= l) {
                fprintf(stderr,
                        "erresc(38): Incorrect number of parameters (%d)\n",
                        *npar);
                break;
            }
            *npar += 2;
            if (!BETWEEN(attr[*npar], 0, 255)) {
                fprintf(stderr, "erresc: bad fgcolor %d\n", attr[*npar]);
            } else {
                idx = attr[*npar];
            }
            break;
        case 0: /* implemented defined (only foreground) */
        case 1: /* transparent */
        case 3: /* direct colour in CMY space */
        case 4: /* direct colour in CMYK space */
        default:
            fprintf(stderr,
                    "erresc(38): gfx attr %d unknown\n", attr[*npar]);
    }

    return idx;
}

void tsetattr(int* attr, int l) {
    int i;
    long idx;

    for (i = 0; i < l; i++) {
        switch (attr[i]) {
            case 0:
                term.c.attr.mode &= ~(ATTR_REVERSE | ATTR_UNDERLINE | ATTR_BOLD | ATTR_ITALIC | ATTR_BLINK);
                term.c.attr.fg = term.defaultfg;
                term.c.attr.bg = term.defaultbg;
                break;
            case 1:
                term.c.attr.mode |= ATTR_BOLD;
                break;
            case 3:
                term.c.attr.mode |= ATTR_ITALIC;
                break;
            case 4:
                term.c.attr.mode |= ATTR_UNDERLINE;
                break;
            case 5: /* slow blink */
            case 6: /* rapid blink */
                term.c.attr.mode |= ATTR_BLINK;
                break;
            case 7:
                term.c.attr.mode |= ATTR_REVERSE;
                break;
            case 21:
            case 22:
                term.c.attr.mode &= ~ATTR_BOLD;
                break;
            case 23:
                term.c.attr.mode &= ~ATTR_ITALIC;
                break;
            case 24:
                term.c.attr.mode &= ~ATTR_UNDERLINE;
                break;
            case 25:
            case 26:
                term.c.attr.mode &= ~ATTR_BLINK;
                break;
            case 27:
                term.c.attr.mode &= ~ATTR_REVERSE;
                break;
            case 38:
                if ((idx = tdefcolor(attr, &i, l)) >= 0) {
                    term.c.attr.fg = idx;
                }
                break;
            case 39:
                term.c.attr.fg = term.defaultfg;
                break;
            case 48:
                if ((idx = tdefcolor(attr, &i, l)) >= 0) {
                    term.c.attr.bg = idx;
                }
                break;
            case 49:
                term.c.attr.bg = term.defaultbg;
                break;
            default:
                if (BETWEEN(attr[i], 30, 37)) {
                    term.c.attr.fg = attr[i] - 30;
                } else if (BETWEEN(attr[i], 40, 47)) {
                    term.c.attr.bg = attr[i] - 40;
                } else if (BETWEEN(attr[i], 90, 97)) {
                    term.c.attr.fg = attr[i] - 90 + 8;
                } else if (BETWEEN(attr[i], 100, 107)) {
                    term.c.attr.bg = attr[i] - 100 + 8;
                } else {
                    fprintf(stderr,
                            "erresc(default): gfx attr %d unknown\n",
                            attr[i]), csidump();
                }
                break;
        }
    }
}

void tsetscroll(int t, int b) {
    int temp;

    LIMIT(t, 0, term.row - 1);
    LIMIT(b, 0, term.row - 1);
    if (t > b) {
        temp = t;
        t = b;
        b = temp;
    }
    term.top = t;
    term.bot = b;
}

void tsetmode(bool priv, bool set, int* args, int narg) {
    int* lim;
    bool alt;

    for (lim = args + narg; args < lim; ++args) {
        if (priv) {
            switch (*args) {
                break;
                case 1: /* DECCKM -- Cursor key */
                    MODBIT(term.mode, set, MODE_APPCURSOR);
                    break;
                case 5: /* DECSCNM -- Reverse video */
                    libsuckterm_cb_set_reverse_video(set);
                    break;
                case 6: /* DECOM -- Origin */
                    MODBIT(term.c.state, set, CURSOR_ORIGIN);
                    tmoveato(0, 0);
                    break;
                case 7: /* DECAWM -- Auto wrap */
                    MODBIT(term.mode, set, MODE_WRAP);
                    break;
                case 0:  /* Error (IGNORED) */
                case 2:  /* DECANM -- ANSI/VT52 (IGNORED) */
                case 3:  /* DECCOLM -- Column  (IGNORED) */
                case 4:  /* DECSCLM -- Scroll (IGNORED) */
                case 8:  /* DECARM -- Auto repeat (IGNORED) */
                case 18: /* DECPFF -- Printer feed (IGNORED) */
                case 19: /* DECPEX -- Printer extent (IGNORED) */
                case 42: /* DECNRCM -- National characters (IGNORED) */
                case 12: /* att610 -- Start blinking cursor (IGNORED) */
                    break;
                case 25: /* DECTCEM -- Text Cursor Enable Mode */
                    libsuckterm_cb_set_cursor_visibility(set);
                    break;
                case 9:    /* X10 mouse compatibility mode */
                    libsuckterm_cb_set_pointer_motion(0);
                    MODBIT(term.mode, 0, MODE_MOUSE);
                    MODBIT(term.mode, set, MODE_MOUSEX10);
                    break;
                case 1000: /* 1000: report button press */
                    libsuckterm_cb_set_pointer_motion(0);
                    MODBIT(term.mode, 0, MODE_MOUSE);
                    MODBIT(term.mode, set, MODE_MOUSEBTN);
                    break;
                case 1002: /* 1002: report motion on button press */
                    libsuckterm_cb_set_pointer_motion(0);
                    MODBIT(term.mode, 0, MODE_MOUSE);
                    MODBIT(term.mode, set, MODE_MOUSEMOTION);
                    break;
                case 1003: /* 1003: enable all mouse motions */
                    libsuckterm_cb_set_pointer_motion(set);
                    MODBIT(term.mode, 0, MODE_MOUSE);
                    MODBIT(term.mode, set, MODE_MOUSEMANY);
                    break;
                case 1004: /* 1004: send focus events to tty */
                    MODBIT(term.mode, set, MODE_FOCUS);
                    break;
                case 1006: /* 1006: extended reporting mode */
                    MODBIT(term.mode, set, MODE_MOUSESGR);
                    break;
                case 1034:
                    MODBIT(term.mode, set, MODE_8BIT);
                    break;
                case 1049: /* swap screen & set/restore cursor as xterm */
                    tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
                case 47: /* swap screen */
                case 1047:
                    alt = IS_SET(MODE_ALTSCREEN);
                    if (alt) {
                        tclearregion(0, 0, term.col - 1,
                                term.row - 1);
                    }
                    if (set ^ alt) { /* set is always 1 or 0 */
                        tswapscreen();
                    }
                    if (*args != 1049) {
                        break;
                    }
                    /* FALLTRU */
                case 1048:
                    tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
                    break;
                case 2004: /* 2004: bracketed paste mode */
                    MODBIT(term.mode, set, MODE_BRCKTPASTE);
                    break;
                    /* Not implemented mouse modes. See comments there. */
                case 1001: /* mouse highlight mode; can hang the
				      terminal by design when implemented. */
                case 1005: /* UTF-8 mouse mode; will confuse
				      applications not supporting UTF-8
				      and luit. */
                case 1015: /* urxvt mangled mouse mode; incompatible
				      and can be mistaken for other control
				      codes. */
                default:
                    fprintf(stderr,
                            "erresc: unknown private set/reset mode %d\n",
                            *args);
                    break;
            }
        } else {
            switch (*args) {
                case 0:  /* Error (IGNORED) */
                    break;
                case 2:  /* KAM -- keyboard action */
                    MODBIT(term.mode, set, MODE_KBDLOCK);
                    break;
                case 4:  /* IRM -- Insertion-replacement */
                    MODBIT(term.mode, set, MODE_INSERT);
                    break;
                case 12: /* SRM -- Send/Receive */
                    MODBIT(term.mode, !set, MODE_ECHO);
                    break;
                case 20: /* LNM -- Linefeed/new line */
                    MODBIT(term.mode, set, MODE_CRLF);
                    break;
                default:
                    fprintf(stderr,
                            "erresc: unknown set/reset mode %d\n",
                            *args);
                    break;
            }
        }
    }
}

void csihandle(void) {
    char buf[40];
    int len;

    switch (csiescseq.mode) {
        default:
        unknown:
            fprintf(stderr, "erresc: unknown csi ");
            csidump();
            /* die(""); */
            break;
        case '@': /* ICH -- Insert <n> blank char */
            DEFAULT(csiescseq.arg[0], 1);
            tinsertblank(csiescseq.arg[0]);
            break;
        case 'A': /* CUU -- Cursor <n> Up */
            DEFAULT(csiescseq.arg[0], 1);
            tmoveto(term.c.x, term.c.y - csiescseq.arg[0]);
            break;
        case 'B': /* CUD -- Cursor <n> Down */
        case 'e': /* VPR --Cursor <n> Down */
            DEFAULT(csiescseq.arg[0], 1);
            tmoveto(term.c.x, term.c.y + csiescseq.arg[0]);
            break;
        case 'c': /* DA -- Device Attributes */
            if (csiescseq.arg[0] == 0) {
                ttywrite(VT102ID, sizeof(VT102ID) - 1);
            }
            break;
        case 'C': /* CUF -- Cursor <n> Forward */
        case 'a': /* HPR -- Cursor <n> Forward */
            DEFAULT(csiescseq.arg[0], 1);
            tmoveto(term.c.x + csiescseq.arg[0], term.c.y);
            break;
        case 'D': /* CUB -- Cursor <n> Backward */
            DEFAULT(csiescseq.arg[0], 1);
            tmoveto(term.c.x - csiescseq.arg[0], term.c.y);
            break;
        case 'E': /* CNL -- Cursor <n> Down and first col */
            DEFAULT(csiescseq.arg[0], 1);
            tmoveto(0, term.c.y + csiescseq.arg[0]);
            break;
        case 'F': /* CPL -- Cursor <n> Up and first col */
            DEFAULT(csiescseq.arg[0], 1);
            tmoveto(0, term.c.y - csiescseq.arg[0]);
            break;
        case 'g': /* TBC -- Tabulation clear */
            switch (csiescseq.arg[0]) {
                case 0: /* clear current tab stop */
                    term.tabs[term.c.x] = 0;
                    break;
                case 3: /* clear all the tabs */
                    memset(term.tabs, 0, term.col * sizeof(*term.tabs));
                    break;
                default:
                    goto unknown;
            }
            break;
        case 'G': /* CHA -- Move to <col> */
        case '`': /* HPA */
            DEFAULT(csiescseq.arg[0], 1);
            tmoveto(csiescseq.arg[0] - 1, term.c.y);
            break;
        case 'H': /* CUP -- Move to <row> <col> */
        case 'f': /* HVP */
            DEFAULT(csiescseq.arg[0], 1);
            DEFAULT(csiescseq.arg[1], 1);
            tmoveato(csiescseq.arg[1] - 1, csiescseq.arg[0] - 1);
            break;
        case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
            DEFAULT(csiescseq.arg[0], 1);
            while (csiescseq.arg[0]--) {
                tputtab(1);
            }
            break;
        case 'J': /* ED -- Clear screen */
            switch (csiescseq.arg[0]) {
                case 0: /* below */
                    tclearregion(term.c.x, term.c.y, term.col - 1, term.c.y);
                    if (term.c.y < term.row - 1) {
                        tclearregion(0, term.c.y + 1, term.col - 1,
                                term.row - 1);
                    }
                    break;
                case 1: /* above */
                    if (term.c.y > 1) {
                        tclearregion(0, 0, term.col - 1, term.c.y - 1);
                    }
                    tclearregion(0, term.c.y, term.c.x, term.c.y);
                    break;
                case 2: /* all */
                    tclearregion(0, 0, term.col - 1, term.row - 1);
                    break;
                default:
                    goto unknown;
            }
            break;
        case 'K': /* EL -- Clear line */
            switch (csiescseq.arg[0]) {
                case 0: /* right */
                    tclearregion(term.c.x, term.c.y, term.col - 1,
                            term.c.y);
                    break;
                case 1: /* left */
                    tclearregion(0, term.c.y, term.c.x, term.c.y);
                    break;
                case 2: /* all */
                    tclearregion(0, term.c.y, term.col - 1, term.c.y);
                    break;
            }
            break;
        case 'S': /* SU -- Scroll <n> line up */
            DEFAULT(csiescseq.arg[0], 1);
            tscrollup(term.top, csiescseq.arg[0]);
            break;
        case 'T': /* SD -- Scroll <n> line down */
            DEFAULT(csiescseq.arg[0], 1);
            tscrolldown(term.top, csiescseq.arg[0]);
            break;
        case 'L': /* IL -- Insert <n> blank lines */
            DEFAULT(csiescseq.arg[0], 1);
            tinsertblankline(csiescseq.arg[0]);
            break;
        case 'l': /* RM -- Reset Mode */
            tsetmode(csiescseq.priv, 0, csiescseq.arg, csiescseq.narg);
            break;
        case 'M': /* DL -- Delete <n> lines */
            DEFAULT(csiescseq.arg[0], 1);
            tdeleteline(csiescseq.arg[0]);
            break;
        case 'X': /* ECH -- Erase <n> char */
            DEFAULT(csiescseq.arg[0], 1);
            tclearregion(term.c.x, term.c.y,
                    term.c.x + csiescseq.arg[0] - 1, term.c.y);
            break;
        case 'P': /* DCH -- Delete <n> char */
            DEFAULT(csiescseq.arg[0], 1);
            tdeletechar(csiescseq.arg[0]);
            break;
        case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
            DEFAULT(csiescseq.arg[0], 1);
            while (csiescseq.arg[0]--) {
                tputtab(0);
            }
            break;
        case 'd': /* VPA -- Move to <row> */
            DEFAULT(csiescseq.arg[0], 1);
            tmoveato(term.c.x, csiescseq.arg[0] - 1);
            break;
        case 'h': /* SM -- Set terminal mode */
            tsetmode(csiescseq.priv, 1, csiescseq.arg, csiescseq.narg);
            break;
        case 'm': /* SGR -- Terminal attribute (color) */
            tsetattr(csiescseq.arg, csiescseq.narg);
            break;
        case 'n': /* DSR – Device Status Report (cursor position) */
            if (csiescseq.arg[0] == 6) {
                len = snprintf(buf, sizeof(buf), "\033[%i;%iR",
                        term.c.y + 1, term.c.x + 1);
                ttywrite(buf, len);
                break;
            }
        case 'r': /* DECSTBM -- Set Scrolling Region */
            if (csiescseq.priv) {
                goto unknown;
            } else {
                DEFAULT(csiescseq.arg[0], 1);
                DEFAULT(csiescseq.arg[1], term.row);
                tsetscroll(csiescseq.arg[0] - 1, csiescseq.arg[1] - 1);
                tmoveato(0, 0);
            }
            break;
        case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
            tcursor(CURSOR_SAVE);
            break;
        case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
            tcursor(CURSOR_LOAD);
            break;
    }
}

void csireset(void) {
    memset(&csiescseq, 0, sizeof(csiescseq));
}

void strhandle(void) {
    char* p = NULL;
    int i, j, narg;

    strparse();
    narg = strescseq.narg;

    switch (strescseq.type) {
        case ']': /* OSC -- Operating System Command */
            switch (i = atoi(strescseq.args[0])) {
                case 0:
                case 1:
                case 2:
                    if (narg > 1) {
                        libsuckterm_cb_set_title(strescseq.args[1]);
                    }
                    break;
                case 4: /* color set */
                    if (narg < 3) {
                        break;
                    }
                    p = strescseq.args[2];
                    /* fall through */
                case 104: /* color reset, here p = NULL */
                    j = (narg > 1) ? atoi(strescseq.args[1]) : -1;
                    if (!libsuckterm_cb_set_color(j, p)) {
                        fprintf(stderr, "erresc: invalid color %s\n", p);
                    }
                    break;
                default:
                    fprintf(stderr, "erresc: unknown str ");
                    strdump();
                    break;
            }
            break;
        case 'k': /* old title set compatibility */
            libsuckterm_cb_set_title(strescseq.args[0]);
            break;
        case 'P': /* DSC -- Device Control String */
        case '_': /* APC -- Application Program Command */
        case '^': /* PM -- Privacy Message */
        default:
            fprintf(stderr, "erresc: unknown str ");
            strdump();
            /* die(""); */
            break;
    }
}

void strparse(void) {
    char* p = strescseq.buf;

    strescseq.narg = 0;
    strescseq.buf[strescseq.len] = '\0';
    while (p && strescseq.narg < STR_ARG_SIZ) {
        strescseq.args[strescseq.narg++] = strsep(&p, ";");
    }
}

void strreset(void) {
    memset(&strescseq, 0, sizeof(strescseq));
}

void tputtab(bool forward) {
    uint x = term.c.x;

    if (forward) {
        if (x == term.col) {
            return;
        }
        for (++x; x < term.col && !term.tabs[x]; ++x) {
            /* nothing */ }
    } else {
        if (x == 0) {
            return;
        }
        for (--x; x > 0 && !term.tabs[x]; --x) {
            /* nothing */ }
    }
    tmoveto(x, term.c.y);
}

void techo(char* buf, int len) {
    for (; len > 0; buf++, len--) {
        char c = *buf;

        if (c == '\033') { /* escape */
            tputc("^", 1);
            tputc("[", 1);
        } else if (c < '\x20') { /* control code */
            if (c != '\n' && c != '\r' && c != '\t') {
                c |= '\x40';
                tputc("^", 1);
            }
            tputc(&c, 1);
        } else {
            break;
        }
    }
    if (len) {
        tputc(buf, len);
    }
}

void tdeftran(char ascii) {
    char c, (* bp)[2];
    static char tbl[][2] = {
            { '0', CS_GRAPHIC0 }, { '1', CS_GRAPHIC1 }, { 'A', CS_UK },
            { 'B', CS_USA }, { '<', CS_MULTI }, { 'K', CS_GER },
            { '5', CS_FIN }, { 'C', CS_FIN },
            { 0, 0 }
    };

    for (bp = &tbl[0]; (c = (*bp)[0]) && c != ascii; ++bp) {
        /* nothing */}

    if (c == 0) {
        fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
    } else {
        term.trantbl[term.icharset] = (*bp)[1];
    }
}

void tselcs(void) {
    if (term.trantbl[term.charset] == CS_GRAPHIC0) {
        term.c.attr.mode |= ATTR_GFX;
    } else {
        term.c.attr.mode &= ~ATTR_GFX;
    }
}

void tputc(char* c, int len) {
    uchar ascii = *c;
    bool control = ascii < '\x20' || ascii == 0177;
    long u8char;
    int width;

    if (len == 1) {
        width = 1;
    } else {
        utf8decode(c, &u8char);
        width = wcwidth(u8char);
    }

    /*
     * STR sequences must be checked before anything else
     * because it can use some control codes as part of the sequence.
     */
    if (term.esc & ESC_STR) {
        switch (ascii) {
            case '\033':
                term.esc = ESC_START | ESC_STR_END;
                break;
            case '\a': /* backwards compatibility to xterm */
                term.esc = 0;
                strhandle();
                break;
            default:
                if (strescseq.len + len < sizeof(strescseq.buf) - 1) {
                    memmove(&strescseq.buf[strescseq.len], c, len);
                    strescseq.len += len;
                } else {
                    /*
                     * Here is a bug in terminals. If the user never sends
                     * some code to stop the str or esc command, then st
                     * will stop responding. But this is better than
                     * silently failing with unknown characters. At least
                     * then users will report back.
                     *
                     * In the case users ever get fixed, here is the code:
                     */
                    /*
                     * term.esc = 0;
                     * strhandle();
                     */
                }
        }
        return;
    }

    /*
     * Actions of control codes must be performed as soon they arrive
     * because they can be embedded inside a control sequence, and
     * they must not cause conflicts with sequences.
     */
    if (control) {
        switch (ascii) {
            case '\t':   /* HT */
                tputtab(1);
                return;
            case '\b':   /* BS */
                tmoveto(term.c.x - 1, term.c.y);
                return;
            case '\r':   /* CR */
                tmoveto(0, term.c.y);
                return;
            case '\f':   /* LF */
            case '\v':   /* VT */
            case '\n':   /* LF */
                /* go to first col if the mode is set */
                tnewline(IS_SET(MODE_CRLF));
                return;
            case '\a':   /* BEL */
                libsuckterm_cb_bell();
                return;
            case '\033': /* ESC */
                csireset();
                term.esc = ESC_START;
                return;
            case '\016': /* SO */
                term.charset = 0;
                tselcs();
                return;
            case '\017': /* SI */
                term.charset = 1;
                tselcs();
                return;
            case '\032': /* SUB */
            case '\030': /* CAN */
                csireset();
                return;
            case '\005': /* ENQ (IGNORED) */
            case '\000': /* NUL (IGNORED) */
            case '\021': /* XON (IGNORED) */
            case '\023': /* XOFF (IGNORED) */
            case 0177:   /* DEL (IGNORED) */
                return;
        }
    } else if (term.esc & ESC_START) {
        if (term.esc & ESC_CSI) {
            csiescseq.buf[csiescseq.len++] = ascii;
            if (BETWEEN(ascii, 0x40, 0x7E)
                    || csiescseq.len >= \
                    sizeof(csiescseq.buf) - 1) {
                term.esc = 0;
                csiparse();
                csihandle();
            }
        } else if (term.esc & ESC_STR_END) {
            term.esc = 0;
            if (ascii == '\\') {
                strhandle();
            }
        } else if (term.esc & ESC_ALTCHARSET) {
            tdeftran(ascii);
            tselcs();
            term.esc = 0;
        } else if (term.esc & ESC_TEST) {
            if (ascii == '8') { /* DEC screen alignment test. */
                char E[UTF_SIZ] = "E";
                int x, y;

                for (x = 0; x < term.col; ++x) {
                    for (y = 0; y < term.row; ++y) {
                        tsetchar(E, &term.c.attr, x, y);
                    }
                }
            }
            term.esc = 0;
        } else {
            switch (ascii) {
                case '[':
                    term.esc |= ESC_CSI;
                    break;
                case '#':
                    term.esc |= ESC_TEST;
                    break;
                case 'P': /* DCS -- Device Control String */
                case '_': /* APC -- Application Program Command */
                case '^': /* PM -- Privacy Message */
                case ']': /* OSC -- Operating System Command */
                case 'k': /* old title set compatibility */
                    strreset();
                    strescseq.type = ascii;
                    term.esc |= ESC_STR;
                    break;
                case '(': /* set primary charset G0 */
                case ')': /* set secondary charset G1 */
                case '*': /* set tertiary charset G2 */
                case '+': /* set quaternary charset G3 */
                    term.icharset = ascii - '(';
                    term.esc |= ESC_ALTCHARSET;
                    break;
                case 'D': /* IND -- Linefeed */
                    if (term.c.y == term.bot) {
                        tscrollup(term.top, 1);
                    } else {
                        tmoveto(term.c.x, term.c.y + 1);
                    }
                    term.esc = 0;
                    break;
                case 'E': /* NEL -- Next line */
                    tnewline(1); /* always go to first col */
                    term.esc = 0;
                    break;
                case 'H': /* HTS -- Horizontal tab stop */
                    term.tabs[term.c.x] = 1;
                    term.esc = 0;
                    break;
                case 'M': /* RI -- Reverse index */
                    if (term.c.y == term.top) {
                        tscrolldown(term.top, 1);
                    } else {
                        tmoveto(term.c.x, term.c.y - 1);
                    }
                    term.esc = 0;
                    break;
                case 'Z': /* DECID -- Identify Terminal */
                    ttywrite(VT102ID, sizeof(VT102ID) - 1);
                    term.esc = 0;
                    break;
                case 'c': /* RIS -- Reset to inital state */
                    treset();
                    term.esc = 0;
                    libsuckterm_cb_reset_title();
                    libsuckterm_cb_reset_colors();
                    break;
                case '=': /* DECPAM -- Application keypad */
                    term.mode |= MODE_APPKEYPAD;
                    term.esc = 0;
                    break;
                case '>': /* DECPNM -- Normal keypad */
                    term.mode &= ~MODE_APPKEYPAD;
                    term.esc = 0;
                    break;
                case '7': /* DECSC -- Save Cursor */
                    tcursor(CURSOR_SAVE);
                    term.esc = 0;
                    break;
                case '8': /* DECRC -- Restore Cursor */
                    tcursor(CURSOR_LOAD);
                    term.esc = 0;
                    break;
                case '\\': /* ST -- Stop */
                    term.esc = 0;
                    break;
                default:
                    fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
                            (uchar)ascii, isprint(ascii) ? ascii : '.');
                    term.esc = 0;
            }
        }
        /*
         * All characters which form part of a sequence are not
         * printed
         */
        return;
    }
    /*
     * Display control codes only if we are in graphic mode
     */
    if (control && !(term.c.attr.mode & ATTR_GFX)) {
        return;
    }
    if (IS_SET(MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT)) {
        term.line[term.c.y][term.c.x].mode |= ATTR_WRAP;
        tnewline(1);
    }

    if (IS_SET(MODE_INSERT) && term.c.x + 1 < term.col) {
        memmove(&term.line[term.c.y][term.c.x + 1],
                &term.line[term.c.y][term.c.x],
                (term.col - term.c.x - 1) * sizeof(Cell));
    }

    if (term.c.x + width > term.col) {
        tnewline(1);
    }

    tsetchar(c, &term.c.attr, term.c.x, term.c.y);

    if (width == 2) {
        term.line[term.c.y][term.c.x].mode |= ATTR_WIDE;
        if (term.c.x + 1 < term.col) {
            term.line[term.c.y][term.c.x + 1].c[0] = '\0';
            term.line[term.c.y][term.c.x + 1].mode = ATTR_WDUMMY;
        }
    }
    if (term.c.x + width < term.col) {
        tmoveto(term.c.x + width, term.c.y);
    } else {
        term.c.state |= CURSOR_WRAPNEXT;
    }
}

int tresize(int col, int row) {
    int i;
    int minrow = MIN(row, term.row);
    int mincol = MIN(col, term.col);
    int slide = term.c.y - row + 1;
    bool* bp;
    Line* orig;

    if (col < 1 || row < 1) {
        return 0;
    }

    /* free unneeded rows */
    i = 0;
    if (slide > 0) {
        /*
         * slide screen to keep cursor where we expect it -
         * tscrollup would work here, but we can optimize to
         * memmove because we're freeing the earlier lines
         */
        for (/* i = 0 */; i < slide; i++) {
            free(term.line[i]);
            free(term.alt[i]);
        }
        memmove(term.line, term.line + slide, row * sizeof(Line));
        memmove(term.alt, term.alt + slide, row * sizeof(Line));
    }
    for (i += row; i < term.row; i++) {
        free(term.line[i]);
        free(term.alt[i]);
    }

    /* resize to new height */
    term.line = xrealloc(term.line, row * sizeof(Line));
    term.alt = xrealloc(term.alt, row * sizeof(Line));
    term.dirty = xrealloc(term.dirty, row * sizeof(*term.dirty));
    term.tabs = xrealloc(term.tabs, col * sizeof(*term.tabs));

    /* resize each row to new width, zero-pad if needed */
    for (i = 0; i < minrow; i++) {
        term.dirty[i] = 1;
        term.line[i] = xrealloc(term.line[i], col * sizeof(Cell));
        term.alt[i] = xrealloc(term.alt[i], col * sizeof(Cell));
    }

    /* allocate any new rows */
    for (/* i == minrow */; i < row; i++) {
        term.dirty[i] = 1;
        term.line[i] = xmalloc(col * sizeof(Cell));
        term.alt[i] = xmalloc(col * sizeof(Cell));
    }
    if (col > term.col) {
        bp = term.tabs + term.col;

        memset(bp, 0, sizeof(*term.tabs) * (col - term.col));
        while (--bp > term.tabs && !*bp) {
            /* nothing */ }
        for (bp += term.tabspaces; bp < term.tabs + col; bp += term.tabspaces) {
            *bp = 1;
        }
    }
    /* update terminal size */
    term.col = col;
    term.row = row;
    /* reset scrolling region */
    tsetscroll(0, row - 1);
    /* make use of the LIMIT in tmoveto */
    tmoveto(term.c.x, term.c.y);
    /* Clearing both screens */
    orig = term.line;
    do {
        if (mincol < col && 0 < minrow) {
            tclearregion(mincol, 0, col - 1, minrow - 1);
        }
        if (0 < col && minrow < row) {
            tclearregion(0, minrow, col - 1, row - 1);
        }
        tswapscreen();
    } while (orig != term.line);

    return (slide > 0);
}

void libsuckterm_notify_set_size(int col, int row, int cw, int ch) {
    term.tw = MAX(1, col * cw);
    term.th = MAX(1, row * ch);
    tresize(col, row);
    ttyresize();
}

void libsuckterm_notify_exit(void) {
    /* Send SIGHUP to shell */
    kill(child_pid, SIGHUP);
}
