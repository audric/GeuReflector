/**
@file	 svxreflector.cpp
@brief   Main source file for the SvxReflector application
@author  Tobias Blomberg / SM0SVX
@date	 2017-02-11

The SvxReflector application is a central hub used to connect multiple SvxLink
nodes together.

\verbatim
SvxReflector - An audio reflector for connecting SvxLink Servers
Copyright (C) 2003-2025 Tobias Blomberg / SM0SVX

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/



/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <grp.h>
#include <pwd.h>
#include <dirent.h>
#include <termios.h>
#include <errno.h>

#include <popt.h>
#include <sigc++/sigc++.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <iomanip>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncCppApplication.h>
#include <AsyncFdWatch.h>
#include <AsyncConfig.h>
#include <config.h>
#include <LogWriter.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "version/SVXREFLECTOR.h"
#include "Reflector.h"
#include <hiredis/hiredis.h>
#include "RedisStore.h"


/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace std;
using namespace Async;


/****************************************************************************
 *
 * Defines & typedefs
 *
 ****************************************************************************/

#define PROGRAM_NAME "SvxReflector"


/****************************************************************************
 *
 * Local class definitions
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Prototypes
 *
 ****************************************************************************/

static void parse_arguments(int argc, const char **argv);
static void stdinHandler(FdWatch *w);
static void sighup_handler(int signal);
static void sigterm_handler(int signal);
static void handle_unix_signal(int signum);


/****************************************************************************
 *
 * Exported Global Variables
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local Global Variables
 *
 ****************************************************************************/

namespace {
  char*                 pidfile_name = nullptr;
  char*                 logfile_name = nullptr;
  char*                 runasuser = nullptr;
  char*                 config = nullptr;
  int                   daemonize = 0;
  int                   quiet = 0;
  FdWatch*              stdin_watch = 0;
  LogWriter             logwriter;
};

static int import_to_redis = 0;
static int dry_run = 0;


/****************************************************************************
 *
 * Local helper: import [USERS]/[PASSWORDS] from conf into Redis
 *
 ****************************************************************************/

