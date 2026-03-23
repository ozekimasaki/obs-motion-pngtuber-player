#include "mpt-obs-util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <malloc.h>
#include <process.h>

struct os_process_args {
	char **args;
	size_t count;
	size_t capacity;
};

struct os_process_pipe {
	HANDLE process;
	HANDLE thread;
	HANDLE stdout_read;
	HANDLE stderr_read;
};

struct mpt_thread_state {
	void *(*start_routine)(void *);
	void *arg;
	void *result;
};

struct mpt_thread_handle {
	HANDLE handle;
	struct mpt_thread_state *state;
};

static bool dstr_reserve(struct dstr *dst, size_t needed)
{
	if (needed <= dst->capacity)
		return true;

	size_t new_capacity = dst->capacity ? dst->capacity : 64;
	while (new_capacity < needed)
		new_capacity *= 2;

	char *new_array = (char *)brealloc(dst->array, new_capacity);
	if (!new_array)
		return false;

	dst->array = new_array;
	dst->capacity = new_capacity;
	return true;
}

void dstr_copy(struct dstr *dst, const char *src)
{
	dst->len = 0;
	if (dst->array)
		dst->array[0] = '\0';

	if (!src)
		return;

	size_t src_len = strlen(src);
	if (!dstr_reserve(dst, src_len + 1))
		return;

	memcpy(dst->array, src, src_len + 1);
	dst->len = src_len;
}

void dstr_cat(struct dstr *dst, const char *src)
{
	if (!src)
		return;

	size_t src_len = strlen(src);
	if (!dstr_reserve(dst, dst->len + src_len + 1))
		return;

	memcpy(dst->array + dst->len, src, src_len + 1);
	dst->len += src_len;
}

void dstr_cat_ch(struct dstr *dst, char ch)
{
	if (!dstr_reserve(dst, dst->len + 2))
		return;

	dst->array[dst->len++] = ch;
	dst->array[dst->len] = '\0';
}

void dstr_cat_dstr(struct dstr *dst, const struct dstr *src)
{
	if (!src || !src->array)
		return;
	dstr_cat(dst, src->array);
}

void dstr_catf(struct dstr *dst, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	va_list args_copy;
	va_copy(args_copy, args);
	int required = _vscprintf(format, args_copy);
	va_end(args_copy);
	if (required < 0) {
		va_end(args);
		return;
	}

	if (!dstr_reserve(dst, dst->len + (size_t)required + 1)) {
		va_end(args);
		return;
	}

	vsnprintf(dst->array + dst->len, dst->capacity - dst->len, format, args);
	dst->len += (size_t)required;
	va_end(args);
}

void dstr_free(struct dstr *dst)
{
	if (dst->array)
		bfree(dst->array);
	dst->array = NULL;
	dst->len = 0;
	dst->capacity = 0;
}

void *bzalloc(size_t size)
{
	void *memory = bmalloc(size);
	if (memory)
		memset(memory, 0, size);
	return memory;
}

char *bstrdup(const char *text)
{
	if (!text)
		return NULL;
	size_t length = strlen(text) + 1;
	char *copy = (char *)bmalloc(length);
	if (!copy)
		return NULL;
	memcpy(copy, text, length);
	return copy;
}

static unsigned __stdcall mpt_thread_start(void *param)
{
	struct mpt_thread_state *state = (struct mpt_thread_state *)param;
	state->result = state->start_routine(state->arg);
	return 0;
}

int pthread_create(pthread_t *thread, const void *attr, void *(*start_routine)(void *), void *arg)
{
	UNUSED_PARAMETER(attr);

	if (!thread || !start_routine)
		return EINVAL;

	struct mpt_thread_handle *handle = (struct mpt_thread_handle *)calloc(1, sizeof(*handle));
	struct mpt_thread_state *state = (struct mpt_thread_state *)calloc(1, sizeof(*state));
	if (!handle || !state) {
		free(handle);
		free(state);
		return ENOMEM;
	}

	state->start_routine = start_routine;
	state->arg = arg;
	uintptr_t thread_handle = _beginthreadex(NULL, 0, mpt_thread_start, state, 0, NULL);
	if (!thread_handle) {
		free(handle);
		free(state);
		return errno ? errno : EINVAL;
	}

	handle->handle = (HANDLE)thread_handle;
	handle->state = state;
	*thread = handle;
	return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
	if (!thread)
		return EINVAL;

	DWORD wait_result = WaitForSingleObject(thread->handle, INFINITE);
	if (wait_result != WAIT_OBJECT_0)
		return EINVAL;

	if (retval)
		*retval = thread->state ? thread->state->result : NULL;

	CloseHandle(thread->handle);
	free(thread->state);
	free(thread);
	return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr)
{
	UNUSED_PARAMETER(attr);
	InitializeCriticalSection(mutex);
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	DeleteCriticalSection(mutex);
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	EnterCriticalSection(mutex);
	return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	LeaveCriticalSection(mutex);
	return 0;
}

