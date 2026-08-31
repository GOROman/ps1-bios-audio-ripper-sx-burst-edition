#pragma once

/* 7zTypes.h exposes POSIX error constants even though the freestanding
 * browser decoder never uses them. */
#define EINVAL 22
#define EEXIST 17
#define ENOENT 2
#define ENOSPC 28