static bool runImportConfToRedis(Async::Config& cfg, bool dry_run)
{
  RedisStore::Config rcfg;
  cfg.getValue("REDIS", "HOST", rcfg.host);
  std::string port_s; cfg.getValue("REDIS", "PORT", port_s);
  if (!port_s.empty()) rcfg.port = static_cast<uint16_t>(std::stoul(port_s));
  cfg.getValue("REDIS", "PASSWORD", rcfg.password);
  std::string db_s; cfg.getValue("REDIS", "DB", db_s);
  if (!db_s.empty()) rcfg.db = std::stoi(db_s);
  cfg.getValue("REDIS", "KEY_PREFIX", rcfg.key_prefix);
  cfg.getValue("REDIS", "UNIX_SOCKET", rcfg.unix_socket);
  if (rcfg.host.empty() && rcfg.unix_socket.empty()) {
    std::cerr << "*** ERROR: --import-conf-to-redis requires [REDIS] in conf"
              << std::endl;
    return false;
  }

  redisContext* ctx = nullptr;
  if (!dry_run) {
    struct timeval tv = { 5, 0 };
    ctx = rcfg.unix_socket.empty()
        ? redisConnectWithTimeout(rcfg.host.c_str(), rcfg.port, tv)
        : redisConnectUnixWithTimeout(rcfg.unix_socket.c_str(), tv);
    if (!ctx || ctx->err) {
      std::cerr << "*** ERROR: import: Redis connect failed: "
                << (ctx ? ctx->errstr : "alloc failed") << std::endl;
      if (ctx) redisFree(ctx);
      return false;
    }
    if (!rcfg.password.empty()) {
      redisReply* r = (redisReply*)redisCommand(ctx, "AUTH %s", rcfg.password.c_str());
      if (r) freeReplyObject(r);
    }
    if (rcfg.db != 0) {
      redisReply* r = (redisReply*)redisCommand(ctx, "SELECT %d", rcfg.db);
      if (r) freeReplyObject(r);
    }
  }

  auto kf = [&](const std::string& s) {
    return rcfg.key_prefix.empty() ? s : rcfg.key_prefix + ":" + s;
  };

  size_t users = 0, groups = 0;

  for (auto& cs : cfg.listSection("USERS")) {
    std::string group;
    if (!cfg.getValue("USERS", cs, group) || group.empty()) continue;
    std::string ukey = kf("user:" + cs);
    if (dry_run) {
      std::cout << "[dry-run] HSET " << ukey << " group " << group
                << " enabled 1" << std::endl;
    } else {
      redisReply* r = (redisReply*)redisCommand(ctx,
          "HSET %s group %s enabled 1", ukey.c_str(), group.c_str());
      if (r) freeReplyObject(r);
    }
    ++users;
  }

  for (auto& g : cfg.listSection("PASSWORDS")) {
    std::string pw;
    if (!cfg.getValue("PASSWORDS", g, pw) || pw.empty()) continue;
    std::string gkey = kf("group:" + g);
    if (dry_run) {
      std::cout << "[dry-run] HSET " << gkey << " password <REDACTED>"
                << std::endl;
    } else {
      redisReply* r = (redisReply*)redisCommand(ctx,
          "HSET %s password %s", gkey.c_str(), pw.c_str());
      if (r) freeReplyObject(r);
    }
    ++groups;
  }

  size_t cluster_n = 0;
  std::string cluster_str;
  if (cfg.getValue("GLOBAL", "CLUSTER_TGS", cluster_str))
  {
    std::istringstream ss(cluster_str);
    std::string tok;
    std::string ck = kf("cluster:tgs");
    while (std::getline(ss, tok, ','))
    {
      tok.erase(0, tok.find_first_not_of(" \t"));
      tok.erase(tok.find_last_not_of(" \t") + 1);
      if (tok.empty()) continue;
      if (dry_run) {
        std::cout << "[dry-run] SADD " << ck << " " << tok << std::endl;
      } else {
        redisReply* r = (redisReply*)redisCommand(ctx, "SADD %s %s",
                                                  ck.c_str(), tok.c_str());
        if (r) freeReplyObject(r);
      }
      ++cluster_n;
    }
  }

  size_t trunks_n = 0;
  std::list<std::string> sections = cfg.listSections();
  for (auto& sec : sections)
  {
    if (sec.rfind("TRUNK_", 0) != 0) continue;
    ++trunks_n;
    std::string trunk_label = sec.substr(6);  // "TRUNK_AB" → "AB"

    // Import the peer definition hash.
    std::string host, port_s_peer, secret, remote_prefix, peer_id;
    cfg.getValue(sec, "HOST",          host);
    cfg.getValue(sec, "PORT",          port_s_peer);
    cfg.getValue(sec, "SECRET",        secret);
    cfg.getValue(sec, "REMOTE_PREFIX", remote_prefix);
    cfg.getValue(sec, "PEER_ID",       peer_id);
    if (!host.empty() && !secret.empty() && !remote_prefix.empty())
    {
      std::string pk = kf("trunk:" + trunk_label + ":peer");
      auto setField = [&](const char* field, const std::string& value) {
        if (value.empty()) return;
        if (dry_run) {
          std::cout << "[dry-run] HSET " << pk << " " << field
                    << " " << (std::string(field) == "secret" ? "<REDACTED>" : value)
                    << std::endl;
        } else {
          redisReply* r = (redisReply*)redisCommand(ctx, "HSET %s %s %s",
              pk.c_str(), field, value.c_str());
          if (r) freeReplyObject(r);
        }
      };
      setField("host",          host);
      setField("port",          port_s_peer);
      setField("secret",        secret);
      setField("remote_prefix", remote_prefix);
      setField("peer_id",       peer_id);
    }

    auto importSet = [&](const std::string& field, const std::string& subkey) {
      std::string s;
      if (!cfg.getValue(sec, field, s) || s.empty()) return;
      std::string k = kf("trunk:" + trunk_label + ":" + subkey);
      std::istringstream ss(s);
      std::string tok;
      while (std::getline(ss, tok, ','))
      {
        tok.erase(0, tok.find_first_not_of(" \t"));
        tok.erase(tok.find_last_not_of(" \t") + 1);
        if (tok.empty()) continue;
        if (dry_run) {
          std::cout << "[dry-run] SADD " << k << " " << tok << std::endl;
        } else {
          redisReply* r = (redisReply*)redisCommand(ctx, "SADD %s %s",
                                                    k.c_str(), tok.c_str());
          if (r) freeReplyObject(r);
        }
      }
    };
    importSet("BLACKLIST_TGS", "blacklist");
    importSet("ALLOW_TGS",     "allow");

    std::string tgmap_str;
    if (cfg.getValue(sec, "TG_MAP", tgmap_str) && !tgmap_str.empty())
    {
      std::string k = kf("trunk:" + trunk_label + ":tgmap");
      std::istringstream ss(tgmap_str);
      std::string pair;
      while (std::getline(ss, pair, ','))
      {
        auto colon = pair.find(':');
        if (colon == std::string::npos) continue;
        std::string peer  = pair.substr(0, colon);
        std::string local = pair.substr(colon + 1);
        // Trim whitespace
        peer.erase(0, peer.find_first_not_of(" \t"));
        peer.erase(peer.find_last_not_of(" \t") + 1);
        local.erase(0, local.find_first_not_of(" \t"));
        local.erase(local.find_last_not_of(" \t") + 1);
        if (dry_run) {
          std::cout << "[dry-run] HSET " << k << " " << peer
                    << " " << local << std::endl;
        } else {
          redisReply* r = (redisReply*)redisCommand(ctx, "HSET %s %s %s",
              k.c_str(), peer.c_str(), local.c_str());
          if (r) freeReplyObject(r);
        }
      }
    }
  }

  std::cout << "Imported " << users << " users, " << groups << " groups, "
            << cluster_n << " cluster TGs, " << trunks_n << " trunks."
            << std::endl;

  if (ctx) redisFree(ctx);
  return true;
}


