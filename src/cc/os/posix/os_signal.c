/* Copyright (c) Eric McCorkle 2007, 2008.  All rights reserved. */

#include <pthread.h>
#include <signal.h>
#include "definitions.h"
#include "os_thread.h"
#include "os_signal.h"


const unsigned int os_signal_check_mbox = SIGUSR1;


internal void os_sigset_fill(os_sigset_t* restrict set) {

  sigfillset(set);

}


internal void os_sigset_empty(os_sigset_t* restrict set) {

  sigemptyset(set);

}


internal void os_sigset_set(os_sigset_t* restrict set, unsigned int sig) {

  sigaddset(set, sig);

}


internal void os_sigset_clear(os_sigset_t* restrict set, unsigned int sig) {

  sigdelset(set, sig);

}


internal void os_thread_signal_send(os_thread_t thread, unsigned int sig) {

  pthread_kill(thread, sig);

}


internal void os_thread_sigmask_block(os_sigset_t* restrict mask,
				      os_sigset_t* restrict old) {

  pthread_sigmask(SIG_BLOCK, mask, old);

}


internal void os_thread_sigmask_unblock(os_sigset_t* restrict mask,
					os_sigset_t* restrict old) {

  pthread_sigmask(SIG_UNBLOCK, mask, old);

}


internal void os_thread_sigmask_set(os_sigset_t* restrict mask,
				    os_sigset_t* restrict old) {

  pthread_sigmask(SIG_SETMASK, mask, old);

}


internal void os_proc_sigmask_block(os_sigset_t* restrict mask,
				    os_sigset_t* restrict old) {

  sigprocmask(SIG_BLOCK, mask, old);

}


internal void os_proc_sigmask_unblock(os_sigset_t* restrict mask,
				      os_sigset_t* restrict old) {

  sigprocmask(SIG_UNBLOCK, mask, old);

}


internal void os_proc_sigmask_set(os_sigset_t* restrict mask,
				  os_sigset_t* restrict old) {

  sigprocmask(SIG_SETMASK, mask, old);

}


internal void os_sig_handler(void (*handler)(int sig), int sig) {

  struct sigaction act;

  act.sa_handler = handler;
  act.sa_flags = 0;
  sigfillset(&act.sa_mask);
  sigaction(sig, &act, NULL);

}


internal void os_thread_sigset_set_mandatory(os_sigset_t* restrict mask) {

  sigaddset(mask, SIGABRT);
  sigaddset(mask, SIGINT);
  sigaddset(mask, SIGILL);
  sigaddset(mask, SIGHUP);
  sigaddset(mask, SIGEMT);
  sigaddset(mask, SIGQUIT);
  sigaddset(mask, SIGFPE);
  sigaddset(mask, SIGKILL);
  sigaddset(mask, SIGBUS);
  sigaddset(mask, SIGSEGV);
  sigaddset(mask, SIGSYS);
  sigaddset(mask, SIGPIPE);

}


internal void os_thread_sigset_clear_mandatory(os_sigset_t* restrict mask) {

  sigdelset(mask, SIGABRT);
  sigdelset(mask, SIGINT);
  sigdelset(mask, SIGILL);
  sigdelset(mask, SIGHUP);
  sigdelset(mask, SIGEMT);
  sigdelset(mask, SIGQUIT);
  sigdelset(mask, SIGFPE);
  sigdelset(mask, SIGKILL);
  sigdelset(mask, SIGBUS);
  sigdelset(mask, SIGSEGV);
  sigdelset(mask, SIGSYS);
  sigdelset(mask, SIGPIPE);

}


internal void os_sigsuspend(os_sigset_t* restrict mask) {

  sigsuspend(mask);

}
