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
