/*
   mountinfo.c
   Obtains information about mounted filesystems.

   Copyright 2007 Gentoo Foundation
   */

#define APPLET "mountinfo"

#include <sys/types.h>

#if defined(__DragonFly__) || defined(__FreeBSD__) || \
	defined(__NetBSD__) || defined(__OpenBSD__)
#define BSD
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#endif

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

#include "builtins.h"
#include "einfo.h"
#include "rc.h"
#include "rc-misc.h"
#include "strlist.h"

typedef enum {
	mount_from,
	mount_to,
	mount_fstype,
	mount_options
} mount_type;

struct args {
	regex_t *node_regex;
	regex_t *skip_node_regex;
	regex_t *fstype_regex;
	regex_t *skip_fstype_regex;
	regex_t *options_regex;
	regex_t *skip_options_regex;
	char **mounts;
	mount_type mount_type;
};

static int process_mount (char ***list, struct args *args,
						  char *from, char *to, char *fstype, char *options)
{
	char *p;

	errno = ENOENT;

#ifdef __linux__
	/* Skip the really silly rootfs */
	if (strcmp (fstype, "rootfs") == 0)
		return (-1);
#endif

	if (args->node_regex &&
		regexec (args->node_regex, from, 0, NULL, 0) != 0)
		return (1);
	if (args->skip_node_regex &&
		regexec (args->skip_node_regex, from, 0, NULL, 0) == 0)
		return (1);

	if (args->fstype_regex &&
		regexec (args->fstype_regex, fstype, 0, NULL, 0) != 0)
		return (-1);
	if (args->skip_fstype_regex &&
		regexec (args->skip_fstype_regex, fstype, 0, NULL, 0) == 0)
		return (-1);

	if (args->options_regex &&
		regexec (args->options_regex, options, 0, NULL, 0) != 0)
		return (-1);
	if (args->skip_options_regex &&
		regexec (args->skip_options_regex, options, 0, NULL, 0) == 0)
		return (-1);

	if (args->mounts)   {
		bool found = false;
		int j;
		char *mnt;
		STRLIST_FOREACH (args->mounts, mnt, j)
			if (strcmp (mnt, to) == 0) {
				found = true;
				break;
			}
		if (! found)
			return (-1);
	}

	switch (args->mount_type) {
		case mount_from:
			p = from;
			break;
		case mount_to:
			p = to;
			break;
		case mount_fstype:
			p = fstype;
			break;
		case mount_options:
			p = options;
			break;
		default:
			p = NULL;
			errno = EINVAL;
			break;
	}

	if (p) {
		errno = 0;
		rc_strlist_addsortc (list, p);
		return (0);
	}

	return (-1);
}

#ifdef BSD 

/* Translate the mounted options to english
 * This is taken directly from FreeBSD mount.c */
static struct opt {
	int o_opt;
	const char *o_name;
} optnames[] = {
	{ MNT_ASYNC,        "asynchronous" },
	{ MNT_EXPORTED,     "NFS exported" },
	{ MNT_LOCAL,        "local" },
	{ MNT_NOATIME,      "noatime" },
	{ MNT_NOEXEC,       "noexec" },
	{ MNT_NOSUID,       "nosuid" },
	{ MNT_NOSYMFOLLOW,  "nosymfollow" },
	{ MNT_QUOTA,        "with quotas" },
	{ MNT_RDONLY,       "read-only" },
	{ MNT_SYNCHRONOUS,  "synchronous" },
	{ MNT_UNION,        "union" },
	{ MNT_NOCLUSTERR,   "noclusterr" },
	{ MNT_NOCLUSTERW,   "noclusterw" },
	{ MNT_SUIDDIR,      "suiddir" },
	{ MNT_SOFTDEP,      "soft-updates" },
	{ MNT_MULTILABEL,   "multilabel" },
	{ MNT_ACLS,         "acls" },
#ifdef MNT_GJOURNAL
	{ MNT_GJOURNAL,     "gjournal" },
#endif
	{ 0, NULL }
};

static char **find_mounts (struct args *args)
{
	struct statfs *mnts;
	int nmnts;
	int i;
	char **list = NULL;
	char *options = NULL;
	int flags;
	struct opt *o;

	if ((nmnts = getmntinfo (&mnts, MNT_NOWAIT)) == 0)
		eerrorx ("getmntinfo: %s", strerror (errno));

	for (i = 0; i < nmnts; i++) {
		flags = mnts[i].f_flags & MNT_VISFLAGMASK;
		for (o = optnames; flags && o->o_opt; o++) {
			if (flags & o->o_opt) {
				if (! options)
					options = rc_xstrdup (o->o_name);
				else {
					char *tmp = NULL;
					asprintf (&tmp, "%s,%s", options, o->o_name);
					free (options);
					options = tmp;
				}
			}
			flags &= ~o->o_opt;
		}

		process_mount (&list, args,
					   mnts[i].f_mntfromname,
					   mnts[i].f_mntonname,
					   mnts[i].f_fstypename,
					   options);

		free (options);
		options = NULL;
	}

	return (list);
}

