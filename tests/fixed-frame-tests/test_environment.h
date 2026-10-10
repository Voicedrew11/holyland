// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>

inline void setTestEnvironment(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
