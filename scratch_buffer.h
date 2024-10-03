/*
----- ATTENTION! ALL THE CODE IS WRITTEN BY <https://github.com/lerno>, I've simply stitched together parts of his code from different files and made a header-only, stb-style library out of them. The files I've copied code from are: -----
  1. https://github.com/c3lang/c3c/blob/master/src/utils/malloc.c
  2. https://github.com/c3lang/c3c/blob/master/src/utils/malloc.h
  3. https://github.com/c3lang/c3c/blob/master/src/utils/stringutils.c
  4. https://github.com/c3lang/c3c/blob/master/src/utils/vmem.c
  5. https://github.com/c3lang/c3c/blob/master/src/utils/vmem.h
  6. https://github.com/c3lang/c3c/blob/master/src/utils/lib.h
  7. https://github.com/c3lang/c3c/blob/master/src/utils/common.h
  8. https://github.com/c3lang/c3c/blob/master/src/utils/errors.c

---------- The license of the original author, Christoffer Lerno ----------
  Copyright (c) 2019 Christoffer Lerno. All rights reserved.
  Use of this source code is governed by the GNU LGPLv3.0 license
  a copy of which can be found in the LICENSE file.

            -------------------- USAGE --------------------

  First, to include all the implementations you need to define SCRATCH_BUFFER_IMPLEMENTATION, and then just include this file into your project. This is how you do that:

#define SCRATCH_BUFFER_IMPLEMENTATION
#include "scratch_buffer.h"

  If you need to save strings on the heap, created by the scratch buffer, you can use `scratch_buffer_copy` function, but first, you need to initialize `char_arena` on which strings will be allocated on, in order to do that, call `memory_init` function and pass the maximum amount of megabytes can be allocated on the arena, I usually set it something like 1-3. And, do not forget to call `memory_release` at the end of your program to `free` all the data and avoid memory leaks.

  You can take a deeper look into the scratch buffer functions, they are so simple and self-explanatory! To find those, you can search `SCRATCH BUFFER FUNCTIONS` in your editor or just jump to 237th line.
*/

#ifndef SCRATCH_BUFFER_H
#define SCRATCH_BUFFER_H

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32 ) || defined(__WIN32__ ) || defined(_WIN64)
	#define PLATFORM_WINDOWS 1
	#define PLATFORM_POSIX 0
#else
	#define PLATFORM_WINDOWS 0
	#define PLATFORM_POSIX 1
#endif

#define MAX_STRING_BUFFER 0x10000

#if (defined(__GNUC__) && __GNUC__ >= 7) || defined(__clang__)
	#define INLINE __attribute__((always_inline)) inline
	#define NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
	#define INLINE __forceinline
	#define NORETURN __declspec(noreturn)
#else
	#define INLINE inline
	#define NORETURN
#endif

struct ScratchBuf { char str[MAX_STRING_BUFFER]; uint32_t len; };

#ifdef __cplusplus
extern "C" {
#endif

void scratch_buffer_clear(void);
void scratch_buffer_append(const char *string);
void scratch_buffer_append_len(const char *string, size_t len);
void scratch_buffer_append_char(char c);
void scratch_buffer_append_in_quote(const char *string);
void scratch_buffer_append_char_repeat(char c, size_t count);
void scratch_buffer_append_signed_int(int64_t i);
void scratch_buffer_append_double(double d);
void scratch_buffer_append_unsigned_int(uint64_t i);
void scratch_buffer_printf(const char *format, ...);
char *scratch_buffer_to_string(void);
char *scratch_buffer_copy(void);

NORETURN void error_exit(const char *format, ...);

#ifdef __cplusplus
}
#endif

#ifdef SCRATCH_BUFFER_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <assert.h>

#define KB 1024ul
#define MB (KB * 1024ul)

#if PLATFORM_POSIX
#include <sys/mman.h>
#include <errno.h>
#endif
#if PLATFORM_WINDOWS
#include <windows.h>
#define COMMIT_PAGE_SIZE 0x10000
#endif

