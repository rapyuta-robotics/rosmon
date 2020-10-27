// rosmon - versatile ROS node launcher & monitor
// Author: Max Schwarz <max.schwarz@uni-bonn.de>

#include <ros/init.h>
#include <ros/master.h>
#include <ros/rate.h>
#include <ros/node_handle.h>
#include <ros/package.h>
#include <ros/console_backend.h>
#include <ros/this_node.h>

#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <getopt.h>
#include <csignal>

#include <iostream>

#include "launch/launch_config.h"
#include "launch/bytes_parser.h"
#include "monitor/monitor.h"
#include "ui.h"
#include "ros_interface.h"
#include "package_registry.h"
#include "fd_watcher.h"
#include "logger.h"
#include "fmt_no_throw.h"

namespace fs = boost::filesystem;

bool g_shouldStop = false;
bool g_flushStdout = false;

static fs::path findFile(const fs::path& base, const std::string& name)
{
	for(fs::directory_iterator it(base); it != fs::directory_iterator(); ++it)
	{
		if(fs::is_directory(*it))
		{
			fs::path p = findFile(*it, name);
			if(!p.empty())
				return p;
		}
		else if(it->path().leaf() == name)
			return *it;
	}

	return fs::path();
}

void usage()
{
	fprintf(stderr,
		"Usage:\n"
		"  rosmon [actions] [options] some_package test.launch [arg1:=value1 ...]\n"
		"  rosmon [actions] [options] path/to/test.launch [arg1:=value1 ...]\n"
		"\n"
		"Actions (default is to launch the launch file):\n"
		"  --benchmark     Exit after loading the launch file\n"
		"  --list-args     List launch file arguments\n"
		"\n"
		"Options:\n"
		"  --disable-ui    Disable fancy terminal UI\n"
		"  --flush-log     Flush logfile after writing an entry\n"
		"  --flush-stdout  Flush stdout after writing an entry\n"
		"  --help	  This help screen\n"
		"  --log=DIR       Write log file to file in DIR\n"
		"  --name=NAME     Use NAME as ROS node name. By default, an anonymous\n"
		"		  name is chosen.\n"
		"  --robot=ROBOT  Use ROBOT as name of robot publishing. By default, empty\n"
		"  --launch-group=GROUP\n"
		"		 Use GROUP as name of the launch group. By default, empty\n"
		"  --launch-config=CONFIG\n"
		"		 Use CONFIG as name of the launch config. By default, empty\n"
		"  --respawn-attr=obey_default_true|obey_default_false|force_true|force_false\n"
		"		  Force all nodes in launch group to respawn or not respawn,\n"
		"		  or obey launch file and default to true/false if not \n"
		"		  specified in launch file. By default, nodes will obey and \n"
		"		  default to false.\n"
		"  --no-start      Don't automatically start the nodes in the beginning\n"
		"  --stop-timeout=SECONDS\n"
		"		  Kill a process if it is still running this long\n"
		"		  after the initial signal is send.\n"
		"  --disable-diagnostics\n"
		"		  Disable publication of ros diagnostics message about\n"
		"		  monitored nodes\n"
		"  --diagnostics-prefix=PREFIX\n"
		"		  Prefix for the ros diagnostics generated by this node.\n"
		"		  By default this will be the node name.\n"
		"  --cpu-limit=[0-n]\n"
		"		  Default CPU Usage limit of monitored process. n is the\n"
		"		  number of CPU cores. This is the sum of system and user\n"
		"		  CPU usage.\n"
		"  --memory-limit=15MB\n"
		"		  Default memory limit usage of monitored process.\n"
		"\n"
		"rosmon also obeys some environment variables:\n"
		"  ROSMON_COLOR_MODE   Can be set to 'truecolor', '256colors', 'ansi'\n"
		"		      to force a specific color mode\n"
		"		      If unset, rosmon tries to detect the best\n"
		"		      available color mode.\n"
	);
}

void handleSignal(int)
{
	g_shouldStop = true;
}

void logToStdout(const rosmon::LogEvent& event)
{
	std::string clean = event.message;
	unsigned int len = clean.length();
	while(len != 0 && (clean[len-1] == '\n' || clean[len-1] == '\r'))
		len--;

	clean.resize(len);

	fmtNoThrow::print("{:>20}: {}\n", event.source, clean);

	if(g_flushStdout)
		fflush(stdout);
}

