/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define ARRAY_SIZE(A) (sizeof(A)/sizeof(*(A)))

void LOG(int severity, const char* fmt, ...);

#define ERROR                 (1)
#define WARNING               (2)
#define INFO                  (4)

#endif
