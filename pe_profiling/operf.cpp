/**
 * @file operf.cpp
 * Front-end (containing main) for handling a user request to run a profile
 * using the new Linux Performance Events Subsystem.
 *
 * @remark Copyright 2011 OProfile authors
 * @remark Read the file COPYING
 *
 * Created on: Dec 7, 2011
 * @author Maynard Johnson
 * (C) Copyright IBM Corp. 2011
 *
 * Modified by Maynard Johnson <maynardj@us.ibm.com>
 * (C) Copyright IBM Corporation 2012
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <dirent.h>
#include <exception>
#include <pwd.h>
#include <errno.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ftw.h>
#include <iostream>
#include "operf_utils.h"
#include "popt_options.h"
#include "op_libiberty.h"
#include "string_manip.h"
#include "cverb.h"
#include "operf_counter.h"
#include "op_cpu_type.h"
#include "op_cpufreq.h"
#include "op_events.h"
#include "op_string.h"
#include "operf_kernel.h"
#include "child_reader.h"

using namespace std;

typedef enum END_CODE {
	ALL_OK = 0,
	APP_ABNORMAL_END =  1,
	PERF_RECORD_ERROR = 2,
	PERF_READ_ERROR   = 4,
	PERF_BOTH_ERROR   = 8
} end_code_t;

// Globals
char * app_name = NULL;
uint64_t kernel_start, kernel_end;
operf_read operfRead;
op_cpu cpu_type;
double cpu_speed;
char op_samples_current_dir[PATH_MAX];
uint op_nr_counters;
verbose vmisc("misc");
static void convert_sample_data(void);
static int sample_data_pipe[2];


#define CALLGRAPH_MIN_COUNT_SCALE 15

static char full_pathname[PATH_MAX];
static char * app_name_SAVE = NULL;
static char * app_args = NULL;
static pid_t app_PID = -1;
static 	pid_t jitconv_pid = -1;
static bool app_started;
static pid_t operf_pid;
static pid_t convert_pid;
static string samples_dir;
static bool startApp;
static char start_time_str[32];
static vector<operf_event_t> events;
static bool jit_conversion_running;
static uid_t my_uid;


namespace operf_options {
bool system_wide;
bool append;
int pid;
bool callgraph;
int mmap_pages_mult;
string session_dir;
string vmlinux;
bool separate_cpu;
bool separate_thread;
vector<string> evts;
}

namespace {
vector<string> verbose_string;

popt::option options_array[] = {
	popt::option(verbose_string, "verbose", 'V',
	             "verbose output", "debug,perf_events,misc,sfile,arcs,all"),
	popt::option(operf_options::session_dir, "session-dir", 'd',
	             "session path to hold sample data", "path"),
	popt::option(operf_options::vmlinux, "vmlinux", 'k',
	             "pathname for vmlinux file to use for symbol resolution and debuginfo", "path"),
	popt::option(operf_options::callgraph, "callgraph", 'g',
	             "enable callgraph recording"),
	popt::option(operf_options::system_wide, "system-wide", 's',
	             "profile entire system"),
	popt::option(operf_options::append, "append", 'a',
	             "add new profile data to old profile data"),
	popt::option(operf_options::pid, "pid", 'p',
	             "process ID to profile", "PID"),
	popt::option(operf_options::mmap_pages_mult, "kernel-buffersize-multiplier", 'b',
	             "factor by which kernel buffer size should be increased", "buffersize"),
	popt::option(operf_options::evts, "events", 'e',
	             "comma-separated list of event specifications for profiling. Event spec form is:\n"
	             "name:count[:unitmask[:kernel[:user]]]",
	             "events"),
	popt::option(operf_options::separate_cpu, "separate-cpu", 'c',
	             "Categorize samples by cpu"),
	popt::option(operf_options::separate_thread, "separate-thread", 't',
	             "Categorize samples by thread group and thread ID"),
};
}

static void __print_usage_and_exit(const char * extra_msg)
{
	if (extra_msg)
		cerr << extra_msg << endl;
	cerr << "usage: operf [ options ] [ --system-wide | --pid <pid> | [ command [ args ] ] ]" << endl;
	exit(EXIT_FAILURE);
}


static void op_sig_stop(int val __attribute__((unused)))
{
	// Received a signal to quit, so we need to stop the
	// app being profiled.
	if (cverb << vdebug)
		write(1, "in op_sig_stop ", 15);
	if (startApp)
		kill(app_PID, SIGKILL);
}

void set_signals(void)
{
	struct sigaction act;
	sigset_t ss;

	sigfillset(&ss);
	sigprocmask(SIG_UNBLOCK, &ss, NULL);

	act.sa_handler = op_sig_stop;
	act.sa_flags = 0;
	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, SIGINT);

	if (sigaction(SIGINT, &act, NULL)) {
		perror("operf: install of SIGINT handler failed: ");
		exit(EXIT_FAILURE);
	}
}

static int app_ready_pipe[2], start_app_pipe[2], operf_record_ready_pipe[2];

void run_app(void)
{
	char * app_fname = rindex(app_name, '/') + 1;
	if (!app_fname) {
		string msg = "Error trying to parse app name ";
		msg += app_name;
		__print_usage_and_exit(msg.c_str());
	}

	vector<string> exec_args_str;
	if (app_args) {
		size_t end_pos;
		string app_args_str = app_args;
		// Since the strings returned from substr would otherwise be ephemeral, we
		// need to store them into the exec_args_str vector so we can reference
		// them later when we call execvp.
		do {
			end_pos = app_args_str.find_first_of(' ', 0);
			if (end_pos != string::npos) {
				exec_args_str.push_back(app_args_str.substr(0, end_pos));
				app_args_str = app_args_str.substr(end_pos + 1);
			} else {
				exec_args_str.push_back(app_args_str);
			}
		} while (end_pos != string::npos);
	}

	vector<const char *> exec_args;
	exec_args.push_back(app_fname);
	vector<string>::iterator it;
	cverb << vdebug << "Exec args are: " << app_fname << " ";
	// Now transfer the args from the intermediate exec_args_str container to the
	// exec_args container that can be passed to execvp.
	for (it = exec_args_str.begin(); it != exec_args_str.end(); it++) {
		exec_args.push_back((*it).c_str());
		cverb << vdebug << (*it).c_str() << " ";
	}
	exec_args.push_back((char *) NULL);
	cverb << vdebug << endl;
	// Fake an exec to warm-up the resolver
	execvp("", ((char * const *)&exec_args[0]));
	// signal to the parent that we're ready to exec
	int startup = 1;
	if (write(app_ready_pipe[1], &startup, sizeof(startup)) < 0) {
		perror("Internal error on app_ready_pipe");
		_exit(EXIT_FAILURE);
	}

	// wait for parent to tell us to start
	int startme = 0;
	if (read(start_app_pipe[0], &startme, sizeof(startme)) == -1) {
		perror("Internal error in run_app on start_app_pipe");
		_exit(EXIT_FAILURE);
	}
	if (startme != 1)
		goto fail;

	cverb << vdebug << "parent says start app " << app_name << endl;
	app_started = true;
	execvp(app_name, ((char * const *)&exec_args[0]));
	cerr <<  "Failed to exec " << exec_args[0] << ": " << strerror(errno) << endl;
fail:
	/* We don't want any cleanup in the child */
	_exit(EXIT_FAILURE);

}

