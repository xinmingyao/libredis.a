#ifndef _REDIS_FMACRO_H
#define _REDIS_FMACRO_H

#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#endif

#ifdef __linux__
#ifndef _XOPEN_SOURCE 
#define _XOPEN_SOURCE 700
#endif
#else
#define _XOPEN_SOURCE
#endif

#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE
#endif

#define _FILE_OFFSET_BITS 64

#endif
