/*
 * A process that forks after Python started (HAProxy's master starting its
 * workers): the plugin's pthread_atfork handlers bracket the fork the way
 * os.fork() does, so each child can call into Python, even when another
 * thread held the GIL at the moment of the fork. os.fork() itself, from
 * Python code, is left to Python.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ugly.h"

static int n, failed ;

static void ok(int cond, const char *what){
	printf("%sok %d - %s\n", cond ? "" : "not ", ++n, what) ;
	fflush(stdout) ;
	failed |= ! cond ;
}

static ugly_ctx *ctx ;
static ugly_rt *rt ;
static ugly_handle sqrt_fn ;


static int py_sqrt(double x, double *r){
	ugly_val v, arg = ugly_double(x) ;
	if (ugly_call(ctx, sqrt_fn, &arg, 1, UGLY_DOUBLE, &v) != UGLY_OK){
		return 0 ;
	}
	*r = v.as.d ;
	return 1 ;
}


static int run(const char *code){
	ugly_val v ;
	ugly_status st = ugly_rt_eval(ctx, rt, code, strlen(code), UGLY_ANY, &v) ;
	if (st != UGLY_OK){
		const ugly_error *e = ugly_last_error(ctx) ;
		printf("# %s: %s\n", e ? ugly_error_type(e) : "?", e ? ugly_error_message(e) : "?") ;
		return 0 ;
	}
	ugly_val_release(ctx, &v) ;
	return 1 ;
}


/*
 * Forks; the child calls Python's math.sqrt(16) and reports through a pipe
 * whether it got 4. 1 if it did. The child ends with SIGKILL, not exit:
 * under valgrind an exit would leak-check a heap the child inherited (and
 * PyOS_AfterFork_Child drops the dead threads' locks), which says nothing.
 */
static int fork_and_call(void){
	int p[2] ;
	if (pipe(p) != 0){
		return 0 ;
	}
	pid_t pid = fork() ;
	if (pid == 0){
		close(p[0]) ;
		alarm(10) ;     /* a deadlock fails, rather than hangs, the test */
		double r = 0 ;
		char got = (py_sqrt(16, &r) && (r == 4.0)) ? '1' : '0' ;
		if (write(p[1], &got, 1) != 1){
			got = '0' ;
		}
		kill(getpid(), SIGKILL) ;
	}
	close(p[1]) ;
	char got = 0 ;
	ssize_t n = (pid > 0 ? read(p[0], &got, 1) : -1) ;
	close(p[0]) ;
	int status = 0 ;
	if (pid > 0){
		waitpid(pid, &status, 0) ;
	}
	if ((n != 1)||(got != '1')){
		printf("# child: %s\n", n == 1 ? "Python failed" : WIFSIGNALED(status) && (WTERMSIG(status) == SIGALRM)
			? "deadlocked (killed by its alarm)" : "no answer") ;
		return 0 ;
	}
	return 1 ;
}


int main(void){
	ugly_lang *lang ;
	ugly_handle math ;
	ugly_val v ;
	if ((ugly_ctx_new(&ctx) != UGLY_OK)||(ugly_lang_load(ctx, "python", &lang) != UGLY_OK)
		||(ugly_rt_new(ctx, lang, &rt) != UGLY_OK)||(ugly_rt_import(ctx, rt, "math", &math) != UGLY_OK)
		||(ugly_get_member(ctx, math, "sqrt", UGLY_HANDLE, &v) != UGLY_OK)){
		printf("Bail out! can't start Python\n") ;
		return 1 ;
	}
	sqrt_fn = v.as.h ;
	double r = 0 ;
	ok(py_sqrt(9, &r) && (r == 3.0), "Python works before the fork") ;

	ok(fork_and_call(), "a child forked after Python started calls into it") ;

	/* os.fork() from Python code: Python brackets it itself, and the plugin stays out of the way. */
	ok(run("import os, math, signal\n"
		"_r, _w = os.pipe()\n"
		"_pid = os.fork()\n"
		"if _pid == 0:\n"
		"    os.write(_w, b'1' if math.sqrt(9) == 3 else b'0')\n"
		"    os.kill(os.getpid(), signal.SIGKILL)\n"
		"os.close(_w)\n"
		"_got = os.read(_r, 1)\n"
		"os.close(_r)\n"
		"os.waitpid(_pid, 0)\n")
		&&(ugly_rt_eval(ctx, rt, "_got == b'1'", 12, UGLY_BOOL, &v) == UGLY_OK)&&v.as.b,
		"os.fork() from Python code still works (not bracketed twice)") ;

	/* Another thread runs Python, holding the GIL most of the time, while we fork. */
	ok(run("import threading\n"
		"_stop = False\n"
		"def _spin():\n"
		"    while not _stop:\n"
		"        sum(range(2000))\n"
		"_t = threading.Thread(target=_spin, daemon=True)\n"
		"_t.start()\n"), "a Python thread spins in the parent") ;
	int i, good = 0 ;
	for (i = 0 ; i < 20 ; i++){
		good += fork_and_call() ;
	}
	ok(good == 20, "20 forks while another thread runs Python: every child calls into Python") ;
	if (good != 20){
		printf("# %d of 20\n", good) ;
	}
	ok(run("_stop = True\n_t.join()\n"), "  ... and the parent's thread stops cleanly") ;

	ok(py_sqrt(25, &r) && (r == 5.0), "the parent still works after the forks") ;

	ugly_release(ctx, sqrt_fn) ;
	ugly_release(ctx, math) ;
	ugly_rt_delete(ctx, rt) ;
	ugly_lang_unload(ctx, lang) ;
	ugly_ctx_delete(ctx) ;
	printf("1..%d\n", n) ;
	return failed ;
}