struct ScratchBuf scratch_buffer;

typedef struct
{
	void *ptr;
	size_t allocated;
	size_t size;
#if PLATFORM_WINDOWS
	size_t committed;
#endif
} Vmem;

static int allocations_done;
static Vmem char_arena;
static size_t max = 0x10000000;

static void vmem_set_max_limit(size_t size_in_mb);
static void vmem_init(Vmem *vmem, size_t size_in_mb);
static void *vmem_alloc(Vmem *vmem, size_t alloc);
static void vmem_free(Vmem *vmem);

//////////////////// ARENA FUNCTIONS ////////////////////

INLINE static void memory_init(size_t max_mem)
{
	if (max_mem) vmem_set_max_limit(max_mem);
	vmem_init(&char_arena, 512);
	allocations_done = 0;
}

INLINE static void memory_release(void)
{
	vmem_free(&char_arena);
}

static inline void mmap_init(Vmem *vmem, size_t size)
{
#if PLATFORM_WINDOWS
	void* ptr = VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS);
	vmem->committed = 0;
	if (!ptr)
	{
		assert(0 && "Failed to map virtual memory block");
	}
#elif PLATFORM_POSIX
	void* ptr = NULL;
	size_t min_size = size / 16;
	if (min_size < 1) min_size = size;
	while (size >= min_size)
	{
		ptr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		// It worked?
		if (ptr != MAP_FAILED && ptr) break;
		// Did it fail in a non-retriable way?
		if (errno != ENOMEM && errno != EOVERFLOW && errno != EAGAIN) break;
		// Try a smaller size
		size /= 2;
	}
	// Check if we ended on a failure.
	if ((ptr == MAP_FAILED) || !ptr)
	{
		assert(0 && "Failed to map a virtual memory block.");
	}
	// Otherwise, record the size and we're fine!
#else
	assert(0 && "Unsupported platform.");
#endif
	vmem->size = size;
	vmem->ptr = ptr;
	vmem->allocated = 0;
}

static inline void* mmap_allocate(Vmem *vmem, size_t to_allocate)
{
	size_t allocated_after = to_allocate + vmem->allocated;
#if PLATFORM_WINDOWS
	size_t blocks_committed = vmem->committed / COMMIT_PAGE_SIZE;
	size_t end_block = (allocated_after + COMMIT_PAGE_SIZE - 1) / COMMIT_PAGE_SIZE;  // round up
	size_t blocks_to_allocate = end_block - blocks_committed;
	if (blocks_to_allocate > 0)
	{
		size_t to_commit = blocks_to_allocate * COMMIT_PAGE_SIZE;
		void *res = VirtualAlloc(((char*)vmem->ptr) + vmem->committed, to_commit, MEM_COMMIT, PAGE_READWRITE);
		if (!res) assert(0 && "Failed to allocate more memory.");
		vmem->committed += to_commit;
	}
#endif
	void *ptr = ((uint8_t *)vmem->ptr) + vmem->allocated;
	vmem->allocated = allocated_after;
	assert(vmem->size > vmem->allocated && "You might've forgot to call `memory_init` function, or you've allocated too much memory");
	return ptr;
}

INLINE static void vmem_set_max_limit(size_t size_in_mb)
{
	max = size_in_mb;
}

INLINE static void vmem_init(Vmem *vmem, size_t size_in_mb)
{
	if (size_in_mb > max) size_in_mb = max;
	mmap_init(vmem, 1024 * 1024 * size_in_mb);
}

INLINE static void *vmem_alloc(Vmem *vmem, size_t alloc)
{
	return mmap_allocate(vmem, alloc);
}

static void vmem_free(Vmem *vmem)
{
	if (!vmem->ptr) return;
#if PLATFORM_WINDOWS
	VirtualFree(vmem->ptr, 0, MEM_RELEASE);
#elif PLATFORM_POSIX
	munmap(vmem->ptr, vmem->size);
#endif
	vmem->allocated = 0;
	vmem->ptr = 0;
	vmem->size = 0;
}