int os_event_init(os_event_t **event, int type)
{
	if (!event)
		return EINVAL;

	os_event_t *created = (os_event_t *)calloc(1, sizeof(*created));
	if (!created)
		return ENOMEM;

	created->handle = CreateEventW(NULL, type == OS_EVENT_TYPE_MANUAL, FALSE, NULL);
	if (!created->handle) {
		free(created);
		return EINVAL;
	}

	*event = created;
	return 0;
}

void os_event_destroy(os_event_t *event)
{
	if (!event)
		return;

	if (event->handle)
		CloseHandle(event->handle);
	free(event);
}

void os_event_signal(os_event_t *event)
{
	if (event && event->handle)
		SetEvent(event->handle);
}

int os_event_try(os_event_t *event)
{
	if (!event || !event->handle)
		return EINVAL;

	DWORD wait_result = WaitForSingleObject(event->handle, 0);
	return wait_result == WAIT_OBJECT_0 ? 0 : EAGAIN;
}

void os_utf8_to_wcs_ptr(const char *src, size_t src_len, wchar_t **dst)
{
	if (dst)
		*dst = NULL;
	if (!src || !dst)
		return;

	int input_len = src_len ? (int)src_len : -1;
	int wide_len = MultiByteToWideChar(CP_UTF8, 0, src, input_len, NULL, 0);
	if (wide_len <= 0)
		return;

	wchar_t *buffer = (wchar_t *)bmalloc(((size_t)wide_len + 1) * sizeof(wchar_t));
	if (!buffer)
		return;

	if (MultiByteToWideChar(CP_UTF8, 0, src, input_len, buffer, wide_len) <= 0) {
		bfree(buffer);
		return;
	}

	*dst = buffer;
}

static FILE *wfopen_utf8(const char *path, const char *mode)
{
	wchar_t *path_w = NULL;
	wchar_t *mode_w = NULL;
	os_utf8_to_wcs_ptr(path, 0, &path_w);
	os_utf8_to_wcs_ptr(mode, 0, &mode_w);
	if (!path_w || !mode_w) {
		bfree(path_w);
		bfree(mode_w);
		return NULL;
	}

	FILE *file = _wfopen(path_w, mode_w);
	bfree(path_w);
	bfree(mode_w);
	return file;
}

bool os_file_exists(const char *path)
{
	wchar_t *path_w = NULL;
	os_utf8_to_wcs_ptr(path, 0, &path_w);
	if (!path_w)
		return false;

	DWORD attrs = GetFileAttributesW(path_w);
	bfree(path_w);
	return attrs != INVALID_FILE_ATTRIBUTES;
}

FILE *os_fopen(const char *path, const char *mode)
{
	FILE *file = wfopen_utf8(path, mode);
	if (!file)
		file = fopen(path, mode);
	return file;
}

static int create_directory_utf8(const char *path)
{
	wchar_t *path_w = NULL;
	os_utf8_to_wcs_ptr(path, 0, &path_w);
	if (!path_w)
		return -1;

	BOOL ok = CreateDirectoryW(path_w, NULL);
	DWORD error = ok ? ERROR_SUCCESS : GetLastError();
	bfree(path_w);

	if (ok)
		return MKDIR_SUCCESS;
	if (error == ERROR_ALREADY_EXISTS)
		return MKDIR_EXISTS;
	return -1;
}

int os_mkdirs(const char *path)
{
	if (!path || !*path)
		return -1;

	char *scratch = bstrdup(path);
	if (!scratch)
		return -1;

	for (char *cursor = scratch; *cursor; ++cursor) {
		if (*cursor != '\\' && *cursor != '/')
			continue;
		if (cursor == scratch || cursor[-1] == ':')
			continue;

		char original = *cursor;
		*cursor = '\0';
		if (*scratch) {
			int result = create_directory_utf8(scratch);
			if (result != MKDIR_SUCCESS && result != MKDIR_EXISTS) {
				bfree(scratch);
				return result;
			}
		}
		*cursor = original;
	}

	int final_result = create_directory_utf8(scratch);
	bfree(scratch);
	return final_result;
}

uint64_t os_gettime_ns(void)
{
	static LARGE_INTEGER frequency = {0};
	if (frequency.QuadPart == 0)
		QueryPerformanceFrequency(&frequency);

	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (uint64_t)((counter.QuadPart * 1000000000ULL) / frequency.QuadPart);
}

void os_sleepto_ns(uint64_t target_time_ns)
{
	for (;;) {
		uint64_t now = os_gettime_ns();
		if (now >= target_time_ns)
			return;

		uint64_t remaining_ns = target_time_ns - now;
		if (remaining_ns > 2000000ULL) {
			DWORD sleep_ms = (DWORD)(remaining_ns / 1000000ULL);
			if (sleep_ms > 1)
				sleep_ms -= 1;
			Sleep(sleep_ms);
		} else {
			Sleep(0);
		}
	}
}

os_process_args_t *os_process_args_create(const char *program)
{
	os_process_args_t *args = (os_process_args_t *)calloc(1, sizeof(*args));
	if (!args)
		return NULL;

	os_process_args_add_arg(args, program);
	return args;
}

