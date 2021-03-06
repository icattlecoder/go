// Copyright 2009 The Go Authors.  All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <sys/types.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include "libcgo.h"

static void* threadentry(void*);

// TCB_SIZE is sizeof(struct thread_control_block),
// as defined in /usr/src/lib/librthread/tcb.h
#define TCB_SIZE (4 * sizeof(void *))
#define TLS_SIZE (2 * sizeof(void *))

void *__get_tcb(void);
void __set_tcb(void *);

static int (*sys_pthread_create)(pthread_t *thread, const pthread_attr_t *attr,
	void *(*start_routine)(void *), void *arg);

struct thread_args {
	void *(*func)(void *);
	void *arg;
};

static void
tcb_fixup(int mainthread)
{
	void *newtcb, *oldtcb;

	// The OpenBSD ld.so(1) does not currently support PT_TLS. As a result,
	// we need to allocate our own TLS space while preserving the existing
	// TCB that has been setup via librthread.

	newtcb = malloc(TCB_SIZE + TLS_SIZE);
	if(newtcb == NULL)
		abort();

	// The signal trampoline expects the TLS slots to be zeroed.
	bzero(newtcb, TLS_SIZE);

	oldtcb = __get_tcb();
	bcopy(oldtcb, newtcb + TLS_SIZE, TCB_SIZE);
	__set_tcb(newtcb + TLS_SIZE);

	// The main thread TCB is a static allocation - do not try to free it.
	if(!mainthread)
		free(oldtcb);
}

static void *
thread_start_wrapper(void *arg)
{
	struct thread_args args = *(struct thread_args *)arg;

	free(arg);
	tcb_fixup(0);

	return args.func(args.arg);
}

int
pthread_create(pthread_t *thread, const pthread_attr_t *attr,
	void *(*start_routine)(void *), void *arg)
{
	struct thread_args *p;

	p = malloc(sizeof(*p));
	if(p == NULL) {
		errno = ENOMEM;
		return -1;
	}
	p->func = start_routine;
	p->arg = arg;

	return sys_pthread_create(thread, attr, thread_start_wrapper, p);
}

static void
xinitcgo(G *g)
{
	pthread_attr_t attr;
	size_t size;
	void *handle;

	pthread_attr_init(&attr);
	pthread_attr_getstacksize(&attr, &size);
	g->stackguard = (uintptr)&attr - size + 4096;
	pthread_attr_destroy(&attr);

	// Locate symbol for the system pthread_create function.
	handle = dlopen("libpthread.so", RTLD_LAZY);
	if(handle == NULL) {
		fprintf(stderr, "dlopen: failed to load libpthread: %s\n", dlerror());
		abort();
	}
	sys_pthread_create = dlsym(handle, "pthread_create");
	if(sys_pthread_create == NULL) {
		fprintf(stderr, "dlsym: failed to find pthread_create: %s\n", dlerror());
		abort();
	}
	dlclose(handle);

	tcb_fixup(1);
}

void (*initcgo)(G*) = xinitcgo;

void
libcgo_sys_thread_start(ThreadStart *ts)
{
	pthread_attr_t attr;
	sigset_t ign, oset;
	pthread_t p;
	size_t size;
	int err;

	sigfillset(&ign);
	sigprocmask(SIG_SETMASK, &ign, &oset);

	pthread_attr_init(&attr);
	pthread_attr_getstacksize(&attr, &size);
	ts->g->stackguard = size;
	err = sys_pthread_create(&p, &attr, threadentry, ts);

	sigprocmask(SIG_SETMASK, &oset, nil);

	if (err != 0) {
		fprintf(stderr, "runtime/cgo: pthread_create failed: %s\n", strerror(err));
		abort();
	}
}

static void*
threadentry(void *v)
{
	ThreadStart ts;

	tcb_fixup(0);

	ts = *(ThreadStart*)v;
	free(v);

	ts.g->stackbase = (uintptr)&ts;

	/*
	 * libcgo_sys_thread_start set stackguard to stack size;
	 * change to actual guard pointer.
	 */
	ts.g->stackguard = (uintptr)&ts - ts.g->stackguard + 4096;

	/*
	 * Set specific keys.  On OpenBSD/ELF, the thread local storage
	 * is just before %gs:0.  Our dynamic 8.out's reserve 8 bytes
	 * for the two words g and m at %gs:-8 and %gs:-4.
	 */
	asm volatile (
		"movl %0, %%gs:-8\n"	// MOVL g, -8(GS)
		"movl %1, %%gs:-4\n"	// MOVL m, -4(GS)
		:: "r"(ts.g), "r"(ts.m)
	);

	crosscall_386(ts.fn);
	return nil;
}