// Options
static const struct option OPTIONS[] = {
	{"disable-ui", no_argument, nullptr, 'd'},
	{"benchmark", no_argument, nullptr, 'b'},
	{"flush-log", no_argument, nullptr, 'f'},
	{"flush-stdout", no_argument, nullptr, 'F'},
	{"help", no_argument, nullptr, 'h'},
	{"list-args", no_argument, nullptr, 'L'},
	{"log",  required_argument, nullptr, 'l'},
	{"name", required_argument, nullptr, 'n'},
	{"robot", required_argument, nullptr, 'r'},
	{"launch-group", required_argument, nullptr, 'g'},
	{"launch-config", required_argument, nullptr, 'z'},
	{"respawn-attr", required_argument, nullptr, 'R'},
	{"no-start", no_argument, nullptr, 'S'},
	{"stop-timeout", required_argument, nullptr, 's'},
	{"disable-diagnostics", no_argument, nullptr, 'D'},
	{"cpu-limit", required_argument, nullptr, 'c'},
	{"memory-limit", required_argument, nullptr, 'm'},
	{"diagnostics-prefix", required_argument, nullptr, 'p'},
	{nullptr, 0, nullptr, 0}
};

enum Action {
	ACTION_LAUNCH,
	ACTION_BENCHMARK,
	ACTION_LIST_ARGS,
};

