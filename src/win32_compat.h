#pragma once
#ifdef _WIN32

#include <io.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <cstring>

#define strncasecmp _strnicmp

// getdelim() and mkstemps() are POSIX/BSD extensions MSVCRT/UCRT does not
// provide. HAVE_GETDELIM / HAVE_MKSTEMPS are defined by CMake via
// check_function_exists() when the target runtime already has them, so
// these declarations (and their implementations in system.cc) only apply
// when actually needed. This is scoped to Windows only, so it has no
// effect on the Linux/Mac build.
#ifndef HAVE_GETDELIM
ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* stream);
#endif

#ifndef HAVE_MKSTEMPS
int mkstemps(char* tmpl, int suffixlen);
#endif

#endif
