/*-
 * Copyright (c) 2015-2026 Devin Teske <dteske@FreeBSD.org>
 * Copyright (c) 2021-2026 Faraz Vahedi <kfv@kfv.io>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "figput.h"

/*
 * Parsed extent of a single statement within the in-memory copy of the file.
 * All members are byte offsets into the buffer except as noted.  A statement
 * runs from the first byte of its directive to its terminator (newline, an
 * inline `#' comment, or a semicolon when enabled).
 */
struct figput_stmt {
	size_t	line_start;	/* start of the physical line (for removal) */
	size_t	dir_start;	/* first byte of the directive */
	size_t	dir_end;	/* one past the last byte of the directive */
	size_t	val_start;	/* first byte of the value (== val_end if none) */
	size_t	val_end;	/* one past last byte of value (whitespace trimmed) */
	size_t	term;		/* index of the terminator (`\n', `#', `;', or EOF) */
	size_t	line_end;	/* one past the terminating newline (for removal) */
	uint32_t line;		/* 1-based line number of the directive */
	int	have_value;	/* nonzero if a value was present */
	int	have_equals;	/* nonzero if an `=' separated directive/value */
};

/*
 * Write exactly `len' bytes from `data' to `fd', restarting short and
 * interrupted writes.  Returns zero on success; -1 (with errno set) on error.
 */
static int
figput_writeall(int fd, const void *data, size_t len)
{
	const char *p = data;
	ssize_t w;

	while (len > 0) {
		w = write(fd, p, len);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		p += w;
		len -= (size_t)w;
	}
	return (0);
}

/*
 * Read the entire contents of the open descriptor `fd' (whose size is `size')
 * into a freshly allocated, null-terminated buffer.  The actual number of bytes
 * read is stored through `lenp'.  Returns the buffer on success (which the
 * caller must free) or NULL (with errno set) on error.
 */
static char *
figput_readfile(int fd, size_t size, size_t *lenp)
{
	char *buf;
	size_t off = 0;
	ssize_t r;

	if ((buf = malloc(size + 1)) == NULL)
		return (NULL);

	while (off < size) {
		r = read(fd, buf + off, size - off);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return (NULL);
		}
		if (r == 0) /* premature EOF (file shrank); stop */
			break;
		off += (size_t)r;
	}

	buf[off] = '\0';
	*lenp = off;
	return (buf);
}

/*
 * Write `len' bytes to `fd' and, when any bytes are written, remember the last
 * one through `last' (as an unsigned char, or left untouched for a zero-length
 * write).  This lets the caller track whether the output so far ends in a
 * newline without having to re-read the descriptor.  Returns zero on success;
 * -1 (with errno set) on error.
 */
static int
figput_emit(int fd, const void *data, size_t len, int *last)
{
	if (len == 0)
		return (0);
	if (figput_writeall(fd, data, len) != 0)
		return (-1);
	*last = (unsigned char)((const char *)data)[len - 1];
	return (0);
}

/*
 * Determine whether the string value `s' must be double-quoted on output so
 * that it round-trips through the parser unchanged.  Empty strings are quoted
 * so that they remain distinguishable from a value-less directive.
 */
static int
figput_needs_quote(const char *s, int bsemicolon)
{
	const char *p;

	if (*s == '\0')
		return (1);
	for (p = s; *p != '\0'; p++) {
		if (isspace((unsigned char)*p) || *p == '#' || *p == '"' ||
		    *p == '\\')
			return (1);
		if (bsemicolon && *p == ';')
			return (1);
	}
	return (0);
}

/*
 * Produce the textual value to be written for `option', following the value
 * model documented in figput.h: the value is always taken from value.str and
 * the type governs only whether quoting/escaping is applied.  Numeric and
 * boolean tokens, and everything under FIGPUT_SAVE_UNQUOTED, are emitted
 * verbatim.  Returns a newly allocated null-terminated string (which the
 * caller must free) or NULL (with errno set) on allocation failure.
 */
