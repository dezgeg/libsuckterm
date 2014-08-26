static char *usedfont = NULL;
static int usedfontsize = 0;

/* Font Ring Cache */
enum {
	FRC_NORMAL,
	FRC_ITALIC,
	FRC_BOLD,
	FRC_ITALICBOLD
};

typedef struct {
	XftFont *font;
	int flags;
} Fontcache;

/* Fontcache is an array now. A new font will be appended to the array. */
static Fontcache frc[16];
static int frclen = 0;

void
xresize(int col, int row) {
	xw.tw = MAX(1, col * xw.cw);
	xw.th = MAX(1, row * xw.ch);

	XFreePixmap(xw.dpy, xw.buf);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h,
			DefaultDepth(xw.dpy, xw.scr));
	XftDrawChange(xw.draw, xw.buf);
	xclear(0, 0, xw.w, xw.h);
}

static inline ushort
sixd_to_16bit(int x) {
	return x == 0 ? 0 : 0x3737 + 0x2828 * x;
}

void
xloadcols(void) {
	int i, r, g, b;
	XRenderColor color = { .alpha = 0xffff };
	static bool loaded;
	Colour *cp;

	if(loaded) {
		for (cp = dc.col; cp < dc.col + LEN(dc.col); ++cp)
			XftColorFree(xw.dpy, xw.vis, xw.cmap, cp);
	}

	/* load colors [0-15] colors and [256-LEN(colorname)[ (config.h) */
	for(i = 0; i < LEN(colorname); i++) {
		if(!colorname[i])
			continue;
		if(!XftColorAllocName(xw.dpy, xw.vis, xw.cmap, colorname[i], &dc.col[i])) {
			die("Could not allocate color '%s'\n", colorname[i]);
		}
	}

	/* load colors [16-255] ; same colors as xterm */
	for(i = 16, r = 0; r < 6; r++) {
		for(g = 0; g < 6; g++) {
			for(b = 0; b < 6; b++) {
				color.red = sixd_to_16bit(r);
				color.green = sixd_to_16bit(g);
				color.blue = sixd_to_16bit(b);
				if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &dc.col[i])) {
					die("Could not allocate color %d\n", i);
				}
				i++;
			}
		}
	}

	for(r = 0; r < 24; r++, i++) {
		color.red = color.green = color.blue = 0x0808 + 0x0a0a * r;
		if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color,
					&dc.col[i])) {
			die("Could not allocate color %d\n", i);
		}
	}
	loaded = true;
}

int
xsetcolorname(int x, const char *name) {
	XRenderColor color = { .alpha = 0xffff };
	Colour colour;
	if (x < 0 || x > LEN(colorname))
		return -1;
	if(!name) {
		if(16 <= x && x < 16 + 216) {
			int r = (x - 16) / 36, g = ((x - 16) % 36) / 6, b = (x - 16) % 6;
			color.red = sixd_to_16bit(r);
			color.green = sixd_to_16bit(g);
			color.blue = sixd_to_16bit(b);
			if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &colour))
				return 0; /* something went wrong */
			dc.col[x] = colour;
			return 1;
		} else if (16 + 216 <= x && x < 256) {
			color.red = color.green = color.blue = 0x0808 + 0x0a0a * (x - (16 + 216));
			if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &colour))
				return 0; /* something went wrong */
			dc.col[x] = colour;
			return 1;
		} else {
			name = colorname[x];
		}
	}
	if(!XftColorAllocName(xw.dpy, xw.vis, xw.cmap, name, &colour))
		return 0;
	dc.col[x] = colour;
	return 1;
}

void
xtermclear(int col1, int row1, int col2, int row2) {
	XftDrawRect(xw.draw,
			&dc.col[IS_SET(MODE_REVERSE) ? defaultfg : defaultbg],
			borderpx + col1 * xw.cw,
			borderpx + row1 * xw.ch,
			(col2-col1+1) * xw.cw,
			(row2-row1+1) * xw.ch);
}

/*
 * Absolute coordinates.
 */
void
xclear(int x1, int y1, int x2, int y2) {
	XftDrawRect(xw.draw,
			&dc.col[IS_SET(MODE_REVERSE)? defaultfg : defaultbg],
			x1, y1, x2-x1, y2-y1);
}

int
xloadfontset(Font *f) {
	FcResult result;

	if(!(f->set = FcFontSort(0, f->pattern, FcTrue, 0, &result)))
		return 1;
	return 0;
}

