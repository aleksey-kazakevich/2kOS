#pragma once

typedef void VOID;

#define NULLPTR ((VOID *)0)

typedef enum {
    FALSE = 0,
    TRUE = 1
} BOOL;

#define UINT8MAX   0xFF
#define UINT16MAX  0xFFFF
#define UINT32MAX  0xFFFFFFFF
#define UINT64MAX  0xFFFFFFFFFFFFFFFFULL

#define INT8MAX   0x7F
#define INT16MAX  0x7FFF
#define INT32MAX  0x7FFFFFFF
#define INT64MAX  0x7FFFFFFFFFFFFFFFLL

#define INT8MIN   (-INT8_MAX - 1)
#define INT16MIN  (-INT16_MAX - 1)
#define INT32MIN  (-INT32_MAX - 1)
#define INT64MIN  (-INT64_MAX - 1)

typedef long LONG;
typedef char CHAR;
typedef int INT;
typedef float FLOAT;
typedef double DOUBLE;
typedef short SHORT;

typedef signed char INT8;
typedef unsigned char UINT8;
typedef signed short INT16;
typedef unsigned short UINT16;
typedef signed int INT32;
typedef unsigned int UINT32;
typedef long long INT64;
typedef unsigned long long UINT64;

#if defined(__x86_64__)
    typedef long INTPTR;
    typedef unsigned long UINTPTR;
#elif defined(__i386__)
    typedef INT INTPTR;
    typedef unsigned int UINTPTR; 
#endif

typedef unsigned long long UINTN;
typedef long long INTN;

typedef unsigned long USIZE;

typedef INTPTR PTRDIFF;
typedef INT WCHAR;

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define OffsetOf(TYPE, MEMBER) ((USIZE)&(((TYPE *)0)->MEMBER))
#define ATTRIBUTE(Attr) __attribute__((Attr))
#define EXTERN(Type, Symbol) extern Type Symbol
