/* main.c -- Real-Time Racing Manager .so loader
 *
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2023 Rinnegatamante
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitashark.h>
#include <vitaGL.h>
#include <zlib.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_net.h>
#include <SLES/OpenSLES.h>

#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <mpg123.h>

#include <math.h>
#include <math_neon.h>

#include <errno.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "main.h"
#include "config.h"
#include "dialog.h"
#include "so_util.h"
#include "sha1.h"

#define ENABLE_DEBUG

#ifdef ENABLE_DEBUG
#define dlog printf
#else
#define dlog
#endif

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

static char data_path[256];

static char fake_vm[0x1000];
static char fake_env[0x1000];

int framecap = 0;

void *__wrap_calloc(uint32_t nmember, uint32_t size) { return vglCalloc(nmember, size); }
void __wrap_free(void *addr) { vglFree(addr); };
void *__wrap_malloc(uint32_t size) { return vglMalloc(size); };
void *__wrap_memalign(uint32_t alignment, uint32_t size) { return vglMemalign(alignment, size); };
void *__wrap_realloc(void *ptr, uint32_t size) { return vglRealloc(ptr, size); };

int file_exists(const char *path) {
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;

unsigned int _pthread_stack_default_user = 1 * 1024 * 1024;

so_module cpp_mod, rrm_mod;

void *__wrap_memcpy(void *dest, const void *src, size_t n) {
	return sceClibMemcpy(dest, src, n);
}

void *__wrap_memmove(void *dest, const void *src, size_t n) {
	return sceClibMemmove(dest, src, n);
}

int ret4() { return 4; }

void *__wrap_memset(void *s, int c, size_t n) {
	return sceClibMemset(s, c, n);
}

char *getcwd_hook(char *buf, size_t size) {
	strcpy(buf, data_path);
	return buf;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
	*memptr = memalign(alignment, size);
	return 0;
}

int debugPrintf(char *text, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, text);
	vsprintf(string, text, list);
	va_end(list);

	SceUID fd = sceIoOpen("ux0:data/wb_log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, string, strlen(string));
		sceIoClose(fd);
	}
#endif
	return 0;
}

int snprintf_hook(char *s, size_t maxlen, const char *text, ...) {
	va_list list;
	static char string[0x8000];

	va_start(list, text);
	vsnprintf(string, maxlen, text, list);
	va_end(list);

	strcpy(s, string);
	printf("snprintf: %s\n", string);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	printf("[LOG] %s: %s\n", tag, string);
#endif
	return 0;
}

int vfprintf_hook(FILE *stream, const char *fmt, va_list list) {
	static char string[0x8000];

	vsprintf(string, fmt, list);
	va_end(list);

	printf("[VFPRINTF] %s: %s\n", string);
	
	return 0;
}

int __android_log_write(int prio, const char *tag, const char *fmt, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	printf("[LOGW] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list list) {
#ifdef ENABLE_DEBUG
	static char string[0x8000];

	vsprintf(string, fmt, list);
	va_end(list);

	printf("[LOGV] %s: %s\n", tag, string);
#endif
	return 0;
}

int ret0(void) {
	return 0;
}

int ret1(void) {
	return 1;
}

#define  MUTEX_TYPE_NORMAL     0x0000
#define  MUTEX_TYPE_RECURSIVE  0x4000
#define  MUTEX_TYPE_ERRORCHECK 0x8000

static pthread_t s_pthreadSelfRet;

int pthread_attr_destroy_soloader(pthread_attr_t **attr)
{
    int ret = pthread_attr_destroy(*attr);
    free(*attr);
    return ret;
}

int pthread_attr_getstack_soloader(const pthread_attr_t **attr,
				   void **stackaddr,
				   size_t *stacksize)
{
    return pthread_attr_getstack(*attr, stackaddr, stacksize);
}

__attribute__((unused)) int pthread_condattr_init_soloader(pthread_condattr_t **attr)
{
    *attr = calloc(1, sizeof(pthread_condattr_t));

    return pthread_condattr_init(*attr);
}

__attribute__((unused)) int pthread_condattr_destroy_soloader(pthread_condattr_t **attr)
{
    int ret = pthread_condattr_destroy(*attr);
    free(*attr);
    return ret;
}

int pthread_cond_init_soloader(pthread_cond_t **cond,
			       const pthread_condattr_t **attr)
{
    *cond = calloc(1, sizeof(pthread_cond_t));

	int ret;
    if (attr != NULL)
		ret = pthread_cond_init(*cond, *attr);
    else
		ret = pthread_cond_init(*cond, NULL);
	//printf("pthread_cond_init = %x\n", *cond);
	return ret;
}

int pthread_mutex_init_soloader(pthread_mutex_t **mutex,
				const pthread_mutexattr_t **attr)
{
    *mutex = calloc(1, sizeof(pthread_mutex_t));

	int ret;
    if (attr != NULL)
		ret = pthread_mutex_init(*mutex, *attr);
    else
		ret = pthread_mutex_init(*mutex, NULL);
	//printf("pthread_mutex_init = %x\n", *mutex);
	return ret;
}

static void init_static_mutex(pthread_mutex_t **mutex)
{
	if (*mutex == NULL) {
		//printf("init_static_mutex\n");
		pthread_mutex_init_soloader(mutex, NULL);
	}
}

static void init_static_cond(pthread_cond_t **cond)
{
    if (*cond == NULL) {
		//printf("init_static_cond\n");
		pthread_cond_init_soloader(cond, NULL);
    }
}

int pthread_cond_destroy_soloader(pthread_cond_t **cond)
{
	//printf("pthread_cond_destroy(%x)\n", *cond);
    int ret = pthread_cond_destroy(*cond);
    free(*cond);
	*cond = NULL;
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t **cond)
{
	//printf("pthread_cond_signal(%x)\n", *cond);
    init_static_cond(cond);
    return pthread_cond_signal(*cond);
}

int pthread_cond_timedwait_soloader(pthread_cond_t **cond,
				    pthread_mutex_t **mutex,
				    struct timespec *abstime)
{
	//printf("pthread_cond_timedwait(%x, %x)\n", *cond, *mutex);
    init_static_cond(cond);
    init_static_mutex(mutex);
    return pthread_cond_timedwait(*cond, *mutex, abstime);
}

int pthread_cond_wait_soloader(pthread_cond_t **cond, pthread_mutex_t **mutex)
{
	//printf("pthread_cond_wait(%x, %x)\n", *cond, *mutex);
	init_static_cond(cond);
	init_static_mutex(mutex);
    return pthread_cond_wait(*cond, *mutex);
}

int pthread_cond_broadcast_soloader(pthread_cond_t **cond)
{
	//printf("pthread_cond_broadcast(%x)\n", *cond);
	init_static_cond(cond);
    return pthread_cond_broadcast(*cond);
}

int pthread_create_soloader(pthread_t **thread,
			    const pthread_attr_t **attr,
			    void *(*start)(void *),
			    void *param)
{
    *thread = calloc(1, sizeof(pthread_t));

    if (attr != NULL) {
		pthread_attr_setstacksize(*attr, 512 * 1024);
		return pthread_create(*thread, *attr, start, param);
    } else {
		pthread_attr_t attrr;
		pthread_attr_init(&attrr);
		pthread_attr_setstacksize(&attrr, 512 * 1024);
		return pthread_create(*thread, &attrr, start, param);
    }

}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t **attr)
{
    *attr = calloc(1, sizeof(pthread_mutexattr_t));

    return pthread_mutexattr_init(*attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t **attr, int type)
{
    return pthread_mutexattr_settype(*attr, type);
}

int pthread_mutexattr_setpshared_soloader(pthread_mutexattr_t **attr, int pshared)
{
    return pthread_mutexattr_setpshared(*attr, pshared);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t **attr)
{
    int ret = pthread_mutexattr_destroy(*attr);
    free(*attr);
	*attr = NULL;
    return ret;
}

int pthread_mutex_destroy_soloader(pthread_mutex_t **mutex)
{
	//printf("pthread_mutex_destroy(%x)\n", *mutex);
    int ret = pthread_mutex_destroy(*mutex);
    free(*mutex);
	*mutex = NULL;
    return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t **mutex)
{
	//printf("pthread_mutex_lock(%x)\n", *mutex);
    init_static_mutex(mutex);
    return pthread_mutex_lock(*mutex);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t **mutex)
{
	//printf("pthread_mutex_trylock(%x)\n", *mutex);
    init_static_mutex(mutex);
    return pthread_mutex_trylock(*mutex);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t **mutex)
{
	//printf("pthread_mutex_unlock(%x)\n", *mutex);
	init_static_mutex(mutex);
    return pthread_mutex_unlock(*mutex);
}

int pthread_join_soloader(const pthread_t *thread, void **value_ptr)
{
    return pthread_join(*thread, value_ptr);
}

int pthread_attr_init_soloader(pthread_attr_t **attr)
{
    *attr = calloc(1, sizeof(pthread_attr_t));

    return pthread_attr_init(*attr);
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t **attr, int state)
{
    return pthread_attr_setdetachstate(*attr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t **attr, size_t stacksize)
{
    return pthread_attr_setstacksize(*attr, stacksize);
}

int pthread_attr_setschedparam_soloader(pthread_attr_t **attr,
					const struct sched_param *param)
{
    return pthread_attr_setschedparam(*attr, param);
}

int pthread_attr_setstack_soloader(pthread_attr_t **attr,
				   void *stackaddr,
				   size_t stacksize)
{
    return pthread_attr_setstack(*attr, stackaddr, stacksize);
}

int pthread_setschedparam_soloader(const pthread_t *thread, int policy,
				   const struct sched_param *param)
{
    return pthread_setschedparam(*thread, policy, param);
}

int pthread_getschedparam_soloader(const pthread_t *thread, int *policy,
				   struct sched_param *param)
{
    return pthread_getschedparam(*thread, policy, param);
}

int pthread_detach_soloader(const pthread_t *thread)
{
    return pthread_detach(*thread);
}

int pthread_getattr_np_soloader(pthread_t* thread, pthread_attr_t *attr) {
    fprintf(stderr, "[WARNING!] Not implemented: pthread_getattr_np\n");
    return 0;
}

int pthread_equal_soloader(const pthread_t *t1, const pthread_t *t2)
{
	if (t1 == t2)
		return 1;
	if (!t1 || !t2)
		return 0;
    return pthread_equal(*t1, *t2);
}

pthread_t *pthread_self_soloader()
{
    s_pthreadSelfRet = pthread_self();
    return &s_pthreadSelfRet;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(const pthread_t *thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
	return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
	return ERANGE;
    }

    // TODO: Implement the actual name setting if possible
    fprintf(stderr, "PTHR: pthread_setname_np with name %s\n", thread_name);

    return 0;
}

int clock_gettime_hook(int clk_id, struct timespec *t) {
	struct timeval now;
	int rv = gettimeofday(&now, NULL);
	if (rv)
		return rv;
	t->tv_sec = now.tv_sec;
	t->tv_nsec = now.tv_usec * 1000;

	return 0;
}


int GetCurrentThreadId(void) {
	return sceKernelGetThreadId();
}

extern void *__aeabi_ldiv0;

int GetEnv(void *vm, void **env, int r2) {
	*env = fake_env;
	return 0;
}

void throw_exc(char **str, void *a, int b) {
	printf("throwing %s\n", *str);
}

FILE *fopen_hook(char *fname, char *mode) {
	FILE *f;
	char real_fname[256];
	printf("fopen(%s,%s)\n", fname, mode);
	if (strncmp(fname, "ux0:", 4)) {
		if (!strncmp(fname, "bin_mobile", 10)) {
			sprintf(real_fname, "%s/%s", data_path, fname + 11);
			printf("patched fopen %s\n", real_fname);
			f = fopen(real_fname, mode);
		} else {
			sprintf(real_fname, "%s/%s", data_path, fname + 2);
			f = fopen(real_fname, mode);
		}
	} else {
		f = fopen(fname, mode);
	}
	return f;
}

int open_hook(const char *fname, int flags, mode_t mode) {
	int f;
	char real_fname[256];
	printf("open(%s)\n", fname);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/%s", data_path, fname);
		f = open(real_fname, flags, mode);
	} else {
		f = open(fname, flags, mode);
	}
	return f;
}

extern void *__aeabi_atexit;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_dadd;
extern void *__aeabi_i2d;
extern void *__aeabi_f2d;
extern void *__aeabi_ui2d;
extern void *__aeabi_dsub;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_ldivmod;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_uldivmod;
extern void *__cxa_atexit;
extern void *__cxa_finalize;
extern void *__cxa_call_unexpected;
extern void *__gnu_unwind_frame;
extern void *__stack_chk_fail;
int open(const char *pathname, int flags);

static int __stack_chk_guard_fake = 0x42424242;

static FILE __sF_fake[0x1000][3];

int stat_hook(const char *fname, void *statbuf) {
	char real_fname[256];
	printf("stat(%s)\n", fname);
	struct stat st;
	int res;
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/%s", data_path, fname + 2);
		res = stat(real_fname, &st);
	} else {
		res = stat(fname, &st);
	}
	
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
	return memalign(length, 0x1000);
}

int munmap(void *addr, size_t length) {
	free(addr);
	return 0;
}

int fstat_hook(int fd, void *statbuf) {
	struct stat st;
	int res = fstat(fd, &st);
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;
extern void *__cxa_guard_abort;

void *sceClibMemclr(void *dst, SceSize len) {
	if (!dst) {
		printf("memclr on NULL\n");
		return NULL;
	}
	return sceClibMemset(dst, 0, len);
}

void *sceClibMemset2(void *dst, SceSize len, int ch) {
	return sceClibMemset(dst, ch, len);
}

void *Android_JNI_GetEnv() {
	return fake_env;
}

char *SDL_AndroidGetExternalStoragePath() {
	return "ux0:data/rrm";
}

char *SDL_AndroidGetInternalStoragePath() {
	return "ux0:data/rrm";
}

char *SDL_GetPrefPath_hook(const char *org, const char *app) {
	char *r = SDL_GetPrefPath(org, app);
	printf("Pref Path: %s\n", r);
	r[strlen(r) - 1] = 0;
	return r;
}

int g_SDL_BufferGeometry_w;
int g_SDL_BufferGeometry_h;

void abort_hook() {
	printf("ABORT CALLED (RA: %x)!!!\n", __builtin_return_address(0));
	uint8_t *p = NULL;
	p[0] = 1;
}

int ret99() {
	return 99;
}

int chdir_hook(const char *path) {
	return 0;
}

#define GL_ACTIVE_UNIFORM_BLOCKS 0x8A36
void glGetProgramiv_fake(GLuint program, GLenum pname, GLint *params) {
	if (pname == GL_ACTIVE_UNIFORM_BLOCKS)
		*params = 0;
	else
		glGetProgramiv(program, pname, params);
}

void glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
	printf("glFramebufferTextureLayer %x %x %x %d %d\n", target, attachment, texture, level, layer);
}

void glBindImageTexture() {
	printf("glBindImageTexture\n");
}

void glTexStorage2D() {
	printf("glTexStorage2D\n");
}

void glTexImage3D() {
	printf("glTexImage3D\n");
}

void glCompressedTexSubImage2D() {
	printf("glCompressedTexSubImage2D\n");
}

void glGetUniformBlockIndex() {
	printf("glGetUniformBlockIndex\n");
}

void glDrawBuffers() {
	printf("glDrawBuffers\n");
}

static so_default_dynlib gl_hook[] = {
	{"glPixelStorei", (uintptr_t)&ret0},
	{"glGetError", (uintptr_t)&ret0},
	{"glGetProgramiv", (uintptr_t)&glGetProgramiv_fake},
	//{"glFramebufferTextureLayer", (uintptr_t)&glFramebufferTextureLayer},
	{"glDrawBuffers", (uintptr_t)&glDrawBuffers},
	//{"glDeleteSync", (uintptr_t)&glDeleteSync},
	//{"glFenceSync", (uintptr_t)&glFenceSync},
	//{"glWaitSync", (uintptr_t)&glWaitSync},
	//{"glVertexAttribIPointer", (uintptr_t)&glVertexAttribIPointer},
	//{"glBindBufferRange", (uintptr_t)&glBindBufferRange},
	//{"glGetTexImage", (uintptr_t)&glGetTexImage},
	//{"glGetTextureImage", (uintptr_t)&glGetTextureImage},
	//{"glGetTextureSubImage", (uintptr_t)&glGetTextureSubImage},
	{"glCompressedTexSubImage2D", (uintptr_t)&glCompressedTexSubImage2D},
	//{"glTexStorage2D", (uintptr_t)&glTexStorage2D},
	//{"glBindImageTexture", (uintptr_t)&glBindImageTexture},
	//{"glTexImage3D", (uintptr_t)&glTexImage3D},
	//{"glGetUniformBlockIndex", (uintptr_t)&glGetUniformBlockIndex},
};
static size_t gl_numhook = sizeof(gl_hook) / sizeof(*gl_hook);

void *SDL_GL_GetProcAddress_fake(const char *symbol) {
	dlog("looking for symbol %s\n", symbol);
	for (size_t i = 0; i < gl_numhook; ++i) {
		if (!strcmp(symbol, gl_hook[i].symbol)) {
			return (void *)gl_hook[i].func;
		}
	}
	void *r = vglGetProcAddress(symbol);
	if (!r) {
		dlog("Cannot find symbol %s\n", symbol);
	}
	
	return r;
}

#define SCE_ERRNO_MASK 0xFF

#define DT_DIR 4
#define DT_REG 8

struct android_dirent {
	char pad[18];
	unsigned char d_type;
	char d_name[256];
};

typedef struct {
	SceUID uid;
	struct android_dirent dir;
} android_DIR;

int closedir_fake(android_DIR *dirp) {
	if (!dirp || dirp->uid < 0) {
		errno = EBADF;
		return -1;
	}

	int res = sceIoDclose(dirp->uid);
	dirp->uid = -1;

	free(dirp);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return -1;
	}

	errno = 0;
	return 0;
}

int *__errno_hook(void) {
  int *res = __errno();
  if (*res == ETIMEDOUT)
	  *res = 110;
  // *res = EEXIST;
  //printf("errno: %d\n", *res);
  return res;
}

android_DIR *opendir_fake(const char *fname) {
	printf("opendir(%s)\n", fname);
	SceUID uid;
	char real_fname[256];
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/%s", data_path, fname + 2);
		uid = sceIoDopen(real_fname);
	} else {
		uid = sceIoDopen(fname);
	}

	if (uid < 0) {
		errno = uid & SCE_ERRNO_MASK;
		return NULL;
	}

	android_DIR *dirp = calloc(1, sizeof(android_DIR));

	if (!dirp) {
		sceIoDclose(uid);
		errno = ENOMEM;
		return NULL;
	}

	dirp->uid = uid;

	errno = 0;
	return dirp;
}

struct android_dirent *readdir_fake(android_DIR *dirp) {
	if (!dirp) {
		errno = EBADF;
		return NULL;
	}

	SceIoDirent sce_dir;
	int res = sceIoDread(dirp->uid, &sce_dir);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return NULL;
	}

	if (res == 0) {
		errno = 0;
		return NULL;
	}

	dirp->dir.d_type = SCE_S_ISDIR(sce_dir.d_stat.st_mode) ? DT_DIR : DT_REG;
	strcpy(dirp->dir.d_name, sce_dir.d_name);
	return &dirp->dir;
}

SDL_Surface *IMG_Load_hook(const char *file) {
	char real_fname[256];
	//printf("loading %s\n", file);
	if (strncmp(file, "ux0:", 4)) {
		sprintf(real_fname, "%s/%s", data_path, file);
		return IMG_Load(real_fname);
	}
	return IMG_Load(file);
}

SDL_RWops *SDL_RWFromFile_hook(const char *fname, const char *mode) {
	SDL_RWops *f;
	char real_fname[256];
	//printf("SDL_RWFromFile(%s,%s)\n", fname, mode);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/%s", data_path, fname);
		//printf("SDL_RWFromFile patched to %s\n", real_fname);
		f = SDL_RWFromFile(real_fname, mode);
	} else {
		f = SDL_RWFromFile(fname, mode);
	}
	return f;
}

Mix_Music *Mix_LoadMUS_hook(const char *fname) {
	Mix_Music *f;
	char real_fname[256];
	printf("Mix_LoadMUS(%s)\n", fname);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/assets/%s", data_path, fname);
		f = Mix_LoadMUS(real_fname);
	} else {
		f = Mix_LoadMUS(fname);
	}
	return f;
}

int Mix_OpenAudio_hook(int frequency, Uint16 format, int channels, int chunksize) {
	return Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024);
}

extern void SDL_ResetKeyboard(void);

size_t __strlen_chk(const char *s, size_t s_len) {
	return strlen(s);
}

int SDL_PollEvent_hook(SDL_Event * event) {
	int r = SDL_PollEvent(event);
	if (event->type == SDL_MOUSEMOTION) {
		//printf("pos %d %d\n", event->motion.x, event->motion.y);
		event->motion.x = (((float)event->motion.x / 960.0f) * 1600.0f);
		event->motion.y = (((float)event->motion.y / 544.0f) * 900.0f);
	} else if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP) {
		//printf("CLICK pos %d %d\n", event->button.x, event->button.y);
		event->button.x = (((float)event->button.x / 960.0f) * 1600.0f);
		event->button.y = (((float)event->button.y / 544.0f) * 900.0f);
	}
}

SDL_Window *SDL_CreateWindow_hook(const char *title, int x, int y, int w, int h, Uint32 flags) {
	return SDL_CreateWindow("rrm", x, y, w, h, flags | SDL_WINDOW_FULLSCREEN);
}

uint64_t lseek64(int fd, uint64_t offset, int whence) {
	return lseek(fd, offset, whence);
}

char *SDL_GetBasePath_hook() {
	void *ret = malloc(256);
	sprintf(ret, "%s/assets/", data_path);
	printf("SDL_GetBasePath\n");
	return ret;
}

void SDL_GetVersion_fake(SDL_version *ver){
	ver->major = 2;
	ver->minor = 0;
	ver->patch = 10;
}

const char *SDL_JoystickName_fake(SDL_Joystick *joystick) {
	return "Totally PS4 Controller ( ͡° ͜ʖ ͡°)";
}

int SDL_OpenAudio_fake(SDL_AudioSpec * desired, SDL_AudioSpec * obtained) {
	printf("Calling SDL_OpenAudio with: %d chn, %d samplerate, %d samples, %d bufsize, %x fmt\n",
		desired->channels, desired->freq, desired->samples, desired->size, desired->format);
	desired->freq = 44100;
	return SDL_OpenAudio(desired, obtained);
}

SDL_AudioDeviceID SDL_OpenAudioDevice_fake(const char *device, int iscapture, SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes) {
	printf("Calling SDL_OpenAudioDevice with: %d chn, %d samplerate, %d samples, %d bufsize, %x fmt\n",
		desired->channels, desired->freq, desired->samples, desired->size, desired->format);
	SDL_AudioDeviceID ret = SDL_OpenAudioDevice(NULL, 0, desired, obtained, SDL_AUDIO_ALLOW_ANY_CHANGE);
	printf("Returned %d\n", ret);
	return ret;
}

char *strcpy_chk(char * dest, const char * src, size_t destlen) {
	return strcpy(dest, src);
}

int vsprintf_chk(char * s, int flag, size_t slen, const char * format, va_list args) {
	return __vsprintf_chk(s, flag, slen, format, args);
}

int vsnprintf_chk(char * s, size_t maxlen, int flag, size_t slen, const char * format, va_list args) {
	return __vsnprintf_chk(s, maxlen, flag, slen, format, args);
}

int nanosleep_hook (const struct timespec *rqtp, struct timespec *rmtp) {
  if (!rqtp) {
    errno = EFAULT;
    return -1;
  }

  if (rqtp->tv_sec < 0 || rqtp->tv_nsec < 0 || rqtp->tv_nsec > 999999999) {
    errno = EINVAL;
    return -1;
  }

  const uint32_t us = rqtp->tv_sec * 1000000 + (rqtp->tv_nsec + 999) / 1000;
  // printf("nanosleep %d\n", us);
  sceKernelDelayThread(us);
  return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_wait_soloader (int * uid) {
    if (sceKernelWaitSema(*uid, 1, NULL) < 0)
        return -1;
    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int retminus1() {
	return 22;
}

void __assert2(int a) {
	printf("assert2 called with %x\n", a);
}

pthread_t *SDL_CreateThread_fake(int (*fn)(void *data), const char *name, void *data) {
	pthread_t thd;
	pthread_create(&thd, NULL, fn, data);
	return thd;
}

void SDL_WaitThread_fake(pthread_t *thread, int *status) {
	pthread_join(*thread, status);
}

void SDL_DetachThread_fake(pthread_t *thread) {
}

void glShaderSource_hook(GLuint shader, GLsizei count, const GLchar **string, const GLint *length) {
	uint32_t sha1[5];
	SHA1_CTX ctx;
	
	sha1_init(&ctx);
	for (int i = 0; i < count; i++) {
		sha1_update(&ctx, string[i], strlen(string[i]));
	}
	sha1_final(&ctx, (uint8_t *)sha1);

	char sha_name[64];
	snprintf(sha_name, sizeof(sha_name), "%08x%08x%08x%08x%08x", sha1[0], sha1[1], sha1[2], sha1[3], sha1[4]);

	char cg_path[128];
	snprintf(cg_path, sizeof(cg_path), "app0:/shaders/%s_glsl.gxp", sha_name);

	FILE *file = fopen(cg_path, "rb");
	
	printf("Shader: %s\n", sha_name);
	if (!file) {
		glShaderSource(shader, count, string, length);
		glCompileShader(shader);
		/*void *bin = vglMalloc(32 * 1024);
        GLsizei len;
		vglGetShaderBinary(shader, 32 * 1024, &len, bin);
        file = fopen(cg_path, "wb");
        fwrite(bin, 1, len, file);
        fclose(file);
        vglFree(bin);*/
	} else {
		fseek(file, 0, SEEK_END);
		uint32_t size = ftell(file);
		fseek(file, 0, SEEK_SET);
		char *buf = (char *)malloc(size + 1);
		fread(buf, 1, size, file);
		fclose(file);
		buf[size] = 0;
		glShaderBinary(1, &shader, 0, buf, size);
		free(buf);
	}
}

