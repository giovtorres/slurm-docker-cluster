#include <signal.h>
#include <stdio.h>
#include <unistd.h>

void ouch(int sig)
{
	static int logged = 0;

	if (logged == 0) {
		logged = 1;
		printf("OUCH! - I got signal %d\n", sig);
		fflush(stdout);
	}
}

int main()
{
	struct sigaction act = {
		.sa_handler = ouch,
		.sa_flags = SA_NODEFER,
	};
	sigemptyset(&act.sa_mask);
	sigaction(SIGINT, &act, 0);

	printf("Hello World!\n");
	fflush(stdout);
	while (1) {
		printf("Sleeping\n");
		fflush(stdout);
		sleep(1);
	}
}