static char *
figput_format_value(const struct figput_config *option, int unquoted,
    int bsemicolon)
{
	const char *s;
	char *out;
	char *q;
	size_t extra;
	size_t i;
	size_t slen;

	if (option->type == FIGPUT_TYPE_NONE)
		return (strdup(""));

	s = option->value.str != NULL ? option->value.str : "";

	if (unquoted || option->type == FIGPUT_TYPE_INT ||
	    option->type == FIGPUT_TYPE_UINT || option->type == FIGPUT_TYPE_BOOL)
		return (strdup(s));

	if (!figput_needs_quote(s, bsemicolon))
		return (strdup(s));

	/* Count the characters that require a backslash escape */
	slen = strlen(s);
	extra = 0;
	for (i = 0; i < slen; i++)
		if (s[i] == '"' || s[i] == '\\')
			extra++;

	/* Enclosing quotes plus escapes plus terminator */
	if ((out = malloc(slen + extra + 3)) == NULL)
		return (NULL);
	q = out;
	*q++ = '"';
	for (i = 0; i < slen; i++) {
		if (s[i] == '"' || s[i] == '\\')
			*q++ = '\\';
		*q++ = s[i];
	}
	*q++ = '"';
	*q = '\0';
	return (out);
}

/*
 * Test whether a SET_VALUE/CHECK value is considered "empty" for the purposes
 * of FIGPUT_SAVE_ALLOW_EMPTY.
 */
static int
figput_value_empty(const struct figput_config *option)
{
	return (option->type != FIGPUT_TYPE_NONE &&
	    (option->value.str == NULL || option->value.str[0] == '\0'));
}

/*
 * Match the directive spanning buf[start, end) against the directive of a
 * figput_config option.  Comparison is exact (whole-token); unlike figpar(3),
 * put_config() does not glob-match, since a pattern is not a writable target.
 */
static int
figput_dir_matches(const char *buf, size_t start, size_t end,
    const char *directive, int case_sensitive)
{
	size_t len = end - start;

	if (strlen(directive) != len)
		return (0);
	if (case_sensitive)
		return (strncmp(buf + start, directive, len) == 0);
	return (strncasecmp(buf + start, directive, len) == 0);
}

/*
 * Tokenize the statement beginning at buf[*ip], advancing *ip past it and
 * filling in `st'.  The scan mirrors figpar(3)'s parser exactly (comment,
 * quote, and backslash-escape handling included) so that a file written by
 * put_config() re-parses identically.  Returns 1 if a statement was found, or
 * 0 at end of input.
 */
static int
figput_scan(const char *buf, size_t len, size_t *ip, uint32_t *linep,
    int bequals, int bsemicolon, int strict_equals, struct figput_stmt *st)
{
	size_t i = *ip;
	size_t j;
	uint32_t line = *linep;
	int comment = 0;
	int novalue = 0;
	int quote;

	/* Skip whitespace and comments to the beginning of a directive */
	st->line_start = i;
	while (i < len && (isspace((unsigned char)buf[i]) || buf[i] == '#' ||
	    comment || (bsemicolon && buf[i] == ';'))) {
		if (buf[i] == '#')
			comment = 1;
		else if (buf[i] == '\n') {
			comment = 0;
			line++;
			st->line_start = i + 1;
		}
		i++;
	}
	if (i >= len) {
		*ip = i;
		*linep = line;
		return (0);
	}

	st->line = line;
	st->dir_start = i;
	st->have_equals = 0;

	/* Find the end of the directive */
	while (i < len) {
		if (isspace((unsigned char)buf[i]))
			break;
		if (bequals && buf[i] == '=') {
			st->have_equals = 1;
			break;
		}
		if (bsemicolon && buf[i] == ';')
			break;
		i++;
	}
	st->dir_end = i;

	/* Step over an `=' acting as the directive terminator */
	if (bequals && i < len && buf[i] == '=') {
		i++;
		if (strict_equals && i < len && isspace((unsigned char)buf[i]) &&
		    buf[i] != '\n')
			novalue = 1; /* strict `=' followed by space: no value */
	}

	/* Advance across separating whitespace to the value */
	if (!novalue && !(bsemicolon && i < len && buf[i] == ';') &&
	    !(strict_equals && i < len && buf[i] == '=')) {
		while (i < len && isspace((unsigned char)buf[i]) && buf[i] != '\n')
			i++;
	}

	/* Consume an `=' surrounded by whitespace (non-strict) */
	if (!novalue && i < len && bequals && buf[i] == '=' && !strict_equals) {
		st->have_equals = 1;
		i++;
		while (i < len && isspace((unsigned char)buf[i]) && buf[i] != '\n')
			i++;
	}

	/* A directive with no value */
	if (novalue || i >= len || buf[i] == '\n' || buf[i] == '#' ||
	    (bsemicolon && buf[i] == ';')) {
		st->have_value = 0;
		st->val_start = st->val_end = i;
	} else {
		st->have_value = 1;
		st->val_start = i;
		quote = 0;
		while (i < len) {
			char c = buf[i];

			if (c != '"' && c != '#' && c != '\n' &&
			    (!bsemicolon || c != ';')) {
				i++;
				continue;
			}

			/* Count the backslashes immediately preceding `c' */
			j = i;
			while (j > st->val_start && buf[j - 1] == '\\')
				j--;

			if (((i - j) & 1) == 0) { /* not escaped */
				if (c == '"') {
					quote = !quote;
					i++;
					continue;
				}
				if (c == '#') {
					if (!quote)
						break;
					i++;
					continue;
				}
				if (c == '\n')
					break;
				if (c == ';') {
					if (!quote && bsemicolon)
						break;
					i++;
					continue;
				}
			} else { /* escaped: part of the value */
				if (c == '\n')
					line++;
				i++;
				continue;
			}
		}
		/* Trim trailing whitespace from the value */
		st->val_end = i;
		while (st->val_end > st->val_start &&
		    isspace((unsigned char)buf[st->val_end - 1]))
			st->val_end--;
	}

	/* Record the terminator position and the end of the physical line */
	st->term = i;
	st->line_end = i;
	while (st->line_end < len && buf[st->line_end] != '\n')
		st->line_end++;
	if (st->line_end < len)
		st->line_end++;

	*ip = i;
	*linep = line;
	return (1);
}