#elif defined (__linux__)
static char **find_mounts (struct args *args)
{
	FILE *fp;
	char buffer[PATH_MAX * 3];
	char *p;
	char *from;
	char *to;
	char *fst;
	char *opts;
	char **list = NULL;

	if ((fp = fopen ("/proc/mounts", "r")) == NULL)
		eerrorx ("getmntinfo: %s", strerror (errno));

	while (fgets (buffer, sizeof (buffer), fp)) {
		p = buffer;
		from = strsep (&p, " ");
		to = strsep (&p, " ");
		fst = strsep (&p, " ");
		opts = strsep (&p, " ");

		process_mount (&list, args, from, to, fst, opts);
	}
	fclose (fp);

	return (list);
}

#else
#  error "Operating system not supported!"
#endif

static regex_t *get_regex (char *string)
{
	regex_t *reg = rc_xmalloc (sizeof (regex_t));
	int result;
	char buffer[256];

	if ((result = regcomp (reg, string, REG_EXTENDED | REG_NOSUB)) != 0)
	{
		regerror (result, reg, buffer, sizeof (buffer));
		eerrorx ("%s: invalid regex `%s'", APPLET, buffer);
	}

	return (reg);
}

#include "_usage.h"
#define getoptstring "f:F:n:N:o:O:p:P:iqst" getoptstring_COMMON
static struct option longopts[] = {
	{ "fstype-regex",        1, NULL, 'f'},
	{ "skip-fstype-regex",   1, NULL, 'F'},
	{ "node-regex",          1, NULL, 'n'},
	{ "skip-node-regex",     1, NULL, 'N'},
	{ "options-regex",       1, NULL, 'o'},
	{ "skip-options-regex",  1, NULL, 'O'},
	{ "point-regex",         1, NULL, 'p'},
	{ "skip-point-regex",    1, NULL, 'P'},
	{ "options",             0, NULL, 'i'},
	{ "fstype",              0, NULL, 's'},
	{ "node",                0, NULL, 't'},
	longopts_COMMON
	{ NULL,             0, NULL, 0}
};
#include "_usage.c"

int mountinfo (int argc, char **argv)
{
	int i;
	struct args args;
	regex_t *point_regex = NULL;
	regex_t *skip_point_regex = NULL;
	char **nodes = NULL;
	char *n;
	int opt;
	int result;

#define DO_REG(_var) \
	if (_var) free (_var); \
	_var = get_regex (optarg);

	memset (&args, 0, sizeof (struct args));
	args.mount_type = mount_to;

	while ((opt = getopt_long (argc, argv, getoptstring,
							   longopts, (int *) 0)) != -1)
	{
		switch (opt) {
			case 'f':
				DO_REG (args.fstype_regex);
				break;
			case 'F':
				DO_REG (args.skip_fstype_regex);
				break;
			case 'n':
				DO_REG (args.node_regex);
				break;
			case 'N':
				DO_REG (args.skip_node_regex);
				break;
			case 'o':
				DO_REG (args.options_regex);
				break;
			case 'O':
				DO_REG (args.skip_options_regex);
				break;
			case 'p':
				DO_REG (point_regex);
				break;
			case 'P':
				DO_REG (skip_point_regex);
				break;
			case 'i':
				args.mount_type = mount_options;
				break;
			case 's':
				args.mount_type = mount_fstype;
				break;
			case 't':
				args.mount_type = mount_from;
				break;

				case_RC_COMMON_GETOPT
		}
	}

	while (optind < argc) {
		if (argv[optind][0] != '/')
			eerrorx ("%s: `%s' is not a mount point", argv[0], argv[optind]);
		rc_strlist_add (&args.mounts, argv[optind++]);
	}

	nodes = find_mounts (&args);

	if (args.fstype_regex)
		regfree (args.fstype_regex);
	if (args.skip_fstype_regex)
		regfree (args.skip_fstype_regex);
	if (args.node_regex)
		regfree (args.node_regex);
	if (args.skip_node_regex)
		regfree (args.skip_node_regex);
	if (args.options_regex)
		regfree (args.options_regex);
	if (args.skip_options_regex)
		regfree (args.skip_options_regex);
	
	rc_strlist_reverse (nodes);

	result = EXIT_FAILURE;
	STRLIST_FOREACH (nodes, n, i) {
		if (point_regex && regexec (point_regex, n, 0, NULL, 0) != 0)
			continue;
		if (skip_point_regex && regexec (skip_point_regex, n, 0, NULL, 0) == 0)
			continue;
		if (! rc_is_env ("RC_QUIET", "yes"))
			printf ("%s\n", n);
		result = EXIT_SUCCESS;
	}
	rc_strlist_free (nodes);

	if (point_regex)
		regfree (point_regex);
	if (skip_point_regex)
		regfree (skip_point_regex);

	exit (result);
}
