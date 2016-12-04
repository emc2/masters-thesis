/* Copyright (c) Eric McCorkle 2007, 2008.  All rights reserved. */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include "definitions.h"
#include "os_thread.h"

#ifdef DARWIN
#include <sched.h>
#endif

static pthread_attr_t attrs;

internal unsigned int os_thread_request(cc_stat_t* restrict stat) {

  static int mib[2] = { CTL_HW, HW_NCPU };
  size_t intsize = sizeof(int);
  int ncpu;

  /* First get the number of CPUs */
  if(sysctl(mib, 2, &ncpu, &intsize, NULL, 0)) {

    perror("Error detecting CPU count:\n");
    exit(-1);

  }

  if(stat->cc_num_executors > ncpu || 0 == stat->cc_num_executors)
    stat->cc_num_executors = ncpu;

  /* Now set up the stack size */
  pthread_attr_init(&attrs);
  pthread_attr_setscope(&attrs, PTHREAD_SCOPE_SYSTEM);

  if(0 == stat->cc_executor_stack_size) {

    size_t stack_size;

    pthread_attr_getstacksize(&attrs, &stack_size);
    stat->cc_executor_stack_size = stack_size;

  }

  else
    pthread_attr_setstacksize(&attrs, stat->cc_executor_stack_size);

  /* There is no maximum thread count */
  stat->cc_max_threads = 0;
  stat->cc_num_threads = 0;

  return 0;

}


internal os_thread_t os_thread_create(void* (*start)(void* arg), void* arg) {

  pthread_t out;

  if(0 != pthread_create(&out, &attrs, start, arg)) {

    perror("Fatal error in runtime, unable to create OS thread:\n");
    fputs("Exiting\n", stderr);
    exit(-1);

  }

  return out;

}


internal os_thread_t os_thread_self(void) {

  return pthread_self();

}


internal void os_thread_yield(void) {

#ifdef DARWIN
  sched_yield();
#else
  pthread_yield();
#endif

}


internal void noreturn os_thread_exit(void* value) {

  pthread_exit(value);

}


internal void* os_thread_join(os_thread_t id) {

  void* out;

  pthread_join(id, &out);

  return out;

}


internal void os_thread_key_create(os_thread_key_t* key) {

  pthread_key_create(key, NULL);

}


internal void* os_thread_key_get(os_thread_key_t key) {

  return pthread_getspecific(key);

}


internal void os_thread_key_set(os_thread_key_t key, void* data) {

  pthread_setspecific(key, data);

}


internal void os_thread_key_destroy(os_thread_key_t key) {

  pthread_key_delete(key);

}


internal void os_thread_barrier_init(barrier_t* const restrict barrier,
				     const unsigned int max) {

  barrier->b_max = max;
  barrier->b_count = 0;
  pthread_mutex_init(&(barrier->b_mutex), NULL);
  pthread_cond_init(&(barrier->b_cond), NULL);

}


internal bool os_thread_barrier_enter(barrier_t* const restrict barrier) {

  bool out = false;

  pthread_mutex_lock(&(barrier->b_mutex));

  barrier->b_count++;

  if(barrier->b_max == barrier->b_count)
    out = true;

  else
    pthread_cond_wait(&(barrier->b_cond), &(barrier->b_mutex));

  pthread_mutex_unlock(&(barrier->b_mutex));

  return out;

}


internal void os_thread_barrier_release(barrier_t* const restrict barrier) {

  barrier->b_count = 0;
  pthread_cond_broadcast(&(barrier->b_cond));

}


internal void os_thread_barrier_destroy(barrier_t* const restrict barrier) {

  pthread_mutex_destroy(&(barrier->b_mutex));
  pthread_cond_destroy(&(barrier->b_cond));

}
