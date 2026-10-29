/* SwanStation Xbox: utime on the CRT's _utime. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <sys/utime.h>
#define utimbuf _utimbuf
#define utime _utime
