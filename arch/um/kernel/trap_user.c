/* 
 * Copyright (C) 2000, 2001, 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <asm/page.h>
#include <asm/unistd.h>
#include <asm/ptrace.h>
#include "init.h"
#include "sysdep/ptrace.h"
#include "sigcontext.h"
#include "sysdep/sigcontext.h"
#include "irq_user.h"
#include "frame_user.h"
#include "signal_user.h"
#include "time_user.h"
#include "task.h"
#include "mode.h"
#include "choose-mode.h"
#include "kern_util.h"
#include "user_util.h"
#include "os.h"

void kill_child_dead(int pid)
{
	kill(pid, SIGKILL);
	kill(pid, SIGCONT);
	while(waitpid(pid, NULL, 0) > 0) kill(pid, SIGCONT);
}

/* Unlocked - don't care if this is a bit off */
int nsegfaults = 0;

struct {
	unsigned long address;
	int is_write;
	int pid;
	unsigned long sp;
	int is_user;
} segfault_record[1024];

void segv_handler(int sig, struct uml_pt_regs *regs)
{
	int index, max;

	if(regs->is_user && !UPT_SEGV_IS_FIXABLE(regs)){
		bad_segv(UPT_FAULT_ADDR(regs), UPT_IP(regs), 
			 UPT_FAULT_WRITE(regs));
		return;
	}
	max = sizeof(segfault_record)/sizeof(segfault_record[0]);
	index = next_trap_index(max);

	nsegfaults++;
	segfault_record[index].address = UPT_FAULT_ADDR(regs);
	segfault_record[index].pid = os_getpid();
	segfault_record[index].is_write = UPT_FAULT_WRITE(regs);
	segfault_record[index].sp = UPT_SP(regs);
	segfault_record[index].is_user = regs->is_user;
	segv(UPT_FAULT_ADDR(regs), UPT_IP(regs), UPT_FAULT_WRITE(regs),
	     regs->is_user, regs);
}

void usr2_handler(int sig, struct uml_pt_regs *regs)
{
	CHOOSE_MODE(syscall_handler_tt(sig, regs), (void) 0);
}
 
struct signal_info sig_info[] = {
	[ SIGTRAP ] { handler :		relay_signal,
		      is_irq :		0 },
	[ SIGFPE ] { handler :		relay_signal,
		     is_irq :		0 },
	[ SIGILL ] { handler :		relay_signal,
		     is_irq :		0 },
	[ SIGBUS ] { handler :		bus_handler,
		     is_irq :		0 },
	[ SIGSEGV] { handler :		segv_handler,
		     is_irq :		0 },
	[ SIGIO ] { handler :		sigio_handler,
		    is_irq :		1 },
	[ SIGVTALRM ] { handler :	timer_handler,
			is_irq :	1 },
        [ SIGALRM ] { handler :         timer_handler,
                      is_irq :          1 },
	[ SIGUSR2 ] { handler :		usr2_handler,
		      is_irq :		0 },
};

void sig_handler_common(int sig, struct sigcontext *sc)
{
	struct uml_pt_regs save_regs, *r;
	struct signal_info *info;
	int save_errno = errno, is_user;

	unprotect_kernel_mem();

	r = (struct uml_pt_regs *) TASK_REGS(get_current());
	save_regs = *r;
	is_user = user_context(SC_SP(sc));
	r->is_user = is_user;
	r->mode.tt = sc;
	if(sig != SIGUSR2) r->syscall = -1;

	change_sig(SIGUSR1, 1);
	info = &sig_info[sig];
	if(!info->is_irq) unblock_signals();

	(*info->handler)(sig, r);

	if(is_user){
		interrupt_end();
		block_signals();
		change_sig(SIGUSR1, 0);
		set_user_mode(NULL);
	}
	*r = save_regs;
	errno = save_errno;
	if(is_user) protect_kernel_mem();
}

void sig_handler(int sig, struct sigcontext sc)
{
	sig_handler_common(sig, &sc);
}

extern int timer_irq_inited, missed_ticks[];

void alarm_handler(int sig, struct sigcontext sc)
{
	int user;

	if(!timer_irq_inited) return;
	missed_ticks[cpu()]++;
	user = user_context(SC_SP(&sc));

	if(sig == SIGALRM)
		switch_timers(0);

	sig_handler_common(sig, &sc);

	if(sig == SIGALRM)
		switch_timers(1);
}

void do_longjmp(void *p, int val)
{
    jmp_buf *jbuf = (jmp_buf *) p;

    longjmp(*jbuf, val);
}

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