int start_profiling_app(void)
{
	// The only process that should return from this function is the process
	// which invoked it.  Any forked process must do _exit() rather than return().
	struct timeval tv;
	unsigned long long start_time = 0ULL;
	gettimeofday(&tv, NULL);
	start_time = 0ULL;
	start_time = tv.tv_sec;
	sprintf(start_time_str, "%llu", start_time);
	startApp = ((app_PID != operf_options::pid) && (operf_options::system_wide == false));

	if (startApp) {
		if (pipe(app_ready_pipe) < 0 || pipe(start_app_pipe) < 0) {
			perror("Internal error: operf-record could not create pipe");
			_exit(EXIT_FAILURE);
		}
		app_PID = fork();
		if (app_PID < 0) {
			perror("Internal error: fork failed");
			_exit(EXIT_FAILURE);
		} else if (app_PID == 0) { // child process for exec'ing app
			close(sample_data_pipe[0]);
			close(sample_data_pipe[1]);
			run_app();
		}
		// parent
		if (pipe(operf_record_ready_pipe) < 0) {
			perror("Internal error: could not create pipe");
			return -1;
		}
	}

	//parent
	operf_pid = fork();
	if (operf_pid < 0) {
		return -1;
	} else if (operf_pid == 0) { // operf-record process
		int ready = 0;
		close(sample_data_pipe[0]);
		/*
		 * Since an informative message will be displayed to the user if
		 * an error occurs, we don't want to blow chunks here; instead, we'll
		 * exit gracefully.  Clear out the operf.data file as an indication
		 * to the parent process that the profile data isn't valid.
		 */
		try {
			OP_perf_utils::vmlinux_info_t vi;
			vi.image_name = operf_options::vmlinux;
			vi.start = kernel_start;
			vi.end = kernel_end;
			operf_record operfRecord(sample_data_pipe[1], operf_options::system_wide, app_PID,
			                         (operf_options::pid == app_PID), events, vi,
			                         operf_options::callgraph,
			                         operf_options::separate_cpu);
			if (operfRecord.get_valid() == false) {
				/* If valid is false, it means that one of the "known" errors has
				 * occurred:
				 *   - profiled process has already ended
				 *   - passed PID was invalid
				 *   - device or resource busy
				 */
				cerr << "operf record init failed" << endl;
				cerr << "usage: operf [ options ] [ --system-wide | --pid <pid> | [ command [ args ] ] ]" << endl;
				// Exit with SUCCESS to avoid the unnecessary "operf-record process ended
				// abnormally" message
				goto fail_out;
			}
			if (startApp) {
				ready = 1;
				if (write(operf_record_ready_pipe[1], &ready, sizeof(ready)) < 0) {
					perror("Internal error on operf_record_ready_pipe");
					goto fail_out;
				}
			}

			// start recording
			operfRecord.recordPerfData();
			cverb << vmisc << "Total bytes recorded from perf events: " << dec
					<< operfRecord.get_total_bytes_recorded() << endl;

			operfRecord.~operf_record();
			// done
			_exit(EXIT_SUCCESS);
		} catch (runtime_error re) {
			cerr << "Caught runtime_error: " << re.what() << endl;
			goto fail_out;
		}
fail_out:
		if (startApp && !ready){
			/* ready==0 means we've not yet told parent we're ready,
			 * but the parent is reading our pipe.  So we tell the
			 * parent we're not ready so it can continue.
			 */
			if (write(operf_record_ready_pipe[1], &ready, sizeof(ready)) < 0) {
				perror("Internal error on operf_record_ready_pipe");
			}
			_exit(EXIT_SUCCESS);
		}
	} else {  // parent
		if (startApp) {
			int startup;
			if (read(app_ready_pipe[0], &startup, sizeof(startup)) == -1) {
				perror("Internal error on app_ready_pipe");
				return -1;
			} else if (startup != 1) {
				cerr << "app is not ready to start; exiting" << endl;
				return -1;
			}

			int recorder_ready;
			if (read(operf_record_ready_pipe[0], &recorder_ready, sizeof(recorder_ready)) == -1) {
				perror("Internal error on operf_record_ready_pipe");
				return -1;
			} else if (recorder_ready != 1) {
				cerr << "operf record process failure; exiting" << endl;
				cverb << vdebug << "telling child to abort starting of app" << endl;
				startup = 0;
				if (write(start_app_pipe[1], &startup, sizeof(startup)) < 0) {
					perror("Internal error on start_app_pipe");
				}
				return -1;
			}

			// Tell app_PID to start the app
			cverb << vdebug << "telling child to start app" << endl;
			if (write(start_app_pipe[1], &startup, sizeof(startup)) < 0) {
				perror("Internal error on start_app_pipe");
				return -1;
			}
		}
	}
	if (!operf_options::system_wide)
		app_started = true;

	// parent returns
	return 0;
}

