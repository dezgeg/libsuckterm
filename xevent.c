void
kpress(XEvent *ev) {
	XKeyEvent *e = &ev->xkey;
	KeySym ksym;
	char buf[32], *customkey;
	int len;
	long c;
	Status status;

	if(IS_SET(MODE_KBDLOCK))
		return;

	len = XmbLookupString(xw.xic, e, buf, sizeof buf, &ksym, &status);
	e->state &= ~Mod2Mask;

	/* 2. custom keys from config.h */
	if((customkey = kmap(ksym, e->state))) {
		ttysend(customkey, strlen(customkey));
		return;
	}

	/* 3. composed string from input method */
	if(len == 0)
		return;
	if(len == 1 && e->state & Mod1Mask) {
		if(IS_SET(MODE_8BIT)) {
			if(*buf < 0177) {
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


void
cmessage(XEvent *e) {
	/*
	 * See xembed specs
	 *  http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html
	 */
	if(e->xclient.message_type == xw.xembed && e->xclient.format == 32) {
		if(e->xclient.data.l[1] == XEMBED_FOCUS_IN) {
			xw.state |= WIN_FOCUSED;
			libsuckterm_cb_set_urgency(0);
		} else if(e->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
			xw.state &= ~WIN_FOCUSED;
		}
	} else if(e->xclient.data.l[0] == xw.wmdeletewin) {
		/* Send SIGHUP to shell */
		kill(pid, SIGHUP);
		exit(EXIT_SUCCESS);
	}
}

void
resize(XEvent *e) {
	if(e->xconfigure.width == xw.w && e->xconfigure.height == xw.h)
		return;

	xsetsize(e->xconfigure.width, e->xconfigure.height);
}

void
expose(XEvent *ev) {
	XExposeEvent *e = &ev->xexpose;

	if(xw.state & WIN_REDRAW) {
		if(!e->count)
			xw.state &= ~WIN_REDRAW;
	}
	redraw(0);
}

void
visibility(XEvent *ev) {
	XVisibilityEvent *e = &ev->xvisibility;

	if(e->state == VisibilityFullyObscured) {
		xw.state &= ~WIN_VISIBLE;
	} else if(!(xw.state & WIN_VISIBLE)) {
		/* need a full redraw for next Expose, not just a buf copy */
		xw.state |= WIN_VISIBLE | WIN_REDRAW;
	}
}

void
unmap(XEvent *ev) {
	xw.state &= ~WIN_VISIBLE;
}

void
focus(XEvent *ev) {
	XFocusChangeEvent *e = &ev->xfocus;

	if(e->mode == NotifyGrab)
		return;

	if(ev->type == FocusIn) {
		XSetICFocus(xw.xic);
		xw.state |= WIN_FOCUSED;
		libsuckterm_cb_set_urgency(0);
		if(IS_SET(MODE_FOCUS))
			ttywrite("\033[I", 3);
	} else {
		XUnsetICFocus(xw.xic);
		xw.state &= ~WIN_FOCUSED;
		if(IS_SET(MODE_FOCUS))
			ttywrite("\033[O", 3);
	}
}

void
bpress(XEvent *e) {
	if(IS_SET(MODE_MOUSE)) {
		mousereport(e);
	}
}

void
brelease(XEvent *e) {
	if(IS_SET(MODE_MOUSE)) {
		mousereport(e);
	}
}

void
bmotion(XEvent *e) {
	if(IS_SET(MODE_MOUSE)) {
		mousereport(e);
	}
}

