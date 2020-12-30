#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <termios.h>
#include <fcntl.h>
#include <cstdlib>
#include <cerrno>
#include <sys/ioctl.h>
#include <poll.h>
#include <ctime>
#include <csignal>

#include "EnumState.h"

#define DEV_FILENAME_LEN 64

int serial_fd = -1;
FILE *log_f = nullptr;
/*
 * state SHOULD NOT CHANGE when is_at_cmd_performing == true
 */
STATE state = UNKNOWN;    // this is maintained by program
bool is_at_cmd_performing = false; // PRIVATE unset when operate device into new mode, set when OK or ERROR received
bool app_force_poll_continue = false; // above one responses for AT cmds, and this one responses for non-ATs
/*
 * default to -1, but will be set when any AT cmd(and possibly invalid input) is going to be performed
 * will be updated once poll returned as following:
 * if poll_timeout_remain != -1, poll_timeout_remain = poll_timeout_remain - time_elapsed
 * if poll_timeout_remain < 0, poll_timeout_remain = 0
 */
int poll_timeout_remain = -1;   // record how long should the AT command keep hanging
bool is_manual_mode = false;    // DO NOT CHANGE
bool perform_soft_reset = false;
// System wide buffers for IO. Don't forget to check length.
char w_buf[64] = {0};
char r_buf[64] = {0};
char t_buf[32] = {0};

void int_handler(int);

int main(int argc, char *argv[]);

void print_help_message(char *exec_name);

int open_serial(const char *file);

int prepare_log(const char *file);

int loop();

/*
 * wrapped to add CRLF, so please do not pass in any '\n' but '\0'
 * One AT command per call
 */
int serial_write(const char *buf);

/*
 * Once there is something reported by modem, this function is called and
 * with formatted messages, once per message.
 */
int serial_read(const char *buf);

/*
 * stdin input here.
 * Returns other than 0 will terminate the main loop.
 */
int stdin_read(const char *buf);

void get_time_str();    // operates t_buf[]

/*
 * THIS FUNCTION MODIFIES ORIGINAL POINTER!
 * Read from buf and detect '\n', return the length of the line
 * and replace it with '\0'. buf is set to the first byte of string.
 * next is set to next reading addr, otherwise nullptr if no more reading.
 * When '\0' is detected, 0 is returned.
 * When '\n' appears after nothing, simply ignored and jump to next.
 * Input buffer MUST contains '\0' as ending.
 *
 * Grant that valid data whose first byte is returned and has '\0' ending.
 * Return -1 means error.
 */
int getline(char *&buf, char *&next);

// implementation

void print_help_message(char *exec_name) {
	char help_msg[] = "Help page\n\
%s -d /dev/ttyXXXX [-m] [-l file]\n\
\t -l log file, default to argv[0]+\".log\"\n\
\t -m manual: disable automatic init procedure\n\
\t -r soft-reset device";
	printf(help_msg, exec_name);
}

void get_time_str() {
	time_t t = time(nullptr);
	struct tm *tm = localtime(&t);
	strftime(t_buf, sizeof(t_buf), "%c", tm);
}

int getline(char *&buf, char *&next) {
	char *p_start = buf;  // log the first byte
	char *p_tmp = p_start;    // var which will increase in the loop
	while (*p_tmp != '\0') {
		if (p_tmp - p_start > 256) { // too many characters in one message
			fprintf(stderr, "Warning: string too long\n");
			next = nullptr;
			return -1;
		}

		if (*p_tmp == '\n') {    // meet with NL
			if (p_tmp - p_start == 0) {  // an empty line
				++p_tmp;
				++p_start;
				continue;   // increase both start and tmp, to do a new scan
			} else {
				buf = p_start;
				next = p_tmp + 1;
				*p_tmp = '\0';
				return static_cast<int>(p_tmp - p_start);
			}
		}

		// inc at last
		++p_tmp;
	}

	/*
	 * p_start == buf, buf == p_tmp, p_start == p_tmp : empty input
	 * p_start == buf, buf != p_tmp, p_start != p_tmp : single command without NL
	 * p_start != buf, buf != p_tmp, p_start == p_tmp : meet with bare NL(s) and ends
	 * p_start != buf, buf == p_tmp, p_start != p_tmp : Invalid
	 * p_start != buf, buf != p_tmp, p_start != p_tmp : meet with bare NL(s), but command is not followed by NL
	 */
	if (buf != p_tmp and p_start != p_tmp) {
		fprintf(stderr, "Warning: no NL detected but command ends\n");
		buf = p_start;
		next = nullptr;
		return static_cast<int>(p_tmp - p_start);
	} else {
		next = nullptr;
		return 0;   // p_start == p_tmp
	}
}

