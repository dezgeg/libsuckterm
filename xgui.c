#include <signal.h>
#include <stdbool.h>
#include <time.h>
#include <wchar.h>
#include <X11/cursorfont.h>
#include <X11/Xft/Xft.h>
#include <X11/Xutil.h>
#include <sys/time.h>
#include <locale.h>
#include <libgen.h>
#include "helpers.h"
#include "ptyutils.h"
#include "libsuckterm.h"
#include "arg.h"

/* XEMBED messages */
#define XEMBED_FOCUS_IN  4
#define XEMBED_FOCUS_OUT 5

static char** opt_cmd = NULL;
static char* opt_title = NULL;
static char* opt_embed = NULL;
static char* opt_class = NULL;
static char* opt_font = NULL;
static int oldbutton = 3;

void redraw(int timeout);
void xclear(int x1, int y1, int x2, int y2);
void draw(void);
void drawregion(int x1, int y1, int x2, int y2);
void xsetsize(int width, int height);
void xloadcols(void) ;

static void expose(XEvent*);
static void visibility(XEvent*);
static void unmap(XEvent*);
static void kpress(XEvent*);
static void cmessage(XEvent*);
static void resize(XEvent*);
static void focus(XEvent*);
static void brelease(XEvent*);
static void bpress(XEvent*);
static void bmotion(XEvent*);

#define DRAW_BUF_SIZ  20*1024
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13)

enum window_state {
    WIN_VISIBLE = 1,
    WIN_REDRAW = 2,
    WIN_FOCUSED = 4
};

#define REDRAW_TIMEOUT (80*1000) /* 80 ms */
#define Font Font_
#define Draw XftDraw *
#define Colour XftColor
#define Rectangle XRectangle

/* Purely graphic info */
typedef struct {
    KeySym k;
    uint mask;
    char* s;
    /* three valued logic variables: 0 indifferent, 1 on, -1 off */
    signed char appkey;
    /* application keypad */
    signed char appcursor;
    /* application cursor */
    signed char crlf;      /* crlf mode          */
} Key;

typedef struct {
    Display* dpy;
    Colormap cmap;
    Window win;
    Drawable buf;
    Atom xembed, wmdeletewin;
    XIM xim;
    XIC xic;
    Draw draw;
    Visual* vis;
    XSetWindowAttributes attrs;
    int scr;
    int tw, th;
    /* tty width and height */
    int w, h;
    /* window width and height */
    int ch;
    /* char height */
    int cw;
    /* char width  */
    char state; /* focus, redraw, visible */
} XWindow;

/* Font structure */
typedef struct {
    int height;
    int width;
    int ascent;
    int descent;
    short lbearing;
    short rbearing;
    XftFont* match;
    FcFontSet* set;
    FcPattern* pattern;
} Font;

#include "config.h"

/* Drawing Context */
typedef struct {
    Colour col[LEN(colorname) < 256 ? 256 : LEN(colorname)];
    Font font, bfont, ifont, ibfont;
    GC gc;
} DC;

static XWindow xw;
static DC dc;

bool cursor_visible = true;
bool reverse_video = false;

void libsuckterm_cb_bell(void) {
    if (!(xw.state & WIN_FOCUSED)) {
        libsuckterm_cb_set_urgency(1);
    }
    if (bellvolume) {
        XBell(xw.dpy, bellvolume);
    }
}

void libsuckterm_cb_set_cursor_visibility(bool visible) {
    cursor_visible = visible;
}

void libsuckterm_cb_set_reverse_video(bool enable) {
    bool do_redraw = reverse_video != enable;

    reverse_video = enable;
    if (do_redraw) {
        redraw(REDRAW_TIMEOUT);
    }
}

void libsuckterm_cb_set_title(char* p) {
    XTextProperty prop;

    Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
            &prop);
    XSetWMName(xw.dpy, xw.win, &prop);
    XFree(prop.value);
}

void libsuckterm_cb_reset_title(void) {
    libsuckterm_cb_set_title(opt_title ? opt_title : "st");
}

void libsuckterm_cb_reset_colors(void) {
    xloadcols();
}

void libsuckterm_cb_set_pointer_motion(int set) {
    MODBIT(xw.attrs.event_mask, set, PointerMotionMask);
    XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);
}

void libsuckterm_cb_set_urgency(int add) {
    XWMHints* h = XGetWMHints(xw.dpy, xw.win);

    h->flags = add ? (h->flags | XUrgencyHint) : (h->flags & ~XUrgencyHint);
    XSetWMHints(xw.dpy, xw.win, h);
    XFree(h);
}

/* DRAWING STUFF */

static char* usedfont = NULL;
static int usedfontsize = 0;

/* Font Ring Cache */
enum {
    FRC_NORMAL,
    FRC_ITALIC,
    FRC_BOLD,
    FRC_ITALICBOLD
};

typedef struct {
    XftFont* font;
    int flags;
} Fontcache;

/* Fontcache is an array now. A new font will be appended to the array. */
static Fontcache frc[16];
static int frclen = 0;

