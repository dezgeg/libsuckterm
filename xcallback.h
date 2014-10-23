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
