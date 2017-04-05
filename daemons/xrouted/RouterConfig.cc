#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/param.h>
#include "RouterConfig.hh"

RouterConfig::RouterConfig()
{
	_hostname[0] = _appname[0] = _ident[0] = '\0';
	_verbose = false;
	_loglevel = LOG_ERR;
}

RouterConfig::~RouterConfig()
{
}

void RouterConfig::help()
{
	printf("\nusage: %s [-l level] [-v] [-c config] [-h hostname]\n", _appname);
	printf("where:\n");
	printf(" -l level    : syslog logging level 0 = LOG_EMERG ... 7 = LOG_DEBUG (default=3:LOG_ERR)\n");
	printf(" -v          : log to the console as well as syslog\n");
	printf(" -h hostname : click device name (default=`hostname -s`)\n");
	printf("\n");
}

int RouterConfig::parseCmdLine(int argc, char**argv)
{
	int rc = 0;
	int c;
	char *p;

	strncpy(_appname, basename(argv[0]), sizeof(_appname));

	gethostname(_hostname, sizeof(_hostname));
	if ((p = strchr(_hostname, '.')) != NULL) {
		*p = '\0';
	}

	opterr = 0;
	while ((c = getopt(argc, argv, "h:l:v")) != -1) {
		switch (c) {
			case 'h':
				strncpy(_hostname, optarg, sizeof(_hostname));
				break;
			case 'l':
				_loglevel = MIN(atoi(optarg), LOG_DEBUG);
				break;
			case 'v':
				_verbose = true;
				break;
			case '?':
			default:
				// Help Me!
				help();
				rc = -1;
				break;
		}
	}

	// create the syslog identifier
	snprintf(_ident, sizeof(_ident), "%s:%s", _hostname, _appname);

	return rc;
}