static int x2col(int x) {
    x -= borderpx;
    x /= xw.cw;

    return LIMIT(x, 0, term.col - 1);
}

static int y2row(int y) {
    y -= borderpx;
    y /= xw.ch;

    return LIMIT(y, 0, term.row - 1);
}

void mousereport(XEvent* e) {
    int x = x2col(e->xbutton.x), y = y2row(e->xbutton.y),
            button = e->xbutton.button, state = e->xbutton.state,
            len;
    char buf[40];
    static int ox, oy;

    /* from urxvt */
    if (e->xbutton.type == MotionNotify) {
        if (x == ox && y == oy) {
            return;
        }
        if (!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY)) {
            return;
        }
        /* MOUSE_MOTION: no reporting if no button is pressed */
        if (IS_SET(MODE_MOUSEMOTION) && oldbutton == 3) {
            return;
        }

        button = oldbutton + 32;
        ox = x;
        oy = y;
    } else {
        if (!IS_SET(MODE_MOUSESGR) && e->xbutton.type == ButtonRelease) {
            button = 3;
        } else {
            button -= Button1;
            if (button >= 3) {
                button += 64 - 3;
            }
        }
        if (e->xbutton.type == ButtonPress) {
            oldbutton = button;
            ox = x;
            oy = y;
        } else if (e->xbutton.type == ButtonRelease) {
            oldbutton = 3;
            /* MODE_MOUSEX10: no button release reporting */
            if (IS_SET(MODE_MOUSEX10)) {
                return;
            }
        }
    }

    if (!IS_SET(MODE_MOUSEX10)) {
        button += (state & ShiftMask ? 4 : 0)
                + (state & Mod4Mask ? 8 : 0)
                + (state & ControlMask ? 16 : 0);
    }

    len = 0;
    if (IS_SET(MODE_MOUSESGR)) {
        len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
                button, x + 1, y + 1,
                e->xbutton.type == ButtonRelease ? 'm' : 'M');
    } else if (x < 223 && y < 223) {
        len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
                32 + button, 32 + x + 1, 32 + y + 1);
    } else {
        return;
    }

    ttywrite(buf, len);
}

void xresize(int col, int row) {
    xw.tw = MAX(1, col * xw.cw);
    xw.th = MAX(1, row * xw.ch);

    XFreePixmap(xw.dpy, xw.buf);
    xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h,
            DefaultDepth(xw.dpy, xw.scr));
    XftDrawChange(xw.draw, xw.buf);
    xclear(0, 0, xw.w, xw.h);
}

static inline ushort sixd_to_16bit(int x) {
    return x == 0 ? 0 : 0x3737 + 0x2828 * x;
}

void xloadcols(void) {
    int i, r, g, b;
    XRenderColor color = { .alpha = 0xffff };
    static bool loaded;
    Colour* cp;

    if (loaded) {
        for (cp = dc.col; cp < dc.col + LEN(dc.col); ++cp) {
            XftColorFree(xw.dpy, xw.vis, xw.cmap, cp);
        }
    }

    /* load colors [0-15] colors and [256-LEN(colorname)[ (config.h) */
    for (i = 0; i < LEN(colorname); i++) {
        if (!colorname[i]) {
            continue;
        }
        if (!XftColorAllocName(xw.dpy, xw.vis, xw.cmap, colorname[i], &dc.col[i])) {
            die("Could not allocate color '%s'\n", colorname[i]);
        }
    }

    /* load colors [16-255] ; same colors as xterm */
    for (i = 16, r = 0; r < 6; r++) {
        for (g = 0; g < 6; g++) {
            for (b = 0; b < 6; b++) {
                color.red = sixd_to_16bit(r);
                color.green = sixd_to_16bit(g);
                color.blue = sixd_to_16bit(b);
                if (!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &dc.col[i])) {
                    die("Could not allocate color %d\n", i);
                }
                i++;
            }
        }
    }

    for (r = 0; r < 24; r++, i++) {
        color.red = color.green = color.blue = 0x0808 + 0x0a0a * r;
        if (!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color,
                &dc.col[i])) {
            die("Could not allocate color %d\n", i);
        }
    }
    loaded = true;
}

int libsuckterm_cb_set_color(int x, const char* name) {
    XRenderColor color = { .alpha = 0xffff };
    Colour colour;
    if (x < 0 || x > LEN(colorname)) {
        return -1;
    }
    if (!name) {
        if (16 <= x && x < 16 + 216) {
            int r = (x - 16) / 36, g = ((x - 16) % 36) / 6, b = (x - 16) % 6;
            color.red = sixd_to_16bit(r);
            color.green = sixd_to_16bit(g);
            color.blue = sixd_to_16bit(b);
            if (!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &colour)) {
                return 0;
            } /* something went wrong */
            dc.col[x] = colour;
            return 1;
        } else if (16 + 216 <= x && x < 256) {
            color.red = color.green = color.blue = 0x0808 + 0x0a0a * (x - (16 + 216));
            if (!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &colour)) {
                return 0;
            } /* something went wrong */
            dc.col[x] = colour;
            return 1;
        } else {
            name = colorname[x];
        }
    }
    if (!XftColorAllocName(xw.dpy, xw.vis, xw.cmap, name, &colour)) {
        return 0;
    }
    dc.col[x] = colour;
    /*
     * TODO if defaultbg color is changed, borders
     * are dirty
     */
    redraw(0);
    return 1;
}