/****************************************************************************
 *
 * MAIN
 *
 ****************************************************************************/

/*
 *----------------------------------------------------------------------------
 * Function:  main
 * Purpose:   Start everything...
 * Input:     argc  - The number of arguments passed to this program
 *    	      	      (including the program name).
 *    	      argv  - The arguments passed to this program. argv[0] is the
 *    	      	      program name.
 * Output:    Return 0 on success, else non-zero.
 * Author:    Tobias Blomberg, SM0SVX
 * Created:   2017-02-11
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
int main(int argc, const char *argv[])
{
  setlocale(LC_ALL, "");

  CppApplication app;
  app.catchUnixSignal(SIGHUP);
  app.catchUnixSignal(SIGINT);
  app.catchUnixSignal(SIGTERM);
  app.unixSignalCaught.connect(sigc::ptr_fun(&handle_unix_signal));

  parse_arguments(argc, const_cast<const char **>(argv));

  if (daemonize && (daemon(1, 0) == -1))
  {
    perror("daemon");
    exit(1);
  }

  if (quiet || (logfile_name != 0))
  {
    int devnull = open("/dev/null", O_RDWR);
    if (devnull == -1)
    {
      perror("open(/dev/null)");
      exit(1);
    }

    if (quiet)
    {
        /* Redirect stdout to /dev/null */
      dup2(devnull, STDOUT_FILENO);
    }

    if (logfile_name != 0)
    {
      logwriter.setDestinationName(logfile_name);
      if (!quiet)
      {
        logwriter.redirectStdout();
      }
      logwriter.redirectStderr();
      logwriter.start();

        /* Redirect stdin to /dev/null */
      if (dup2(devnull, STDIN_FILENO) == -1)
      {
        perror("dup2(stdin)");
        exit(1);
      }
    }
    close(devnull);
  }

  if (pidfile_name != NULL)
  {
    FILE *pidfile = fopen(pidfile_name, "w");
    if (pidfile == 0)
    {
      char err[256];
      snprintf(err, sizeof(err), "fopen(\"%s\")", pidfile_name);
      perror(err);
      fflush(stderr);
      exit(1);
    }
    fprintf(pidfile, "%d\n", getpid());
    fclose(pidfile);
  }

  const char *home_dir = 0;
  if (runasuser != NULL)
  {
      // Setup supplementary group IDs
    if (initgroups(runasuser, getgid()))
    {
      perror("initgroups");
      exit(1);
    }

    struct passwd *passwd = getpwnam(runasuser);
    if (passwd == NULL)
    {
      perror("getpwnam");
      exit(1);
    }
    if (setgid(passwd->pw_gid) == -1)
    {
      perror("setgid");
      exit(1);
    }
    if (setuid(passwd->pw_uid) == -1)
    {
      perror("setuid");
      exit(1);
    }
    home_dir = passwd->pw_dir;
  }

  if (home_dir == 0)
  {
    home_dir = getenv("HOME");
  }
  if (home_dir == 0)
  {
    home_dir = ".";
  }

  Config cfg;
  string cfg_filename;
  if (config != NULL)
  {
    cfg_filename = string(config);
    if (!cfg.open(cfg_filename))
    {
      cerr << "*** ERROR: Could not open configuration file: "
           << config << endl;
      exit(1);
    }
  }
  else
  {
    cfg_filename = string(home_dir);
    cfg_filename += "/.svxlink/svxreflector.conf";
    if (!cfg.open(cfg_filename))
    {
      cfg_filename = SVX_SYSCONF_INSTALL_DIR "/svxreflector.conf";
      if (!cfg.open(cfg_filename))
      {
        cerr << "*** ERROR: Could not open configuration file";
        if (errno != 0)
        {
          cerr << " (" << strerror(errno) << ")";
        }
        cerr << ".\n";
        cerr << "Tried the following paths:\n"
             << "\t" << home_dir << "/.svxlink/svxreflector.conf\n"
             << "\t" SVX_SYSCONF_INSTALL_DIR "/svxreflector.conf\n"
             << "Possible reasons for failure are: None of the files exist,\n"
             << "you do not have permission to read the file or there was a\n"
             << "syntax error in the file.\n";
        exit(1);
      }
    }
  }
  string main_cfg_filename(cfg_filename);

  std::string main_cfg_dir = ".";
  auto slash_pos = main_cfg_filename.rfind('/');
  if (slash_pos != std::string::npos)
  {
    main_cfg_dir = main_cfg_filename.substr(0, slash_pos);
  }

  string cfg_dir;
  if (cfg.getValue("GLOBAL", "CFG_DIR", cfg_dir))
  {
    if (cfg_dir[0] != '/')
    {
      cfg_dir = main_cfg_dir + "/" + cfg_dir;
    }

    DIR *dir = opendir(cfg_dir.c_str());
    if (dir == NULL)
    {
      cerr << "*** ERROR: Could not read from directory spcified by "
           << "configuration variable GLOBAL/CFG_DIR=" << cfg_dir << endl;
      exit(1);
    }

    struct dirent *dirent;
    while ((dirent = readdir(dir)) != NULL)
    {
      char *dot = strrchr(dirent->d_name, '.');
      if ((dot == NULL) || (dirent->d_name[0] == '.') ||
          (strcmp(dot, ".conf") != 0))
      {
        continue;
      }
      cfg_filename = cfg_dir + "/" + dirent->d_name;
      if (!cfg.open(cfg_filename))
       {
	 cerr << "*** ERROR: Could not open configuration file: "
	      << cfg_filename << endl;
	 exit(1);
       }
    }

    if (closedir(dir) == -1)
    {
      cerr << "*** ERROR: Error closing directory specified by"
           << "configuration variable GLOBAL/CFG_DIR=" << cfg_dir << endl;
      exit(1);
    }
  }

  std::string tstamp_format = "%c";
  cfg.getValue("GLOBAL", "TIMESTAMP_FORMAT", tstamp_format);
  logwriter.setTimestampFormat(tstamp_format);

  cout << PROGRAM_NAME " v" SVXREFLECTOR_VERSION
          " Copyright (C) 2003-2025 Tobias Blomberg / SM0SVX\n";
  cout << "Modified with server-to-server trunk support by IW1GEU.\n\n";
  cout << PROGRAM_NAME " comes with ABSOLUTELY NO WARRANTY. "
          "This is free software, and you are\n";
  cout << "welcome to redistribute it in accordance with the "
          "terms and conditions in the\n";
  cout << "GNU GPL (General Public License) version 2 or later.\n";

  cout << "\nUsing configuration file: " << main_cfg_filename << endl;

  struct termios org_termios = {0};
  if (logfile_name == 0)
  {
    struct termios termios;
    tcgetattr(STDIN_FILENO, &org_termios);
    termios = org_termios;
    termios.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &termios);

    stdin_watch = new FdWatch(STDIN_FILENO, FdWatch::FD_WATCH_RD);
    stdin_watch->activity.connect(sigc::ptr_fun(&stdinHandler));
  }

  if (import_to_redis) {
    return runImportConfToRedis(cfg, dry_run != 0) ? 0 : 1;
  }

  Reflector ref;
  if (ref.initialize(cfg))
  {
    std::cout << "NOTICE: Initialization done. Starting main application."
              << std::endl;
    app.exec();
  }
  else
  {
    cerr << ":-(" << endl;
  }

  if (stdin_watch != 0)
  {
    delete stdin_watch;
    tcsetattr(STDIN_FILENO, TCSANOW, &org_termios);
  }

  return 0;
} /* main */


