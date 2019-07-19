/*
 * Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "dev.h"
#include "input.h"
#include "util.h"

int dev_init(void)
{
	char path[64];
	int i;

	for (i = 0;; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		if (access(path, F_OK))
			break;
		input_add(path);
	}

	return 0;
}

void dev_close(void)
{
}

void dev_add_fds(fd_set* read_set, fd_set* exception_set, int *maxfd)
{
}

void dev_dispatch_io(fd_set* read_set, fd_set* exception_set)
{
}