int main(int argc, char *argv[]) {
	char dev_name[DEV_FILENAME_LEN] = {-1};
	char logfile_name[DEV_FILENAME_LEN] = {};
	// default log file to argv[0]+".log"
	sprintf(logfile_name, "%s.log", argv[0]);

	// handle SIGINT
	struct sigaction act = {};
	act.sa_handler = int_handler;
	sigaction(SIGINT, &act, nullptr);

	// parse cmd options
	int option;
	while ((option = getopt(argc, argv, "d:hl:mr")) != -1) {
		switch (option) {
			case 'd':
				if (strlen(optarg) > DEV_FILENAME_LEN) {
					fprintf(stderr, "Error: Filename too long\n");
					return -2;
				}
				strcpy(dev_name, optarg);
				break;

			case 'h':
				print_help_message(argv[0]);
				break;

			case 'l':
				if (strlen(optarg) > DEV_FILENAME_LEN) {
					fprintf(stderr, "Error: Filename too long\n");
					return -2;
				}
				strcpy(logfile_name, optarg);
				break;

			case 'm':
				is_manual_mode = true;
				break;

			case 'r':
				perform_soft_reset = true;
				break;

			case '?':
				return -1;

			default:
				break;
		}
	}

	// check necessary args
	if (dev_name[0] == -1) {
		fprintf(stderr, "Error: UART device name must be provided!\n");
		return -3;
	}

	// prepare log method
	int ret = prepare_log(logfile_name);
	if (ret != 0) {
		if (log_f != nullptr) {
			fclose(log_f);
			log_f = nullptr;
		}
		return ret;
	}

	// open and set serial
	ret = open_serial(dev_name);
	if (ret != 0) {
		if (serial_fd != -1) {
			close(serial_fd);
		}
		return ret;
	}

	// main loop
	ret = loop();
	if (ret != 0) {
		if (serial_fd != -1) {
			close(serial_fd);
		}
		return ret;
	}

	// close and exit
	close(serial_fd);
	return 0;
}

// make sure logs are flushed to disk
void int_handler(int) {
	fprintf(stderr, "Debug: SIGINT exiting\n");
	if (log_f != nullptr) {
		fflush(log_f);
		fclose(log_f);
		log_f = nullptr;
	}
	exit(1);
}