static end_code_t _kill_operf_pid(void)
{
	int waitpid_status = 0;
	end_code_t rc = ALL_OK;
	struct timeval tv;
	long long start_time_sec;
	long long usec_timer;
	bool keep_trying = true;

	// stop operf-record process
	if (kill(operf_pid, SIGUSR1) < 0) {
		perror("Attempt to stop operf-record process failed");
		rc = PERF_RECORD_ERROR;
	} else {
		if (waitpid(operf_pid, &waitpid_status, 0) < 0) {
			perror("waitpid for operf-record process failed");
			rc = PERF_RECORD_ERROR;
		} else {
			if (WIFEXITED(waitpid_status) && (!WEXITSTATUS(waitpid_status))) {
				cverb << vdebug << "operf-record process returned OK" << endl;
			} else {
				cerr <<  "operf-record process ended abnormally: "
				     << WEXITSTATUS(waitpid_status) << endl;
				rc = PERF_RECORD_ERROR;
			}
		}
	}

	// Now stop the operf-read process (aka "convert_pid")
	waitpid_status = 0;
	gettimeofday(&tv, NULL);
	start_time_sec = tv.tv_sec;
	usec_timer = tv.tv_usec;
	/* We'll initially try the waitpid with WNOHANG once every 100,000 usecs.
	 * If it hasn't ended within 5 seconds, we'll kill it and do one
	 * final wait.
	 */
	while (keep_trying) {
		int option = WNOHANG;
		gettimeofday(&tv, NULL);
		if (tv.tv_sec > start_time_sec + 5) {
			keep_trying = false;
			option = 0;
			cerr << "now trying to kill convert pid..." << endl;

			if (kill(convert_pid, SIGUSR1) < 0) {
				perror("Attempt to stop operf-read process failed");
				rc = rc ? PERF_BOTH_ERROR : PERF_READ_ERROR;
				break;
			}
		} else {
			/* If we exceed the 100000 usec interval or if the tv_usec
			 * value has rolled over to restart at 0, then we reset
			 * the usec_timer to current tv_usec and try waitpid.
			 */
			if ((tv.tv_usec % 1000000) > (usec_timer + 100000)
					|| (tv.tv_usec < usec_timer))
				usec_timer = tv.tv_usec;
			else
				continue;
		}
		if (waitpid(convert_pid, &waitpid_status, option) < 0) {
			keep_trying = false;
			if (errno != ECHILD) {
				perror("waitpid for operf-read process failed");
				rc = rc ? PERF_BOTH_ERROR : PERF_READ_ERROR;
			}
		} else {
			if (WIFEXITED(waitpid_status)) {
				keep_trying = false;
				if (!WEXITSTATUS(waitpid_status)) {
					cverb << vdebug << "operf-read process returned OK" << endl;
				} else {
					cerr <<  "operf-read process ended abnormally.  Status = "
					     << WEXITSTATUS(waitpid_status) << endl;
					rc = rc ? PERF_BOTH_ERROR : PERF_READ_ERROR;
				}
			}
		}
	}
	return rc;
}

