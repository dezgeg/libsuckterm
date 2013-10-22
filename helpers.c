ssize_t
xwrite(int fd, char *s, size_t len) {
	size_t aux = len;

	while(len > 0) {
		ssize_t r = write(fd, s, len);
		if(r < 0)
			return r;
		len -= r;
		s += r;
	}
	return aux;
}

void *
xmalloc(size_t len) {
	void *p = malloc(len);

	if(!p)
		die("Out of memory\n");

	return p;
}

void *
xrealloc(void *p, size_t len) {
	if((p = realloc(p, len)) == NULL)
		die("Out of memory\n");

	return p;
}

int
utf8decode(char *s, long *u) {
	uchar c;
	int i, n, rtn;

	rtn = 1;
	c = *s;
	if(~c & 0x80) { /* 0xxxxxxx */
		*u = c;
		return rtn;
	} else if((c & 0xE0) == 0xC0) { /* 110xxxxx */
		*u = c & 0x1F;
		n = 1;
	} else if((c & 0xF0) == 0xE0) { /* 1110xxxx */
		*u = c & 0x0F;
		n = 2;
	} else if((c & 0xF8) == 0xF0) { /* 11110xxx */
		*u = c & 0x07;
		n = 3;
	} else {
		goto invalid;
	}

	for(i = n, ++s; i > 0; --i, ++rtn, ++s) {
		c = *s;
		if((c & 0xC0) != 0x80) /* 10xxxxxx */
			goto invalid;
		*u <<= 6;
		*u |= c & 0x3F;
	}

	if((n == 1 && *u < 0x80) ||
	   (n == 2 && *u < 0x800) ||
	   (n == 3 && *u < 0x10000) ||
	   (*u >= 0xD800 && *u <= 0xDFFF)) {
		goto invalid;
	}

	return rtn;
invalid:
	*u = 0xFFFD;

	return rtn;
}

int
utf8encode(long *u, char *s) {
	uchar *sp;
	ulong uc;
	int i, n;

	sp = (uchar *)s;
	uc = *u;
	if(uc < 0x80) {
		*sp = uc; /* 0xxxxxxx */
		return 1;
	} else if(*u < 0x800) {
		*sp = (uc >> 6) | 0xC0; /* 110xxxxx */
		n = 1;
	} else if(uc < 0x10000) {
		*sp = (uc >> 12) | 0xE0; /* 1110xxxx */
		n = 2;
	} else if(uc <= 0x10FFFF) {
		*sp = (uc >> 18) | 0xF0; /* 11110xxx */
		n = 3;
	} else {
		goto invalid;
	}

	for(i=n,++sp; i>0; --i,++sp)
		*sp = ((uc >> 6*(i-1)) & 0x3F) | 0x80; /* 10xxxxxx */

	return n+1;
invalid:
	/* U+FFFD */
	*s++ = '\xEF';
	*s++ = '\xBF';
	*s = '\xBD';

	return 3;
}

/* use this if your buffer is less than UTF_SIZ, it returns 1 if you can decode
   UTF-8 otherwise return 0 */
int
isfullutf8(char *s, int b) {
	uchar *c1, *c2, *c3;

	c1 = (uchar *)s;
	c2 = (uchar *)++s;
	c3 = (uchar *)++s;
	if(b < 1) {
		return 0;
	} else if((*c1 & 0xE0) == 0xC0 && b == 1) {
		return 0;
	} else if((*c1 & 0xF0) == 0xE0 &&
	    ((b == 1) ||
	    ((b == 2) && (*c2 & 0xC0) == 0x80))) {
		return 0;
	} else if((*c1 & 0xF8) == 0xF0 &&
	    ((b == 1) ||
	    ((b == 2) && (*c2 & 0xC0) == 0x80) ||
	    ((b == 3) && (*c2 & 0xC0) == 0x80 && (*c3 & 0xC0) == 0x80))) {
		return 0;
	} else {
		return 1;
	}
}

int
utf8size(char *s) {
	uchar c = *s;

	if(~c & 0x80) {
		return 1;
	} else if((c & 0xE0) == 0xC0) {
		return 2;
	} else if((c & 0xF0) == 0xE0) {
		return 3;
	} else {
		return 4;
	}
}

void
die(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

void
csidump(void) {
	int i;
	uint c;

	printf("ESC[");
	for(i = 0; i < csiescseq.len; i++) {
		c = csiescseq.buf[i] & 0xff;
		if(isprint(c)) {
			putchar(c);
		} else if(c == '\n') {
			printf("(\\n)");
		} else if(c == '\r') {
			printf("(\\r)");
		} else if(c == 0x1b) {
			printf("(\\e)");
		} else {
			printf("(%02x)", c);
		}
	}
	putchar('\n');
}

void
strdump(void) {
	int i;
	uint c;

	printf("ESC%c", strescseq.type);
	for(i = 0; i < strescseq.len; i++) {
		c = strescseq.buf[i] & 0xff;
		if(c == '\0') {
			return;
		} else if(isprint(c)) {
			putchar(c);
		} else if(c == '\n') {
			printf("(\\n)");
		} else if(c == '\r') {
			printf("(\\r)");
		} else if(c == 0x1b) {
			printf("(\\e)");
		} else {
			printf("(%02x)", c);
		}
	}
	printf("ESC\\\n");
}