void glFramebufferTexture2D_hook(GLenum target, GLenum attachment, GLenum textarget, GLuint tex_id, GLint level) {
    if (attachment == GL_COLOR_ATTACHMENT0) {
		printf("COLOR_ATTACHMENT %x\n", target);
        glFramebufferTexture2D(target, attachment, textarget, tex_id, level);
	} else {
		printf("DEPTH_ATTACHMENT %x\n", target);
		GLuint gDepthMap;
		//glGenTextures(1, &gDepthMap);
		glBindTexture(GL_TEXTURE_2D, tex_id);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 960, 544, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, textarget, tex_id, level);
		/*GLuint depthBuffer;
		glGenRenderbuffers(1, &depthBuffer);
		glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, 960, 544);
		glFramebufferRenderbuffer(target, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthBuffer);
		glBindTexture(GL_TEXTURE_2D, tex_id);
		vglFree(vglGetTexDataPointer(GL_TEXTURE_2D)); // We free then its texture data
		vglTexImageDepthBuffer(GL_TEXTURE_2D); // Last we replace the texture data with a pointer to the depth buffer*/
	}
}

void glBindFramebuffer_hook(GLenum target, GLuint framebuffer) {
	//printf("glBindFramebuffer %x %x\n", target, framebuffer);
	glBindFramebuffer(target, framebuffer);
}

