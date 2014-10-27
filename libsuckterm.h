#ifndef LIBSUCKTERM_H
#define LIBSUCKTERM_H

#include <stdbool.h>

#define UTF_SIZ       4

enum glyph_attribute {
    ATTR_NULL = 0,
    ATTR_REVERSE = 1,
    ATTR_UNDERLINE = 2,
    ATTR_BOLD = 4,
    ATTR_GFX = 8,
    ATTR_ITALIC = 16,
    ATTR_BLINK = 32,
    ATTR_WRAP = 64,
    ATTR_WIDE = 128,
    ATTR_WDUMMY = 256,
};

typedef struct {
    char c[UTF_SIZ];
    /* character code */
    ushort mode;
    /* attribute flags */
    ulong fg;
    /* foreground  */
    ulong bg;        /* background  */
} Cell;

typedef Cell* Line;

typedef struct {
    Cell attr;
    /* current char attributes */
    int x;
    int y;
    char state;
} TCursor;

enum term_mode {
    MODE_WRAP = 1,
    MODE_INSERT = 2,
    MODE_APPKEYPAD = 4,
    MODE_ALTSCREEN = 8,
    MODE_CRLF = 16,
    MODE_MOUSEBTN = 32,
    MODE_MOUSEMOTION = 64,
    /* MODE_REVERSE deleted */
            MODE_KBDLOCK = 256,
    /* MODE_HIDE deleted */
            MODE_ECHO = 1024,
    MODE_APPCURSOR = 2048,
    MODE_MOUSESGR = 4096,
    MODE_8BIT = 8192,
    MODE_BLINK = 16384,
    MODE_FBLINK = 32768,
    MODE_FOCUS = 65536,
    MODE_MOUSEX10 = 131072,
    MODE_MOUSEMANY = 262144,
    MODE_BRCKTPASTE = 524288,
    MODE_MOUSE = MODE_MOUSEBTN | MODE_MOUSEMOTION | MODE_MOUSEX10 | MODE_MOUSEMANY,
};

/* Internal representation of the screen */
typedef struct {
    int row;
    /* number of rows */
    int col;
    /* number of columns */
    int tw, th;
    /* tty width and height in pixels, for TIOCSWINSZ */
    Line* line;
    /* screen */
    Line* alt;    /* alternate screen */
    bool* dirty;
    /* dirtyness of lines */
    TCursor c;
    /* cursor */
    int top;
    /* top    scroll limit */
    int bot;
    /* bottom scroll limit */
    int mode;
    /* terminal mode flags */
    int esc;
    /* escape state flags */
    char trantbl[4];
    /* charset table translation */
    int charset;
    /* current charset */
    int icharset; /* selected charset for sequence */
    bool* tabs;

    /* user default settings */
    unsigned int defaultfg, defaultbg;
    unsigned tabspaces;
} Term;
extern Term term;

#define TRUECOLOR(r, g, b) (1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x)    (1 << 24 & (x))
#define TRUERED(x)       (((x) & 0xff0000) >> 8)
#define TRUEGREEN(x)     (((x) & 0xff00))
#define TRUEBLUE(x)      (((x) & 0xff) << 8)

#define ATTRCMP(a, b) ((a).mode != (b).mode || (a).fg != (b).fg || (a).bg != (b).bg)
#define IS_SET(flag) ((term.mode & (flag)) != 0)
#define TIMEDIFF(t1, t2) ((t1.tv_sec-t2.tv_sec)*1000 + (t1.tv_usec-t2.tv_usec)/1000)
#define CEIL(x) (((x) != (int) (x)) ? (x) + 1 : (x))

enum libsuckterm_modifier {
    LIBSUCKTERM_MODIFIER_SHIFT = 4,
    LIBSUCKTERM_MODIFIER_META = 8,
    LIBSUCKTERM_MODIFIER_CONTROL = 16,
};

enum libsuckterm_mouse_event {
    LIBSUCKTERM_MOUSE_PRESSED,
    LIBSUCKTERM_MOUSE_RELEASED,
    LIBSUCKTERM_MOUSE_MOTION,
};

void libsuckterm_cb_bell(void);
void libsuckterm_cb_reset_title(void);
void libsuckterm_cb_reset_colors(void);
void libsuckterm_cb_set_cursor_visibility(bool);
void libsuckterm_cb_set_reverse_video(bool);
void libsuckterm_cb_set_pointer_motion(int);
void libsuckterm_cb_set_title(char*);
void libsuckterm_cb_set_urgency(int);
int libsuckterm_cb_set_color(int x, const char* name);

void libsuckterm_notify_focus(bool in);
void libsuckterm_notify_set_size(int col, int row, int cw, int ch);
void libsuckterm_notify_exit(void);
void libsuckterm_notify_mouse_event(enum libsuckterm_mouse_event event,
        int x, int y, unsigned mods, int button_index);

int libsuckterm_init(unsigned winid, char** cmd, char* shell, char* termname);
static inline int libsuckterm_get_cols() { return term.col; }
static inline int libsuckterm_get_rows() { return term.row; }
static inline int libsuckterm_get_cursor_x() { return term.c.x; }
static inline int libsuckterm_get_cursor_y() { return term.c.y; }

void tnew(int col, int row, unsigned int defaultfg, unsigned int defaultbg, unsigned int tabspaces);
void tfulldirt(void);

void ttyread(void);
void ttyresize(void);
void ttysend(char*, size_t);
void ttywrite(const char*, size_t);
#endif