void xtermclear(int col1, int row1, int col2, int row2) {
    XftDrawRect(xw.draw,
            &dc.col[reverse_video ? defaultfg : defaultbg],
            borderpx + col1 * xw.cw,
            borderpx + row1 * xw.ch,
            (col2 - col1 + 1) * xw.cw,
            (row2 - row1 + 1) * xw.ch);
}

void xclear(int x1, int y1, int x2, int y2) {
    XftDrawRect(xw.draw,
            &dc.col[reverse_video ? defaultfg : defaultbg],
            x1, y1, x2 - x1, y2 - y1);
}

int xloadfontset(Font* f) {
    FcResult result;

    if (!(f->set = FcFontSort(0, f->pattern, FcTrue, 0, &result))) {
        return 1;
    }
    return 0;
}

void xdraws(char* s, Cell base, int x, int y, int charlen, int bytelen) {
    int winx = borderpx + x * xw.cw, winy = borderpx + y * xw.ch,
            width = charlen * xw.cw, xp, i;
    int frcflags;
    int u8fl, u8fblen, u8cblen, doesexist;
    char* u8c, * u8fs;
    long u8char;
    Font* font = &dc.font;
    FcResult fcres;
    FcPattern* fcpattern, * fontpattern;
    FcFontSet* fcsets[] = { NULL };
    FcCharSet* fccharset;
    Colour* fg, * bg, * temp, revfg, revbg, truefg, truebg;
    XRenderColor colfg, colbg;
    Rectangle r;
    int oneatatime;

    frcflags = FRC_NORMAL;

    if (base.mode & ATTR_ITALIC) {
        if (base.fg == defaultfg) {
            base.fg = defaultitalic;
        }
        font = &dc.ifont;
        frcflags = FRC_ITALIC;
    } else if ((base.mode & ATTR_ITALIC) && (base.mode & ATTR_BOLD)) {
        if (base.fg == defaultfg) {
            base.fg = defaultitalic;
        }
        font = &dc.ibfont;
        frcflags = FRC_ITALICBOLD;
    } else if (base.mode & ATTR_UNDERLINE) {
        if (base.fg == defaultfg) {
            base.fg = defaultunderline;
        }
    }

    if (IS_TRUECOL(base.fg)) {
        colfg.red = TRUERED(base.fg);
        colfg.green = TRUEGREEN(base.fg);
        colfg.blue = TRUEBLUE(base.fg);
        XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &truefg);
        fg = &truefg;
    } else {
        fg = &dc.col[base.fg];
    }

    if (IS_TRUECOL(base.bg)) {
        colbg.green = TRUEGREEN(base.bg);
        colbg.red = TRUERED(base.bg);
        colbg.blue = TRUEBLUE(base.bg);
        XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &truebg);
        bg = &truebg;
    } else {
        bg = &dc.col[base.bg];
    }

    if (base.mode & ATTR_BOLD) {
        if (BETWEEN(base.fg, 0, 7)) {
            /* basic system colors */
            fg = &dc.col[base.fg + 8];
        } else if (BETWEEN(base.fg, 16, 195)) {
            /* 256 colors */
            fg = &dc.col[base.fg + 36];
        } else if (BETWEEN(base.fg, 232, 251)) {
            /* greyscale */
            fg = &dc.col[base.fg + 4];
        }
        /*
         * Those ranges will not be brightened:
         *    8 - 15 – bright system colors
         *    196 - 231 – highest 256 color cube
         *    252 - 255 – brightest colors in greyscale
         */
        font = &dc.bfont;
        frcflags = FRC_BOLD;
    }

    if (reverse_video) {
        if (fg == &dc.col[defaultfg]) {
            fg = &dc.col[defaultbg];
        } else {
            colfg.red = ~fg->color.red;
            colfg.green = ~fg->color.green;
            colfg.blue = ~fg->color.blue;
            colfg.alpha = fg->color.alpha;
            XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &revfg);
            fg = &revfg;
        }

        if (bg == &dc.col[defaultbg]) {
            bg = &dc.col[defaultfg];
        } else {
            colbg.red = ~bg->color.red;
            colbg.green = ~bg->color.green;
            colbg.blue = ~bg->color.blue;
            colbg.alpha = bg->color.alpha;
            XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &revbg);
            bg = &revbg;
        }
    }

    if (base.mode & ATTR_REVERSE) {
        temp = fg;
        fg = bg;
        bg = temp;
    }

#if 0
	// Reimplement this in X client.
	if(base.mode & ATTR_BLINK && term.mode & MODE_BLINK)
		fg = bg;