static end_code_t _run(void)
{
	int waitpid_status = 0;
	end_code_t rc = ALL_OK;

	// Fork processes with signals blocked.
	sigset_t ss;
	sigfillset(&ss);
	sigprocmask(SIG_BLOCK, &ss, NULL);

	// Create pipe to which operf-record process writes sample data and
	// from which the operf-read process reads.
	if (pipe(sample_data_pipe) < 0) {
		perror("Internal error: operf-record could not create pipe");
		_exit(EXIT_FAILURE);
	}

	if (start_profiling_app() < 0) {
		return PERF_RECORD_ERROR;
	}
	// parent continues here
	if (startApp)
		cverb << vdebug << "app " << app_PID << " is running" << endl;

	/* If we're not doing system wide profiling and no app is started, then
	 * there's no profile data to convert. So if this condition is NOT true,
	 * then we'll do the convert.
	 */
	if (!(!app_started && !operf_options::system_wide)) {
		convert_pid = fork();
		if (convert_pid < 0) {
			perror("Internal error: fork failed");
			_exit(EXIT_FAILURE);
		} else if (convert_pid == 0) { // child process
			close(sample_data_pipe[1]);
			convert_sample_data();
		}
		// parent
		close(sample_data_pipe[0]);
		close(sample_data_pipe[1]);
	}

	set_signals();
	cout << "operf: Profiler started" << endl;
	if (startApp) {
		// User passed in command or program name to start
		cverb << vdebug << "going into waitpid on profiled app " << app_PID << endl;
		if (waitpid(app_PID, &waitpid_status, 0) < 0) {
			if (errno == EINTR) {
				cverb << vdebug << "Caught ctrl-C.  Killed profiled app." << endl;
			} else {
				cerr << "waitpid errno is " << errno << endl;
				perror("waitpid for profiled app failed");
				rc = APP_ABNORMAL_END;
			}
		} else {
			if (WIFEXITED(waitpid_status) && (!WEXITSTATUS(waitpid_status))) {
				cverb << vdebug << "waitpid for profiled app returned OK" << endl;
			} else if (WIFEXITED(waitpid_status)) {
				cerr <<  "profiled app ended abnormally: "
						<< WEXITSTATUS(waitpid_status) << endl;
				rc = APP_ABNORMAL_END;
			}
		}
		rc = _kill_operf_pid();
	} else {
		// User passed in --pid or --system-wide
		cout << "operf: Press Ctl-c to stop profiling" << endl;
		cverb << vdebug << "going into waitpid on operf record process " << operf_pid << endl;
		if (waitpid(operf_pid, &waitpid_status, 0) < 0) {
			if (errno == EINTR) {
				cverb << vdebug << "Caught ctrl-C. Killing operf-record process . . ." << endl;
				rc = _kill_operf_pid();
			} else {
				cerr << "waitpid errno is " << errno << endl;
				perror("waitpid for operf-record process failed");
				rc = PERF_RECORD_ERROR;
			}
		} else {
			if (WIFEXITED(waitpid_status) && (!WEXITSTATUS(waitpid_status))) {
				cverb << vdebug << "waitpid for operf-record process returned OK" << endl;
			} else if (WIFEXITED(waitpid_status)) {
				cerr <<  "operf-record process ended abnormally: "
				     << WEXITSTATUS(waitpid_status) << endl;
				rc = PERF_RECORD_ERROR;
			} else if (WIFSIGNALED(waitpid_status)) {
				cerr << "operf-record process killed by signal "
				     << WTERMSIG(waitpid_status) << endl;
				rc = PERF_RECORD_ERROR;
			}
		}
	}
	return rc;
}

static void cleanup(void)
{
	free(app_name_SAVE);
	free(app_args);
	events.clear();
	verbose_string.clear();
}

static void _jitconv_complete(int val __attribute__((unused)))
{
	int child_status;
	pid_t the_pid = wait(&child_status);
	if (the_pid != jitconv_pid) {
		return;
	}
	jit_conversion_running = false;
	if (WIFEXITED(child_status) && (!WEXITSTATUS(child_status))) {
		cverb << vmisc << "JIT dump processing complete." << endl;
	} else {
		 if (WIFSIGNALED(child_status))
			 cerr << "child received signal " << WTERMSIG(child_status) << endl;
		 else
			 cerr << "JIT dump processing exited abnormally: "
			 << WEXITSTATUS(child_status) << endl;
	}
}


static void _do_jitdump_convert()
{
	int arg_num;
	unsigned long long end_time = 0ULL;
	struct timeval tv;
	char end_time_str[32];
	char opjitconv_path[PATH_MAX + 1];
	char * exec_args[8];
	struct sigaction act;
	sigset_t ss;

	sigfillset(&ss);
	sigprocmask(SIG_UNBLOCK, &ss, NULL);

	act.sa_handler = _jitconv_complete;
	act.sa_flags = 0;
	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, SIGCHLD);

	if (sigaction(SIGCHLD, &act, NULL)) {
		perror("operf: install of SIGCHLD handler failed: ");
		exit(EXIT_FAILURE);
	}


	jitconv_pid = fork();
	switch (jitconv_pid) {
	case -1:
		perror("Error forking JIT dump process!");
		break;
	case 0: {
		const char * jitconv_pgm = "opjitconv";
		const char * debug_option = "-d";
		const char * non_root_user = "--non-root";
		const char * delete_jitdumps = "--delete-jitdumps";
		gettimeofday(&tv, NULL);
		end_time = tv.tv_sec;
		sprintf(end_time_str, "%llu", end_time);
		sprintf(opjitconv_path, "%s/%s", OP_BINDIR, jitconv_pgm);
		arg_num = 0;
		exec_args[arg_num++] = (char *)jitconv_pgm;
		if (cverb << vmisc)
			exec_args[arg_num++] = (char *)debug_option;
		if (my_uid != 0)
			exec_args[arg_num++] = (char *)non_root_user;
		exec_args[arg_num++] = (char *)delete_jitdumps;
		exec_args[arg_num++] = (char *)operf_options::session_dir.c_str();
		exec_args[arg_num++] = start_time_str;
		exec_args[arg_num++] = end_time_str;
		exec_args[arg_num] = (char *) NULL;
		execvp(opjitconv_path, exec_args);
		fprintf(stderr, "Failed to exec %s: %s\n",
		        exec_args[0], strerror(errno));
		/* We don't want any cleanup in the child */
		_exit(EXIT_FAILURE);
		break;
	}
	default: // parent
		jit_conversion_running = true;
		break;
	}

}