INLINE static void *calloc_string(size_t len)
{
	assert(len > 0);
	allocations_done++;
	return vmem_alloc(&char_arena, len);
}

INLINE static char *str_copy(const char *start, size_t str_len)
{
	char *dst = (char *) calloc_string(str_len + 1);
	memcpy(dst, start, str_len);
	// No need to set the end
	return dst;
}

//////////////////// SCRATCH BUFFER FUNCTIONS ////////////////////

INLINE void scratch_buffer_clear(void)
{
	scratch_buffer.len = 0;
}

INLINE void scratch_buffer_append_len(const char *string, size_t len)
{
	if (len + scratch_buffer.len > MAX_STRING_BUFFER - 1) {
		error_exit("Scratch buffer size (%d chars) exceeded", MAX_STRING_BUFFER - 1);
	}
	memcpy(scratch_buffer.str + scratch_buffer.len, string, len);
	scratch_buffer.len += (uint32_t) len;
}

INLINE void scratch_buffer_append(const char *string)
{
	scratch_buffer_append_len(string, strlen(string));
}

INLINE void scratch_buffer_append_signed_int(int64_t i)
{
	scratch_buffer_printf("%lld", (long long)i);
}

INLINE void scratch_buffer_append_double(double d)
{
	scratch_buffer_printf("%f", d);

	//removing unused zeroes and dot
	while (scratch_buffer.len > 0)
	{
		if (scratch_buffer.str[scratch_buffer.len - 1] != '0' && scratch_buffer.str[scratch_buffer.len - 1] != '.')
		{
			return;
		}
		scratch_buffer.len--;
	}
}

INLINE void scratch_buffer_append_unsigned_int(uint64_t i)
{
	scratch_buffer_printf("%llu", (unsigned long long)i);
}

void scratch_buffer_printf(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	size_t available = MAX_STRING_BUFFER - scratch_buffer.len;
	uint32_t len_needed = (uint32_t)vsnprintf(&scratch_buffer.str[scratch_buffer.len], available, format, args);
	if (len_needed > available - 1)
	{
		error_exit("Scratch buffer size (%d chars) exceeded", MAX_STRING_BUFFER - 1);
	}
	va_end(args);
	scratch_buffer.len += len_needed;
}

void scratch_buffer_append_in_quote(const char *string)
{
	size_t len = strlen(string);
	for (size_t i = 0; i < len; )
	{
		char c = string[i++];
		switch (c)
		{
			case '"':
				scratch_buffer_append("\\\"");
				continue;
			case '\\':
				scratch_buffer_append("\\\\");
				continue;
		}
		scratch_buffer_append_char(c);
	}
}

INLINE void scratch_buffer_append_char(char c)
{
	if (scratch_buffer.len + 1 > MAX_STRING_BUFFER - 1)
	{
		error_exit("Scratch buffer size (%d chars) exceeded", MAX_STRING_BUFFER - 1);
	}

	scratch_buffer.str[scratch_buffer.len++] = c;
}

INLINE void scratch_buffer_append_char_repeat(char c, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		scratch_buffer_append_char(c);
	}
}

INLINE char *scratch_buffer_to_string(void)
{
	scratch_buffer.str[scratch_buffer.len] = '\0';
	return scratch_buffer.str;
}

INLINE char *scratch_buffer_copy(void)
{
	return str_copy(scratch_buffer.str, scratch_buffer.len);
}

NORETURN void error_exit(const char *format, ...)
{
	va_list arglist;
	va_start(arglist, format);
	vfprintf(stderr, format, arglist);
	fprintf(stderr, "\n");
	va_end(arglist);
	exit(EXIT_FAILURE);
}

#endif // SCRATCH_BUFFER_IMPLEMENTATION
#endif // SCRATCH_BUFFER_H