/****************************************************************************
 *
 * Functions
 *
 ****************************************************************************/

/*
 *----------------------------------------------------------------------------
 * Function:  parse_arguments
 * Purpose:   Parse the command line arguments.
 * Input:     argc  - Number of arguments in the command line
 *    	      argv  - Array of strings with the arguments
 * Output:    Returns 0 if all is ok, otherwise -1.
 * Author:    Tobias Blomberg, SM0SVX
 * Created:   2000-06-13
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
static void parse_arguments(int argc, const char **argv)
{
  int print_version = 0;

  poptContext optCon;
  const struct poptOption optionsTable[] =
  {
    POPT_AUTOHELP
    {"pidfile", 0, POPT_ARG_STRING, &pidfile_name, 0,
            "Specify the name of the pidfile to use", "<filename>"},
    {"logfile", 0, POPT_ARG_STRING, &logfile_name, 0,
            "Specify the logfile to use (stdout and stderr)", "<filename>"},
    {"runasuser", 0, POPT_ARG_STRING, &runasuser, 0,
            "Specify the user to run SvxLink as", "<username>"},
    {"config", 0, POPT_ARG_STRING, &config, 0,
	    "Specify the configuration file to use", "<filename>"},
    /*
    {"int_arg", 'i', POPT_ARG_INT, &int_arg, 0,
	    "Description of int argument", "<an int>"},
    */
    {"daemon", 0, POPT_ARG_NONE, &daemonize, 0,
	    "Start " PROGRAM_NAME " as a daemon", NULL},
    {"version", 0, POPT_ARG_NONE, &print_version, 0,
	    "Print the application version string", NULL},
    {"import-conf-to-redis", 0, POPT_ARG_NONE, &import_to_redis, 0,
            "Import [USERS]/[PASSWORDS] from .conf into Redis, then exit", NULL},
    {"dry-run", 0, POPT_ARG_NONE, &dry_run, 0,
            "With --import-conf-to-redis: print commands without executing", NULL},
    {NULL, 0, 0, NULL, 0}
  };
  int err;
  //const char *arg = NULL;
  //int argcnt = 0;

  optCon = poptGetContext(PROGRAM_NAME, argc, argv, optionsTable, 0);
  poptReadDefaultConfig(optCon, 0);

  err = poptGetNextOpt(optCon);
  if (err != -1)
  {
    fprintf(stderr, "\t%s: %s\n",
	    poptBadOption(optCon, POPT_BADOPTION_NOALIAS),
	    poptStrerror(err));
    exit(1);
  }

  /*
  printf("string_arg  = %s\n", string_arg);
  printf("int_arg     = %d\n", int_arg);
  printf("bool_arg    = %d\n", bool_arg);
  */

    /* Parse arguments that do not begin with '-' (leftovers) */
  /*
  arg = poptGetArg(optCon);
  while (arg != NULL)
  {
    printf("arg %2d      = %s\n", ++argcnt, arg);
    arg = poptGetArg(optCon);
  }
  */

  poptFreeContext(optCon);

  if (print_version)
  {
    std::cout << SVXREFLECTOR_VERSION << std::endl;
    exit(0);
  }
} /* parse_arguments */