void
xdraws(char *s, Glyph base, int x, int y, int charlen, int bytelen) {
	int winx = borderpx + x * xw.cw, winy = borderpx + y * xw.ch,
	    width = charlen * xw.cw, xp, i;
	int frcflags;
	int u8fl, u8fblen, u8cblen, doesexist;
	char *u8c, *u8fs;
	long u8char;
	Font *font = &dc.font;
	FcResult fcres;
	FcPattern *fcpattern, *fontpattern;
	FcFontSet *fcsets[] = { NULL };
	FcCharSet *fccharset;
	Colour *fg, *bg, *temp, revfg, revbg, truefg, truebg;
	XRenderColor colfg, colbg;
	Rectangle r;
	int oneatatime;

	frcflags = FRC_NORMAL;

	if(base.mode & ATTR_ITALIC) {
		if(base.fg == defaultfg)
			base.fg = defaultitalic;
		font = &dc.ifont;
		frcflags = FRC_ITALIC;
	} else if((base.mode & ATTR_ITALIC) && (base.mode & ATTR_BOLD)) {
		if(base.fg == defaultfg)
			base.fg = defaultitalic;
		font = &dc.ibfont;
		frcflags = FRC_ITALICBOLD;
	} else if(base.mode & ATTR_UNDERLINE) {
		if(base.fg == defaultfg)
			base.fg = defaultunderline;
	}
	if(IS_TRUECOL(base.fg)) {
		colfg.red = TRUERED(base.fg);
		colfg.green = TRUEGREEN(base.fg);
		colfg.blue = TRUEBLUE(base.fg);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &truefg);
		fg = &truefg;
	} else {
		fg = &dc.col[base.fg];
	}

	if(IS_TRUECOL(base.bg)) {
		colbg.green = TRUEGREEN(base.bg);
		colbg.red = TRUERED(base.bg);
		colbg.blue = TRUEBLUE(base.bg);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &truebg);
		bg = &truebg;
	} else {
		bg = &dc.col[base.bg];
	}



	if(base.mode & ATTR_BOLD) {
		if(BETWEEN(base.fg, 0, 7)) {
			/* basic system colors */
			fg = &dc.col[base.fg + 8];
		} else if(BETWEEN(base.fg, 16, 195)) {
			/* 256 colors */
			fg = &dc.col[base.fg + 36];
		} else if(BETWEEN(base.fg, 232, 251)) {
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

	if(IS_SET(MODE_REVERSE)) {
		if(fg == &dc.col[defaultfg]) {
			fg = &dc.col[defaultbg];
		} else {
			colfg.red = ~fg->color.red;
			colfg.green = ~fg->color.green;
			colfg.blue = ~fg->color.blue;
			colfg.alpha = fg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &revfg);
			fg = &revfg;
		}

		if(bg == &dc.col[defaultbg]) {
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

	if(base.mode & ATTR_REVERSE) {
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
	if(x == 0) {
		xclear(0, (y == 0)? 0 : winy, borderpx,
			winy + xw.ch + ((y >= term.row-1)? xw.h : 0));
	}
	if(x + charlen >= term.col) {
		xclear(winx + width, (y == 0)? 0 : winy, xw.w,
			((y >= term.row-1)? xw.h : (winy + xw.ch)));
	}
	if(y == 0)
		xclear(winx, 0, winx + width, borderpx);
	if(y == term.row-1)
		xclear(winx, winy + xw.ch, winx + width, xw.h);

	/* Clean up the region we want to draw to. */
	XftDrawRect(xw.draw, bg, winx, winy, width, xw.ch);

	/* Set the clip region because Xft is sometimes dirty. */
	r.x = 0;
	r.y = 0;
	r.height = xw.ch;
	r.width = width;
	XftDrawSetClipRectangles(xw.draw, winx, winy, &r, 1);

	for(xp = winx; bytelen > 0;) {
		/*
		 * Search for the range in the to be printed string of glyphs
		 * that are in the main font. Then print that range. If
		 * some glyph is found that is not in the font, do the
		 * fallback dance.
		 */
		u8fs = s;
		u8fblen = 0;
		u8fl = 0;
		oneatatime = font->width != xw.cw;
		for(;;) {
			u8c = s;
			u8cblen = utf8decode(s, &u8char);
			s += u8cblen;
			bytelen -= u8cblen;

			doesexist = XftCharExists(xw.dpy, font->match, u8char);
			if(oneatatime || !doesexist || bytelen <= 0) {
				if(oneatatime || bytelen <= 0) {
					if(doesexist) {
						u8fl++;
						u8fblen += u8cblen;
					}
				}

				if(u8fl > 0) {
					XftDrawStringUtf8(xw.draw, fg,
							font->match, xp,
							winy + font->ascent,
							(FcChar8 *)u8fs,
							u8fblen);
					xp += xw.cw * u8fl;

				}
				break;
			}

			u8fl++;
			u8fblen += u8cblen;
		}
		if(doesexist) {
			if (oneatatime)
				continue;
			break;
		}

		/* Search the font cache. */
		for(i = 0; i < frclen; i++) {
			if(XftCharExists(xw.dpy, frc[i].font, u8char)
					&& frc[i].flags == frcflags) {
				break;
			}
		}

		/* Nothing was found. */
		if(i >= frclen) {
			if(!font->set)
				xloadfontset(font);
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
			if(frclen >= LEN(frc)) {
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
				(FcChar8 *)u8c, u8cblen);

		xp += xw.cw * wcwidth(u8char);
	}

	/*
	XftDrawStringUtf8(xw.draw, fg, font->set, winx,
			winy + font->ascent, (FcChar8 *)s, bytelen);
	*/

	if(base.mode & ATTR_UNDERLINE) {
		XftDrawRect(xw.draw, fg, winx, winy + font->ascent + 1,
				width, 1);
	}

	/* Reset clip to none. */
	XftDrawSetClip(xw.draw, 0);
}

void
xdrawcursor(void) {
	static int oldx = 0, oldy = 0;
	int sl, width, curx;
	Glyph g = {{' '}, ATTR_NULL, defaultbg, defaultcs};

	LIMIT(oldx, 0, term.col-1);
	LIMIT(oldy, 0, term.row-1);

	curx = term.c.x;

	/* adjust position if in dummy */
	if(term.line[oldy][oldx].mode & ATTR_WDUMMY)
		oldx--;
	if(term.line[term.c.y][curx].mode & ATTR_WDUMMY)
		curx--;

	memcpy(g.c, term.line[term.c.y][term.c.x].c, UTF_SIZ);

	/* remove the old cursor */
	sl = utf8size(term.line[oldy][oldx].c);
	width = (term.line[oldy][oldx].mode & ATTR_WIDE)? 2 : 1;
	xdraws(term.line[oldy][oldx].c, term.line[oldy][oldx], oldx,
			oldy, width, sl);

	/* draw the new one */
	if(!(IS_SET(MODE_HIDE))) {
		if(xw.state & WIN_FOCUSED) {
			if(IS_SET(MODE_REVERSE)) {
				g.mode |= ATTR_REVERSE;
				g.fg = defaultcs;
				g.bg = defaultfg;
			}

			sl = utf8size(g.c);
			width = (term.line[term.c.y][curx].mode & ATTR_WIDE)\
				? 2 : 1;
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

void
redraw(int timeout) {
	struct timespec tv = {0, timeout * 1000};

	tfulldirt();
	draw();

	if(timeout > 0) {
		nanosleep(&tv, NULL);
		XSync(xw.dpy, False); /* necessary for a good tput flash */
	}
}

void
draw(void) {
	drawregion(0, 0, term.col, term.row);
	XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc, 0, 0, xw.w,
			xw.h, 0, 0);
	XSetForeground(xw.dpy, dc.gc,
			dc.col[IS_SET(MODE_REVERSE)?
				defaultfg : defaultbg].pixel);
}

void
drawregion(int x1, int y1, int x2, int y2) {
	int ic, ib, x, y, ox, sl;
	Glyph base, new;
	char buf[DRAW_BUF_SIZ];
	long u8char;

	if(!(xw.state & WIN_VISIBLE))
		return;

	for(y = y1; y < y2; y++) {
		if(!term.dirty[y])
			continue;

		xtermclear(0, y, term.col, y);
		term.dirty[y] = 0;
		base = term.line[y][0];
		ic = ib = ox = 0;
		for(x = x1; x < x2; x++) {
			new = term.line[y][x];
			if(new.mode == ATTR_WDUMMY)
				continue;
			if(ib > 0 && (ATTRCMP(base, new)
					|| ib >= DRAW_BUF_SIZ-UTF_SIZ)) {
				xdraws(buf, base, ox, y, ic, ib);
				ic = ib = 0;
			}
			if(ib == 0) {
				ox = x;
				base = new;
			}

			sl = utf8decode(new.c, &u8char);
			memcpy(buf+ib, new.c, sl);
			ib += sl;
			ic += (new.mode & ATTR_WIDE)? 2 : 1;
		}
		if(ib > 0)
			xdraws(buf, base, ox, y, ic, ib);
	}
	xdrawcursor();
}