int loop() {
	if (perform_soft_reset) {    // first deal with this case
		sprintf(w_buf, "AT+ZSOFTRESET");
		serial_write(w_buf);
		return 0;
	}

	int ret = 0;
	bool should_exit = false;
	/*
	 * Before blocked into poll, we first check if there is something to do.
	 * It's easy to understand that automatic procedures must be started at the
	 * very beginning, and under a FSM design it is the best place to trigger
	 * the next stage.
	 */
	while (!should_exit) {
		// do something
		if (!is_at_cmd_performing and !app_force_poll_continue) {    // no AT cmd is performing, safe to do other things
			// set poll timeout for the first time
			poll_timeout_remain = get_state_poll_timeout_ms(state); // init timeout

			switch (state) {
				// Do what state tells you
				case UNKNOWN:
					if (!is_manual_mode) {
						state = TEST_AT;
						continue;
					}
					break;

				case TEST_AT:
					sprintf(w_buf, "AT");
					serial_write(w_buf);
					break;

				case SETUP_PDP:
					sprintf(w_buf, "AT+CGDCONT=1,\"IPV4V6\",\"ctnet\"");
					serial_write(w_buf);
					break;

				case SETUP_MS_MODE:
					sprintf(w_buf, "AT+CFUN=1");
					serial_write(w_buf);
					break;

				case PENDING_SETUP_RNDIS:
					app_force_poll_continue = true;
					break;

				case SETUP_RNDIS:
					sprintf(w_buf, "AT+ZGACT=1,1");
					serial_write(w_buf);
					break;

				case CONNECTED:
					break;

				case __STATE_END:
					// Invalid situation
					exit(0xff);
			}
		}

		// then poll
		struct pollfd p_fd[2];
		p_fd[0].events = POLLIN;    // input data available
		p_fd[0].fd = serial_fd;            // serial fd
		p_fd[1].events = POLLIN;    // input data available
		p_fd[1].fd = 0;             // stdin

		int rdy;
		if (poll_timeout_remain > 0) {
			// mark when poll is started
			struct timespec spec{};
			clock_gettime(CLOCK_MONOTONIC, &spec);
			int64_t timestamp_before_poll_ms =
					spec.tv_sec * 1000 + static_cast<int64_t>(spec.tv_nsec / 1e6);   // too ugly
			// poll
			rdy = poll(p_fd, 2, poll_timeout_remain);
			// update poll_timeout_remain
			clock_gettime(CLOCK_MONOTONIC, &spec);
			int64_t timestamp_after_poll_ms =
					spec.tv_sec * 1000 + static_cast<int64_t>(spec.tv_nsec / 1e6);   // too ugly
			poll_timeout_remain =
					poll_timeout_remain - static_cast<int>(timestamp_after_poll_ms - timestamp_before_poll_ms);
			if (poll_timeout_remain < 0) {
				poll_timeout_remain = 0;
			}
		} else {
			rdy = poll(p_fd, 2, poll_timeout_remain);
		}

		if (rdy == -1) {
			fprintf(stderr, "Error: poll %d\n", errno);
		} else if (rdy == 0) {   // timeout
			switch (state) {
				// action of state is already performed, but you still get timeout here
				case TEST_AT:
					fprintf(stderr, "Error: device not responses to AT cmd\n");
					should_exit = true;
					ret = -13;
					break;

				case PENDING_SETUP_RNDIS:
					fprintf(stderr, "Error: waiting for network timeout\n");
					should_exit = true;
					ret = -13;
					break;

				default:
					fprintf(stderr, "Error: poll timeout! mode=%s\n", get_state_name(state));
					should_exit = true;
					ret = -14;
					break;
			}
		} else {
			// something used inside poll events
			int read_size;

			// check stdin
			if (p_fd[1].revents & POLLERR) { // stdin error
				fprintf(stderr, "Error: poll() on stdin returned %d\n", p_fd[0].revents);
				should_exit = true;
				ret = -11;
			} else if (p_fd[1].revents & p_fd[1].events) {  // stdin
				read_size = read(0, r_buf, sizeof(r_buf) - 1);
				if (read_size < 0) {
					fprintf(stderr, "Error: read stdin %d\n", errno);
					should_exit = true;
				} else {
					r_buf[read_size] = 0x0;

					char *p_tmp = r_buf;
					char *p_next = r_buf;
					while (p_next != nullptr) {
						ssize_t cmd_len = getline(p_tmp, p_next);
						if (cmd_len > 0) {
							if (stdin_read(r_buf) != 0) {
								should_exit = true;
							}
						}
						p_tmp = p_next;
					}
				}
			} else if (p_fd[1].revents == 0) {
				// nothing happened
			} else {
				fprintf(stderr, "Error: Unexpected condition in stdin poll(), revents=%d\n", p_fd[1].revents);
			}

			// check serial
			if (p_fd[0].revents & POLLERR) {   // check serial error at first!
				fprintf(stderr, "Error: poll() on serial_fd returned %d\n", p_fd[0].revents);
				should_exit = true;
				ret = -10;
			} else if (p_fd[0].revents & p_fd[0].events) { // serial normal read
				read_size = read(serial_fd, r_buf, sizeof(r_buf) - 1);
				if (read_size < 0) {
					fprintf(stderr, "Error: read serial_fd %d\n", errno);
				} else {
					r_buf[read_size] = 0x0;
					char *p_tmp = r_buf;
					char *p_next = r_buf;
					while (p_next != nullptr) {
						ssize_t cmd_len = getline(p_tmp, p_next);
						if (cmd_len > 0) {
							get_time_str();
							fprintf(log_f, "%s\t%s\n", t_buf, p_tmp);
							serial_read(p_tmp);
						}
						p_tmp = p_next;
					}
				}
			} else if (p_fd[0].revents == 0) {
				// nothing
			} else {
				fprintf(stderr, "Error: Unknown error in serial_fd poll() %d\n", p_fd[0].revents);
			}
		}
	}

	return ret;
}

