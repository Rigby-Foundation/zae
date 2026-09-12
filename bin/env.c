/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>

extern char **environ;

int main(void)
{
    for (char **e = environ; e && *e; e++)
        puts(*e);
    return 0;
}
