#include <errno.h>
#include <stdio.h>
#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>

#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/inotify.h>
#include <sys/syscall.h>




int restart_fl = 0;
int semid = 0;


int helper_func(int semid, int main_pid);


void exit_handler(int signum)
{	
	exit(EXIT_FAILURE);
}

void restart_handler(int signum)
{
	int main_pid = getpid();
	int res = -1;
	while(res < 0)
		res = fork();
	if (res == 0)
		helper_func(semid, main_pid);
}

ssize_t copy_file_range(int fd_in, loff_t *off_in, int fd_out,
				loff_t *off_out, size_t len, unsigned int flags)
{
	return syscall(__NR_copy_file_range, fd_in, off_in, fd_out, 
		off_out, len, flags);
}




/*this function used only because there are some same single operations I don't think it is useful*/
int semop_wrap(int semid, int op) // only for single operation
{
	struct sembuf sops;
	sops.sem_num = 0;
	sops.sem_op  = op;
	sops.sem_flg = 0;
	while (semop(semid, &sops, 1) != 0)
		if (errno != EINTR)
		{
			perror("semop:");
			return -1;
		}
}

int copy_file(const char* path_in, const char* path_out)
{
	int fd_in = open(path_in, O_RDONLY);
	if (fd_in < 0)
	{
		perror("open fd_in:");
		goto error_in;
	}

	struct stat statbuf;
	if (stat(path_in, &statbuf))
	{
		perror("stat:");
		goto error_in;
	}

	int fd_out = open(path_out, O_WRONLY | O_TRUNC | O_CREAT, 0666);
	if (fd_out < 0)
	{
		perror("open fd_out:");
		goto error_all;
	}

	if (copy_file_range(fd_in, NULL, fd_out, NULL, statbuf.st_size, 0) < 0)
	{
		perror("copy_file_range:");
		goto error_all;
	}

	close(fd_in);
	close(fd_out);
	return 0;

error_all:
	close(fd_out);
error_in:
	close(fd_in);
	return -1;
}

int helper_func(int semid, int main_pid)
{
	struct sigaction act;

	act.sa_handler = SIG_DFL;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	sigaction(SIGCHLD, &act, NULL);

	act.sa_handler = exit_handler;
	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, SIGUSR1);
	act.sa_flags = 0;
	
	sigaction(SIGUSR1, &act, NULL);

	static int fl = 0;

	if (prctl(PR_SET_PDEATHSIG, SIGUSR1, 0, 0, 0) == -1)
	{
		return 0;
	}

	if (getppid() != main_pid)
	{
		return 0;
	}

	int fd = inotify_init();
	if (inotify_add_watch(fd, "DB", IN_MODIFY) < 0)
	{
		perror("inotify_add_watch:");
		return -1;
	}

	struct inotify_event event;
	int res = 0;
	while(1)
	{
		if (fl)
		{
			res = read(fd, &event, sizeof(struct inotify_event));
			if (res < 0 && errno == EINTR)
				continue;
			else if (res < 0)
			{
				perror("read:");
				return -1;
			}


			if (!(event.mask & IN_MODIFY))
				continue;
		}
		fl = 1;

		if (semop_wrap(semid, 0))
			return -1;

		if (copy_file("DB", "CACHE") < 0)
			return -1;

		
		if (semop_wrap(semid, 1))
			return -1;
	}
}

int main_func(int semid)
{
	struct sembuf sops[2];

	int res = 0;
	while(1)
	{
		sops[0].sem_num = 0;
		sops[0].sem_op  = -1;
		sops[0].sem_flg = 0;

		sops[1].sem_num = 0;
		sops[1].sem_op  = 1;
		sops[1].sem_flg = 0;

		while (semop(semid, sops, 2) != 0)
		if (errno != EINTR)
		{
			perror("semop:");
			return -1;
		}

		struct stat statbuf;
		res = stat("CACHE", &statbuf);

		int fd_out = open("OUT", O_CREAT | O_RDWR | O_TRUNC, 0666);
		if (fd_out < 0)
		{
			perror("open fd_out main_func:");
			return -1;
		}
		int fd_cache = open("CACHE", O_RDONLY | O_CREAT, 0666);
		if (fd_cache < 0)
		{
			perror("open fd_cache main_func:");
			return -1;	
		}

		ssize_t cp_res = copy_file_range(fd_cache, NULL, fd_out, NULL, statbuf.st_size, 0);
		if (cp_res < 0)
		{
			perror("copy");
			return -1;
		}

		close(fd_out);
		close(fd_cache);

		semop_wrap(semid, -1);
	}
}




int main(int argc, char const *argv[])
{
	int main_pid = getpid();

	struct sigaction act;
	
	act.sa_handler = restart_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_NOCLDSTOP;
	sigaction(SIGCHLD, &act, NULL);

	semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
	if (semid < 0)
	{
		perror("semget:");
		return 0;
	}



	int res = -1;
	while (res < 0)
	{
		res = fork();
	}

	if (res == 0)
	{
		res = helper_func(semid, main_pid);
	}
	else
	{
		res = main_func(semid);
	}
	return 0;
}