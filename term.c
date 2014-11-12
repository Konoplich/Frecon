/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <ctype.h>
#include <libtsm.h>
#include <paths.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "font.h"
#include "input.h"
#include "shl_pty.h"
#include "term.h"
#include "util.h"
#include "video.h"
#include "dbus_interface.h"

static void __attribute__ ((noreturn)) term_run_child()
{
	char **argv = (char *[]) {
		getenv("SHELL") ? : "/bin/bash",
		"-il",
		NULL
	};

	printf("Welcome to frecon!\n");
	printf("running %s\n", argv[0]);
	/* XXX figure out how to fix "top" for xterm-256color */
	setenv("TERM", "xterm", 1);
	execve(argv[0], argv, environ);
	exit(1);
}

static int term_draw_cell(struct tsm_screen *screen, uint32_t id,
				const uint32_t *ch, size_t len,
				unsigned int cwidth, unsigned int posx,
				unsigned int posy,
				const struct tsm_screen_attr *attr,
				tsm_age_t age, void *data)
{
	terminal_t *terminal = (terminal_t*)data;
	uint32_t front_color, back_color;

	if (age && terminal->term->age && age <= terminal->term->age)
		return 0;

	front_color = (attr->fr << 16) | (attr->fg << 8) | attr->fb;
	back_color = (attr->br << 16) | (attr->bg << 8) | attr->bb;

	if (attr->inverse) {
		uint32_t tmp = front_color;
		front_color = back_color;
		back_color = tmp;
	}

	if (len)
		font_render(terminal->term->dst_image, posx, posy, terminal->term->pitch, *ch,
					front_color, back_color);
	else
		font_fillchar(terminal->term->dst_image, posx, posy, terminal->term->pitch,
						front_color, back_color);

	return 0;
}

void term_redraw(terminal_t *terminal)
{
	uint32_t *video_buffer;
	video_buffer = video_lock(terminal->video);
	if (video_buffer != NULL) {
		terminal->term->dst_image = video_buffer;
		terminal->term->age =
			tsm_screen_draw(terminal->term->screen, term_draw_cell, terminal);
		video_unlock(terminal->video);
	}
}

void term_key_event(terminal_t* terminal, uint32_t keysym, int32_t unicode)
{

	if (tsm_vte_handle_keyboard(terminal->term->vte, keysym, 0, 0, unicode))
		tsm_screen_sb_reset(terminal->term->screen);

	term_redraw(terminal);
}

static void term_read_cb(struct shl_pty *pty, char *u8, size_t len, void *data)
{
	terminal_t *terminal = (terminal_t*)data;

	tsm_vte_input(terminal->term->vte, u8, len);

	term_redraw(terminal);
}

static void term_write_cb(struct tsm_vte *vte, const char *u8, size_t len,
				void *data)
{
	struct term *term = data;
	int r;

	r = shl_pty_write(term->pty, u8, len);
	if (r < 0)
		LOG(ERROR, "OOM in pty-write (%d)", r);

	shl_pty_dispatch(term->pty);
}

static const char *sev2str_table[] = {
	"FATAL",
	"ALERT",
	"CRITICAL",
	"ERROR",
	"WARNING",
	"NOTICE",
	"INFO",
	"DEBUG"
};

static const char *sev2str(unsigned int sev)
{
	if (sev > 7)
		return "DEBUG";

	return sev2str_table[sev];
}

#ifdef __clang__
__attribute__((__format__ (__printf__, 7, 0)))
#endif
static void log_tsm(void *data, const char *file, int line, const char *fn,
				const char *subs, unsigned int sev, const char *format,
				va_list args)
{
	fprintf(stderr, "%s: %s: ", sev2str(sev), subs);
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
}

terminal_t* term_init()
{
	const int scrollback_size = 200;
	uint32_t char_width, char_height;
	int32_t width, height, pitch, scaling;
	int status;
	terminal_t *new_terminal;

	new_terminal = (terminal_t*)calloc(1, sizeof(*new_terminal));
	new_terminal->video = video_init(&width, &height, &pitch, &scaling);
	new_terminal->term = (struct term*)calloc(1, sizeof(*new_terminal->term));

	font_init(video_getscaling(new_terminal->video));
	font_get_size(&char_width, &char_height);

	new_terminal->term->char_x =
		video_getwidth(new_terminal->video) / char_width;
	new_terminal->term->char_y =
		video_getheight(new_terminal->video) / char_height;
	new_terminal->term->pitch = video_getpitch(new_terminal->video);

	status = tsm_screen_new(&new_terminal->term->screen,
			log_tsm, new_terminal->term);
	if (new_terminal < 0) {
		term_close(new_terminal);
		return NULL;
	}
	

	tsm_screen_set_max_sb(new_terminal->term->screen, scrollback_size);

	status = tsm_vte_new(&new_terminal->term->vte, new_terminal->term->screen,
			term_write_cb, new_terminal->term, log_tsm, new_terminal->term);

	if (status < 0) {
		term_close(new_terminal);
		return NULL;
	}

	new_terminal->term->pty_bridge = shl_pty_bridge_new();
	if (new_terminal->term->pty_bridge < 0) {
		term_close(new_terminal);
		return NULL;
	}

	status = shl_pty_open(&new_terminal->term->pty,
			term_read_cb, new_terminal, new_terminal->term->char_x,
			new_terminal->term->char_y);
	if (status < 0) {
		term_close(new_terminal);
		return NULL;
	} else if (status == 0) {
		term_run_child();
		exit(1);
	}

	status = shl_pty_bridge_add(new_terminal->term->pty_bridge, new_terminal->term->pty);
	if (status) {
		shl_pty_close(new_terminal->term->pty);
		term_close(new_terminal);
		return NULL;
	}

	new_terminal->term->pid = shl_pty_get_child(new_terminal->term->pty);

	status = tsm_screen_resize(new_terminal->term->screen,
			new_terminal->term->char_x, new_terminal->term->char_y);
	if (status < 0) {
		shl_pty_close(new_terminal->term->pty);
		term_close(new_terminal);
		return NULL;
	}

	status = shl_pty_resize(new_terminal->term->pty, new_terminal->term->char_x, new_terminal->term->char_y);
	if (status < 0) {
		shl_pty_close(new_terminal->term->pty);
		term_close(new_terminal);
		return NULL;
	}

	return new_terminal;
}

void term_set_dbus(terminal_t *term, dbus_t* dbus)
{
	term->dbus = dbus;
}

void term_close(terminal_t *term)
{
	if (!term)
		return;

	if (term->term) {
		free(term->term);
		term->term = NULL;
	}

	free(term);
}

bool term_is_child_done(terminal_t* terminal)
{
	int status;
	int ret;
	ret = waitpid(terminal->term->pid, &status, WNOHANG);

	return ret != 0;
}
