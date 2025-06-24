/*****************************************************************************\
 * prog1.53.prog.c - Simple signal catching test program
 *****************************************************************************
 * Copyright (C) 2002-2007 The Regents of the University of California.
 * Copyright (C) 2008-2009 Lawrence Livermore National Security.
 * Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 * Written by Morris Jette <jette1@llnl.gov>
\*****************************************************************************/
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int sig_cnt;

void sig_handler(int sig)
{
	switch (sig) {
		case SIGINT:
			printf("Received SIGINT\n");
			sig_cnt++;
			break;
		default:
			printf("Received unexpected signal %d\n", sig);
	}
}

int
main(int argc, char **argv)
{
	struct sigaction act;
	time_t begin_time = time(NULL);

	setbuf(stdout, NULL);
	printf("Begin test\n");

	act.sa_handler = sig_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	if (sigaction(SIGINT, &act, NULL) < 0) {
		perror("sigaction");
		exit(2);
	}

	while (!sig_cnt) {
		sleep(1);
	}
	printf("Job ran for %d secs\n", (int) (time(NULL) - begin_time));
	exit(0);
}
