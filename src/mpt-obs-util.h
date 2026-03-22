#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#define UNUSED_PARAMETER(param) (void)(param)

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifndef EAGAIN
#define EAGAIN 11
#endif

#define MKDIR_SUCCESS 0
#define MKDIR_EXISTS 1
#define OS_EVENT_TYPE_MANUAL 0

struct dstr {
	char *array;
	size_t len;
	size_t capacity;
};

void dstr_copy(struct dstr *dst, const char *src);
void dstr_cat(struct dstr *dst, const char *src);
void dstr_cat_ch(struct dstr *dst, char ch);
void dstr_cat_dstr(struct dstr *dst, const struct dstr *src);
void dstr_catf(struct dstr *dst, const char *format, ...);
void dstr_free(struct dstr *dst);

void *bmalloc(size_t size);
void *bzalloc(size_t size);
void *brealloc(void *ptr, size_t size);
void bfree(void *ptr);
char *bstrdup(const char *text);

#ifdef _WIN32
typedef struct mpt_thread_handle *pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;

int pthread_create(pthread_t *thread, const void *attr, void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

typedef struct os_event {
	HANDLE handle;
} os_event_t;
#else
typedef struct os_event {
	pthread_mutex_t mutex;
	bool signaled;
	bool manual;
} os_event_t;
#endif

int os_event_init(os_event_t **event, int type);
void os_event_destroy(os_event_t *event);
void os_event_signal(os_event_t *event);
int os_event_try(os_event_t *event);

bool os_file_exists(const char *path);
int os_mkdirs(const char *path);
FILE *os_fopen(const char *path, const char *mode);
void os_utf8_to_wcs_ptr(const char *src, size_t src_len, wchar_t **dst);
uint64_t os_gettime_ns(void);
void os_sleepto_ns(uint64_t target_time_ns);

typedef struct os_process_args os_process_args_t;
typedef struct os_process_pipe os_process_pipe_t;

os_process_args_t *os_process_args_create(const char *program);
void os_process_args_destroy(os_process_args_t *args);
void os_process_args_add_arg(os_process_args_t *args, const char *arg);

os_process_pipe_t *os_process_pipe_create2(os_process_args_t *args, const char *mode);
size_t os_process_pipe_read(os_process_pipe_t *pipe, uint8_t *buffer, size_t size);
size_t os_process_pipe_read_err(os_process_pipe_t *pipe, uint8_t *buffer, size_t size);
int os_process_pipe_destroy(os_process_pipe_t *pipe);

#ifdef _WIN32
#define os_fseeki64 _fseeki64
#else
#define os_fseeki64 fseeko
#endif

#ifdef __cplusplus
}
#endif