/*
 * Search for a config option (struct figput_config) in the array of config
 * options and stage the value of the struct whose directive matches the given
 * parameter.  On success, returns 1, otherwise 0.
 */
int
set_config_option(struct figput_config options[], const char *directive,
    union figput_cfgvalue *value)
{
	uint32_t n;

	/* Check arguments */
	if (options == NULL || directive == NULL || value == NULL)
		return (0);

	/* Loop through the array, staging the first match */
	for (n = 0; options[n].directive != NULL; n++) {
		if (strcmp(options[n].directive, directive) == 0) {
			options[n].value = *value;
			return (1);
		}
	}

	return (0);
}

/*
 * Rewrite the configuration file at `path', applying the per-directive actions
 * described by the array of config options (first argument).  Each option's
 * action is one of:
 *
 *	FIGPUT_ACTION_SET_VALUE	 set the directive to option->value.str,
 *				 editing it in place if present or appending it
 *				 to the file if absent;
 *	FIGPUT_ACTION_REMOVE	 delete the directive from the file;
 *	FIGPUT_ACTION_CHECK	 report (via option->result) whether the current
 *				 value differs from option->value, without
 *				 modifying the file.
 *
 * Comments, blank lines, statement ordering, and the formatting of untouched
 * statements are preserved.  For each processed option, option->result is set
 * to a bitmask of FIGPUT_DIRECTIVE_FOUND, FIGPUT_VALUE_CHANGED,
 * FIGPUT_DIRECTIVE_ADDED, and FIGPUT_DIRECTIVE_REMOVED, and option->line is set
 * to the line of the first match (if found).
 *
 * The file is replaced atomically: output is written to a temporary file in
 * the same directory, flushed to disk, given the mode (and, if permitted, the
 * ownership) of the original, and then renamed over it.
 *
 * Returns zero on success; otherwise returns -1 and errno should be consulted.
 */