void os_process_args_destroy(os_process_args_t *args)
{
	if (!args)
		return;

	for (size_t idx = 0; idx < args->count; ++idx)
		bfree(args->args[idx]);
	free(args->args);
	free(args);
}

void os_process_args_add_arg(os_process_args_t *args, const char *arg)
{
	if (!args)
		return;

	if (args->count == args->capacity) {
		size_t new_capacity = args->capacity ? args->capacity * 2 : 8;
		char **new_args = (char **)realloc(args->args, new_capacity * sizeof(*new_args));
		if (!new_args)
			return;
		args->args = new_args;
		args->capacity = new_capacity;
	}

	args->args[args->count++] = bstrdup(arg ? arg : "");
}

static void append_quoted_arg(struct dstr *cmdline, const char *arg)
{
	if (cmdline->len)
		dstr_cat(cmdline, " ");

	dstr_cat(cmdline, "\"");
	for (const char *ptr = arg; ptr && *ptr; ++ptr) {
		if (*ptr == '"')
			dstr_cat(cmdline, "\\\"");
		else
			dstr_cat_ch(cmdline, *ptr);
	}
	dstr_cat(cmdline, "\"");
}

os_process_pipe_t *os_process_pipe_create2(os_process_args_t *args, const char *mode)
{
	UNUSED_PARAMETER(mode);

	if (!args || args->count == 0)
		return NULL;

	SECURITY_ATTRIBUTES sa = {0};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE stdout_read = NULL;
	HANDLE stdout_write = NULL;
	HANDLE stderr_read = NULL;
	HANDLE stderr_write = NULL;

	if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0))
		return NULL;
	if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
		CloseHandle(stdout_read);
		CloseHandle(stdout_write);
		return NULL;
	}

	if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
		CloseHandle(stdout_read);
		CloseHandle(stdout_write);
		return NULL;
	}
	if (!SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
		CloseHandle(stdout_read);
		CloseHandle(stdout_write);
		CloseHandle(stderr_read);
		CloseHandle(stderr_write);
		return NULL;
	}

	struct dstr cmdline = {0};
	for (size_t idx = 0; idx < args->count; ++idx)
		append_quoted_arg(&cmdline, args->args[idx]);

	wchar_t *cmdline_w = NULL;
	os_utf8_to_wcs_ptr(cmdline.array, 0, &cmdline_w);
	dstr_free(&cmdline);
	if (!cmdline_w) {
		CloseHandle(stdout_read);
		CloseHandle(stdout_write);
		CloseHandle(stderr_read);
		CloseHandle(stderr_write);
		return NULL;
	}

	STARTUPINFOW si = {0};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = stdout_write;
	si.hStdError = stderr_write;
	si.wShowWindow = SW_HIDE;

	PROCESS_INFORMATION pi = {0};
	BOOL ok = CreateProcessW(NULL, cmdline_w, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	bfree(cmdline_w);
	CloseHandle(stdout_write);
	CloseHandle(stderr_write);

	if (!ok) {
		CloseHandle(stdout_read);
		CloseHandle(stderr_read);
		return NULL;
	}

	os_process_pipe_t *pipe = (os_process_pipe_t *)calloc(1, sizeof(*pipe));
	if (!pipe) {
		CloseHandle(stdout_read);
		CloseHandle(stderr_read);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return NULL;
	}

	pipe->process = pi.hProcess;
	pipe->thread = pi.hThread;
	pipe->stdout_read = stdout_read;
	pipe->stderr_read = stderr_read;
	return pipe;
}

static size_t read_pipe(HANDLE handle, uint8_t *buffer, size_t size)
{
	if (!handle || !buffer || size == 0)
		return 0;

	DWORD bytes_read = 0;
	if (!ReadFile(handle, buffer, (DWORD)size, &bytes_read, NULL) || bytes_read == 0)
		return 0;

	return (size_t)bytes_read;
}

size_t os_process_pipe_read(os_process_pipe_t *pipe, uint8_t *buffer, size_t size)
{
	return pipe ? read_pipe(pipe->stdout_read, buffer, size) : 0;
}

size_t os_process_pipe_read_err(os_process_pipe_t *pipe, uint8_t *buffer, size_t size)
{
	return pipe ? read_pipe(pipe->stderr_read, buffer, size) : 0;
}

int os_process_pipe_destroy(os_process_pipe_t *pipe)
{
	if (!pipe)
		return -1;

	WaitForSingleObject(pipe->process, INFINITE);
	DWORD exit_code = 0;
	GetExitCodeProcess(pipe->process, &exit_code);

	if (pipe->stdout_read)
		CloseHandle(pipe->stdout_read);
	if (pipe->stderr_read)
		CloseHandle(pipe->stderr_read);
	if (pipe->thread)
		CloseHandle(pipe->thread);
	if (pipe->process)
		CloseHandle(pipe->process);

	free(pipe);
	return (int)exit_code;
}