#endif

    /* Intelligent cleaning up of the borders. */
    if (x == 0) {
        xclear(0, (y == 0) ? 0 : winy, borderpx,
                winy + xw.ch + ((y >= term.row - 1) ? xw.h : 0));
    }
    if (x + charlen >= term.col) {
        xclear(winx + width, (y == 0) ? 0 : winy, xw.w,
                ((y >= term.row - 1) ? xw.h : (winy + xw.ch)));
    }
    if (y == 0) {
        xclear(winx, 0, winx + width, borderpx);
    }
    if (y == term.row - 1) {
        xclear(winx, winy + xw.ch, winx + width, xw.h);
    }

    /* Clean up the region we want to draw to. */
    XftDrawRect(xw.draw, bg, winx, winy, width, xw.ch);

    /* Set the clip region because Xft is sometimes dirty. */
    r.x = 0;
    r.y = 0;
    r.height = xw.ch;
    r.width = width;
    XftDrawSetClipRectangles(xw.draw, winx, winy, &r, 1);

    for (xp = winx; bytelen > 0;) {
        /*
         * Search for the range in the to be printed string of Cells
         * that are in the main font. Then print that range. If
         * some Cell is found that is not in the font, do the
         * fallback dance.
         */
        u8fs = s;
        u8fblen = 0;
        u8fl = 0;
        oneatatime = font->width != xw.cw;
        for (; ;) {
            u8c = s;
            u8cblen = utf8decode(s, &u8char);
            s += u8cblen;
            bytelen -= u8cblen;

            doesexist = XftCharExists(xw.dpy, font->match, u8char);
            if (oneatatime || !doesexist || bytelen <= 0) {
                if (oneatatime || bytelen <= 0) {
                    if (doesexist) {
                        u8fl++;
                        u8fblen += u8cblen;
                    }
                }

                if (u8fl > 0) {
                    XftDrawStringUtf8(xw.draw, fg,
                            font->match, xp,
                            winy + font->ascent,
                            (FcChar8*)u8fs,
                            u8fblen);
                    xp += xw.cw * u8fl;

                }
                break;
            }

            u8fl++;
            u8fblen += u8cblen;
        }
        if (doesexist) {
            if (oneatatime) {
                continue;
            }
            break;
        }

        /* Search the font cache. */
        for (i = 0; i < frclen; i++) {
            if (XftCharExists(xw.dpy, frc[i].font, u8char)
                    && frc[i].flags == frcflags) {
                break;
            }
        }

        /* Nothing was found. */
        if (i >= frclen) {
            if (!font->set) {
                xloadfontset(font);
            }
            fcsets[0] = font->set;

            /*
             * Nothing was found in the cache. Now use
             * some dozen of Fontconfig calls to get the
             * font for one single character.
             */
            fcpattern = FcPatternDuplicate(font->pattern);
            fccharset = FcCharSetCreate();

            FcCharSetAddChar(fccharset, u8char);
            FcPatternAddCharSet(fcpattern, FC_CHARSET,
                    fccharset);
            FcPatternAddBool(fcpattern, FC_SCALABLE,
                    FcTrue);

            FcConfigSubstitute(0, fcpattern,
                    FcMatchPattern);
            FcDefaultSubstitute(fcpattern);

            fontpattern = FcFontSetMatch(0, fcsets,
                    FcTrue, fcpattern, &fcres);

            /*
             * Overwrite or create the new cache entry.
             */
            if (frclen >= LEN(frc)) {
                frclen = LEN(frc) - 1;
                XftFontClose(xw.dpy, frc[frclen].font);
            }

            frc[frclen].font = XftFontOpenPattern(xw.dpy,
                    fontpattern);
            frc[frclen].flags = frcflags;

            i = frclen;
            frclen++;

            FcPatternDestroy(fcpattern);
            FcCharSetDestroy(fccharset);
        }

        XftDrawStringUtf8(xw.draw, fg, frc[i].font,
                xp, winy + frc[i].font->ascent,
                (FcChar8*)u8c, u8cblen);

        xp += xw.cw * wcwidth(u8char);
    }

    /*
    XftDrawStringUtf8(xw.draw, fg, font->set, winx,
            winy + font->ascent, (FcChar8 *)s, bytelen);
    */

    if (base.mode & ATTR_UNDERLINE) {
        XftDrawRect(xw.draw, fg, winx, winy + font->ascent + 1, width, 1);
    }

    /* Reset clip to none. */
    XftDrawSetClip(xw.draw, 0);
}