int main(int argc, char** argv)
{
	std::string name;
	rosmon::LaunchInfo launchInfo;
	std::string logDir;
	std::string launchFilePath;

	Action action = ACTION_LAUNCH;
	bool enableUI = true;
	bool flushLog = false;
	bool respawnAll = false;
	bool respawnObey = true;
	bool respawnDefault = false;
	bool startNodes = true;
	double stopTimeout = rosmon::launch::LaunchConfig::DEFAULT_STOP_TIMEOUT;
	uint64_t memoryLimit = rosmon::launch::LaunchConfig::DEFAULT_MEMORY_LIMIT;
	float cpuLimit = rosmon::launch::LaunchConfig::DEFAULT_CPU_LIMIT;
	bool disableDiagnostics = false;
	std::string diagnosticsPrefix;

	// Parse options
	while(true)
	{
		int option_index;
		int c = getopt_long(argc, argv, "h", OPTIONS, &option_index);

		if(c == -1)
			break;

		switch(c)
		{
			case 'h':
				usage();
				return 0;
			case 'n':
				name = optarg;
				break;
			case 'r':
				launchInfo.robot_name = optarg;
				break;
			case 'g':
				launchInfo.launch_group = optarg;
				break;
			case 'z':
				launchInfo.launch_config = optarg;
				break;
			case 'l':
				logDir = optarg;
				break;
			case 'L':
				action = ACTION_LIST_ARGS;
				break;
			case 'b':
				action = ACTION_BENCHMARK;
				break;
			case 'd':
				enableUI = false;
				break;
			case 'f':
				flushLog = true;
				break;
			case 'F':
				g_flushStdout = true;
				break;
			case 'S':
				startNodes = false;
				break;
			case 's':
				try
				{
					stopTimeout = boost::lexical_cast<double>(optarg);
				}
				catch(boost::bad_lexical_cast&)
				{
					fmtNoThrow::print(stderr, "Bad value for --stop-timeout argument: '{}'\n", optarg);
					return 1;
				}

				if(stopTimeout < 0)
				{
					fmtNoThrow::print(stderr, "Stop timeout cannot be negative\n");
					return 1;
				}
				break;
			case 'D' :
				disableDiagnostics = true;
				break;
			case 'c':
				try
				{
					cpuLimit = boost::lexical_cast<float>(optarg);
				}
				catch(boost::bad_lexical_cast&)
				{
					fmtNoThrow::print(stderr, "Bad value for --cpu-limit argument: '{}'\n", optarg);
					return 1;
				}

				if(cpuLimit < 0)
				{
					fmtNoThrow::print(stderr, "CPU Limit cannot be negative\n");
					return 1;
				}
				break;
			case 'm':
			{
				bool ok;
				std::tie(memoryLimit, ok) = rosmon::launch::parseMemory(optarg);
				if(!ok)
				{
					fmtNoThrow::print(stderr, "Bad value for --memory-limit argument: '{}'\n", optarg);
					return 1;
				}
				break;
			}
			case 'p':
				fmtNoThrow::print(stderr, "Prefix : {}", optarg);
				diagnosticsPrefix = std::string(optarg);
				break;
			case 'R':
				if (optarg && (strcmp(optarg,"force_true") == 0 || strcmp(optarg,"force_false") == 0))
				{
					respawnAll = strcmp(optarg,"force_true") == 0;
					respawnObey = false;
				}
				else if (optarg && (strcmp(optarg,"obey_default_true") == 0 || strcmp(optarg,"obey_default_false") == 0))
				{
					respawnDefault = strcmp(optarg,"obey_default_true") == 0;
				}
				else
				{
					fmtNoThrow::print(stderr, "Bad value for --respawn-attr argument: '{}'\n", optarg);
					return 1;
				}
				break;
		}
	}

	// Parse the positional arguments
	if(optind == argc)
	{
		usage();
		return 1;
	}

	// Find first launch file argument (must contain ':=')
	int firstArg = optind + 1;
	for(; firstArg < argc; ++firstArg)
	{
		if(strstr(argv[firstArg], ":="))
			break;
	}

	// From the position of the argument (or the end-of-options), we know
	// if we were called with a) package + filename or b) just a path.

	if(firstArg - optind == 1)
	{
		launchFilePath = argv[optind];
	}
	else if(firstArg - optind == 2)
	{
		const char* packageName = argv[optind];
		const char* fileName = argv[optind + 1];

		std::string package = rosmon::PackageRegistry::getPath(packageName);
		if(package.empty())
		{
			fmtNoThrow::print(stderr, "Could not find path of package '{}'\n", packageName);
			return 1;
		}

		fs::path path = findFile(package, fileName);
		if(path.empty())
		{
			fmtNoThrow::print(stderr, "Could not find launch file '{}' in package '{}'\n",
				fileName, packageName
			);
			return 1;
		}

		launchFilePath = path.string();
	}
	else
	{
		usage();
		return 1;
	}

	// Setup logging
	boost::scoped_ptr<rosmon::Logger> logger;
	std::string workDir;
	{
		// Setup a sane ROSCONSOLE_FORMAT if the user did not already
		setenv("ROSCONSOLE_FORMAT", "[${function}] [${time}]: ${message}", 0);

		// Disable direct logging to stdout
		ros::console::backend::function_print = nullptr;

		std::string logFile; 

		// Open logger
		if(!logDir.empty())
		{
			if (logDir.back() == '/')
			{
				logDir.pop_back();
			}
			workDir = logDir + "/rosmon";
			if (chdir(workDir.c_str()) == 0 || mkdir(workDir.c_str(), 0777) == 0) 
			{
				logDir = workDir + "/core_dumps";
				if (!(chdir(logDir.c_str()) == 0 || mkdir(logDir.c_str(), 0777) == 0))
				{
					fmtNoThrow::print(stderr, "Could not create rosmon/core_dumps directory\n");
				}

				logDir = workDir + "/roslogs";
				if (chdir(logDir.c_str()) == 0 || mkdir(logDir.c_str(), 0777) == 0) 
				{
					logFile = logDir + "/" + launchInfo.launch_group + "_" + launchInfo.launch_config + ".log";
				}
				else
				{
					fmtNoThrow::print(stderr, "Could not create rosmon/roslogs directory\n");
				}
			}
			else
			{
				fmtNoThrow::print(stderr, "Could not create rosmon directory\n");
			}
		} 
		if (logFile.empty())
		{
			// Log to /tmp by default

			time_t t = time(nullptr);
			tm currentTime;
			memset(&currentTime, 0, sizeof(currentTime));
			localtime_r(&t, &currentTime);

			char buf[256];
			strftime(buf, sizeof(buf), "/tmp/rosmon_%Y_%m_%d_%H_%M_%S.log", &currentTime);

			logFile = buf;
		}
		fmtNoThrow::print("Creating logfile {}\n", logFile);
		logger.reset(new rosmon::Logger(logFile, flushLog));
	}

	rosmon::FDWatcher::Ptr watcher(new rosmon::FDWatcher);

	rosmon::launch::LaunchConfig::Ptr config(new rosmon::launch::LaunchConfig);
	config->setDefaultStopTimeout(stopTimeout);
	config->setDefaultCPULimit(cpuLimit);
	config->setDefaultMemoryLimit(memoryLimit);
	config->setWorkingDirectory(workDir);
	config->setRespawnBehaviour(respawnAll, respawnObey, respawnDefault);

	// Parse launch file arguments from command line
	for(int i = firstArg; i < argc; ++i)
	{
		char* arg = strstr(argv[i], ":=");

		if(!arg)
		{
			fmtNoThrow::print(stderr, "You specified a non-argument after an argument\n");
			return 1;
		}

		char* name = argv[i];

		*arg = 0;

		char* value = arg + 2;

		config->setArgument(name, value);
	}

	bool onlyArguments = (action == ACTION_LIST_ARGS);

	try
	{
		config->parse(launchFilePath, onlyArguments);
		config->evaluateParameters();
	}
	catch(rosmon::launch::ParseException& e)
	{
		fmtNoThrow::print(stderr, "Could not load launch file: {}\n", e.what());
		return 1;
	}

	switch(action)
	{
		case ACTION_BENCHMARK:
			return 0;
		case ACTION_LIST_ARGS:
			for(const auto& arg : config->arguments())
				std::cout << arg.first << std::endl;

			return 0;
		case ACTION_LAUNCH:
			break;
	}

	// Initialize the ROS node.
	{
		uint32_t init_options = ros::init_options::NoSigintHandler;

		// If the user did not specify a node name on the command line, look in
		// the launch file
		if(name.empty())
			name = config->rosmonNodeName();

		// As last fallback, choose anonymous name.
		if(name.empty())
		{
			name = "rosmon_" + launchInfo.launch_group + "_" + launchInfo.launch_config + "_anon";
			init_options |= ros::init_options::AnonymousName;
		}

		// prevent ros::init from parsing the launch file arguments as remappings
		int dummyArgc = 1;
		ros::init(dummyArgc, argv, name, init_options);
	}

	// Check connectivity to ROS master
	{
		fmtNoThrow::print("ROS_MASTER_URI: '{}'\n", ros::master::getURI());
		if(ros::master::check())
		{
			fmtNoThrow::print("roscore is already running.\n");
		}
		else
		{
			fmtNoThrow::print("roscore is not runnning.\n");
			fmtNoThrow::print("Waiting until it is up (abort with CTRL+C)...\n");

			while(!ros::master::check())
				ros::WallDuration(0.5).sleep();

			fmtNoThrow::print("roscore is running now.\n");
		}
	}

	ros::NodeHandle nh;

	fmtNoThrow::print("Running as '{}'\n", ros::this_node::getName());

	rosmon::monitor::Monitor monitor(config, watcher, logDir, flushLog, launchInfo.launch_group, launchInfo.launch_config);
	monitor.logMessageSignal.connect(boost::bind(&rosmon::Logger::log, logger.get(), _1));

	fmtNoThrow::print("\n\n");
	monitor.setParameters();

	if(config->nodes().empty())
	{
		fmtNoThrow::print("No ROS nodes to be launched. Finished...\n");
		return 0;
	}

	// Should we automatically start the nodes?
	if(startNodes)
		monitor.start();

	// Start the ncurses UI
	boost::scoped_ptr<rosmon::UI> ui;
	if(enableUI)
	{
		ui.reset(new rosmon::UI(&monitor, watcher));
	}
	else
	{
		for(auto& node : monitor.nodes())
		{
			node->logMessageSignal.connect(logToStdout);
		}
	}

	// ROS interface
	rosmon::ROSInterface rosInterface(&monitor, &launchInfo, !disableDiagnostics, diagnosticsPrefix);

	ros::WallDuration waitDuration(0.1);

	// On SIGINT, SIGTERM, SIGHUP we stop gracefully.
	signal(SIGINT, handleSignal);
	signal(SIGHUP, handleSignal);
	signal(SIGTERM, handleSignal);

	// Main loop
	while(ros::ok() && monitor.ok() && !g_shouldStop)
	{
		ros::spinOnce();
		watcher->wait(waitDuration);

		if(ui)
			ui->update();
	}

	if(ui)
		ui->log({"[rosmon]", "Shutting down..."});
	monitor.shutdown();

	// Wait for graceful shutdown
	ros::WallTime start = ros::WallTime::now();
	while(!monitor.allShutdown() && ros::WallTime::now() - start < ros::WallDuration(monitor.shutdownTimeout()))
	{
		watcher->wait(waitDuration);

		if(ui)
			ui->update();
	}

	// If we timed out, force exit (kill the nodes with SIGKILL)
	if(!monitor.allShutdown())
		monitor.forceExit();

	rosInterface.shutdown();

	// Wait until that is finished (should always work)
	while(!monitor.allShutdown())
	{
		watcher->wait(waitDuration);

		if(enableUI)
			ui->update();
	}

	// If coredumps are available, be helpful and display gdb commands
	bool coredumpsAvailable = std::any_of(monitor.nodes().begin(), monitor.nodes().end(),
		[](const rosmon::monitor::NodeMonitor::Ptr& n) { return n->coredumpAvailable(); }
	);

	if(ui && coredumpsAvailable)
	{
		ui->log({"[rosmon]", "\n"});
		ui->log({"[rosmon]", "If you want to debug one of the crashed nodes, you can use the following commands"});
		for(auto& node : monitor.nodes())
		{
			if(node->coredumpAvailable())
			{
				ui->log({
					"[rosmon]",
					fmt::format("{:20}: # {}", node->name(), node->debuggerCommand())
				});
			}
		}
	}

	return 0;
}
