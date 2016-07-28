/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

void daemonize()
{
	pid_t pid;
	int fd;

	pid = fork();
	if (pid == -1)
		return;
	else if (pid != 0)
		exit(EXIT_SUCCESS);

	if (setsid() == -1)
		return;

	// Re-direct stderr/stdout to the system message log
	close(0);
	close(1);
	close(2);

	open("/dev/kmsg", O_RDWR);

	fd = dup(0);
	if (fd != STDOUT_FILENO) {
		close(fd);
		return;
	}
	fd = dup(0);
	if (fd != STDERR_FILENO) {
		close(fd);
		return;
	}
}

#ifdef __clang__
__attribute__((format (__printf__, 2, 0)))
#endif
void LOG(int severity, const char* fmt, ...)
{
	va_list arg_list;
	fprintf(stderr, "frecon(%d): ", getpid());
	va_start( arg_list, fmt);
	vfprintf(stderr, fmt, arg_list);
	va_end(arg_list);
	fprintf(stderr, "\n");
}

void parse_location(char* loc_str, int* x, int* y)
{
	int count = 0;
	char* savedptr;
	char* str;
	int* results[] = {x, y};
	long tmp;

	for (char* token = str = loc_str; token != NULL; str = NULL) {
		if (count > 1)
			break;

		token = strtok_r(str, ",", &savedptr);
		if (token) {
			tmp = MIN(INT_MAX, strtol(token, NULL, 0));
			*(results[count++]) = (int)tmp;
		}
	}
}

void parse_filespec(char* filespec, char* filename,
		    int32_t* offset_x, int32_t* offset_y, uint32_t* duration,
		    uint32_t default_duration,
		    int32_t default_x, int32_t default_y)
{
	char* saved_ptr;
	char* token;

	// defaults
	*offset_x = default_x;
	*offset_y = default_y;
	*duration = default_duration;

	token = filespec;
	token = strtok_r(token, ":", &saved_ptr);
	if (token)
		strcpy(filename, token);

	token = strtok_r(NULL, ":", &saved_ptr);
	if (token) {
		*duration = strtoul(token, NULL, 0);
		token = strtok_r(NULL, ",", &saved_ptr);
		if (token) {
			token = strtok_r(token, ",", &saved_ptr);
			if (token) {
				*offset_x = strtol(token, NULL, 0);
				token = strtok_r(token, ",", &saved_ptr);
				if (token)
					*offset_y = strtol(token, NULL, 0);
			}
		}
	}
}

void parse_image_option(char* optionstr, char** name, char** val)
{
	char** result[2] = { name, val };
	int count = 0;
	char* str;
	char* savedptr;

	for (char* token = str = optionstr; token != NULL; str = NULL) {
		if (count > 1)
			break;

		token = strtok_r(str, ":", &savedptr);
		if (token) {
			*(result[count]) = malloc(strlen(token) + 1);
			strcpy(*(result[count]), token);
			count++;
		}
	}
}
