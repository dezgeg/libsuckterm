void
execsh(void) {
	char **args;
	char *envshell = getenv("SHELL");
	const struct passwd *pass = getpwuid(getuid());
	char buf[sizeof(long) * 8 + 1];

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");

	if(pass) {
		setenv("LOGNAME", pass->pw_name, 1);
		setenv("USER", pass->pw_name, 1);
		setenv("SHELL", pass->pw_shell, 0);
		setenv("HOME", pass->pw_dir, 0);
	}

	snprintf(buf, sizeof(buf), "%lu", xw.win);
	setenv("WINDOWID", buf, 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	DEFAULT(envshell, shell);
	setenv("TERM", termname, 1);
	args = opt_cmd ? opt_cmd : (char *[]){envshell, "-i", NULL};
	execvp(args[0], args);
	exit(EXIT_FAILURE);
}

void
sigchld(int a) {
	int stat = 0;

	if(waitpid(pid, &stat, 0) < 0)
		die("Waiting for pid %hd failed: %s\n", pid, SERRNO);

	if(WIFEXITED(stat)) {
		exit(WEXITSTATUS(stat));
	} else {
		exit(EXIT_FAILURE);
	}
}

void
ttynew(void) {
	int m, s;
	struct winsize w = {term.row, term.col, 0, 0};

	/* seems to work fine on linux, openbsd and freebsd */
	if(openpty(&m, &s, NULL, NULL, &w) < 0)
		die("openpty failed: %s\n", SERRNO);

	switch(pid = fork()) {
	case -1:
		die("fork failed\n");
		break;
	case 0:
		setsid(); /* create a new process group */
		dup2(s, STDIN_FILENO);
		dup2(s, STDOUT_FILENO);
		dup2(s, STDERR_FILENO);
		if(ioctl(s, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", SERRNO);
		close(s);
		close(m);
		execsh();
		break;
	default:
		close(s);
		cmdfd = m;
		signal(SIGCHLD, sigchld);
	}
}
