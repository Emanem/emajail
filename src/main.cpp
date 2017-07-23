/*
*	emajail (C) 2017 E. Oriani, ema <AT> fastwebnet <DOT> it
*
*	This file is part of emajail.
*
*	emajail is free software: you can redistribute it and/or modify
*	it under the terms of the GNU General Public License as published by
*	the Free Software Foundation, either version 3 of the License, or
*	(at your option) any later version.
*
*	emajail is distributed in the hope that it will be useful,
*	but WITHOUT ANY WARRANTY; without even the implied warranty of
*	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*	GNU General Public License for more details.
*
*	You should have received a copy of the GNU General Public License
*	along with emajail.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>
#include <exception>
#include <cerrno>
#include <sstream>
#include <fstream>
#include <memory>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <linux/limits.h>

namespace log {
	bool	silent = false;

	std::string header(const char* type) {
		char			tm_str[64],
					p_fmt[260];
		struct timeval  	cur_tv;
		struct tm       	cur_tm;
		static const pid_t	cur_pid = getpid();

		gettimeofday(&cur_tv, 0);
		localtime_r(&cur_tv.tv_sec, &cur_tm);
		strftime(tm_str, 63, "[%%d]\t%Y-%m-%d %H:%M:%S.%%03d\t(%%s)\t", &cur_tm);
		snprintf(p_fmt, 259, tm_str, cur_pid, cur_tv.tv_usec/1000, type);

		return p_fmt;
	}
}

#define LOG_BASE(t, l)	if(!log::silent) { std::cerr << log::header(t) << l << std::endl; }

#define LOG_INFO(l)	LOG_BASE("INFO", l)
#define LOG_ERR(l)	LOG_BASE("ERR", l)
#define LOG_WARN(l)	LOG_BASE("WARN", l)

namespace {
	class errno_except : public std::exception {
		const std::string	w_;

		errno_except& operator=(const errno_except& e);

		static std::string fmt_exception(const std::string& s) {
			std::ostringstream	oss;
			oss << s << " (errno: " << errno << ", " << strerror(errno) << ")";

			return oss.str();
		}
	public:
		errno_except(const std::string& s) : w_(fmt_exception(s)) {
		}

		errno_except(const errno_except& e) : w_(e.w_) {
		}

		virtual const char* what() const noexcept {
			return w_.c_str();
		}
	};

	class tmp_dir {
		char	path_[64];
	public:
		tmp_dir() : path_("/dev/shm/emajail_XXXXXX") {
			if(!mkdtemp(path_))
				throw errno_except("Error in mkdtemp");
		}

		~tmp_dir() {
			// initial version, for now leave the overlays around...
			//system((std::string("rm -rf ") + path_).c_str());
		}

		const char* get(void) const {
			return path_;
		}
	};

	const static char	*HOME_DIR = "home",
				*PROC_DIR = "proc",
				*SKIP_DIRS[] = { PROC_DIR, "dev", "run", "mnt", "var", "sys" };

	// thread function and structures to overlay the FS
	// and setup the child process
	struct child_args {
		char	**argv,
			*overlayfs;
		int	pipe_fd[2];
		bool	empty_home,
			empty_proc,
			strict_mode;
	};

	void setup_overlays(const char* basepath, const bool empty_home, const bool empty_proc) {
		auto	is_skip_dir = [&](const char *p) -> bool {
			if(empty_home && !strcmp(p, HOME_DIR))
				return true;

			for(size_t i = 0; i < sizeof(SKIP_DIRS)/(sizeof(char*)); ++i)
				if(!strcmp(p, SKIP_DIRS[i]))
					return true;
			
			return false;
		};

		std::unique_ptr<DIR, void(*)(DIR*)>	root_dir(opendir("/"), [](DIR* p) -> void { closedir(p); });

		mkdir(basepath, 0755);
		struct dirent	*cur_el = 0;
		while((cur_el = readdir(root_dir.get()))) {
			// only choose directories (and not . and ..)
			if(!strcmp(cur_el->d_name, ".") || !strcmp(cur_el->d_name, ".."))
				continue;
			if(cur_el->d_type != DT_DIR)
				continue; 
			if(is_skip_dir(cur_el->d_name))
				continue;

			// prepare the directories
			auto	mount_func = [&basepath](const char *dir) -> void {
				const std::string	bdir = std::string("/") + dir,
							bpath = std::string(basepath) + "/" + dir,
							upper_d = bpath + "/upper",
							work_d = bpath + "/work",
							mount_opt = "lowerdir=" + bdir + ",upperdir=" + upper_d + ",workdir=" + work_d;
				mkdir(bpath.c_str(), 0755);
				mkdir(upper_d.c_str(), 0755);
				mkdir(work_d.c_str(), 0755);
				if(-1 == mount("overlay", bdir.c_str(), "overlay", MS_MGC_VAL, mount_opt.c_str()))
					throw errno_except("Error in mount");
			};
			LOG_INFO("Overlaying " << cur_el->d_name << " ...");
			mount_func(cur_el->d_name);
		}
		// this is technically not an overlay but similar
		// take care of creating an empty home
		if(empty_home) {
			const uid_t	u = getuid();
			const char	*home_dir = u ? "/home" : "/root",
					*mode_home = u ? "mode=755" : "mode=700";
			if(-1 == mount("tmpfs", home_dir, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_STRICTATIME | MS_REC, mode_home))
				throw errno_except("Error when mounting /home on tmpfs");
			// if we aren't root, create home also
			if(u) {
				const char	*cur_home = std::getenv("HOME");
				if(!cur_home)
					throw std::runtime_error("Environment variable $HOME not set");
				if(mkdir(cur_home, 0755))
					throw errno_except("Can't create /home/... directory");
			}
			LOG_INFO("Created empty /home for user " << u);
		}
		// similar as above, take care of setting up
		// a /proc fs
		if(empty_proc) {
			char	fproc[8] = {0};
			snprintf(fproc, 8, "/%s", PROC_DIR);
			if(-1 == mount("proc", fproc, "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_REC, NULL))
				throw errno_except("Can't create empty /proc directory");
		}
	}

	void setup_pulseaudio(void) {
		// first check if we have pulse installed
		static const char	pulse_path[] = "/etc/pulse/client.conf";
		std::ifstream		g_pulse(pulse_path);
		if(!g_pulse) {
			LOG_WARN("Pulseaudio not installed - skipping config creation");
			return;
		}
		// ok pulse is there in the system, create its directory
		// under home no matter what
		const char		*cur_home = std::getenv("HOME");
		if(!cur_home)
			throw std::runtime_error("Environment variable $HOME not set");
		const std::string	lcl_config = std::string(cur_home) + "/.config",
					lcl_pulse = lcl_config + "/pulse",
					lcl_pulse_f = lcl_pulse + "/client.conf";
		mkdir(lcl_config.c_str(), 0700);
		mkdir(lcl_pulse.c_str(), 0700);
		// check if we have the file
		struct stat		s = {0};
		const bool		l_pulse_exists = !stat(lcl_pulse_f.c_str(), &s);
		std::ofstream		l_pulse(lcl_pulse_f.c_str(), std::ios_base::app);
		if(!l_pulse_exists) {
			char	buf[2048];
			while(g_pulse) {
				g_pulse.read(buf, 2048);
				const int sz = g_pulse.gcount();
				l_pulse.write(buf, sz);
				if(!l_pulse)
					throw errno_except("Can't write into local pulse file");
			}
		}
		l_pulse << "\nenable-shm = no\n";
		LOG_INFO("Local pulse file created");
	}

	int child_func(void* p) {
		try {
			child_args	*args = reinterpret_cast<child_args*>(p);
			// wait for user setup to happen, we expect pipe_fd[0] to close only...
			char	ch = 0x00;
			close(args->pipe_fd[1]);
			if(read(args->pipe_fd[0], &ch, 1))
				throw std::runtime_error("Sync pipe not properly closed");

			// create overlays
			tmp_dir		t_dir;
			if(args->overlayfs) {
				setup_overlays(args->overlayfs, args->empty_home, args->empty_proc);
			} else {
				if(-1 == mount("tmpfs", t_dir.get(), "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_STRICTATIME | MS_REC, ""))
					throw errno_except("Error in mount tmpfs");
				setup_overlays(t_dir.get(), args->empty_home, args->empty_proc);
			}
			// setup Pulse
			if(args->strict_mode || args->empty_proc)
				setup_pulseaudio();

			// execute child process
			const pid_t	pid = fork();
			if(-1 == pid)
				throw errno_except("Error in fork");

			if(0 == pid) {
				execvp(args->argv[0], args->argv);
				return -1;
			} else {
				int	rc = 0;
				if(pid != wait(&rc))
					throw errno_except("Error in wait");
				LOG_INFO("Child process (" << args->argv[0] << ") exited with code " << rc);
				return 0;
			}
		} catch(const std::exception& e) {
			LOG_ERR("Exception: " << e.what());
			return -1;
		} catch(...) {
			LOG_ERR("Unknown exception");
			return -1;
		}
		return 0;
	}

	// simple function to update the user and group 
	// mappings, this has to be executed before
	// we execvp in the child thread/process
	void set_ugid(const pid_t child_pid, const uid_t uid, const gid_t gid) {
           	char		map_path[PATH_MAX];

		auto	w_func = [](const char* fname, const long id, const char *stropt = NULL) -> void {
			const size_t 	MAP_BUF_SIZE = 128;
			char		map_buf[MAP_BUF_SIZE];
			const int	n_chars = snprintf(map_buf, MAP_BUF_SIZE, "%ld %ld 1", id, id);
			std::ofstream	ostr_uid(fname, std::ios_base::binary);
			if(!ostr_uid)
				throw errno_except("Error in std::ofstream::<constructor>");
			if(!stropt) {
				if(!ostr_uid.write(map_buf, n_chars))
					throw errno_except("Error in std::ofstream::write");
			} else {
				if(!ostr_uid.write(stropt, strlen(stropt)))
					throw errno_except("Error in std::ofstream::write");
			}
		};
		
		// first user id
		snprintf(map_path, PATH_MAX, "/proc/%ld/uid_map", static_cast<long>(child_pid));
		w_func(map_path, static_cast<long>(uid));
		// then the groups "deny"
		snprintf(map_path, PATH_MAX, "/proc/%ld/setgroups", static_cast<long>(child_pid));
		w_func(map_path, -1, "deny");
		// last group id
		snprintf(map_path, PATH_MAX, "/proc/%ld/gid_map", static_cast<long>(child_pid));
		w_func(map_path, static_cast<long>(gid));
	}

	void skip_dirs(void) {
		std::cout << "Directories to not overlay:\n\n";
		for(size_t i = 0; i < sizeof(SKIP_DIRS)/sizeof(char*); ++i)
			std::cout << SKIP_DIRS[i] << '\n';
		std::cout << std::flush;
		std::exit(0);
	}

	void usage(const char* p) {
		std::cout << "Usage: " << p << " [options] cmd [arg...]\n" << std::endl;
		std::cout << "Create a child process in a sandboxed environment\n"
			  << "without modifying any existing file (using overlayFS)\n" << std::endl;
		std::cout << "Options can be:\n\n"
			  << "--empty-home              Sets up an empty /home on 'tmpfs'\n\n"
			  << "--empty-proc              Sets up an empty /proc on 'proc'. Please note\n"
			  << "                          this will set the flag CLONE_NEWPID and implies\n"
			  << "                          software may not work (PulseAudio, ...)\n\n"
			  << "-h,--help                 Print this screen and quit\n\n"
			  << "-j,--jail                 Quick combination as specifying the three options\n"
			  << "                          --empty-home, --empty-proc and -s\n\n"
			  << "-o,--overlay-dir path     Use a fixed path for overlayFS\n"
			  << "                          (otherwise on /dev/shm/emajail_XXXXXX)\n\n"
			  << "--skip-dirs               Print directories to not overlay and quit\n\n"
			  << "--silent                  Do not print any logline\n\n"
			  << "-s,--strict               Add IPC isolation level and create a new PID group; this\n"
			  << "                          option might imply some software to not work or\n"
			  << "                          fail at unexpected points, but increases security\n"
			  << "                          levels greatly\n\n"
			  << "";
		std::cout << std::flush;
		std::exit(0);
	}
}

int main(int argc, char *argv[]) {
	try {
		LOG_INFO("Starting emajail ...");
		child_args	ca = {NULL, NULL, {-1, -1}, false, false, false};
		// parse options
		{
			const static struct option	long_options[] = {
				{"empty-home",	no_argument,		0,  0   },
				{"empty-proc",	no_argument,		0,  0   },
				{"help",	no_argument, 		0,  'h' },
				{"jail",	no_argument,		0,  'j' },
				{"overlay-dir",	required_argument,	0,  'o' },
				{"skip-dirs",	no_argument,		0,  0 },
				{"silent",	no_argument,		0,  0   },
				{"strict",	no_argument,		0,  's' },
				{0,         	0,                 	0,  0 }
			};
			int	opt = -1,
				opt_idx = 0;
			while ((opt = getopt_long(argc, argv, "hjo:s", long_options, &opt_idx)) != -1) {
				switch (opt) {
				case 0: {
					if(!strcmp("silent", long_options[opt_idx].name)) log::silent = true;
					else if(!strcmp("empty-home", long_options[opt_idx].name)) ca.empty_home = true;
					else if(!strcmp("empty-proc", long_options[opt_idx].name)) ca.empty_proc = true;
					else if(!strcmp("skip-dirs", long_options[opt_idx].name)) skip_dirs();
				} break;
				case 'h': usage(argv[0]); 						break;
				case 'j': ca.empty_home = ca.empty_proc = ca.strict_mode = true;	break;
				case 'o': ca.overlayfs = optarg;					break;
				case 's': ca.strict_mode = true;					break;
				default:  throw std::runtime_error("Unknown option");
				}
			}
		}
		if(optind == argc)
			throw std::runtime_error("No program specified, exiting");
		ca.argv = &argv[optind];
		if(pipe(ca.pipe_fd) == -1)
			throw errno_except("Error in pipe");

		const size_t 	STACK_SIZE = 1024 * 1024;
		char 		child_stack[STACK_SIZE];
		int		clone_flags = CLONE_NEWNS | CLONE_NEWUSER | SIGCHLD;
		if(ca.empty_proc) {
			clone_flags |= CLONE_NEWPID;
		}
		if(ca.strict_mode) {
			clone_flags |= CLONE_NEWIPC;
			clone_flags |= CLONE_NEWPID;
		}
		// some warnings...
		if(clone_flags & CLONE_NEWPID) {
			LOG_WARN("CLONE_NEWPID flag has been added: not all software relying on same PID namespace may work (i.e. PulseAudio, ...)");
		}

		const pid_t	child_pid = clone(child_func, child_stack + STACK_SIZE, clone_flags, reinterpret_cast<void*>(&ca));
		if(-1 == child_pid)
			throw errno_except("Error in clone");

		// set the user and group ids and let 
		// child_func proceed
		set_ugid(child_pid, getuid(), getgid());
		close(ca.pipe_fd[1]);

		if(child_pid != wait(NULL))
			throw errno_except("Error in wait");
	} catch(const std::exception& e) {
		LOG_ERR("Exception: " << e.what());
		return -1;
	} catch(...) {
		LOG_ERR("Unknown exception");
		return -1;
	}
	return 0;
}