int prepare_log(const char *file) {
	log_f = fopen(file, "w");
	if (log_f == nullptr) {
		fprintf(stderr, "Error: prepare_log() failed %d!\n", errno);
		return errno;
	}
	get_time_str();
	fprintf(log_f, ">>Log started at %s\n", t_buf);
	return 0;
}

int open_serial(const char *file) {
	// open fd
	// rw, non-blocking, sync
	serial_fd = open(file, O_RDWR | O_NOCTTY | O_NONBLOCK | O_SYNC);
	if (serial_fd == -1) {
		if (errno == ENOENT) {
			fprintf(stderr, "Error: Device not exists!\n");
			return errno;
		}
		fprintf(stderr, "Error: open() failed %d!\n", errno);
		return errno;
	}
	if (isatty(serial_fd) == 0) {
		fprintf(stderr, "Error: Device %s not a UART device!\n", file);
		return -4;
	}
	if (ioctl(serial_fd, TIOCEXCL, NULL) < 0) {
		fprintf(stderr, "Warning: Failed to gain exclusive access!\n");
	}

	// configure serial
	struct termios tio = {};

	if (tcgetattr(serial_fd, &tio) < 0) {
		fprintf(stderr, "Error: tcgetattr %d\n", errno);
		return -5;
	}

	cfmakeraw(&tio);
	tio.c_cflag |= (CLOCAL | CREAD);
	tio.c_iflag |= IGNCR;   // remove CR, so we can simply deal with LF

	if (cfsetispeed(&tio, B115200) < 0 || cfsetospeed(&tio, B115200) < 0) {
		fprintf(stderr, "Error: cfsetispeed cfsetospeed %d\n", errno);
		return -6;
	}

	if (tcsetattr(serial_fd, TCSAFLUSH, &tio) < 0) {
		fprintf(stderr, "Error: tcsetattr %d\n", errno);
		return -5;
	}

	return 0;
}

int serial_write(const char *buf) {
	get_time_str();
	fprintf(log_f, "%s\t%s\n", t_buf, buf);
	int ret = static_cast<int>(dprintf(serial_fd, "%s\r\n", buf) - (strlen(buf) + 2));
	if (ret == 0) {
		is_at_cmd_performing = true;    // write anything will cause modem responding
	}
	return ret;
}

int serial_read(const char *buf) {
	if (is_manual_mode) {
		printf("%s\n", buf);
		return 0;
	}
	//printf("%s\n",buf);
	if (strstr(buf, "OK") == buf) {
		is_at_cmd_performing = false;
		switch (state) {
			case TEST_AT:
				state = SETUP_PDP;
				fprintf(stderr, "Debug: mode change from TEST_AT to SETUP_PDP\n");
				break;

			case SETUP_PDP:
				state = SETUP_MS_MODE;
				fprintf(stderr, "Debug: mode change from SETUP_PDP to SETUP_MS_MODE\n");
				break;

			case SETUP_MS_MODE:
				// do nothing, as this mode switch happend only if valid network
				state = PENDING_SETUP_RNDIS;
				fprintf(stderr, "Debug: mode change from SETUP_MS_MODE to PENDING_SETUP_RNDIS\n");
				break;

			case SETUP_RNDIS:
				state = CONNECTED;
				fprintf(stderr, "Debug: mode change from SETUP_RNDIS to CONNECTED\n");
				break;

			default:
				break;
		}
	} else if (strstr(buf, "+CME ERROR:") == buf) {
		is_at_cmd_performing = false;
		fprintf(stderr, "Error: %s\n", buf);
		fprintf(stderr, "Debug: mode change from %s to ERROR\n", get_state_name(state));
		state = ERROR;
		exit(-16);
	} else if (strstr(buf, "+CGEV") == buf) {
		switch (state) {
			case PENDING_SETUP_RNDIS:
				app_force_poll_continue = false;
				if (strstr(buf, "+CGEV: ME PDN ACT") == buf) {
					state = SETUP_RNDIS;
					fprintf(stderr, "Debug: mode change from PENDING_SETUP_RNDIS to SETUP_RNDIS\n");
				}
				break;

			default:
				break;

		}
	}
	return 0;
}

int stdin_read(const char *buf) {
	int ret;
	if (strcmp(buf, "EXIT") == 0) {
		return 2;
	}

	if (is_manual_mode) {
		serial_write(buf);
	} else {
		// automatic cmds
	}
	return 0;
}