void glViewport_hook(GLint x, GLint y, GLsizei width, GLsizei height) {
	//printf("glViewport %d %d %u %u\n", x, y, width, height);
	glViewport(0, 0, 960, 544);
}

static so_default_dynlib default_dynlib[] = {
	{ "glViewport", (uintptr_t)&glViewport_hook},
	{ "glBindFramebuffer", (uintptr_t)&glBindFramebuffer_hook},
	{ "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D_hook},
	{ "glCompileShader", (uintptr_t)&ret0},
	{ "newlocale", (uintptr_t)&newlocale},
	{ "uselocale", (uintptr_t)&uselocale},
	{ "freelocale", (uintptr_t)&freelocale},
	{ "sem_init", (uintptr_t)&sem_init_soloader},
	{ "sem_init", (uintptr_t)&sem_init_soloader},
	{ "sem_post", (uintptr_t)&sem_post_soloader},
	{ "sem_wait", (uintptr_t)&sem_wait_soloader},
	{ "sem_destroy", (uintptr_t)&sem_destroy_soloader},
	{ "glGetError", (uintptr_t)&ret0},
	{ "glShaderSource", (uintptr_t)&glShaderSource_hook},
	{ "SDL_GetPlatform", (uintptr_t)&SDL_GetPlatform},
	{ "sincosf", (uintptr_t)&sincosf },
	{ "sincos", (uintptr_t)&sincos },
	{ "opendir", (uintptr_t)&opendir_fake },
	{ "readdir", (uintptr_t)&readdir_fake },
	{ "closedir", (uintptr_t)&closedir_fake },
	{ "g_SDL_BufferGeometry_w", (uintptr_t)&g_SDL_BufferGeometry_w },
	{ "g_SDL_BufferGeometry_h", (uintptr_t)&g_SDL_BufferGeometry_h },
	{ "__aeabi_memclr", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr4", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr8", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove4", (uintptr_t)&memmove },
	{ "__aeabi_memmove8", (uintptr_t)&memmove },
	{ "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove", (uintptr_t)&memmove },
	{ "__aeabi_memset", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset4", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset8", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
	{ "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
	{ "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
	{ "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },
	{ "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
	{ "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
	{ "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
	{ "__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv },
	{ "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
	{ "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
	{ "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
	{ "__aeabi_f2d", (uintptr_t)&__aeabi_f2d },
	{ "__aeabi_ui2d", (uintptr_t)&__aeabi_ui2d },
	{ "__aeabi_dsub", (uintptr_t)&__aeabi_dsub },
	{ "__android_log_print", (uintptr_t)&__android_log_print },
	{ "__android_log_vprint", (uintptr_t)&__android_log_vprint },
	{ "__android_log_write", (uintptr_t)&__android_log_write },
	{ "__cxa_atexit", (uintptr_t)&__cxa_atexit },
	{ "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
	{ "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
	{ "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
	{ "__cxa_finalize", (uintptr_t)&__cxa_finalize },
	{ "__errno", (uintptr_t)&__errno_hook },
	{ "__strlen_chk", (uintptr_t)&__strlen_chk },
	{ "strerror_r", (uintptr_t)&ret0 },
	{ "__strcpy_chk", (uintptr_t)&strcpy_chk },
	{ "__vsprintf_chk", (uintptr_t)&vsprintf_chk },
	{ "__vsnprintf_chk", (uintptr_t)&vsnprintf_chk },
	{ "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
	{ "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
	{ "dl_unwind_find_exidx", (uintptr_t)&ret0 },
	{ "__sF", (uintptr_t)&__sF_fake },
	{ "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
	{ "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
	{ "_ctype_", (uintptr_t)&BIONIC_ctype_},
	{ "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_},
	{ "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_},
	{ "abort", (uintptr_t)&abort_hook },
	{ "access", (uintptr_t)&access },
	{ "acos", (uintptr_t)&acos },
	{ "acosh", (uintptr_t)&acosh },
	{ "asctime", (uintptr_t)&asctime },
	{ "acosf", (uintptr_t)&acosf },
	{ "asin", (uintptr_t)&asin },
	{ "asinh", (uintptr_t)&asinh },
	{ "asinf", (uintptr_t)&asinf },
	{ "atan", (uintptr_t)&atan },
	{ "atanh", (uintptr_t)&atanh },
	{ "atan2", (uintptr_t)&atan2 },
	{ "atan2f", (uintptr_t)&atan2f },
	{ "atanf", (uintptr_t)&atanf },
	{ "atoi", (uintptr_t)&atoi },
	{ "atol", (uintptr_t)&atol },
	{ "atoll", (uintptr_t)&atoll },
	{ "basename", (uintptr_t)&basename },
	// { "bind", (uintptr_t)&bind },
	{ "bsd_signal", (uintptr_t)&ret0 },
	{ "bsearch", (uintptr_t)&bsearch },
	{ "btowc", (uintptr_t)&btowc },
	{ "mbtowc", (uintptr_t)&mbtowc },
	{ "mbrlen", (uintptr_t)&mbrlen },
	{ "mbsrtowcs", (uintptr_t)&mbsrtowcs },
	{ "wcsnrtombs", (uintptr_t)&wcsnrtombs },
	{ "mbsnrtowcs", (uintptr_t)&mbsnrtowcs },
	{ "calloc", (uintptr_t)&calloc },
	{ "ceil", (uintptr_t)&ceil },
	{ "ceilf", (uintptr_t)&ceilf },
	{ "chdir", (uintptr_t)&chdir_hook },
	{ "clearerr", (uintptr_t)&clearerr },
	{ "clock", (uintptr_t)&clock },
	{ "clock_gettime", (uintptr_t)&clock_gettime_hook },
	{ "close", (uintptr_t)&close },
	{ "cos", (uintptr_t)&cos },
	{ "cosf", (uintptr_t)&cosf },
	{ "cosh", (uintptr_t)&cosh },
	{ "crc32", (uintptr_t)&crc32 },
	{ "deflate", (uintptr_t)&deflate },
	{ "deflateEnd", (uintptr_t)&deflateEnd },
	{ "deflateInit_", (uintptr_t)&deflateInit_ },
	{ "deflateInit2_", (uintptr_t)&deflateInit2_ },
	{ "deflateReset", (uintptr_t)&deflateReset },
	{ "dlopen", (uintptr_t)&ret0 },
	// { "dlsym", (uintptr_t)&dlsym_hook },
	{ "exit", (uintptr_t)&exit },
	{ "exp", (uintptr_t)&exp },
	{ "exp2", (uintptr_t)&exp2 },
	{ "expf", (uintptr_t)&expf },
	{ "fabsf", (uintptr_t)&fabsf },
	{ "fclose", (uintptr_t)&fclose },
	{ "fcntl", (uintptr_t)&ret0 },
	// { "fdopen", (uintptr_t)&fdopen },
	{ "feof", (uintptr_t)&feof },
	{ "ferror", (uintptr_t)&ferror },
	{ "fflush", (uintptr_t)&fflush },
	{ "fgets", (uintptr_t)&fgets },
	{ "floor", (uintptr_t)&floor },
	{ "fileno", (uintptr_t)&fileno },
	{ "floorf", (uintptr_t)&floorf },
	{ "fmod", (uintptr_t)&fmod },
	{ "fmodf", (uintptr_t)&fmodf },
	{ "fopen", (uintptr_t)&fopen_hook },
	{ "open", (uintptr_t)&open_hook },
	{ "fprintf", (uintptr_t)&fprintf },
	{ "fputc", (uintptr_t)&fputc },
	// { "fputwc", (uintptr_t)&fputwc },
	{ "fputs", (uintptr_t)&fputs },
	{ "fread", (uintptr_t)&fread },
	{ "free", (uintptr_t)&free },
	{ "frexp", (uintptr_t)&frexp },
	{ "frexpf", (uintptr_t)&frexpf },
	{ "fscanf", (uintptr_t)&fscanf },
	{ "fseek", (uintptr_t)&fseek },
	{ "fseeko", (uintptr_t)&fseeko },
	{ "fstat", (uintptr_t)&fstat_hook },
	{ "ftell", (uintptr_t)&ftell },
	{ "ftello", (uintptr_t)&ftello },
	// { "ftruncate", (uintptr_t)&ftruncate },
	{ "fwrite", (uintptr_t)&fwrite },
	{ "getc", (uintptr_t)&getc },
	{ "getpid", (uintptr_t)&ret0 },
	{ "getcwd", (uintptr_t)&getcwd_hook },
	{ "getenv", (uintptr_t)&ret0 },
	{ "getwc", (uintptr_t)&getwc },
	{ "gettimeofday", (uintptr_t)&gettimeofday },
	{ "gzopen", (uintptr_t)&gzopen },
	{ "inflate", (uintptr_t)&inflate },
	{ "inflateEnd", (uintptr_t)&inflateEnd },
	{ "inflateInit_", (uintptr_t)&inflateInit_ },
	{ "inflateInit2_", (uintptr_t)&inflateInit2_ },
	{ "inflateReset", (uintptr_t)&inflateReset },
	{ "inflateReset2", (uintptr_t)&inflateReset2 },
	{ "isascii", (uintptr_t)&isascii },
	{ "isalnum", (uintptr_t)&isalnum },
	{ "isalpha", (uintptr_t)&isalpha },
	{ "isblank", (uintptr_t)&isblank },
	{ "iscntrl", (uintptr_t)&iscntrl },
	{ "isdigit", (uintptr_t)&isdigit },
	{ "islower", (uintptr_t)&islower },
	{ "ispunct", (uintptr_t)&ispunct },
	{ "isprint", (uintptr_t)&isprint },
	{ "isspace", (uintptr_t)&isspace },
	{ "isupper", (uintptr_t)&isupper },
	{ "iswalpha", (uintptr_t)&iswalpha },
	{ "iswcntrl", (uintptr_t)&iswcntrl },
	{ "iswctype", (uintptr_t)&iswctype },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswlower", (uintptr_t)&iswlower },
	{ "iswprint", (uintptr_t)&iswprint },
	{ "iswpunct", (uintptr_t)&iswpunct },
	{ "iswspace", (uintptr_t)&iswspace },
	{ "iswupper", (uintptr_t)&iswupper },
	{ "iswxdigit", (uintptr_t)&iswxdigit },
	{ "isxdigit", (uintptr_t)&isxdigit },
	{ "ldexp", (uintptr_t)&ldexp },
	{ "ldexpf", (uintptr_t)&ldexpf },
	// { "listen", (uintptr_t)&listen },
	{ "localtime", (uintptr_t)&localtime },
	{ "localtime_r", (uintptr_t)&localtime_r },
	{ "log", (uintptr_t)&log },
	{ "logf", (uintptr_t)&logf },
	{ "log10", (uintptr_t)&log10 },
	{ "log10f", (uintptr_t)&log10f },
	{ "longjmp", (uintptr_t)&longjmp },
	{ "lrand48", (uintptr_t)&lrand48 },
	{ "lrint", (uintptr_t)&lrint },
	{ "lrintf", (uintptr_t)&lrintf },
	{ "lseek", (uintptr_t)&lseek },
	{ "lseek64", (uintptr_t)&lseek64 },
	{ "malloc", (uintptr_t)&malloc },
	{ "mbrtowc", (uintptr_t)&mbrtowc },
	{ "memalign", (uintptr_t)&memalign },
	{ "memchr", (uintptr_t)&sceClibMemchr },
	{ "memcmp", (uintptr_t)&memcmp },
	{ "memcpy", (uintptr_t)&sceClibMemcpy },
	{ "memmove", (uintptr_t)&memmove },
	{ "memmem", (uintptr_t)&memmem },
	{ "memset", (uintptr_t)&sceClibMemset },
	{ "mkdir", (uintptr_t)&mkdir },
	// { "mmap", (uintptr_t)&mmap},
	// { "munmap", (uintptr_t)&munmap},
	{ "modf", (uintptr_t)&modf },
	{ "modff", (uintptr_t)&modff },
	{ "nanosleep", (uintptr_t)&nanosleep_hook },
	// { "poll", (uintptr_t)&poll },
	{ "pow", (uintptr_t)&pow },
	{ "powf", (uintptr_t)&powf },
	{ "printf", (uintptr_t)&printf },
	{ "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
	{ "pthread_attr_getstack", (uintptr_t)&pthread_attr_getstack_soloader },
	{ "pthread_attr_init", (uintptr_t) &pthread_attr_init_soloader },
	{ "pthread_attr_setdetachstate", (uintptr_t) &pthread_attr_setdetachstate_soloader },
	{ "pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam_soloader },
	{ "pthread_attr_setstack", (uintptr_t)&pthread_attr_setstack_soloader },
	{ "pthread_attr_setstacksize", (uintptr_t) &pthread_attr_setstacksize_soloader },
	{ "pthread_cond_broadcast", (uintptr_t) &pthread_cond_broadcast_soloader },
	{ "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
	{ "pthread_cond_init", (uintptr_t) &pthread_cond_init_soloader },
	{ "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
	{ "pthread_cond_timedwait", (uintptr_t) &pthread_cond_timedwait_soloader },
	{ "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },
	{ "pthread_create", (uintptr_t) &pthread_create_soloader },
	{ "pthread_detach", (uintptr_t) &pthread_detach_soloader },
	{ "pthread_equal", (uintptr_t) &pthread_equal_soloader },
	{ "pthread_exit", (uintptr_t) &pthread_exit },
	{ "pthread_join", (uintptr_t) &pthread_join_soloader },
	{ "pthread_getattr_np", (uintptr_t) &pthread_getattr_np_soloader },
	{ "pthread_getschedparam", (uintptr_t) &pthread_getschedparam_soloader },
	{ "pthread_getspecific", (uintptr_t)&pthread_getspecific },
	{ "pthread_key_create", (uintptr_t)&pthread_key_create },
	{ "pthread_key_delete", (uintptr_t)&pthread_key_delete },
	{ "pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_soloader },
	{ "pthread_mutex_init", (uintptr_t) &pthread_mutex_init_soloader },
	{ "pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_soloader },
	{ "pthread_mutex_trylock", (uintptr_t) &pthread_mutex_trylock_soloader},
	{ "pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_soloader },
	{ "pthread_mutexattr_destroy", (uintptr_t) &pthread_mutexattr_destroy_soloader},
	{ "pthread_mutexattr_init", (uintptr_t) &pthread_mutexattr_init_soloader},
	{ "pthread_mutexattr_setpshared", (uintptr_t) &pthread_mutexattr_setpshared_soloader},
	{ "pthread_mutexattr_settype", (uintptr_t) &pthread_mutexattr_settype_soloader},
	{ "pthread_once", (uintptr_t)&pthread_once },
	{ "pthread_self", (uintptr_t) &pthread_self_soloader },
	{ "pthread_setschedparam", (uintptr_t) &pthread_setschedparam_soloader },
	{ "pthread_setspecific", (uintptr_t)&pthread_setspecific },
	{ "sched_get_priority_min", (uintptr_t)&ret0 },
	{ "sched_get_priority_max", (uintptr_t)&ret99 },
	{ "putc", (uintptr_t)&putc },
	{ "puts", (uintptr_t)&puts },
	{ "putwc", (uintptr_t)&putwc },
	{ "qsort", (uintptr_t)&qsort },
	{ "rand", (uintptr_t)&rand },
	{ "read", (uintptr_t)&read },
	{ "realpath", (uintptr_t)&realpath },
	{ "realloc", (uintptr_t)&realloc },
	// { "recv", (uintptr_t)&recv },
	{ "roundf", (uintptr_t)&roundf },
	{ "rint", (uintptr_t)&rint },
	{ "rintf", (uintptr_t)&rintf },
	// { "send", (uintptr_t)&send },
	// { "sendto", (uintptr_t)&sendto },
	{ "setenv", (uintptr_t)&ret0 },
	{ "setjmp", (uintptr_t)&setjmp },
	{ "setlocale", (uintptr_t)&setlocale },
	// { "setsockopt", (uintptr_t)&setsockopt },
	{ "setvbuf", (uintptr_t)&setvbuf },
	{ "sin", (uintptr_t)&sin },
	{ "sinf", (uintptr_t)&sinf },
	{ "sinh", (uintptr_t)&sinh },
	//{ "sincos", (uintptr_t)&sincos },
	{ "snprintf", (uintptr_t)&snprintf },
	// { "socket", (uintptr_t)&socket },
	{ "sprintf", (uintptr_t)&sprintf },
	{ "sqrt", (uintptr_t)&sqrt },
	{ "sqrtf", (uintptr_t)&sqrtf },
	{ "srand", (uintptr_t)&srand },
	{ "srand48", (uintptr_t)&srand48 },
	{ "sscanf", (uintptr_t)&sscanf },
	{ "stat", (uintptr_t)&stat_hook },
	{ "strcasecmp", (uintptr_t)&strcasecmp },
	{ "strcasestr", (uintptr_t)&strstr },
	{ "strcat", (uintptr_t)&strcat },
	{ "strchr", (uintptr_t)&strchr },
	{ "strcmp", (uintptr_t)&sceClibStrcmp },
	{ "strcoll", (uintptr_t)&strcoll },
	{ "strcpy", (uintptr_t)&strcpy },
	{ "strcspn", (uintptr_t)&strcspn },
	{ "strdup", (uintptr_t)&strdup },
	{ "strerror", (uintptr_t)&strerror },
	{ "strftime", (uintptr_t)&strftime },
	{ "strlcpy", (uintptr_t)&strlcpy },
	{ "strlen", (uintptr_t)&strlen },
	{ "strncasecmp", (uintptr_t)&sceClibStrncasecmp },
	{ "strncat", (uintptr_t)&sceClibStrncat },
	{ "strncmp", (uintptr_t)&sceClibStrncmp },
	{ "strncpy", (uintptr_t)&sceClibStrncpy },
	{ "strpbrk", (uintptr_t)&strpbrk },
	{ "strrchr", (uintptr_t)&sceClibStrrchr },
	{ "strstr", (uintptr_t)&sceClibStrstr },
	{ "strtod", (uintptr_t)&strtod },
	{ "strtof", (uintptr_t)&strtof },
	{ "strtol", (uintptr_t)&strtol },
	{ "strtoul", (uintptr_t)&strtoul },
	{ "strtoll", (uintptr_t)&strtoll },
	{ "strtoull", (uintptr_t)&strtoull },
	{ "strtok", (uintptr_t)&strtok },
	{ "strxfrm", (uintptr_t)&strxfrm },
	{ "sysconf", (uintptr_t)&ret0 },
	{ "tan", (uintptr_t)&tan },
	{ "tanf", (uintptr_t)&tanf },
	{ "tanh", (uintptr_t)&tanh },
	{ "time", (uintptr_t)&time },
	{ "tolower", (uintptr_t)&tolower },
	{ "toupper", (uintptr_t)&toupper },
	{ "towlower", (uintptr_t)&towlower },
	{ "towupper", (uintptr_t)&towupper },
	{ "ungetc", (uintptr_t)&ungetc },
	{ "ungetwc", (uintptr_t)&ungetwc },
	{ "usleep", (uintptr_t)&usleep },
	{ "vfprintf", (uintptr_t)&vfprintf_hook },
	{ "vprintf", (uintptr_t)&vprintf },
	{ "vsnprintf", (uintptr_t)&vsnprintf },
	{ "vasprintf", (uintptr_t)&vasprintf },
	{ "vsscanf", (uintptr_t)&vsscanf },
	{ "vsprintf", (uintptr_t)&vsprintf },
	{ "vswprintf", (uintptr_t)&vswprintf },
	{ "wcrtomb", (uintptr_t)&wcrtomb },
	{ "wcscoll", (uintptr_t)&wcscoll },
	{ "wcscmp", (uintptr_t)&wcscmp },
	{ "wcsncpy", (uintptr_t)&wcsncpy },
	{ "wcsftime", (uintptr_t)&wcsftime },
	{ "wcslen", (uintptr_t)&wcslen },
	{ "wcsxfrm", (uintptr_t)&wcsxfrm },
	{ "wctob", (uintptr_t)&wctob },
	{ "wctype", (uintptr_t)&wctype },
	{ "wmemchr", (uintptr_t)&wmemchr },
	{ "wmemcmp", (uintptr_t)&wmemcmp },
	{ "wmemcpy", (uintptr_t)&wmemcpy },
	{ "wmemmove", (uintptr_t)&wmemmove },
	{ "wmemset", (uintptr_t)&wmemset },
	{ "write", (uintptr_t)&write },
	{ "sigaction", (uintptr_t)&ret0 },
	{ "zlibVersion", (uintptr_t)&zlibVersion },
	// { "writev", (uintptr_t)&writev },
	{ "unlink", (uintptr_t)&unlink },
	{ "SDL_AndroidGetActivityClass", (uintptr_t)&ret0 },
	{ "SDL_IsTextInputActive", (uintptr_t)&SDL_IsTextInputActive },
	{ "SDL_GameControllerEventState", (uintptr_t)&SDL_GameControllerEventState },
	{ "SDL_WarpMouseInWindow", (uintptr_t)&SDL_WarpMouseInWindow },
	{ "SDL_AndroidGetExternalStoragePath", (uintptr_t)&SDL_AndroidGetExternalStoragePath },
	{ "SDL_AndroidGetInternalStoragePath", (uintptr_t)&SDL_AndroidGetInternalStoragePath },
	{ "SDL_Android_Init", (uintptr_t)&ret1 },
	{ "SDL_AddTimer", (uintptr_t)&SDL_AddTimer },
	{ "SDL_CondSignal", (uintptr_t)&SDL_CondSignal },
	{ "SDL_CondWait", (uintptr_t)&SDL_CondWait },
	{ "SDL_ConvertSurfaceFormat", (uintptr_t)&SDL_ConvertSurfaceFormat },
	{ "SDL_CreateCond", (uintptr_t)&SDL_CreateCond },
	{ "SDL_CreateMutex", (uintptr_t)&SDL_CreateMutex },
	{ "SDL_CreateRenderer", (uintptr_t)&SDL_CreateRenderer },
	{ "SDL_CreateRGBSurface", (uintptr_t)&SDL_CreateRGBSurface },
	{ "SDL_CreateTexture", (uintptr_t)&SDL_CreateTexture },
	{ "SDL_CreateTextureFromSurface", (uintptr_t)&SDL_CreateTextureFromSurface },
	{ "SDL_CreateThread", (uintptr_t)&SDL_CreateThread_fake },
	{ "SDL_CreateWindow", (uintptr_t)&SDL_CreateWindow_hook },
	{ "SDL_Delay", (uintptr_t)&SDL_Delay },
	{ "SDL_DestroyMutex", (uintptr_t)&SDL_DestroyMutex },
	{ "SDL_DestroyRenderer", (uintptr_t)&SDL_DestroyRenderer },
	{ "SDL_DestroyTexture", (uintptr_t)&SDL_DestroyTexture },
	{ "SDL_DestroyWindow", (uintptr_t)&SDL_DestroyWindow },
	{ "SDL_FillRect", (uintptr_t)&SDL_FillRect },
	{ "SDL_FreeSurface", (uintptr_t)&SDL_FreeSurface },
	{ "SDL_GetCurrentDisplayMode", (uintptr_t)&SDL_GetCurrentDisplayMode },
	{ "SDL_GetDisplayMode", (uintptr_t)&SDL_GetDisplayMode },
	{ "SDL_GetError", (uintptr_t)&SDL_GetError },
	{ "SDL_GetModState", (uintptr_t)&SDL_GetModState },
	{ "SDL_GetMouseState", (uintptr_t)&SDL_GetMouseState },
	{ "SDL_GetRGBA", (uintptr_t)&SDL_GetRGBA },
	{ "SDL_GameControllerAddMappingsFromRW", (uintptr_t)&SDL_GameControllerAddMappingsFromRW },
	{ "SDL_GetNumDisplayModes", (uintptr_t)&SDL_GetNumDisplayModes },
	{ "SDL_GetRendererInfo", (uintptr_t)&SDL_GetRendererInfo },
	{ "SDL_GetTextureBlendMode", (uintptr_t)&SDL_GetTextureBlendMode },
	{ "SDL_GetPrefPath", (uintptr_t)&SDL_GetPrefPath },
	{ "SDL_GetTextureColorMod", (uintptr_t)&SDL_GetTextureColorMod },
	{ "SDL_GetDisplayDPI", (uintptr_t)&SDL_GetDisplayDPI },
	{ "SDL_GetClipboardText", (uintptr_t)&SDL_GetClipboardText },
	{ "SDL_SetClipboardText", (uintptr_t)&SDL_SetClipboardText },
	{ "SDL_GameControllerFromInstanceID", (uintptr_t)&SDL_GameControllerFromInstanceID },
	{ "SDL_DisableScreenSaver", (uintptr_t)&SDL_DisableScreenSaver },
	{ "SDL_EnableScreenSaver", (uintptr_t)&SDL_EnableScreenSaver },
	{ "SDL_HideWindow", (uintptr_t)&SDL_HideWindow },
	{ "SDL_ShowWindow", (uintptr_t)&SDL_ShowWindow },
	{ "SDL_SetWindowResizable", (uintptr_t)&SDL_SetWindowResizable },
	{ "SDL_GL_GetSwapInterval", (uintptr_t)&SDL_GL_GetSwapInterval },
	{ "SDL_GetCPUCount", (uintptr_t)&SDL_GetCPUCount },
	{ "SDL_GetTicks", (uintptr_t)&SDL_GetTicks },
	{ "SDL_GetVersion", (uintptr_t)&SDL_GetVersion_fake },
	{ "SDL_GL_BindTexture", (uintptr_t)&SDL_GL_BindTexture },
	{ "SDL_GL_GetCurrentContext", (uintptr_t)&SDL_GL_GetCurrentContext },
	{ "SDL_GL_MakeCurrent", (uintptr_t)&SDL_GL_MakeCurrent },
	{ "SDL_GL_SetAttribute", (uintptr_t)&SDL_GL_SetAttribute },
	{ "SDL_Init", (uintptr_t)&SDL_Init },
	{ "SDL_InitSubSystem", (uintptr_t)&SDL_InitSubSystem },
	{ "SDL_IntersectRect", (uintptr_t)&SDL_IntersectRect },
	{ "SDL_LockMutex", (uintptr_t)&SDL_LockMutex },
	{ "SDL_LockSurface", (uintptr_t)&SDL_LockSurface },
	{ "SDL_memcpy", (uintptr_t)&SDL_memcpy },
	{ "SDL_Log", (uintptr_t)&ret0 },
	{ "SDL_LogError", (uintptr_t)&ret0 },
	{ "SDL_LogSetPriority", (uintptr_t)&ret0 },
	{ "SDL_MapRGB", (uintptr_t)&SDL_MapRGB },
	{ "SDL_JoystickInstanceID", (uintptr_t)&SDL_JoystickInstanceID },
	{ "SDL_GameControllerGetAxis", (uintptr_t)&SDL_GameControllerGetAxis },
	{ "SDL_MinimizeWindow", (uintptr_t)&SDL_MinimizeWindow },
	{ "SDL_PeepEvents", (uintptr_t)&SDL_PeepEvents },
	{ "SDL_PumpEvents", (uintptr_t)&SDL_PumpEvents },
	{ "SDL_PushEvent", (uintptr_t)&SDL_PushEvent },
	{ "SDL_PollEvent", (uintptr_t)&SDL_PollEvent_hook },
	{ "SDL_QueryTexture", (uintptr_t)&SDL_QueryTexture },
	{ "SDL_Quit", (uintptr_t)&SDL_Quit },
	{ "SDL_RemoveTimer", (uintptr_t)&SDL_RemoveTimer },
	{ "SDL_RenderClear", (uintptr_t)&SDL_RenderClear },
	{ "SDL_RenderCopy", (uintptr_t)&SDL_RenderCopy },
	{ "SDL_RenderFillRect", (uintptr_t)&SDL_RenderFillRect },
	{ "SDL_RenderPresent", (uintptr_t)&SDL_RenderPresent },
	{ "SDL_RWFromFile", (uintptr_t)&SDL_RWFromFile_hook },
	{ "SDL_RWread", (uintptr_t)&SDL_RWread },
	{ "SDL_RWwrite", (uintptr_t)&SDL_RWwrite },
	{ "SDL_RWclose", (uintptr_t)&SDL_RWclose },
	{ "SDL_RWsize", (uintptr_t)&SDL_RWsize },
	{ "SDL_RWFromMem", (uintptr_t)&SDL_RWFromMem },
	{ "SDL_SetColorKey", (uintptr_t)&SDL_SetColorKey },
	{ "SDL_SetEventFilter", (uintptr_t)&SDL_SetEventFilter },
	{ "SDL_SetHint", (uintptr_t)&SDL_SetHint },
	{ "SDL_SetMainReady_REAL", (uintptr_t)&SDL_SetMainReady },
	{ "SDL_SetRenderDrawBlendMode", (uintptr_t)&SDL_SetRenderDrawBlendMode },
	{ "SDL_SetRenderDrawColor", (uintptr_t)&SDL_SetRenderDrawColor },
	{ "SDL_SetRenderTarget", (uintptr_t)&SDL_SetRenderTarget },
	{ "SDL_SetTextureBlendMode", (uintptr_t)&SDL_SetTextureBlendMode },
	{ "SDL_SetTextureColorMod", (uintptr_t)&SDL_SetTextureColorMod },
	{ "SDL_ShowCursor", (uintptr_t)&SDL_ShowCursor },
	{ "SDL_ShowSimpleMessageBox", (uintptr_t)&SDL_ShowSimpleMessageBox },
	{ "SDL_StartTextInput", (uintptr_t)&SDL_StartTextInput },
	{ "SDL_StopTextInput", (uintptr_t)&SDL_StopTextInput },
	{ "SDL_strdup", (uintptr_t)&SDL_strdup },
	{ "SDL_UnlockMutex", (uintptr_t)&SDL_UnlockMutex },
	{ "SDL_UnlockSurface", (uintptr_t)&SDL_UnlockSurface },
	{ "SDL_UpdateTexture", (uintptr_t)&SDL_UpdateTexture },
	{ "SDL_UpperBlit", (uintptr_t)&SDL_UpperBlit },
	{ "SDL_WaitThread", (uintptr_t)&SDL_WaitThread_fake },
	{ "SDL_GetKeyFromScancode", (uintptr_t)&SDL_GetKeyFromScancode },
	{ "SDL_GetNumVideoDisplays", (uintptr_t)&SDL_GetNumVideoDisplays },
	{ "SDL_GetDisplayBounds", (uintptr_t)&SDL_GetDisplayBounds },
	{ "SDL_UnionRect", (uintptr_t)&SDL_UnionRect },
	{ "SDL_GetKeyboardFocus", (uintptr_t)&SDL_GetKeyboardFocus },
	{ "SDL_GetRelativeMouseMode", (uintptr_t)&SDL_GetRelativeMouseMode },
	{ "SDL_NumJoysticks", (uintptr_t)&SDL_NumJoysticks },
	{ "SDL_GL_GetDrawableSize", (uintptr_t)&SDL_GL_GetDrawableSize },
	{ "SDL_GameControllerOpen", (uintptr_t)&SDL_GameControllerOpen },
	{ "SDL_GameControllerGetJoystick", (uintptr_t)&SDL_GameControllerGetJoystick },
	{ "SDL_HapticOpenFromJoystick", (uintptr_t)&SDL_HapticOpenFromJoystick },
	{ "SDL_GetPerformanceFrequency", (uintptr_t)&SDL_GetPerformanceFrequency },
	{ "SDL_GetPerformanceCounter", (uintptr_t)&SDL_GetPerformanceCounter },
	{ "SDL_GetMouseFocus", (uintptr_t)&SDL_GetMouseFocus },
	{ "SDL_ShowMessageBox", (uintptr_t)&SDL_ShowMessageBox },
	{ "SDL_RaiseWindow", (uintptr_t)&SDL_RaiseWindow },
	{ "SDL_GL_GetAttribute", (uintptr_t)&SDL_GL_GetAttribute },
	{ "SDL_GL_CreateContext", (uintptr_t)&SDL_GL_CreateContext },
	{ "SDL_GL_GetProcAddress", (uintptr_t)&SDL_GL_GetProcAddress_fake },
	{ "SDL_GL_DeleteContext", (uintptr_t)&SDL_GL_DeleteContext },
	{ "SDL_GetDesktopDisplayMode", (uintptr_t)&SDL_GetDesktopDisplayMode },
	{ "SDL_SetWindowData", (uintptr_t)&SDL_SetWindowData },
	{ "SDL_GetWindowFlags", (uintptr_t)&SDL_GetWindowFlags },
	{ "SDL_GetWindowSize", (uintptr_t)&SDL_GetWindowSize },
	{ "SDL_GetWindowDisplayIndex", (uintptr_t)&SDL_GetWindowDisplayIndex },
	{ "SDL_SetWindowFullscreen", (uintptr_t)&SDL_SetWindowFullscreen },
	{ "SDL_SetWindowSize", (uintptr_t)&SDL_SetWindowSize },
	{ "SDL_SetWindowPosition", (uintptr_t)&SDL_SetWindowPosition },
	{ "SDL_GL_GetCurrentWindow", (uintptr_t)&SDL_GL_GetCurrentWindow },
	{ "SDL_GetWindowData", (uintptr_t)&SDL_GetWindowData },
	{ "SDL_GetWindowTitle", (uintptr_t)&SDL_GetWindowTitle },
	{ "SDL_ResetKeyboard", (uintptr_t)&SDL_ResetKeyboard },
	{ "SDL_SetWindowTitle", (uintptr_t)&SDL_SetWindowTitle },
	{ "SDL_GetWindowPosition", (uintptr_t)&SDL_GetWindowPosition },
	{ "SDL_GL_SetSwapInterval", (uintptr_t)&SDL_GL_SetSwapInterval },
	{ "SDL_IsGameController", (uintptr_t)&SDL_IsGameController },
	{ "SDL_JoystickGetDeviceGUID", (uintptr_t)&SDL_JoystickGetDeviceGUID },
	{ "SDL_GameControllerNameForIndex", (uintptr_t)&SDL_GameControllerNameForIndex },
	{ "SDL_GetWindowFromID", (uintptr_t)&SDL_GetWindowFromID },
	{ "SDL_GL_SwapWindow", (uintptr_t)&SDL_GL_SwapWindow },
	{ "SDL_SetMainReady", (uintptr_t)&SDL_SetMainReady },
	{ "SDL_NumAccelerometers", (uintptr_t)&ret0 },
	{ "SDL_AndroidGetJNIEnv", (uintptr_t)&Android_JNI_GetEnv },
	{ "Android_JNI_GetEnv", (uintptr_t)&Android_JNI_GetEnv },
	{ "SDL_RWFromConstMem", (uintptr_t)&SDL_RWFromConstMem },
	{ "SDL_ConvertSurface", (uintptr_t)&SDL_ConvertSurface },
	{ "SDL_SetError", (uintptr_t)&SDL_SetError },
	{ "SDL_MapRGBA", (uintptr_t)&SDL_MapRGBA },
	{ "SDL_EventState", (uintptr_t)&SDL_EventState },
	{ "SDL_SetSurfaceBlendMode", (uintptr_t)&SDL_SetSurfaceBlendMode },
	{ "SDL_UpperBlitScaled", (uintptr_t)&SDL_UpperBlitScaled },
	{ "SDL_FreeRW", (uintptr_t)&SDL_FreeRW },
	{ "SDL_GetKeyboardState", (uintptr_t)&SDL_GetKeyboardState },
	{ "SDL_JoystickNumAxes", (uintptr_t)&ret4 },
	{ "SDL_JoystickUpdate", (uintptr_t)&SDL_JoystickUpdate },
	{ "SDL_JoystickGetAxis", (uintptr_t)&SDL_JoystickGetAxis },
	{ "SDL_JoystickGetButton", (uintptr_t)&SDL_JoystickGetButton },
	{ "SDL_GetScancodeFromKey", (uintptr_t)&SDL_GetScancodeFromKey },
	{ "SDL_GetKeyName", (uintptr_t)&SDL_GetKeyName },
	{ "SDL_GetScancodeName", (uintptr_t)&SDL_GetScancodeName },
	{ "SDL_JoystickGetHat", (uintptr_t)&SDL_JoystickGetHat },
	{ "SDL_JoystickClose", (uintptr_t)&SDL_JoystickClose },
	{ "SDL_JoystickOpen", (uintptr_t)&SDL_JoystickOpen },
	{ "SDL_JoystickEventState", (uintptr_t)&SDL_JoystickEventState },
	{ "SDL_LogSetAllPriority", (uintptr_t)&SDL_LogSetAllPriority },
	{ "SDL_LogMessageV", (uintptr_t)&SDL_LogMessageV },
	{ "SDL_RWtell", (uintptr_t)&SDL_RWtell },
	{ "SDL_AndroidGetActivity", (uintptr_t)&ret0 },
	{ "SDL_free", (uintptr_t)&SDL_free },
	{ "SDL_AtomicAdd", (uintptr_t)&SDL_AtomicAdd },
	{ "SDL_AtomicSet", (uintptr_t)&SDL_AtomicSet },
	{ "SDL_CreateSystemCursor", (uintptr_t)&SDL_CreateSystemCursor },
	{ "SDL_OpenAudio", (uintptr_t)&SDL_OpenAudio },
	{ "SDL_CloseAudio", (uintptr_t)&SDL_CloseAudio },
	{ "SDL_PauseAudio", (uintptr_t)&SDL_PauseAudio },
	{ "SDL_CreateCursor", (uintptr_t)&SDL_CreateCursor },
	{ "SDL_SetCursor", (uintptr_t)&SDL_SetCursor },
	{ "SDL_GameControllerClose", (uintptr_t)&SDL_GameControllerClose },
	{ "SDL_FreeCursor", (uintptr_t)&SDL_FreeCursor },
	{ "SDL_CreateColorCursor", (uintptr_t)&SDL_CreateColorCursor },
	{ "IMG_Init", (uintptr_t)&IMG_Init },
	{ "IMG_Quit", (uintptr_t)&IMG_Quit },
	{ "Mix_PauseMusic", (uintptr_t)&Mix_PauseMusic },
	{ "Mix_ResumeMusic", (uintptr_t)&Mix_ResumeMusic },
	{ "Mix_VolumeMusic", (uintptr_t)&Mix_VolumeMusic },
	{ "Mix_LoadMUS", (uintptr_t)&Mix_LoadMUS_hook },
	{ "Mix_PlayMusic", (uintptr_t)&Mix_PlayMusic },
	{ "Mix_FreeMusic", (uintptr_t)&ret0 }, // FIXME
	{ "Mix_RewindMusic", (uintptr_t)&Mix_RewindMusic },
	{ "Mix_SetMusicPosition", (uintptr_t)&Mix_SetMusicPosition },
	{ "Mix_CloseAudio", (uintptr_t)&Mix_CloseAudio },
	{ "Mix_OpenAudio", (uintptr_t)&Mix_OpenAudio_hook },
	{ "Mix_RegisterEffect", (uintptr_t)&Mix_RegisterEffect },
	{ "Mix_Resume", (uintptr_t)&Mix_Resume },
	{ "Mix_AllocateChannels", (uintptr_t)&Mix_AllocateChannels },
	{ "Mix_ChannelFinished", (uintptr_t)&Mix_ChannelFinished },
	{ "Mix_LoadWAV_RW", (uintptr_t)&Mix_LoadWAV_RW },
	{ "Mix_FreeChunk", (uintptr_t)&Mix_FreeChunk },
	{ "Mix_PausedMusic", (uintptr_t)&Mix_PausedMusic },
	{ "Mix_Paused", (uintptr_t)&Mix_Paused },
	{ "Mix_PlayingMusic", (uintptr_t)&Mix_PlayingMusic },
	{ "Mix_Playing", (uintptr_t)&Mix_Playing },
	{ "Mix_Volume", (uintptr_t)&Mix_Volume },
	{ "Mix_SetDistance", (uintptr_t)&Mix_SetDistance },
	{ "Mix_SetPanning", (uintptr_t)&Mix_SetPanning },
	{ "Mix_QuerySpec", (uintptr_t)&Mix_QuerySpec },
	{ "Mix_UnregisterEffect", (uintptr_t)&Mix_UnregisterEffect },
	{ "Mix_HaltMusic", (uintptr_t)&Mix_HaltMusic },
	{ "Mix_HaltChannel", (uintptr_t)&Mix_HaltChannel },
	{ "Mix_LoadMUS_RW", (uintptr_t)&Mix_LoadMUS_RW },
	{ "Mix_PlayChannelTimed", (uintptr_t)&Mix_PlayChannelTimed },
	{ "Mix_Pause", (uintptr_t)&Mix_Pause },
	{ "Mix_Init", (uintptr_t)&Mix_Init },
	/*{ "TTF_Quit", (uintptr_t)&TTF_Quit },
	{ "TTF_Init", (uintptr_t)&TTF_Init },
	{ "TTF_RenderText_Blended", (uintptr_t)&TTF_RenderText_Blended },
	{ "TTF_OpenFontRW", (uintptr_t)&TTF_OpenFontRW },
	{ "TTF_SetFontOutline", (uintptr_t)&TTF_SetFontOutline },
	{ "TTF_CloseFont", (uintptr_t)&TTF_CloseFont },
	{ "TTF_GlyphIsProvided", (uintptr_t)&TTF_GlyphIsProvided },*/
	{ "IMG_Load", (uintptr_t)&IMG_Load_hook },
	{ "IMG_Load_RW", (uintptr_t)&IMG_Load_RW },
	{ "raise", (uintptr_t)&raise },
	{ "posix_memalign", (uintptr_t)&posix_memalign },
	{ "swprintf", (uintptr_t)&swprintf },
	{ "wcscpy", (uintptr_t)&wcscpy },
	{ "wcscat", (uintptr_t)&wcscat },
	{ "wcstombs", (uintptr_t)&wcstombs },
	{ "wcsstr", (uintptr_t)&wcsstr },
	{ "compress", (uintptr_t)&compress },
	{ "uncompress", (uintptr_t)&uncompress },
	{ "atof", (uintptr_t)&atof },
	{ "SDLNet_FreePacket", (uintptr_t)&SDLNet_FreePacket },
	{ "SDLNet_Quit", (uintptr_t)&SDLNet_Quit },
	{ "SDLNet_GetError", (uintptr_t)&SDLNet_GetError },
	{ "SDLNet_Init", (uintptr_t)&SDLNet_Init },
	{ "SDLNet_AllocPacket", (uintptr_t)&SDLNet_AllocPacket },
	{ "SDLNet_UDP_Recv", (uintptr_t)&SDLNet_UDP_Recv },
	{ "SDLNet_UDP_Send", (uintptr_t)&SDLNet_UDP_Send },
	{ "SDLNet_GetLocalAddresses", (uintptr_t)&SDLNet_GetLocalAddresses },
	{ "SDLNet_UDP_Close", (uintptr_t)&SDLNet_UDP_Close },
	{ "SDLNet_ResolveHost", (uintptr_t)&SDLNet_ResolveHost },
	{ "SDLNet_UDP_Open", (uintptr_t)&SDLNet_UDP_Open },
	{ "remove", (uintptr_t)&remove },
	{ "IMG_SavePNG", (uintptr_t)&IMG_SavePNG },
	{ "SDL_DetachThread", (uintptr_t)&SDL_DetachThread_fake },
	/*{ "TTF_SetFontHinting", (uintptr_t)&TTF_SetFontHinting },
	{ "TTF_FontHeight", (uintptr_t)&TTF_FontHeight },
	{ "TTF_FontAscent", (uintptr_t)&TTF_FontAscent },
	{ "TTF_FontDescent", (uintptr_t)&TTF_FontDescent },
	{ "TTF_SizeUTF8", (uintptr_t)&TTF_SizeUTF8 },
	{ "TTF_SizeText", (uintptr_t)&TTF_SizeText },
	{ "TTF_SetFontStyle", (uintptr_t)&TTF_SetFontStyle },
	{ "TTF_RenderUTF8_Blended", (uintptr_t)&TTF_RenderUTF8_Blended },*/
	{ "SDL_strlen", (uintptr_t)&SDL_strlen },
	{ "SDL_LogDebug", (uintptr_t)&ret0 },
	{ "SDL_HasEvents", (uintptr_t)&SDL_HasEvents },
	{ "SDL_RWseek", (uintptr_t)&SDL_RWseek },
	{ "SDL_JoystickNameForIndex", (uintptr_t)&SDL_JoystickNameForIndex },
	{ "SDL_JoystickNumButtons", (uintptr_t)&SDL_JoystickNumButtons },
	{ "SDL_JoystickGetGUID", (uintptr_t)&SDL_JoystickGetGUID },
	{ "SDL_JoystickGetGUIDString", (uintptr_t)&SDL_JoystickGetGUIDString },
	{ "SDL_JoystickNumHats", (uintptr_t)&SDL_JoystickNumHats },
	{ "SDL_JoystickNumBalls", (uintptr_t)&SDL_JoystickNumBalls },
	{ "SDL_JoystickName", (uintptr_t)&SDL_JoystickName_fake },
	{ "SDL_GetNumRenderDrivers", (uintptr_t)&SDL_GetNumRenderDrivers },
	{ "SDL_GetRenderDriverInfo", (uintptr_t)&SDL_GetRenderDriverInfo },
	{ "SDL_GetNumVideoDrivers", (uintptr_t)&SDL_GetNumVideoDrivers },
	{ "SDL_GetVideoDriver", (uintptr_t)&SDL_GetVideoDriver },
	{ "SDL_GetBasePath", (uintptr_t)&SDL_GetBasePath_hook },
	{ "SDL_RenderReadPixels", (uintptr_t)&SDL_RenderReadPixels },
	{ "SDL_CreateRGBSurfaceFrom", (uintptr_t)&SDL_CreateRGBSurfaceFrom },
	{ "SDL_SetWindowBordered", (uintptr_t)&SDL_SetWindowBordered },
	{ "SDL_RestoreWindow", (uintptr_t)&SDL_RestoreWindow },
	{ "SDL_sqrt", (uintptr_t)&SDL_sqrt },
	{ "SDL_ThreadID", (uintptr_t)&SDL_ThreadID },
	{ "__system_property_get", (uintptr_t)&ret0 },
	{ "strnlen", (uintptr_t)&strnlen },
	{ "wcstoul", (uintptr_t)&wcstoul },
	{ "wcstoll", (uintptr_t)&wcstoll },
	{ "wcstoull", (uintptr_t)&wcstoull },
	{ "wcstof", (uintptr_t)&wcstof },
	{ "wcstod", (uintptr_t)&wcstod },
	{ "wcstol", (uintptr_t)&wcstol },
	{ "__assert2", (uintptr_t)&__assert2 },
	{ "SDL_LogInfo", (uintptr_t)&ret0 },
	{ "SDL_LogWarn", (uintptr_t)&ret0 },
	{ "glDetachShader", (uintptr_t)&ret0 },
	{ "strtok_r", (uintptr_t)&strtok_r },
	{ "Mix_FadeOutChannel", (uintptr_t)&Mix_FadeOutChannel },
	{ "isnan", (uintptr_t)&isnan },
};
static size_t numhooks = sizeof(default_dynlib) / sizeof(*default_dynlib);

int check_kubridge(void) {
	int search_unk[2];
	return _vshKernelSearchModuleByName("kubridge", search_unk);
}

enum MethodIDs {
	UNKNOWN = 0,
	INIT,
	GET_CURRENT_LANGUAGE
} MethodIDs;

typedef struct {
	char *name;
	enum MethodIDs id;
} NameToMethodID;

static NameToMethodID name_to_method_ids[] = {
	{ "<init>", INIT },
	{ "getCurrentLanguage", GET_CURRENT_LANGUAGE },
};

int GetMethodID(void *env, void *class, const char *name, const char *sig) {
	printf("GetMethodID: %s\n", name);

	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0) {
			return name_to_method_ids[i].id;
		}
	}

	return UNKNOWN;
}

int GetStaticMethodID(void *env, void *class, const char *name, const char *sig) {
	printf("GetStaticMethodID: %s\n", name);
	
	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0)
			return name_to_method_ids[i].id;
	}

	return UNKNOWN;
}

void CallStaticVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
}

int CallStaticBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

int CallStaticIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

int64_t CallStaticLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

uint64_t CallLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return -1;
}

float CallFloatMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return 220.0f;
}

void *FindClass(void) {
	return (void *)0x41414141;
}

void *NewGlobalRef(void *env, char *str) {
	return (void *)0x42424242;
}

void DeleteGlobalRef(void *env, char *str) {
}

void *NewObjectV(void *env, void *clazz, int methodID, uintptr_t args) {
	return (void *)0x43434343;
}

void *GetObjectClass(void *env, void *obj) {
	return (void *)0x44444444;
}

char *NewStringUTF(void *env, char *bytes) {
	return bytes;
}

char *GetStringUTFChars(void *env, char *string, int *isCopy) {
	return string;
}

size_t GetStringUTFLength(void *env, char *string) {
	return strlen(string);	
}

int GetJavaVM(void *env, void **vm) {
	*vm = fake_vm;
	return 0;
}

int GetFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

int GetBooleanField(void *env, void *obj, int fieldID) {
	return 1;
}

void *GetObjectArrayElement(void *env, uint8_t *obj, int idx) {
	return NULL;
}

int CallBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

char duration[32];
void *CallObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	int lang = -1;
	switch (methodID) {
	default:
		return 0x34343434;
	}
}

int CallIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	case GET_CURRENT_LANGUAGE:
		return 1;
	default:
		return 0;
	}
}

void CallVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		break;
	}
}

int GetStaticFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

void *GetStaticObjectField(void *env, void *clazz, int fieldID) {
	switch (fieldID) {
	default:
		return NULL;
	}
}

void GetStringUTFRegion(void *env, char *str, size_t start, size_t len, char *buf) {
	sceClibMemcpy(buf, &str[start], len);
	buf[len] = 0;
}

void *CallStaticObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return NULL;
}

int GetIntField(void *env, void *obj, int fieldID) { return 0; }

float GetFloatField(void *env, void *obj, int fieldID) {
	switch (fieldID) {
	default:
		return 0.0f;
	}
}

float CallStaticFloatMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		if (methodID != UNKNOWN) {
			dlog("CallStaticDoubleMethodV(%d)\n", methodID);
		}
		return 0;
	}
}

int GetArrayLength(void *env, void *array) {
	printf("GetArrayLength returned %d\n", *(int *)array);
	return *(int *)array;
}

int crasher(unsigned int argc, void *argv) {
	uint32_t *nullptr = NULL;
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_SELECT) *nullptr = 0;
		sceKernelDelayThread(100);
	}
}

const char *simple_shd_vp = R"(
	attribute vec4 a_Location;
	attribute vec4 a_Color;
	uniform mat4 u_Projection;
	uniform mat4 u_View;
	uniform mat4 u_Model;
	varying vec4 var_color;
	void main() {
		gl_Position =  u_Projection * u_View * u_Model * a_Location;
		var_color = a_Color;
	}
)";

const char *depth_fp = R"(
	void main() {
		gl_FragDepth =  gl_FragCoord.z;
	}
)";

const char *simple_shd_fp = R"(
	varying vec4 var_color;
	void main() {
		gl_FragColor =  var_color;
	}
)";

const char *vp_3d = R"(
	attribute vec4 a_Location;                              
    attribute vec3 a_Normal;                                
    attribute vec2 a_TexCoords;                             
    uniform mat4 u_Projection;                                     
    uniform mat4 u_View;                                      
    uniform mat4 u_Model;                                      
    uniform mat4 u_DepthMVP;                                
    varying vec3 var_normal;                                
    varying vec3 var_worldPos;                       
    varying vec2 var_texCoords;                             
    varying vec4 var_shadowLoc;                             
    void main(void)                                         
	{                                                       
        var_texCoords = a_TexCoords;         
        var_worldPos = vec3(u_Model * a_Location);           
        var_normal = mat3(u_Model) * a_Normal;               
        gl_Position =  u_Projection * u_View * vec4(var_worldPos, 1.0);         
        var_shadowLoc = u_DepthMVP * a_Location;      
})";                                                    

const char *vp_2d = R"(
	attribute vec2 a_Location;
	attribute vec2 a_TexCoords;
	attribute vec4 a_Color;
	uniform vec2 u_WH;
	uniform float u_Zoom;
	uniform vec2 u_CamPos;
	varying vec4 var_color;
	varying vec2 var_TexCoords;
	void main() {
		gl_Position =  vec4((a_Location - u_CamPos) / (u_WH * 0.5) * u_Zoom, 0.0, 1.0);
		var_color = a_Color;
		var_TexCoords = a_TexCoords;
	}
)";

const char *fp_2d = R"(
	varying vec4 var_color;
	varying vec2 var_TexCoords;
	uniform int u_UseTexture;
	uniform sampler2D u_Texture;
	void main() {
		vec4 texColor = vec4(1.0, 1.0, 1.0, 1.0);
		if (u_UseTexture>0)
			texColor = texture2D( u_Texture, var_TexCoords );
		gl_FragColor = texColor * var_color;
	}
)";

so_hook shd_prog;
void *ShaderProgram(void *this, const char *vp, const char *fp) {
	// Game has a brainfart and attempts to use GLSL ES 3.00 shader even if it has a GLSL ES 1.3 variant
	if (fp == rrm_mod.text_base + 0x000BF5E9) {
		printf("Patching fp from 0x000BF5E9 (3.00 -> 1.00)\n");
		fp = rrm_mod.text_base + 0x000C5022;
	}
	if (fp == rrm_mod.text_base + 0x000DFD3C) {
		printf("Patching fp from 0x000DFD3C (3.00 -> 1.00)\n");
		fp = rrm_mod.text_base + 0x000C5022;
	}
	if (vp == rrm_mod.text_base + 0x000D2EC1) {
		printf("Patching 2D shaders (2D)\n");
		vp = vp_2d;
		fp = fp_2d;
	}
	if (vp == rrm_mod.text_base + 0x000D3CC2 || fp == rrm_mod.text_base + 0x000D3CC2) {
		printf("Patching from 0x000D3CC2 (Simple)\n");
		vp = simple_shd_vp;
		fp = simple_shd_fp;
	}
	if (fp == rrm_mod.text_base + 0x000E4722) {
		printf("Patching fp from 0x000E4722 (Depth Replace)\n");
		fp = depth_fp;
	}
	if (vp == rrm_mod.text_base + 0x000D264F ) {
		printf("Patching vp from 0x000D264F (Mesh Vertex)\n");
		vp = vp_3d;
	}
	return SO_CONTINUE(void *, shd_prog, this, vp, fp);
}

void patch_game(void) {
	shd_prog = hook_addr((uintptr_t)so_symbol(&rrm_mod, "_ZN13ShaderProgramC2EPKcS1_"), (uintptr_t)&ShaderProgram);
	
	hook_addr((uintptr_t)so_symbol(&rrm_mod, "_ZN8firebase5admob14InterstitialAdC2Ev"), (uintptr_t)&ret0);
	hook_addr((uintptr_t)so_symbol(&rrm_mod, "_ZN14InterstitialAdC2EP8_jobject"), (uintptr_t)&ret0);
	
	hook_addr((uintptr_t)so_symbol(&rrm_mod, "__cxa_guard_acquire"), (uintptr_t)&__cxa_guard_acquire);
	hook_addr((uintptr_t)so_symbol(&rrm_mod, "__cxa_guard_release"), (uintptr_t)&__cxa_guard_release);
	//hook_addr((uintptr_t)so_symbol(&rrm_mod, "__cxa_guard_abort"), (uintptr_t)&__cxa_guard_abort);
}

void *pthread_main(void *arg) {
	char fname[256];
	sprintf(data_path, "ux0:data/rrm");

	printf("Loading libc++_shared\n");
	sprintf(fname, "%s/libc++_shared.so", data_path);
	if (so_file_load(&cpp_mod, fname, 0x98000000) < 0)
		fatal_error("Error could not load %s.", fname);
	so_relocate(&cpp_mod);
	so_resolve(&cpp_mod, default_dynlib, sizeof(default_dynlib), 0);
	so_flush_caches(&cpp_mod);
	so_initialize(&cpp_mod);

	printf("Loading libmain\n");
	sprintf(fname, "%s/libmain.so", data_path);
	if (so_file_load(&rrm_mod, fname, LOAD_ADDRESS) < 0)
		fatal_error("Error could not load %s.", fname);
	so_relocate(&rrm_mod);
	so_resolve(&rrm_mod, default_dynlib, sizeof(default_dynlib), 0);
	
	vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, SHARK_ENABLE, SHARK_ENABLE, SHARK_ENABLE);
	vglInitExtended(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
	
	patch_game();
	so_flush_caches(&rrm_mod);
	so_initialize(&rrm_mod);
	
	memset(fake_vm, 'A', sizeof(fake_vm));
	*(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm; // just point to itself...
	*(uintptr_t *)(fake_vm + 0x10) = (uintptr_t)GetEnv;
	*(uintptr_t *)(fake_vm + 0x14) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x18) = (uintptr_t)GetEnv;

	memset(fake_env, 'A', sizeof(fake_env));
	*(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env; // just point to itself...
	*(uintptr_t *)(fake_env + 0x18) = (uintptr_t)FindClass;
	*(uintptr_t *)(fake_env + 0x3C) = (uintptr_t)ret0; // HookExceptionOccurred
	*(uintptr_t *)(fake_env + 0x4C) = (uintptr_t)ret0; // PushLocalFrame
	*(uintptr_t *)(fake_env + 0x50) = (uintptr_t)ret0; // PopLocalFrame
	*(uintptr_t *)(fake_env + 0x54) = (uintptr_t)NewGlobalRef;
	*(uintptr_t *)(fake_env + 0x58) = (uintptr_t)DeleteGlobalRef;
	*(uintptr_t *)(fake_env + 0x5C) = (uintptr_t)ret0; // DeleteLocalRef
	*(uintptr_t *)(fake_env + 0x74) = (uintptr_t)NewObjectV;
	*(uintptr_t *)(fake_env + 0x7C) = (uintptr_t)GetObjectClass;
	*(uintptr_t *)(fake_env + 0x84) = (uintptr_t)GetMethodID;
	*(uintptr_t *)(fake_env + 0x8C) = (uintptr_t)CallObjectMethodV;
	*(uintptr_t *)(fake_env + 0x98) = (uintptr_t)CallBooleanMethodV;
	*(uintptr_t *)(fake_env + 0xC8) = (uintptr_t)CallIntMethodV;
	*(uintptr_t *)(fake_env + 0xD4) = (uintptr_t)CallLongMethodV;
	*(uintptr_t *)(fake_env + 0xE0) = (uintptr_t)CallFloatMethodV;
	*(uintptr_t *)(fake_env + 0xF8) = (uintptr_t)CallVoidMethodV;
	*(uintptr_t *)(fake_env + 0x178) = (uintptr_t)GetFieldID;
	*(uintptr_t *)(fake_env + 0x17C) = (uintptr_t)GetBooleanField;
	*(uintptr_t *)(fake_env + 0x190) = (uintptr_t)GetIntField;
	*(uintptr_t *)(fake_env + 0x198) = (uintptr_t)GetFloatField;
	*(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetStaticMethodID;
	*(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallStaticObjectMethodV;
	*(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallStaticBooleanMethodV;
	*(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallStaticIntMethodV;
	*(uintptr_t *)(fake_env + 0x21C) = (uintptr_t)CallStaticLongMethodV;
	*(uintptr_t *)(fake_env + 0x220) = (uintptr_t)CallStaticFloatMethodV;
	*(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallStaticVoidMethodV;
	*(uintptr_t *)(fake_env + 0x240) = (uintptr_t)GetStaticFieldID;
	*(uintptr_t *)(fake_env + 0x244) = (uintptr_t)GetStaticObjectField;
	*(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
	*(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
	*(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
	*(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0; // ReleaseStringUTFChars
	*(uintptr_t *)(fake_env + 0x2AC) = (uintptr_t)GetArrayLength;
	*(uintptr_t *)(fake_env + 0x2B4) = (uintptr_t)GetObjectArrayElement;
	*(uintptr_t *)(fake_env + 0x35C) = (uintptr_t)ret0; // RegisterNatives
	*(uintptr_t *)(fake_env + 0x36C) = (uintptr_t)GetJavaVM;
	*(uintptr_t *)(fake_env + 0x374) = (uintptr_t)GetStringUTFRegion;
	
	// Disabling rearpad
	SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);
	
	int (*JNI_OnLoad)(void *jvm) = (void*)so_symbol(&rrm_mod,"JNI_OnLoad");
	if (JNI_OnLoad)
		JNI_OnLoad(fake_vm);
	
	int (*SDL_main)(int argc, char *argv[]) = (void *) so_symbol(&rrm_mod, "SDL_main");
	printf("Jumping in SDL_main\n");
	SDL_main(0, NULL);
	printf("Exiting\n");
}	

int main(int argc, char *argv[]) {
	//sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_CAPTURE);
	//SceUID crasher_thread = sceKernelCreateThread("crasher", crasher, 0x40, 0x1000, 0, 0, NULL);
	//sceKernelStartThread(crasher_thread, 0, NULL);
	
	vglUseLowPrecision(GL_TRUE);
	SceAppUtilInitParam init_param = {0};
	SceAppUtilBootParam boot_param = {0};
	sceAppUtilInit(&init_param, &boot_param);
	SceAppUtilAppEventParam eventParam;
	sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
	sceAppUtilReceiveAppEvent(&eventParam);
	if (eventParam.type == 0x05) {
		char buffer[2048];
		sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
		if (strstr(buffer, "custom"))
			vglUseLowPrecision(GL_FALSE);
	}
	
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	if (check_kubridge() < 0)
		fatal_error("Error kubridge.skprx is not installed.");

	if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx"))
		fatal_error("Error libshacccg.suprx is not installed.");
	
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 1024 * 1024);
	pthread_create(&t, &attr, pthread_main, NULL);

	pthread_join(t, NULL);
	
	return 0;
}