int
put_config(struct figput_config options[], const char *path,
    uint16_t processing_options, uint16_t put_options)
{
	int backup;
	int bequals;
	int bsemicolon;
	int case_sensitive;
	int emptyok;
	int nodflt;
	int nodup;
	int require_equals;
	int strict_equals;
	int unquoted;
	int dirfd = -1;
	int fd = -1;
	int last_ch = -1; /* last byte emitted, or -1 if none, for newlines */
	int rv = -1;
	int saved_errno;
	int tmpfd = -1;
	char *buf = NULL;
	char *slash;
	char *val = NULL;
	const char *sep;
	size_t buflen = 0;
	size_t i;
	size_t n;
	size_t vlen;
	size_t wc; /* write cursor: next unemitted byte of buf */
	uint32_t line = 1;
	struct figput_config *option;
	struct figput_stmt st;
	struct stat sb;
	char rpath[PATH_MAX];
	char tpath[PATH_MAX];

	/* Sanity check the arguments */
	if (options == NULL || path == NULL) {
		errno = EINVAL;
		return (-1);
	}

	/* Decode processing options */
	bequals = (processing_options & FIGPUT_BREAK_ON_EQUALS) != 0;
	bsemicolon = (processing_options & FIGPUT_BREAK_ON_SEMICOLON) != 0;
	case_sensitive = (processing_options & FIGPUT_CASE_SENSITIVE) != 0;
	require_equals = (processing_options & FIGPUT_REQUIRE_EQUALS) != 0;
	strict_equals = (processing_options & FIGPUT_STRICT_EQUALS) != 0;

	/* Decode put options */
	backup = (put_options & FIGPUT_SAVE_BACKUP) != 0;
	emptyok = (put_options & FIGPUT_SAVE_ALLOW_EMPTY) != 0;
	nodflt = (put_options & FIGPUT_NO_SAVE_DEFAULTS) != 0;
	nodup = (put_options & FIGPUT_NO_SAVE_DUPLICATES) != 0;
	unquoted = (put_options & FIGPUT_SAVE_UNQUOTED) != 0;

	/* Reset per-option results and reject empty values up front */
	for (n = 0; options[n].directive != NULL; n++) {
		options[n].result = 0;
		options[n].line = 0;
		if (!emptyok && options[n].action == FIGPUT_ACTION_SET_VALUE &&
		    figput_value_empty(&options[n])) {
			errno = EINVAL;
			return (-1);
		}
	}

	/* Resolve the file path */
	if (realpath(path, rpath) == NULL)
		return (-1);

	/* Open the original for reading */
	if ((fd = open(rpath, O_RDONLY)) < 0)
		return (-1);
	if (fstat(fd, &sb) != 0)
		goto cleanup;
	if (!S_ISREG(sb.st_mode)) {
		errno = EINVAL;
		goto cleanup;
	}

	/* Slurp the original into memory */
	if ((buf = figput_readfile(fd, (size_t)sb.st_size, &buflen)) == NULL)
		goto cleanup;

	/* Build the temporary file path in the target's own directory */
	if (strlcpy(tpath, rpath, sizeof(tpath)) >= sizeof(tpath)) {
		errno = ENAMETOOLONG;
		goto cleanup;
	}
	if ((slash = strrchr(tpath, '/')) != NULL)
		slash[1] = '\0'; /* keep trailing slash, drop basename */
	else
		tpath[0] = '\0';
	if (strlcat(tpath, ".figput.XXXXXXXXXX", sizeof(tpath)) >=
	    sizeof(tpath)) {
		errno = ENAMETOOLONG;
		goto cleanup;
	}
	if ((tmpfd = mkstemp(tpath)) == -1)
		goto cleanup;

	/* Give the temporary file the original's mode (and, if we can, owner) */
	if (fchmod(tmpfd, sb.st_mode & ALLPERMS) != 0)
		goto cleanup;
	(void)fchown(tmpfd, sb.st_uid, sb.st_gid); /* best effort */

	/*
	 * Walk the original statement by statement.  Bytes are emitted to the
	 * temporary file in order via the write cursor `wc'; a matched
	 * statement diverts around the region it edits or removes.
	 */
	wc = 0;
	i = 0;
	while (figput_scan(buf, buflen, &i, &line, bequals, bsemicolon,
	    strict_equals, &st)) {
		/* Locate the option (if any) matching this directive */
		option = NULL;
		for (n = 0; options[n].directive != NULL; n++) {
			if (figput_dir_matches(buf, st.dir_start, st.dir_end,
			    options[n].directive, case_sensitive)) {
				option = &options[n];
				break;
			}
		}
		if (option == NULL)
			continue; /* untouched; bytes flushed later */

		/* Enforce the no-duplicates policy */
		if (nodup && (option->result & FIGPUT_DIRECTIVE_FOUND) != 0) {
			errno = EEXIST;
			goto cleanup;
		}
		if ((option->result & FIGPUT_DIRECTIVE_FOUND) == 0)
			option->line = st.line;
		option->result |= FIGPUT_DIRECTIVE_FOUND;

		if (option->action == FIGPUT_ACTION_REMOVE) {
			size_t rm_start;
			size_t rm_end;
			size_t k;
			int done = 0;
			int first_on_line = 1;

			/*
			 * Is this the first statement on its physical line?
			 * With FIGPUT_BREAK_ON_SEMICOLON an earlier statement
			 * may precede it on the same line.
			 */
			for (k = st.line_start; k < st.dir_start; k++)
				if (!isspace((unsigned char)buf[k])) {
					first_on_line = 0;
					break;
				}

			/*
			 * If a further statement follows on the same line
			 * (terminator is a semicolon with a directive after
			 * it), drop this statement and the trailing separator,
			 * leaving the neighbour intact.
			 */
			if (bsemicolon && st.term < buflen &&
			    buf[st.term] == ';') {
				size_t f = st.term + 1;

				while (f < buflen &&
				    isspace((unsigned char)buf[f]) &&
				    buf[f] != '\n')
					f++;
				if (f < buflen && buf[f] != '\n' &&
				    buf[f] != '#') {
					rm_start = st.dir_start;
					rm_end = f;
					done = 1;
				}
			}

			if (!done && first_on_line) {
				/* Alone on the line: drop the whole line */
				rm_start = st.line_start;
				rm_end = st.line_end;
			} else if (!done) {
				/*
				 * Last on a shared line: drop the preceding
				 * separator (whitespace, `;', whitespace) along
				 * with this statement, keeping the terminator.
				 */
				size_t s = st.dir_start;

				while (s > st.line_start &&
				    isspace((unsigned char)buf[s - 1]))
					s--;
				if (s > st.line_start && buf[s - 1] == ';')
					s--;
				while (s > st.line_start &&
				    isspace((unsigned char)buf[s - 1]))
					s--;
				rm_start = s;
				rm_end = st.term;
			}

			/* Never rewind before already-emitted bytes */
			if (rm_start < wc)
				rm_start = wc;
			if (figput_emit(tmpfd, buf + wc, rm_start - wc,
			    &last_ch) != 0)
				goto cleanup;
			if (rm_end > wc)
				wc = rm_end;
			option->result |= FIGPUT_DIRECTIVE_REMOVED;
			continue;
		}

		/* SET_VALUE and CHECK both need the formatted value */
		free(val);
		if ((val = figput_format_value(option, unquoted,
		    bsemicolon)) == NULL)
			goto cleanup;
		vlen = strlen(val);

		/* Compare against the value currently in the file */
		n = st.val_end - st.val_start;
		if (vlen == n && (n == 0 ||
		    memcmp(val, buf + st.val_start, n) == 0)) {
			/* Unchanged; nothing to do for either action */
			continue;
		}

		if (option->action == FIGPUT_ACTION_CHECK) {
			option->result |= FIGPUT_VALUE_CHANGED;
			continue;
		}

		/* FIGPUT_ACTION_SET_VALUE and the value differs */
		if (nodflt)
			continue; /* skip redundant/default-equal rewrites */

		if (st.have_value) {
			/* Replace the existing value in place */
			if (figput_emit(tmpfd, buf + wc, st.val_start - wc,
			    &last_ch) != 0)
				goto cleanup;
			if (figput_emit(tmpfd, val, vlen, &last_ch) != 0)
				goto cleanup;
			wc = st.val_end;
		} else if (option->type != FIGPUT_TYPE_NONE) {
			/* Insert a value onto a value-less directive */
			if (figput_emit(tmpfd, buf + wc, st.val_start - wc,
			    &last_ch) != 0)
				goto cleanup;
			/* Supply a separator unless one already precedes us */
			if (st.val_start == wc || (buf[st.val_start - 1] != '=' &&
			    !isspace((unsigned char)buf[st.val_start - 1]))) {
				sep = st.have_equals || require_equals || bequals ?
				    "=" : " ";
				if (figput_emit(tmpfd, sep, 1, &last_ch) != 0)
					goto cleanup;
			}
			if (figput_emit(tmpfd, val, vlen, &last_ch) != 0)
				goto cleanup;
			wc = st.val_start;
		}
		option->result |= FIGPUT_VALUE_CHANGED;
	}

	/* Flush the tail of the original */
	if (figput_emit(tmpfd, buf + wc, buflen - wc, &last_ch) != 0)
		goto cleanup;

	/*
	 * Append any SET_VALUE directives that were not found, and finalize the
	 * result flags for CHECK directives that were absent.
	 */
	sep = (bequals || require_equals) ? "=" : " ";
	for (n = 0; options[n].directive != NULL; n++) {
		option = &options[n];
		if ((option->result & FIGPUT_DIRECTIVE_FOUND) != 0)
			continue;
		if (option->action == FIGPUT_ACTION_CHECK) {
			/* Absent means it differs from the desired value */
			option->result |= FIGPUT_VALUE_CHANGED;
			continue;
		}
		if (option->action != FIGPUT_ACTION_SET_VALUE)
			continue; /* REMOVE of an absent directive: no-op */

		/* Ensure the emitted output ends with a newline first */
		if (last_ch != -1 && last_ch != '\n') {
			if (figput_emit(tmpfd, "\n", 1, &last_ch) != 0)
				goto cleanup;
		}

		if (figput_emit(tmpfd, option->directive,
		    strlen(option->directive), &last_ch) != 0)
			goto cleanup;
		if (option->type != FIGPUT_TYPE_NONE) {
			free(val);
			if ((val = figput_format_value(option, unquoted,
			    bsemicolon)) == NULL)
				goto cleanup;
			if (figput_emit(tmpfd, sep, 1, &last_ch) != 0)
				goto cleanup;
			if (figput_emit(tmpfd, val, strlen(val), &last_ch) != 0)
				goto cleanup;
		}
		if (figput_emit(tmpfd, "\n", 1, &last_ch) != 0)
			goto cleanup;
		option->result |= FIGPUT_DIRECTIVE_ADDED | FIGPUT_VALUE_CHANGED;
	}

	/* Optionally back up the original before replacing it */
	if (backup) {
		char bpath[PATH_MAX];
		int bfd;

		if (strlcpy(bpath, rpath, sizeof(bpath)) >= sizeof(bpath) ||
		    strlcat(bpath, ".bak", sizeof(bpath)) >= sizeof(bpath)) {
			errno = ENAMETOOLONG;
			goto cleanup;
		}
		if ((bfd = open(bpath, O_WRONLY | O_CREAT | O_TRUNC,
		    sb.st_mode & ALLPERMS)) < 0)
			goto cleanup;
		if (figput_writeall(bfd, buf, buflen) != 0) {
			saved_errno = errno;
			close(bfd);
			errno = saved_errno;
			goto cleanup;
		}
		if (close(bfd) != 0)
			goto cleanup;
	}

	/* Commit: flush to disk, then atomically replace the original */
	if (fsync(tmpfd) != 0)
		goto cleanup;
	if (close(tmpfd) != 0) {
		tmpfd = -1;
		goto cleanup;
	}
	tmpfd = -1;
	if (rename(tpath, rpath) != 0)
		goto cleanup;
	tpath[0] = '\0'; /* renamed away; nothing to unlink */

	/* Best-effort: persist the directory entry change */
	if ((slash = strrchr(rpath, '/')) != NULL) {
		char dpath[PATH_MAX];
		size_t dlen = (size_t)(slash - rpath);

		if (dlen == 0)
			dlen = 1; /* the root directory */
		if (dlen < sizeof(dpath)) {
			memcpy(dpath, rpath, dlen);
			dpath[dlen] = '\0';
			if ((dirfd = open(dpath, O_RDONLY)) >= 0) {
				(void)fsync(dirfd);
				close(dirfd);
				dirfd = -1;
			}
		}
	}

	rv = 0;

cleanup:
	saved_errno = errno;
	if (fd >= 0)
		close(fd);
	if (tmpfd >= 0)
		close(tmpfd);
	if (dirfd >= 0)
		close(dirfd);
	if (rv != 0 && tpath[0] != '\0')
		(void)unlink(tpath); /* discard the incomplete temporary */
	free(buf);
	free(val);
	errno = saved_errno;
	return (rv);
}