void xdrawcursor(void) {
    static int oldx = 0, oldy = 0;
    int sl, width, curx;
    Cell g = { { ' ' }, ATTR_NULL, defaultbg, defaultcs };

    LIMIT(oldx, 0, term.col - 1);
    LIMIT(oldy, 0, term.row - 1);

    curx = term.c.x;

    /* adjust position if in dummy */
    if (term.line[oldy][oldx].mode & ATTR_WDUMMY) {
        oldx--;
    }
    if (term.line[term.c.y][curx].mode & ATTR_WDUMMY) {
        curx--;
    }

    memcpy(g.c, term.line[term.c.y][term.c.x].c, UTF_SIZ);

    /* remove the old cursor */
    sl = utf8size(term.line[oldy][oldx].c);
    width = (term.line[oldy][oldx].mode & ATTR_WIDE) ? 2 : 1;
    xdraws(term.line[oldy][oldx].c, term.line[oldy][oldx], oldx,
            oldy, width, sl);

    /* draw the new one */
    if (cursor_visible) {
        if (xw.state & WIN_FOCUSED) {
            if (reverse_video) {
                g.mode |= ATTR_REVERSE;
                g.fg = defaultcs;
                g.bg = defaultfg;
            }

            sl = utf8size(g.c);
            width = (term.line[term.c.y][curx].mode & ATTR_WIDE) ? 2 : 1;
            xdraws(g.c, g, term.c.x, term.c.y, width, sl);
        } else {
            XftDrawRect(xw.draw, &dc.col[defaultcs],
                    borderpx + curx * xw.cw,
                    borderpx + term.c.y * xw.ch,
                    xw.cw - 1, 1);
            XftDrawRect(xw.draw, &dc.col[defaultcs],
                    borderpx + curx * xw.cw,
                    borderpx + term.c.y * xw.ch,
                    1, xw.ch - 1);
            XftDrawRect(xw.draw, &dc.col[defaultcs],
                    borderpx + (curx + 1) * xw.cw - 1,
                    borderpx + term.c.y * xw.ch,
                    1, xw.ch - 1);
            XftDrawRect(xw.draw, &dc.col[defaultcs],
                    borderpx + curx * xw.cw,
                    borderpx + (term.c.y + 1) * xw.ch - 1,
                    xw.cw, 1);
        }
        oldx = curx, oldy = term.c.y;
    }
}

void redraw(int timeout) {
    struct timespec tv = { 0, timeout * 1000 };

    tfulldirt();
    draw();

    if (timeout > 0) {
        nanosleep(&tv, NULL);
        XSync(xw.dpy, False); /* necessary for a good tput flash */
    }
}

void draw(void) {
    drawregion(0, 0, term.col, term.row);
    XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc, 0, 0, xw.w,
            xw.h, 0, 0);
    XSetForeground(xw.dpy, dc.gc,
            dc.col[reverse_video ?
                    defaultfg : defaultbg].pixel);
}

void drawregion(int x1, int y1, int x2, int y2) {
    int ic, ib, x, y, ox, sl;
    Cell base, new;
    char buf[DRAW_BUF_SIZ];
    long u8char;

    if (!(xw.state & WIN_VISIBLE)) {
        return;
    }

    for (y = y1; y < y2; y++) {
        if (!term.dirty[y]) {
            continue;
        }

        xtermclear(0, y, term.col, y);
        term.dirty[y] = 0;
        base = term.line[y][0];
        ic = ib = ox = 0;
        for (x = x1; x < x2; x++) {
            new = term.line[y][x];
            if (new.mode == ATTR_WDUMMY) {
                continue;
            }
            if (ib > 0 && (ATTRCMP(base, new) || ib >= DRAW_BUF_SIZ - UTF_SIZ)) {
                xdraws(buf, base, ox, y, ic, ib);
                ic = ib = 0;
            }
            if (ib == 0) {
                ox = x;
                base = new;
            }

            sl = utf8decode(new.c, &u8char);
            memcpy(buf + ib, new.c, sl);
            ib += sl;
            ic += (new.mode & ATTR_WIDE) ? 2 : 1;
        }
        if (ib > 0) {
            xdraws(buf, base, ox, y, ic, ib);
        }
    }
    xdrawcursor();
}

/* INIT STUFF */

static int xloadfont(Font*, FcPattern*);
static void xloadfonts(char*, int);

int xloadfont(Font* f, FcPattern* pattern) {
    FcPattern* match;
    FcResult result;

    match = FcFontMatch(NULL, pattern, &result);
    if (!match) {
        return 1;
    }

    if (!(f->match = XftFontOpenPattern(xw.dpy, match))) {
        FcPatternDestroy(match);
        return 1;
    }

    f->set = NULL;
    f->pattern = FcPatternDuplicate(pattern);

    f->ascent = f->match->ascent;
    f->descent = f->match->descent;
    f->lbearing = 0;
    f->rbearing = f->match->max_advance_width;

    f->height = f->ascent + f->descent;
    f->width = f->lbearing + f->rbearing;

    return 0;
}

