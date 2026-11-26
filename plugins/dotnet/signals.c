/*
 * Starting the CLR leaves the host's signals alone (ugly_plugin.h, init).
 * The CLR keeps its own handlers for the faults it turns into exceptions,
 * which call the host's for a fault outside managed code; its others
 * (SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGABRT) are put back.
 */
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ugly.h"

static int n, failed ;

static void ok(int cond, const char *what){
	printf("%sok %d - %s\n", cond ? "" : "not ", ++n, what) ;
	failed |= ! cond ;
}

/* The host's fault handler: exits with a status naming the signal (nothing unwinds out of the CLR's handler). */
static void host(int s){
	_exit(100 + s) ;
}

static void host_int(int s){
	(void)s ;
}


static ugly_status start(ugly_ctx **ctx, ugly_rt **rt, ugly_handle *math){
	ugly_lang *lang = NULL ;
	ugly_ctx_new(ctx) ;
	ugly_status st = ugly_lang_load(*ctx, "dotnet", &lang) ;
	if (st == UGLY_OK){
		st = ugly_rt_new(*ctx, lang, rt) ;
	}
	if (st == UGLY_OK){
		st = ugly_rt_import(*ctx, *rt, "System.Math", math) ;
	}
	return st ;
}


/* A child: the host's handler, then the CLR, then a fault in C. */
static int fault(const char *which){
	struct sigaction a ;
	memset(&a, 0, sizeof(a)) ;
	a.sa_handler = host ;
	sigaction(SIGSEGV, &a, NULL) ;
	sigaction(SIGFPE, &a, NULL) ;
	ugly_ctx *ctx ;
	ugly_rt *rt ;
	ugly_handle math ;
	if (start(&ctx, &rt, &math) != UGLY_OK){
		return 2 ;
	}
	if (strcmp(which, "segv") == 0){
		*(volatile int *)NULL = 1 ;
	}
	else {
		volatile int zero = 0 ;
		printf("%d\n", 1 / zero) ;
	}
	return 3 ;     /* no fault */
}


/* Runs this program as a child that faults; 1 if the host's handler for 'sig' got the fault. */
static int child(const char *self, const char *which, int sig){
	char *argv[] = { (char *)self, (char *)which, NULL } ;
	extern char **environ ;
	pid_t pid ;
	int status = 0 ;
	if ((posix_spawn(&pid, self, NULL, NULL, argv, environ) != 0)||(waitpid(pid, &status, 0) < 0)){
		return 0 ;
	}
	if (! (WIFEXITED(status)&&(WEXITSTATUS(status) == 100 + sig))){
		printf("# child %s: %s %d\n", which, WIFEXITED(status) ? "exit status" : "signal",
			WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status)) ;
		return 0 ;
	}
	return 1 ;
}


int main(int argc, char **argv){
	if (argc == 2){
		return fault(argv[1]) ;
	}
	static const int kept[] = { SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGABRT } ;
	struct sigaction a, before[sizeof(kept) / sizeof(kept[0])], after ;
	memset(&a, 0, sizeof(a)) ;
	a.sa_handler = host ;
	sigaction(SIGSEGV, &a, NULL) ;
	sigaction(SIGFPE, &a, NULL) ;
	a.sa_handler = host_int ;
	sigaction(SIGINT, &a, NULL) ;
	size_t i ;
	for (i = 0 ; i < sizeof(kept) / sizeof(kept[0]) ; i++){
		sigaction(kept[i], NULL, &before[i]) ;
	}

	ugly_ctx *ctx = NULL ;
	ugly_rt *rt = NULL ;
	ugly_handle math ;
	ugly_status st = start(&ctx, &rt, &math) ;
	ok(st == UGLY_OK, "load dotnet (starts the CLR), import System.Math") ;
	if (st != UGLY_OK){
		printf("Bail out! %s\n", ugly_error_message(ugly_last_error(ctx))) ;
		return 1 ;
	}

	for (i = 0 ; i < sizeof(kept) / sizeof(kept[0]) ; i++){
		char what[64] ;
		sigaction(kept[i], NULL, &after) ;
		snprintf(what, sizeof(what), "%s: the host's handler, unchanged", strsignal(kept[i])) ;
		ok(after.sa_handler == before[i].sa_handler, what) ;
	}

	/* A fault in C, outside managed code: the CLR's handler passes it on to the host's (in a child each). */
	ok(child(argv[0], "segv", SIGSEGV), "a SIGSEGV in C reaches the host's handler") ;
	ok(child(argv[0], "fpe", SIGFPE), "a SIGFPE in C reaches the host's handler") ;

	ugly_val v, x = ugly_int(-7) ;
	st = ugly_invoke(ctx, math, "Abs", &x, 1, UGLY_INT, &v) ;
	ok((st == UGLY_OK)&&(v.as.i == 7), "and C# still works: Math.Abs(-7) == 7") ;

	ugly_release(ctx, math) ;
	ugly_rt_delete(ctx, rt) ;
	ugly_ctx_delete(ctx) ;
	printf("1..%d\n", n) ;
	return failed ;
}