static void stdinHandler(FdWatch *w)
{
  char buf[1];
  int cnt = ::read(STDIN_FILENO, buf, 1);
  if (cnt == -1)
  {
    fprintf(stderr, "*** ERROR: Reading from stdin failed\n");
    Application::app().quit();
    return;
  }
  else if (cnt == 0)
  {
      /* Stdin file descriptor closed */
    delete stdin_watch;
    stdin_watch = 0;
    return;
  }

  switch (toupper(buf[0]))
  {
    case 'Q':
      Application::app().quit();
      break;

    case '\n':
      putchar('\n');
      break;

    default:
      break;
  }
} /* stdinHandler */


static void sighup_handler(int signal)
{
  if (logfile_name == 0)
  {
    cout << "Ignoring SIGHUP\n";
    return;
  }
  std::cout << "SIGHUP received" << std::endl;
  logwriter.reopenLogfile();
} /* sighup_handler */


static void sigterm_handler(int signal)
{
  const char *signame = 0;
  switch (signal)
  {
    case SIGTERM:
      signame = "SIGTERM";
      break;
    case SIGINT:
      signame = "SIGINT";
      break;
    default:
      signame = "Unknown signal";
      break;
  }

  std::cout << "\nNOTICE: " << signame
            << " received. Shutting down application..."
            << std::endl;
  Application::app().quit();
} /* sigterm_handler */


static void handle_unix_signal(int signum)
{
  switch (signum)
  {
    case SIGHUP:
      sighup_handler(signum);
      break;
    case SIGINT:
    case SIGTERM:
      sigterm_handler(signum);
      break;
  }
} /* handle_unix_signal */


/*
 * This file has not been truncated
 */