void xloadfonts(char* fontstr, int fontsize) {
    FcPattern* pattern;
    FcResult result;
    double fontval;

    if (fontstr[0] == '-') {
        pattern = XftXlfdParse(fontstr, False, False);
    } else {
        pattern = FcNameParse((FcChar8*)fontstr);
    }

    if (!pattern) {
        die("st: can't open font %s\n", fontstr);
    }

    if (fontsize > 0) {
        FcPatternDel(pattern, FC_PIXEL_SIZE);
        FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)fontsize);
        usedfontsize = fontsize;
    } else {
        result = FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval);
        if (result == FcResultMatch) {
            usedfontsize = (int)fontval;
        } else {
            /*
             * Default font size is 12, if none given. This is to
             * have a known usedfontsize value.
             */
            FcPatternAddDouble(pattern, FC_PIXEL_SIZE, 12);
            usedfontsize = 12;
        }
    }

    FcConfigSubstitute(0, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    if (xloadfont(&dc.font, pattern)) {
        die("st: can't open font %s\n", fontstr);
    }

    /* Setting character width and height. */
    xw.cw = CEIL(dc.font.width * cwscale);
    xw.ch = CEIL(dc.font.height * chscale);

    FcPatternDel(pattern, FC_SLANT);
    FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
    if (xloadfont(&dc.ifont, pattern)) {
        die("st: can't open font %s\n", fontstr);
    }

    FcPatternDel(pattern, FC_WEIGHT);
    FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
    if (xloadfont(&dc.ibfont, pattern)) {
        die("st: can't open font %s\n", fontstr);
    }

    FcPatternDel(pattern, FC_SLANT);
    FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
    if (xloadfont(&dc.bfont, pattern)) {
        die("st: can't open font %s\n", fontstr);
    }

    FcPatternDestroy(pattern);
}

void xhints(void) {
    XClassHint class = { opt_class ? opt_class : termname, termname };
    XWMHints wm = { .flags = InputHint, .input = 1 };
    XSizeHints* sizeh = NULL;

    sizeh = XAllocSizeHints();
    sizeh->flags = PSize | PResizeInc | PBaseSize;
    sizeh->height = xw.h;
    sizeh->width = xw.w;
    sizeh->height_inc = xw.ch;
    sizeh->width_inc = xw.cw;
    sizeh->base_height = 2 * borderpx;
    sizeh->base_width = 2 * borderpx;

    XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm, &class);
    XFree(sizeh);
}

void xinit(void) {
    XGCValues gcvalues;
    Cursor cursor;
    Window parent;

    if (!(xw.dpy = XOpenDisplay(NULL))) {
        die("Can't open display\n");
    }
    xw.scr = XDefaultScreen(xw.dpy);
    xw.vis = XDefaultVisual(xw.dpy, xw.scr);

    /* font */
    if (!FcInit()) {
        die("Could not init fontconfig.\n");
    }

    usedfont = (opt_font == NULL) ? font : opt_font;
    xloadfonts(usedfont, 0);

    /* colors */
    xw.cmap = XDefaultColormap(xw.dpy, xw.scr);
    xloadcols();

    /* window - default size */
    xw.h = 2 * borderpx + term.row * xw.ch;
    xw.w = 2 * borderpx + term.col * xw.cw;

    /* Events */
    xw.attrs.background_pixel = dc.col[defaultbg].pixel;
    xw.attrs.border_pixel = dc.col[defaultbg].pixel;
    xw.attrs.bit_gravity = NorthWestGravity;
    xw.attrs.event_mask = FocusChangeMask | KeyPressMask
            | ExposureMask | VisibilityChangeMask | StructureNotifyMask
            | ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
    xw.attrs.colormap = xw.cmap;

    parent = opt_embed ? strtol(opt_embed, NULL, 0) : \
            XRootWindow(xw.dpy, xw.scr);
    xw.win = XCreateWindow(xw.dpy, parent, 0, 0,
            xw.w, xw.h, 0, XDefaultDepth(xw.dpy, xw.scr), InputOutput,
            xw.vis, CWBackPixel | CWBorderPixel | CWBitGravity
                    | CWEventMask | CWColormap, &xw.attrs);

    memset(&gcvalues, 0, sizeof(gcvalues));
    gcvalues.graphics_exposures = False;
    dc.gc = XCreateGC(xw.dpy, parent, GCGraphicsExposures,
            &gcvalues);
    xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h,
            DefaultDepth(xw.dpy, xw.scr));
    XSetForeground(xw.dpy, dc.gc, dc.col[defaultbg].pixel);
    XFillRectangle(xw.dpy, xw.buf, dc.gc, 0, 0, xw.w, xw.h);

    /* Xft rendering context */
    xw.draw = XftDrawCreate(xw.dpy, xw.buf, xw.vis, xw.cmap);

    /* input methods */
    if ((xw.xim = XOpenIM(xw.dpy, NULL, NULL, NULL)) == NULL) {
        XSetLocaleModifiers("@im=local");
        if ((xw.xim = XOpenIM(xw.dpy, NULL, NULL, NULL)) == NULL) {
            XSetLocaleModifiers("@im=");
            if ((xw.xim = XOpenIM(xw.dpy,
                    NULL, NULL, NULL)) == NULL) {
                die("XOpenIM failed. Could not open input"
                        " device.\n");
            }
        }
    }
    xw.xic = XCreateIC(xw.xim, XNInputStyle, XIMPreeditNothing
            | XIMStatusNothing, XNClientWindow, xw.win,
            XNFocusWindow, xw.win, NULL);
    if (xw.xic == NULL) {
        die("XCreateIC failed. Could not obtain input method.\n");
    }

    /* white cursor, black outline */
    cursor = XCreateFontCursor(xw.dpy, XC_xterm);
    XDefineCursor(xw.dpy, xw.win, cursor);
    XRecolorCursor(xw.dpy, cursor,
            &(XColor){ .red = 0xffff, .green = 0xffff, .blue = 0xffff },
            &(XColor){ .red = 0x0000, .green = 0x0000, .blue = 0x0000 });

    xw.xembed = XInternAtom(xw.dpy, "_XEMBED", False);
    xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

    libsuckterm_cb_reset_title();
    XMapWindow(xw.dpy, xw.win);
    xhints();
    XSync(xw.dpy, 0);
}

