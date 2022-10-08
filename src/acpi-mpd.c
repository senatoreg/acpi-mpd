#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <error.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <mpd/client.h>

#define EVENT_BUFFER_SIZE 128
#define BACKLIGHT_BUFFER_SIZE 16

typedef struct _mpd_info {
    char *mpd_host;
    struct mpd_connection* mpd_conn;
    int acpid_fd;
    int idle_factor;
    struct pollfd *pfds;
} mpd_info_t;

typedef enum _mpd_cmd {
    PLAY = 0,
    STOP,
    PREV,
    NEXT
} mpd_cmd_t;

int
acpi_open(const char* name) {
    int fd;
    int r;
    struct sockaddr_un addr;

    if (strnlen(name, sizeof(addr.sun_path)) > sizeof(addr.sun_path) - 1) {
        return -1;
    }
    
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
	return fd;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    sprintf(addr.sun_path, "%s", name);
    /* safer: */
    /*strncpy(addr.sun_path, name, sizeof(addr.sun_path) - 1);*/

    r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0) {
	close(fd);
	return r;
    }

    return fd;
}

int
setup_acpi(char* acpid_socketfile, mpd_info_t* mpd_info) {
    /* open the socket */
    mpd_info->acpid_fd = acpi_open(acpid_socketfile);
    if (mpd_info->acpid_fd < 0) {
	fprintf(stderr, "setup_acpi: can't open acpi file\n");	
        return EXIT_FAILURE;
    }

    mpd_info->pfds->fd = mpd_info->acpid_fd;
    mpd_info->pfds->events = POLLIN;

    return 0;
}

int
setup_mpd(mpd_info_t* mpd_info) {
    bool ret;
    mpd_info->mpd_conn = mpd_connection_new(mpd_info->mpd_host, 0, 0);

    // error handling
    if (mpd_info->mpd_conn == NULL) {
	fprintf(stderr, "setup_mpd: out of memory\n");
	return EXIT_FAILURE;
    }
    if (mpd_connection_get_error(mpd_info->mpd_conn) != MPD_ERROR_SUCCESS) {
	fprintf(stderr, "setup_mpd: got error %s connecting to %s\n",
		mpd_connection_get_error_message(mpd_info->mpd_conn),
		mpd_info->mpd_host);
	mpd_connection_free(mpd_info->mpd_conn);
	return EXIT_FAILURE;
    }

    ret = mpd_connection_set_keepalive(mpd_info->mpd_conn, true);
    if (!ret)
	fprintf(stderr, "setup_mpd: KeepAlive not enabled.\n");
    return ret ? EXIT_SUCCESS : EXIT_FAILURE;
}

int
close_acpi(mpd_info_t* mpd_info) {
    return close(mpd_info->acpid_fd);
}

int
close_mpd(mpd_info_t* mpd_info) {
    mpd_connection_free(mpd_info->mpd_conn);
    return EXIT_SUCCESS;
}

int
send_cmd(mpd_cmd_t cmd, mpd_info_t* mpd_info) {
    char buffer[BACKLIGHT_BUFFER_SIZE];
    struct mpd_status* cur_status;
    int err = 0, ret;
    int c = 3;

    do {
	switch(cmd) {
	case PLAY:
	    if ((cur_status = mpd_run_status(mpd_info->mpd_conn)) != NULL) {
		if (mpd_status_get_state(cur_status) == MPD_STATE_STOP)
		    err = mpd_run_play(mpd_info->mpd_conn);
		else
		    err = mpd_run_toggle_pause(mpd_info->mpd_conn);
	    }
	    break;
	case STOP:
	    err = mpd_run_stop(mpd_info->mpd_conn);
	    break;
	case PREV:
	    err = mpd_run_previous(mpd_info->mpd_conn);
	    break;
	case NEXT:
	    err = mpd_run_next(mpd_info->mpd_conn);
	    break;
	};
	if (err == 0) {
	    fprintf(stderr, "send_cmd: got error %d reconnecting\n", err);
	    ret = close_mpd(mpd_info);
	    ret = setup_mpd(mpd_info);
	}
	c--;
    } while (err == 0 && c > -1);
 exit:
    return err == 1;
}

int
acpi_event_handler(mpd_info_t* mpd_info){
    int err = 0;
    char event[EVENT_BUFFER_SIZE];
    struct pollfd* pfds = mpd_info->pfds;
    
    while (1) {
        /* read and handle an event */
	err = poll(pfds, 1, -1);

	if (pfds->revents && POLLIN) {
	    err = read(pfds->fd, event, EVENT_BUFFER_SIZE);
	    if (err > 0) { 
		if ((err=strncmp(event,"cd/play CDPLAY",14)) == 0) {
		    if ((err=send_cmd(PLAY, mpd_info)) < 0) break;
		} else if ((err=strncmp(event,"cd/stop CDSTOP",14)) == 0) {
		    if ((err=send_cmd(STOP, mpd_info)) < 0) break;
		} else if ((err=strncmp(event,"cd/prev CDPREV",14)) == 0) {
		    if ((err=send_cmd(PREV, mpd_info)) < 0) break;
		} else if ((err=strncmp(event,"cd/next CDNEXT",14)) == 0) {
		    if ((err=send_cmd(NEXT, mpd_info)) < 0) break;
		}
	    }
	}
    }

    if (err != 0)
	fprintf(stderr, "acpi_event_handler: got error %d\n", err);
    return err;
}

int
main(int argc, char** argv, char** envp) {
    char *acpid_socket = "/var/run/acpid.socket";
    char *run_dir = getenv("XDG_RUNTIME_DIR");
    int run_dir_len = strlen(run_dir);
    int err, c, fd;
    char event[EVENT_BUFFER_SIZE];
    mpd_info_t* mpd_info;
    struct sockaddr_un addr;

    mpd_info = malloc(sizeof(mpd_info_t));
    mpd_info->pfds = malloc(sizeof(struct pollfd));

    while ((c = getopt (argc, argv, "a:s:")) != -1)
	switch (c)
	    {
	    case 'a':
		mpd_info->mpd_host = optarg;
		break;
	    default:
		break;
	    }

        
    if ( (err=setup_acpi(acpid_socket, mpd_info)) != 0 )
        return err;

    if (!mpd_info->mpd_host) {
	int l = run_dir_len + 12;
	mpd_info->mpd_host = malloc(l);
	snprintf(mpd_info->mpd_host, l, "%s/%s", run_dir, "mpd/socket");
    }
    if ( (err=setup_mpd(mpd_info)) != 0 )
	return err;

    err = acpi_event_handler(mpd_info);

    close_acpi(mpd_info);
    close_mpd(mpd_info);

    free(mpd_info->pfds);
    free(mpd_info);

    return err;
}