static int __delete_old_previous_sample_data(const char *fpath,
                                const struct stat *sb  __attribute__((unused)),
                                int tflag  __attribute__((unused)),
                                struct FTW *ftwbuf __attribute__((unused)))
{
	if (remove(fpath)) {
		perror("sample data removal error");
		return FTW_STOP;
	} else {
		return FTW_CONTINUE;
	}
}

/* Read perf_events sample data written by the operf-record process
 * through the sample_data_pipe and convert this to oprofile format
 * sample files.
 */
static void convert_sample_data(void)
{
	int rc;
	string current_sampledir = samples_dir + "/current/";
	string previous_sampledir = samples_dir + "/previous";
	current_sampledir.copy(op_samples_current_dir, current_sampledir.length(), 0);

	if (!operf_options::append) {
                int flags = FTW_DEPTH | FTW_ACTIONRETVAL;
		errno = 0;
		if (nftw(previous_sampledir.c_str(), __delete_old_previous_sample_data, 32, flags) !=0 &&
				errno != ENOENT) {
			cerr << "Unable to remove old sample data at " << previous_sampledir << "." << endl;
			if (errno)
				cerr << strerror(errno) << endl;
			cleanup();
			_exit(EXIT_FAILURE);
		}
		if (rename(current_sampledir.c_str(), previous_sampledir.c_str()) < 0) {
			if (errno && (errno != ENOENT)) {
				cerr << "Unable to move old profile data to " << previous_sampledir << endl;
				cerr << strerror(errno) << endl;
				cleanup();
				_exit(EXIT_FAILURE);
			}
		}
	}
	rc = mkdir(current_sampledir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	if (rc && (errno != EEXIST)) {
		cerr << "Error trying to create " << current_sampledir << " dir." << endl;
		perror("mkdir failed with");
		_exit(EXIT_FAILURE);
	}

	operfRead.init(sample_data_pipe[0], current_sampledir, cpu_type, events);
	if ((rc = operfRead.readPerfHeader()) < 0) {
		if (rc != OP_PERF_HANDLED_ERROR)
			cerr << "Error: Cannot create read header info for sample data " << endl;
		cleanup();
		_exit(EXIT_FAILURE);
	}
	cverb << vdebug << "Successfully read header info for sample data " << endl;
	if (operfRead.is_valid()) {
		try {
			operfRead.convertPerfData();
		} catch (runtime_error e) {
			cerr << "Caught exception from operf_read::convertPerfData" << endl;
			cerr << e.what() << endl;
			cleanup();
			_exit(EXIT_FAILURE);
		}
	}
	// Invoke opjitconv and set up a SIGCHLD signal for when it's done
	_do_jitdump_convert();
	int keep_waiting = 0;
	while (jit_conversion_running && (keep_waiting < 2)) {
		sleep(1);
		keep_waiting++;
	}
	if (jit_conversion_running) {
		kill(jitconv_pid, SIGKILL);
	}
	_exit(EXIT_SUCCESS);

}


static int find_app_file_in_dir(const struct dirent * d)
{
	if (!strcmp(d->d_name, app_name))
		return 1;
	else
		return 0;
}

static int get_PATH_based_pathname(char * path_holder, size_t n)
{
	int retval = -1;

	char * real_path = getenv("PATH");
	char * path = (char *) xstrdup(real_path);
	char * segment = strtok(path, ":");
	while (segment) {
		struct dirent ** namelist;
		int rc = scandir(segment, &namelist, find_app_file_in_dir, NULL);
		if (rc < 0) {
			cerr << app_name << " cannot be found in your PATH." << endl;
			break;
		} else if (rc == 1) {
			size_t applen = strlen(app_name);
			size_t dirlen = strlen(segment);

			if (applen + dirlen + 2 > n) {
				cerr << "Path segment " << segment
				     << " prepended to the passed app name is too long"
				     << endl;
				retval = -1;
				break;
			}
			strncpy(path_holder, segment, dirlen);
			strcat(path_holder, "/");
			strncat(path_holder, app_name, applen);
			retval = 0;
			break;
		}
		segment = strtok(NULL, ":");
	}
	return retval;
}
int validate_app_name(void)
{
	int rc = 0;
	struct stat filestat;
	size_t len = strlen(app_name);

	if (len > (size_t) (OP_APPNAME_LEN - 1)) {
		cerr << "app name longer than max allowed (" << OP_APPNAME_LEN
		     << " chars)\n";
		cerr << app_name << endl;
		rc = -1;
		goto out;
	}

	if (index(app_name, '/') == app_name) {
		// Full pathname of app was specified, starting with "/".
		strncpy(full_pathname, app_name, len);
	} else if ((app_name[0] == '.') && (app_name[1] == '/')) {
		// Passed app is in current directory; e.g., "./myApp"
		getcwd(full_pathname, PATH_MAX);
		strcat(full_pathname, "/");
		strcat(full_pathname, (app_name + 2));
	} else if (index(app_name, '/')) {
		// Passed app is in a subdirectory of cur dir; e.g., "test-stuff/myApp"
		getcwd(full_pathname, PATH_MAX);
		strcat(full_pathname, "/");
		strcat(full_pathname, app_name);
	} else {
		// Pass app name, at this point, MUST be found in PATH
		rc = get_PATH_based_pathname(full_pathname, PATH_MAX);
	}

	if (rc) {
		cerr << "Problem finding app name " << app_name << ". Aborting."
		     << endl;
		goto out;
	}
	app_name_SAVE = app_name;
	app_name = full_pathname;
	if (stat(app_name, &filestat)) {
		char msg[OP_APPNAME_LEN + 50];
		snprintf(msg, OP_APPNAME_LEN + 50, "Non-existent app name \"%s\"",
		         app_name);
		perror(msg);
		rc = -1;
	}

	out: return rc;
}

static u32 _get_event_code(char name[])
{
	FILE * fp;
	char oprof_event_code[9];
	string command;
	command = "ophelp ";
	command += name;

	fp = popen(command.c_str(), "r");
	if (fp == NULL) {
		cerr << "Unable to execute ophelp to get info for event "
		     << name << endl;
		exit(EXIT_FAILURE);
	}
	if (fgets(oprof_event_code, sizeof(oprof_event_code), fp) == NULL) {
		cerr << "Unable to find info for event "
		     << name << endl;
		exit(EXIT_FAILURE);
	}

	return atoi(oprof_event_code);
}

static void _process_events_list(void)
{
	string cmd = "ophelp --check-events ";
	for (unsigned int i = 0; i <  operf_options::evts.size(); i++) {
		FILE * fp;
		string full_cmd = cmd;
		string event_spec = operf_options::evts[i];
		full_cmd += event_spec;
		if (operf_options::callgraph) {
			full_cmd += " --callgraph=1";
		}
		fp = popen(full_cmd.c_str(), "r");
		if (fp == NULL) {
			cerr << "Unable to execute ophelp to get info for event "
			     << event_spec << endl;
			exit(EXIT_FAILURE);
		}
		if (fgetc(fp) == EOF) {
			cerr << "Error retrieving info for event "
			     << event_spec << endl;
			if (operf_options::callgraph)
				cerr << "Note: When doing callgraph profiling, the sample count must be"
				     << endl << "15 times the minimum count value for the event."  << endl;
			exit(EXIT_FAILURE);
		}
		char * event_str = op_xstrndup(event_spec.c_str(), event_spec.length());
		operf_event_t event;
		strncpy(event.name, strtok(event_str, ":"), OP_MAX_EVT_NAME_LEN);
		event.count = atoi(strtok(NULL, ":"));
		/* Name and count are required in the event spec in order for
		 * 'ophelp --check-events' to pass.  But since unit mask is
		 * optional, we need to ensure the result of strtok is valid.
		 */
		char * um = strtok(NULL, ":");
		if (um)
			event.evt_um = atoi(um);
		else
			event.evt_um = 0;
		event.op_evt_code = _get_event_code(event.name);
		event.evt_code = event.op_evt_code;
		events.push_back(event);
	}
#if (defined(__powerpc__) || defined(__powerpc64__))
	{
		/* This section of code is for architectures such as ppc[64] for which
		 * the oprofile event code needs to be converted to the appropriate event
		 * code to pass to the perf_event_open syscall.
		 */

		using namespace OP_perf_utils;
		if (!op_convert_event_vals(&events)) {
			cerr << "Unable to convert all oprofile event values to perf_event values" << endl;
			exit(EXIT_FAILURE);
		}
	}
#endif
}

static void get_default_event(void)
{
	operf_event_t dft_evt;
	struct op_default_event_descr descr;
	vector<operf_event_t> tmp_events;


	op_default_event(cpu_type, &descr);
	if (descr.name[0] == '\0') {
		cerr << "Unable to find default event" << endl;
		exit(EXIT_FAILURE);
	}

	memset(&dft_evt, 0, sizeof(dft_evt));
	if (operf_options::callgraph) {
		struct op_event * _event;
		op_events(cpu_type);
		if ((_event = find_event_by_name(descr.name, 0, 0))) {
			dft_evt.count = _event->min_count * CALLGRAPH_MIN_COUNT_SCALE;
		} else {
			cerr << "Error getting event info for " << descr.name << endl;
			exit(EXIT_FAILURE);
		}
	} else {
		dft_evt.count = descr.count;
	}
	dft_evt.evt_um = descr.um;
	strncpy(dft_evt.name, descr.name, OP_MAX_EVT_NAME_LEN - 1);
	dft_evt.op_evt_code = _get_event_code(dft_evt.name);
	dft_evt.evt_code = dft_evt.op_evt_code;
	events.push_back(dft_evt);

#if (defined(__powerpc__) || defined(__powerpc64__))
	{
		/* This section of code is for architectures such as ppc[64] for which
		 * the oprofile event code needs to be converted to the appropriate event
		 * code to pass to the perf_event_open syscall.
		 */

		using namespace OP_perf_utils;
		if (!op_convert_event_vals(&events)) {
			cerr << "Unable to convert all oprofile event values to perf_event values" << endl;
			exit(EXIT_FAILURE);
		}
	}
#endif
}

static void _process_session_dir(void)
{
	if (operf_options::session_dir.empty()) {
		char * cwd = NULL;
		int rc;
		cwd = (char *) xmalloc(PATH_MAX);
		// set default session dir
		cwd = getcwd(cwd, PATH_MAX);
		operf_options::session_dir = cwd;
		operf_options::session_dir +="/oprofile_data";
		samples_dir = operf_options::session_dir + "/samples";
		free(cwd);
		rc = mkdir(operf_options::session_dir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
		if (rc && (errno != EEXIST)) {
			cerr << "Error trying to create " << operf_options::session_dir << " dir." << endl;
			perror("mkdir failed with");
			exit(EXIT_FAILURE);
		}
		rc = mkdir(samples_dir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
		if (rc && (errno != EEXIST)) {
			cerr << "Error trying to create " << samples_dir << " dir." << endl;
			perror("mkdir failed with");
			exit(EXIT_FAILURE);
		}
	} else {
		struct stat filestat;
		int rc;
		if (stat(operf_options::session_dir.c_str(), &filestat)) {
			perror("stat operation on passed session-dir failed");
			exit(EXIT_FAILURE);
		}
		if (!S_ISDIR(filestat.st_mode)) {
			cerr << "Passed session-dir " << operf_options::session_dir
			     << " is not a directory" << endl;
			exit(EXIT_FAILURE);
		}
		string tmp = operf_options::session_dir + "/oprofile_data";
		rc = mkdir(tmp.c_str(), S_IRWXU);
		if (rc && (errno != EEXIST)) {
			cerr << "Error trying to create " << tmp << " dir." << endl;
			perror("mkdir failed with");
			exit(EXIT_FAILURE);
		}
		samples_dir = tmp + "/samples";
		rc = mkdir(samples_dir.c_str(), S_IRWXU);
		if (rc && (errno != EEXIST)) {
			cerr << "Error trying to create " << samples_dir << " dir." << endl;
			perror("mkdir failed with");
			exit(EXIT_FAILURE);
		}
	}
	cverb << vdebug << "Using samples dir " << samples_dir << endl;
}

bool _get_vmlinux_address_info(vector<string> args, string cmp_val, string &str)
{
	bool found = false;
	child_reader reader("objdump", args);
	if (reader.error()) {
		cerr << "An error occurred while trying to get vmlinux address info:\n\n";
		cerr << reader.error_str() << endl;
		exit(EXIT_FAILURE);
	}

	while (reader.getline(str)) {
		if (str.find(cmp_val.c_str()) != string::npos) {
			found = true;
			break;
		}
	}
	// objdump always returns SUCCESS so we must rely on the stderr state
	// of objdump. If objdump error message is cryptic our own error
	// message will be probably also cryptic
	ostringstream std_err;
	ostringstream std_out;
	reader.get_data(std_out, std_err);
	if (std_err.str().length()) {
		cerr << "An error occurred while getting vmlinux address info:\n\n";
		cerr << std_err.str() << endl;
		// If we found the string we were looking for in objdump output,
		// treat this as non-fatal error.
		if (!found)
			exit(EXIT_FAILURE);
	}

	// force error code to be acquired
	reader.terminate_process();

	// required because if objdump stop by signal all above things suceeed
	// (signal error message are not output through stdout/stderr)
	if (reader.error()) {
		cerr << "An error occur during the execution of objdump to get vmlinux address info:\n\n";
		cerr << reader.error_str() << endl;
		if (!found)
			exit(EXIT_FAILURE);
	}
	return found;
}

string _process_vmlinux(string vmlinux_file)
{
	vector<string> args;
	char start[17], end[17];
	string str, start_end;
	bool found;
	int ret;

	no_vmlinux = false;
	args.push_back("-h");
	args.push_back(vmlinux_file);
	if ((found = _get_vmlinux_address_info(args, " .text", str))) {
		cverb << vmisc << str << endl;
		ret = sscanf(str.c_str(), " %*s %*s %*s %s", start);
	}
	if (!found || ret != 1){
		cerr << "Unable to obtain vmlinux start address." << endl;
		cerr << "The specified vmlinux file (" << vmlinux_file << ") "
		     << "does not seem to be valid." << endl;
		cerr << "Make sure you are using a non-compressed image file "
		     << "(e.g. vmlinux not vmlinuz)" << endl;
		exit(EXIT_FAILURE);
	}

	args.clear();
	args.push_back("-t");
	args.push_back(vmlinux_file);
	if ((found = _get_vmlinux_address_info(args, " _etext", str))) {
		cverb << vmisc << str << endl;
		ret = sscanf(str.c_str(), "%s", end);
	}
	if (!found || ret != 1){
		cerr << "Unable to obtain vmlinux end address." << endl;
		cerr << "The specified vmlinux file (" << vmlinux_file << ") "
		     << "does not seem to be valid." << endl;
		cerr << "Make sure you are using a non-compressed image file "
		     << "(e.g. vmlinux not vmlinuz)" << endl;
		exit(EXIT_FAILURE);
	}

	errno = 0;
	kernel_start = strtoull(start, NULL, 16);
	if (errno) {
		cerr << "Unable to convert vmlinux start address " << start
		     << " to a valid hex value. errno is " << strerror(errno) << endl;
		exit(EXIT_FAILURE);
	}
	errno = 0;
	kernel_end =  strtoull(end, NULL, 16);
	if (errno) {
		cerr << "Unable to convert vmlinux end address " << start
		     << " to a valid hex value. errno is " << strerror(errno) << endl;
		exit(EXIT_FAILURE);
	}

	start_end = start;
	start_end.append(",");
	start_end.append(end);
	return start_end;
}

static void process_args(int argc, char const ** argv)
{
	vector<string> non_options;
	popt::parse_options(argc, argv, non_options, true/*non-options IS an app*/);

	if (!non_options.empty()) {
		if (operf_options::pid || operf_options::system_wide)
			__print_usage_and_exit(NULL);

		vector<string>::iterator it = non_options.begin();
		app_name = (char *) xmalloc((*it).length() + 1);
		strncpy(app_name, ((char *)(*it).c_str()), (*it).length() + 1);
		if (it++ != non_options.end()) {
			if ((*it).length() > 0) {
				app_args = (char *) xmalloc((*it).length() + 1);
				strncpy(app_args, ((char *)(*it).c_str()), (*it).length() + 1);
			}
		}
		if (validate_app_name() < 0) {
			__print_usage_and_exit(NULL);
		}
	} else if (operf_options::pid) {
		if (operf_options::system_wide)
			__print_usage_and_exit(NULL);
		app_PID = operf_options::pid;
	} else if (operf_options::system_wide) {
		app_PID = -1;
	} else {
		__print_usage_and_exit(NULL);
	}
	/*  At this point, we know which of the three kinds of profiles the user requested:
	 *    - profile app by name
	 *    - profile app by PID
	 *    - profile whole system
	 */

	if (!verbose::setup(verbose_string)) {
		cerr << "unknown --verbose= options\n";
		exit(EXIT_FAILURE);
	}

	_process_session_dir();

	if (operf_options::evts.empty()) {
		// Use default event
		get_default_event();
	} else  {
		_process_events_list();
	}

	if (operf_options::vmlinux.empty()) {
		no_vmlinux = true;
		operf_create_vmlinux(NULL, NULL);
	} else {
		string startEnd = _process_vmlinux(operf_options::vmlinux);
		operf_create_vmlinux(operf_options::vmlinux.c_str(), startEnd.c_str());
	}

	return;
}

static int _check_perf_events_cap(void)
{
	/* If perf_events syscall is not implemented, the syscall below will fail
	 * with ENOSYS (38).  If implemented, but the processor type on which this
	 * program is running is not supported by perf_events, the syscall returns
	 * ENOENT (2).
	 */
	struct perf_event_attr attr;
	pid_t pid ;
        memset(&attr, 0, sizeof(attr));
        attr.size = sizeof(attr);
        attr.sample_type = PERF_SAMPLE_IP;

	pid = getpid();
	syscall(__NR_perf_event_open, &attr, pid, 0, -1, 0);
	return errno;

}

static void _precheck_permissions_to_samplesdir(string sampledir, bool for_current)
{
	/* Pre-check to make sure we have permission to remove old sample data
	 * or to create new sample data in the specified sample data directory.
	 * If the user wants us to remove old data, we don't actually do it now,
	 * since the profile session may fail for some reason or the user may do ctl-c.
	 * We should exit without unnecessarily removing the old sample data as
	 * the user may expect it to still be there after an aborted run.
	 */
	string sampledir_testfile = sampledir + "/.xxxTeStFiLe";
	ofstream afile;
	errno = 0;
	afile.open(sampledir_testfile.c_str());
	if (!afile.is_open() && (errno != ENOENT)) {
		if (operf_options::append && for_current)
			cerr << "Unable to write to sample data directory at "
			     << sampledir << "." << endl;
		else
			cerr << "Unable to remove old sample data at "
			     << sampledir << "." << endl;
		if (errno)
			cerr << strerror(errno) << endl;
		cerr << "Try a manual removal of " << sampledir << endl;
		cleanup();
		exit(1);
	}
	afile.close();

}

bool no_vmlinux;
int main(int argc, char const *argv[])
{
	int rc;
	if ((rc = _check_perf_events_cap())) {
		if (rc == ENOSYS) {
			cerr << "Your kernel does not implement a required syscall"
			     << "  for the operf program." << endl;
		} else if (rc == ENOENT) {
			cerr << "Your kernel's Performance Events Subsystem does not support"
			     << " your processor type." << endl;
		} else {
			cerr << "Unexpected error running operf: " << strerror(rc) << endl;
		}
		cerr << "Please use the opcontrol command instead of operf." << endl;
		exit(1);
	}

	cpu_type = op_get_cpu_type();
	cpu_speed = op_cpu_frequency();
	process_args(argc, argv);
	my_uid = geteuid();
	if (operf_options::system_wide && my_uid != 0) {
		cerr << "You must be root to do system-wide profiling." << endl;
		cleanup();
		exit(1);
	}

	if (cpu_type == CPU_NO_GOOD) {
		cerr << "Unable to ascertain cpu type.  Exiting." << endl;
		cleanup();
		exit(1);
	}
	op_nr_counters = op_get_nr_counters(cpu_type);

	if (my_uid != 0) {
		bool for_current = true;
		string current_sampledir = samples_dir + "/current";
		_precheck_permissions_to_samplesdir(current_sampledir, for_current);
		if (!operf_options::append) {
			string previous_sampledir = samples_dir + "/previous";
			for_current = false;
			_precheck_permissions_to_samplesdir(previous_sampledir, for_current);
		}
	}
	end_code_t run_result;
	if ((run_result = _run())) {
		if (app_started && (run_result != APP_ABNORMAL_END)) {
			int rc;
			cverb << vdebug << "Killing profiled app . . ." << endl;
			rc = kill(app_PID, SIGKILL);
			if (rc) {
				if (errno == ESRCH)
					cverb << vdebug
					      << "Unable to kill profiled app because it has already ended"
					      << endl;
				else
					perror("Attempt to kill profiled app failed.");
			}
		}
		if ((run_result == PERF_RECORD_ERROR) || (run_result == PERF_BOTH_ERROR)) {
			cerr <<  "Error running profiler" << endl;
		} else if (run_result == PERF_READ_ERROR) {
			cerr << "Error converting operf sample data to oprofile sample format" << endl;
		} else {
			cerr << "WARNING: Profile results may be incomplete due to to abend of profiled app." << endl;
		}
	} else {
		cout << endl << "Use '--session-dir=" << operf_options::session_dir << "'" << endl
		     << "with opreport and other post-processing tools to view your profile data."
		     << endl;
	}
	cleanup();
	return run_result;;
}