/* EVENT STUFF */

static void (* handler[LASTEvent])(XEvent*) = {
        [KeyPress] = kpress,
        [ClientMessage] = cmessage,
        [ConfigureNotify] = resize,
        [VisibilityNotify] = visibility,
        [UnmapNotify] = unmap,
        [Expose] = expose,
        [FocusIn] = focus,
        [FocusOut] = focus,
        [MotionNotify] = bmotion,
        [ButtonPress] = bpress,
        [ButtonRelease] = brelease,
};

static inline bool match(uint mask, uint state) {
    state &= ~ignoremod;

    if (mask == XK_NO_MOD && state) {
        return false;
    }
    if (mask != XK_ANY_MOD && mask != XK_NO_MOD && !state) {
        return false;
    }
    if (mask == XK_ANY_MOD) {
        return true;
    }
    return state == mask;
}

char* kmap(KeySym k, uint state) {
    Key* kp;
    int i;

    /* Check for mapped keys out of X11 function keys. */
    for (i = 0; i < LEN(mappedkeys); i++) {
        if (mappedkeys[i] == k) {
            break;
        }
    }
    if (i == LEN(mappedkeys)) {
        if ((k & 0xFFFF) < 0xFD00) {
            return NULL;
        }
    }

    for (kp = key; kp < key + LEN(key); kp++) {
        if (kp->k != k) {
            continue;
        }

        if (!match(kp->mask, state)) {
            continue;
        }

        if (kp->appkey > 0) {
            if (!IS_SET(MODE_APPKEYPAD)) {
                continue;
            }
        } else if (kp->appkey < 0 && IS_SET(MODE_APPKEYPAD)) {
            continue;
        }

        if ((kp->appcursor < 0 && IS_SET(MODE_APPCURSOR)) ||
                (kp->appcursor > 0
                        && !IS_SET(MODE_APPCURSOR))) {
            continue;
        }

        if ((kp->crlf < 0 && IS_SET(MODE_CRLF)) ||
                (kp->crlf > 0 && !IS_SET(MODE_CRLF))) {
            continue;
        }

        return kp->s;
    }

    return NULL;
}

void kpress(XEvent* ev) {
    XKeyEvent* e = &ev->xkey;
    KeySym ksym;
    char buf[32];
    char* customkey;
    int len;
    long c;
    Status status;

    if (IS_SET(MODE_KBDLOCK)) {
        return;
    }

    len = XmbLookupString(xw.xic, e, buf, sizeof buf, &ksym, &status);
    e->state &= ~Mod2Mask;

    /* 2. custom keys from config.h */
    if ((customkey = kmap(ksym, e->state))) {
        ttysend(customkey, strlen(customkey));
        return;
    }

    /* 3. composed string from input method */
    if (len == 0) {
        return;
    }
    if (len == 1 && e->state & Mod1Mask) {
        if (IS_SET(MODE_8BIT)) {
            if (*buf < 0177) {
                c = *buf | 0x80;
                len = utf8encode(&c, buf);
            }
        } else {
            buf[1] = buf[0];
            buf[0] = '\033';
            len = 2;
        }
    }
    ttysend(buf, len);
}

void cmessage(XEvent* e) {
    /*
     * See xembed specs
     *  http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html
     */
    if (e->xclient.message_type == xw.xembed && e->xclient.format == 32) {
        if (e->xclient.data.l[1] == XEMBED_FOCUS_IN) {
            xw.state |= WIN_FOCUSED;
            libsuckterm_cb_set_urgency(0);
        } else if (e->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
            xw.state &= ~WIN_FOCUSED;
        }
    } else if (e->xclient.data.l[0] == xw.wmdeletewin) {
        /* Send SIGHUP to shell */
        kill(pid, SIGHUP);
        exit(EXIT_SUCCESS);
    }
}

void resize(XEvent* e) {
    if (e->xconfigure.width == xw.w && e->xconfigure.height == xw.h) {
        return;
    }

    xsetsize(e->xconfigure.width, e->xconfigure.height);
}

void expose(XEvent* ev) {
    XExposeEvent* e = &ev->xexpose;

    if (xw.state & WIN_REDRAW) {
        if (!e->count) {
            xw.state &= ~WIN_REDRAW;
        }
    }
    redraw(0);
}

void visibility(XEvent* ev) {
    XVisibilityEvent* e = &ev->xvisibility;

    if (e->state == VisibilityFullyObscured) {
        xw.state &= ~WIN_VISIBLE;
    } else if (!(xw.state & WIN_VISIBLE)) {
        /* need a full redraw for next Expose, not just a buf copy */
        xw.state |= WIN_VISIBLE | WIN_REDRAW;
    }
}

void unmap(XEvent* ev) {
    xw.state &= ~WIN_VISIBLE;
}

void focus(XEvent* ev) {
    XFocusChangeEvent* e = &ev->xfocus;

    if (e->mode == NotifyGrab) {
        return;
    }

    if (ev->type == FocusIn) {
        XSetICFocus(xw.xic);
        xw.state |= WIN_FOCUSED;
        libsuckterm_cb_set_urgency(0);
        if (IS_SET(MODE_FOCUS)) {
            ttywrite("\033[I", 3);
        }
    } else {
        XUnsetICFocus(xw.xic);
        xw.state &= ~WIN_FOCUSED;
        if (IS_SET(MODE_FOCUS)) {
            ttywrite("\033[O", 3);
        }
    }
}

void bpress(XEvent* e) {
    if (IS_SET(MODE_MOUSE)) {
        mousereport(e);
    }
}

void brelease(XEvent* e) {
    if (IS_SET(MODE_MOUSE)) {
        mousereport(e);
    }
}

void bmotion(XEvent* e) {
    if (IS_SET(MODE_MOUSE)) {
        mousereport(e);
    }
}

void xsetsize(int width, int height) {
    int col, row;

    if (width != 0) {
        xw.w = width;
    }
    if (height != 0) {
        xw.h = height;
    }

    col = (xw.w - 2 * borderpx) / xw.cw;
    row = (xw.h - 2 * borderpx) / xw.ch;

    libsuckterm_set_size(col, row, xw.cw, xw.ch);
    xresize(col, row);
}

void run(void) {
    XEvent ev;
    int w = xw.w, h = xw.h;
    fd_set rfd;
    int xfd = XConnectionNumber(xw.dpy), xev, dodraw = 0;
    struct timeval drawtimeout, * tv = NULL, now, last;

    /* Waiting for window mapping */
    while (1) {
        XNextEvent(xw.dpy, &ev);
        if (ev.type == ConfigureNotify) {
            w = ev.xconfigure.width;
            h = ev.xconfigure.height;
        } else if (ev.type == MapNotify) {
            break;
        }
    }

    xsetsize(w, h);
    cmdfd = ttynew(term.row, term.col, xw.win, opt_cmd, shell, termname);

    gettimeofday(&last, NULL);

    for (xev = actionfps; ;) {
        FD_ZERO(&rfd);
        FD_SET(cmdfd, &rfd);
        FD_SET(xfd, &rfd);

        if (select(MAX(xfd, cmdfd) + 1, &rfd, NULL, NULL, tv) < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("select failed: %s\n", SERRNO);
        }
        if (FD_ISSET(cmdfd, &rfd)) {
            ttyread();
        }

        if (FD_ISSET(xfd, &rfd)) {
            xev = actionfps;
        }

        gettimeofday(&now, NULL);
        drawtimeout.tv_sec = 0;
        drawtimeout.tv_usec = (1000 / xfps) * 1000;
        tv = &drawtimeout;

        dodraw = 0;
        if (TIMEDIFF(now, last) > (xev ? (1000 / xfps) : (1000 / actionfps))) {
            dodraw = 1;
            last = now;
        }

        if (dodraw) {
            while (XPending(xw.dpy)) {
                XNextEvent(xw.dpy, &ev);
                if (XFilterEvent(&ev, None)) {
                    continue;
                }
                if (handler[ev.type]) {
                    (handler[ev.type])(&ev);
                }
            }

            draw();
            XFlush(xw.dpy);

            if (xev && !FD_ISSET(xfd, &rfd)) {
                xev--;
            }
            if (!FD_ISSET(cmdfd, &rfd) && !FD_ISSET(xfd, &rfd)) {
                tv = NULL;
            }
        }
    }
}

void usage(void) {
    die("%s " VERSION " (c) 2010-2013 st engineers\n" \
    "usage: st [-a] [-v] [-c class] [-f font] [-g geometry] [-o file]" \
    " [-t title] [-w windowid] [-e command ...]\n", argv0);
}

int main(int argc, char* argv[]) {
    char* titles;

    ARGBEGIN {
                case 'c':
                    opt_class = EARGF(usage());
                    break;
                case 'e':
                    /* eat all remaining arguments */
                    if (argc > 1) {
                        opt_cmd = &argv[1];
                        if (argv[1] != NULL && opt_title == NULL) {
                            titles = strdup(argv[1]);
                            opt_title = basename(titles);
                        }
                    }
                    goto run;
                case 'f':
                    opt_font = EARGF(usage());
                    break;
                case 't':
                    opt_title = EARGF(usage());
                    break;
                case 'w':
                    opt_embed = EARGF(usage());
                    break;
                case 'v':
                default:
                    usage();
            } ARGEND;

    run:
    setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");
    tnew(80, 24, defaultfg, defaultbg, tabspaces);
    xinit();
    run();

    return 0;
}
