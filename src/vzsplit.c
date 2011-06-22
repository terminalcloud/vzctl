/*
 *  Copyright (C) 2000-2009, Parallels, Inc. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "types.h"
#include "config.h"

#include "logger.h"
#include "util.h"

/* #include <linux/magic.h>
 * Instead of including a file which is not present in all environments,
 * we copy-paste reiserfs magic from it
 */
#define REISERFS_SUPER_MAGIC	0x52654973

#define SYSRSRV		52428800
#define MEMPERVE	5542912
#define LOWPERVE	1348608
#define MAXVAL		LONG_MAX
#define DEF_CPUUNITS	1000
#define CHECK_LIMIT(val)	(val > MAXVAL) ? MAXVAL: val

#define PROCMEM		"/proc/meminfo"
#define PROCTHREADS	"/proc/sys/kernel/threads-max"

#define MAX_SL		3

/* maximal per system values */
#define MAX_TOTAL_PIDS	8000
#define MAXIPTENT	2000

/* minimal per CT values */
#define MINPROC		30
#define MINTCPBUF	65536
#define MINDGRAMBUF	32768
#define MINSOCKBUF	2560
#define MIN_PTY		4
#define MIN_IPTENT	4
#define MIN_PGLOCK	4
#define MIN_GUARPG	1024

/* maximal per CT values */
#define PRIVVM_PVE	0.6 / 4096
#define IPTENT_PVE	200
#define FLOCK_PVE	1000
#define PTY_PVE		512

/* default diskspace values */
#define HOST_DS		10485760ULL /* 10 GB */
#define DEF_DS		225280
#define HOST_DI		100000
#define DEF_DI		88000

/* helper values */
#define SHMPG_PRIVVM	0.10
#define NFLOCK_NFILE	0.10
#define NFILE_AVNPROC	32
#define PTY_PROC	0.1
#define PGLOCK_KMEM	204800 /* 2% KMEM */
#define K_KMEM_MAX	2.0

/* limit/barrier delta values */
#define KMEM_DELTA	1.1
#define NFLOCK_DELTA	1.1
#define PRIVVM_DELTA	1.1
#define DCACHE_DELTA	1.03
#define DS_DELTA	1.1
#define DI_DELTA	1.1

#define NUMUBC		23

enum {	NPROC = 0,
	AVNUMPROC,
	NTCPSOCK,
	NOTHSOCK,
	VMGUAR,
	KMEM,
	TCPSND,
	TCPRCV,
	SOCKOTH,
	DGRAM,
	OOMGUAR,
	PRIVVMPG,
	LOCKPG,
	SHMPG,
	NPHYPG,
	NFILE,
	NFLOCK,
	NPTY,
	NSIGINFO,
	DCACHE,
	IPTENT,
	DISKSPACE,
	DISKINODES
};

char *ubcnames[] = {
	"NUMPROC",
	"AVNUMPROC",
	"NUMTCPSOCK",
	"NUMOTHERSOCK",
	"VMGUARPAGES",
	"KMEMSIZE",
	"TCPSNDBUF",
	"TCPRCVBUF",
	"OTHERSOCKBUF",
	"DGRAMRCVBUF",
	"OOMGUARPAGES",
	"PRIVVMPAGES",
	"LOCKEDPAGES",
	"SHMPAGES",
	"PHYSPAGES",
	"NUMFILE",
	"NUMFLOCK",
	"NUMPTY",
	"NUMSIGINFO",
	"DCACHESIZE",
	"NUMIPTENT",
	"DISKSPACE",
	"DISKINODES",
	NULL};

struct par_limits {
	unsigned long long bar;
	unsigned long long lim;
};

struct par_limits params[NUMUBC];

/* Global variables */
unsigned long long mem_total, low_total, swap_total, ds_total, di_total;
long pagesize, proc_calc;
int num_ve, ve_allowed, osl;

float	k_kmem[MAX_SL]		= {1, 1.2, 1.8};
float	k_nproc[MAX_SL]		= {1, 1.5, 2};
float	k_avnpr[MAX_SL]		= {8, 5, 3};
int	k_kpp[MAX_SL]		= {81920, 65536, 53248};
int	k_sock1[MAX_SL]		= {262144, 131072, 65536};
int	k_sock2[MAX_SL]		= {4096, 3036, 2560};
int	numsiginfo[MAX_SL]	= {1024, 512, 256};
float	k_dcache[MAX_SL]	= {1.5 * 384, 1.2 * 384, 1 * 384};
float	k_privvm[MAX_SL]	= {6, 3, 1.5};
int	k_pglock[MAX_SL]	= {10, 3, 1};
int	k_msl[MAX_SL]		= {10485760, 2097152, 0};

char *level_string[MAX_SL + 1] = {
	"Free resource distribution. Any parameters may be increased",
	"Normal resource distribution. Secondary parameters may be increased",
	"Partial resource shortage. Auxiliary parameters may be increased",
	"Overall resource shortage. Please, do not change any parameters!"
};

void usage(int rc)
{
	fprintf(rc ? stderr : stdout, "Usage: vzsplit [-f config_name] "
		"| [-n numves] | [-s swap_size]\n"
		"\t -f specified config name\n"
		"\t -n specified number of containers\n"
		"\t -s specified swap in Kbytes\n");
	exit(rc);
}

void header(FILE *fp)
{
	fprintf(fp,
		"# Configuration file generated by vzsplit for %d containers\n"
		"# on HN with total amount of physical mem %llu Mb\n"
		"# low memory %llu Mb, swap size %llu Mb, Max treads %lu\n"
		"# Resourse commit level %d:\n# %s\n",
		num_ve,
		(mem_total >> 20),
		(low_total >> 20), (swap_total >> 20), proc_calc,
		osl, level_string[osl]
		);
	return;
}

int lconv(char *name)
{
	int i;
	FILE *fp;

	if (name != NULL) {
		if ((fp = fopen(name, "w")) == NULL) {
			logger(-1, errno, "Unable to create %s", name);
			return 1;
		}
	} else {
		fp = stdout;
	}
	header(fp);

	fprintf(fp, "# Primary parameters\n");
	for (i = 0; i < NUMUBC; i++) {
		if (i == KMEM)
			fprintf(fp, "\n# Secondary parameters\n");
		else if (i ==LOCKPG)
			fprintf(fp, "\n# Auxiliary parameters\n");
		fprintf(fp, "%s=\"%s\"\n",
				ubcnames[i], ubcstr(
				(unsigned long)params[i].bar,
				(unsigned long)params[i].lim));
	}
	fprintf(fp, "CPUUNITS=\"%d\"\n", DEF_CPUUNITS);
	if (name) {
		fclose(fp);
		logger(-1, 0, "Config %s was created", name);
	}
	return 0;
}

int calculate_values()
{
	long long rest, delta, low_pve, tot_pve;
	long long kmem, numproc, avnproc, sockbuf, tcpbuf, iptent, numfile;
	long long dcache, numflock;
	long long lockedpages, numpty, guarpg, privvm, shmpg, di_pve, ds_pve;
	int sl;

	sl = avnproc = tcpbuf = 0;
	tot_pve = (mem_total + swap_total - SYSRSRV) / num_ve;
	for (osl = 0; osl < MAX_SL; osl++) {
		numproc = k_nproc[osl] * proc_calc / num_ve;
		low_pve = low_total * 0.4 * k_kmem[osl] / num_ve;
		guarpg = (tot_pve - low_pve) / pagesize;
		if (guarpg < MIN_GUARPG) {
			guarpg = MIN_GUARPG;
			low_pve = tot_pve - guarpg * pagesize;
		}
		kmem = low_pve / 2;
		if (low_pve < k_msl[osl])
			continue;
		if (numproc < MINPROC)
			numproc = MINPROC;
		avnproc = kmem / k_kpp[osl];
		if (avnproc < MINPROC / 2)
			continue;

		if (numproc < avnproc)
			numproc = avnproc;
		if ((numproc < 2 * avnproc) && (osl < MAX_SL))
			numproc = 2 * avnproc;
		if (numproc > avnproc * k_avnpr[osl])
			numproc = avnproc * k_avnpr[osl];

		sockbuf = low_pve - kmem;
		tcpbuf = sockbuf / 3;
		if (tcpbuf < k_sock1[osl] + k_sock2[osl] * numproc)
			continue;

		sl = osl;
		break;
	}

	if (osl == MAX_SL) {
		sl = osl - 1;
		numproc = k_nproc[sl] * proc_calc / num_ve;
		low_pve = low_total * 0.4 * K_KMEM_MAX / num_ve;
		guarpg = (tot_pve - low_pve) / pagesize;
		if (guarpg < MIN_GUARPG) {
			guarpg = MIN_GUARPG;
			low_pve = tot_pve - guarpg * pagesize;
		}
		do {
			numproc /= 2;
			if (numproc < MINPROC)
				numproc = MINPROC;
			avnproc = numproc / 2;
			delta = MINSOCKBUF * numproc;
			sockbuf = 2 * (MINDGRAMBUF + MINTCPBUF + delta);
			kmem = low_pve - sockbuf;
			rest = kmem - avnproc * k_kpp[sl];
		} while ((rest < 0) && (numproc > MINPROC));

		if (rest < 0) {
			logger(-1, 0, "Fatal resource shortage, try to "
					"decrease the number of containers");
			return 1;
		}
		params[DGRAM].lim = params[DGRAM].bar = MINDGRAMBUF;
		params[SOCKOTH].bar = MINDGRAMBUF;
		params[SOCKOTH].lim = MINDGRAMBUF + delta;
		params[TCPSND].bar = MINTCPBUF;
		params[TCPSND].lim = MINTCPBUF + delta;
		params[TCPRCV].bar = MINTCPBUF;
		if (rest > delta) {
			params[TCPRCV].lim = MINTCPBUF + delta;
			kmem += rest - delta;
		} else
			params[TCPRCV].lim = MINTCPBUF + rest;
	} else {
		delta = k_sock2[sl] * numproc;
		rest = tcpbuf - delta;
		params[TCPSND].lim = params[TCPRCV].lim = CHECK_LIMIT(tcpbuf);
		params[TCPSND].bar = params[TCPRCV].bar = CHECK_LIMIT(rest);
		params[DGRAM].lim = params[DGRAM].bar =
			params[SOCKOTH].bar = CHECK_LIMIT(rest / 2);
		params[SOCKOTH].lim = CHECK_LIMIT(rest / 2 + delta);
	}

	params[KMEM].lim = CHECK_LIMIT(kmem * KMEM_DELTA);
	params[KMEM].bar = CHECK_LIMIT(kmem);
	params[NPROC].lim = params[NPROC].bar = CHECK_LIMIT(numproc);
	params[AVNUMPROC].lim = params[AVNUMPROC].bar = CHECK_LIMIT(avnproc);
	params[NTCPSOCK].lim = params[NTCPSOCK].bar = CHECK_LIMIT(numproc);
	params[NOTHSOCK].lim = params[NOTHSOCK].bar = CHECK_LIMIT(numproc);

	iptent = MAXIPTENT / num_ve;
	if (iptent > IPTENT_PVE)
		iptent = IPTENT_PVE;
	else if (iptent < MIN_IPTENT)
		iptent = MIN_IPTENT;

	params[IPTENT].lim = params[IPTENT].bar = iptent;

	numfile = avnproc * NFILE_AVNPROC;
	params[NFILE].lim = params[NFILE].bar = CHECK_LIMIT(numfile);

	dcache = numfile * k_dcache[sl];
	params[DCACHE].lim = CHECK_LIMIT(dcache);
	params[DCACHE].bar = CHECK_LIMIT(dcache / DCACHE_DELTA);

	numflock = numfile * NFLOCK_NFILE;
	if (numflock > FLOCK_PVE)
		numflock = FLOCK_PVE;
	params[NFLOCK].lim = CHECK_LIMIT(numflock * NFLOCK_DELTA);
	params[NFLOCK].bar = numflock;

	lockedpages = kmem * k_pglock[sl] / PGLOCK_KMEM;
	if (lockedpages < MIN_PGLOCK)
		lockedpages = MIN_PGLOCK;
	params[LOCKPG].lim = params[LOCKPG].bar = CHECK_LIMIT(lockedpages);

	numpty = numproc * PTY_PROC;
	if (numpty < MIN_PTY)
		numpty = MIN_PTY;
	if (numpty > PTY_PVE)
		numpty = PTY_PVE;

	params[NPTY].lim = params[NPTY].bar = numpty;

	params[NSIGINFO].lim = params[NSIGINFO].bar = numsiginfo[sl];
	params[NPHYPG].bar = 0; params[NPHYPG].lim = MAXVAL;

	privvm = guarpg * k_privvm[sl];
	if (privvm > tot_pve / pagesize)
		privvm = guarpg;
	if (num_ve > 1 && privvm > PRIVVM_PVE * mem_total) {
		privvm = PRIVVM_PVE * mem_total;
		if (guarpg > privvm)
			guarpg = privvm;
	}
	params[PRIVVMPG].bar = CHECK_LIMIT(privvm);
	params[PRIVVMPG].lim = CHECK_LIMIT(privvm * PRIVVM_DELTA);
	params[VMGUAR].bar = params[OOMGUAR].bar = CHECK_LIMIT(guarpg);
	params[VMGUAR].lim = params[OOMGUAR].lim = MAXVAL;

	shmpg = privvm * SHMPG_PRIVVM;
	params[SHMPG].bar = params[SHMPG].lim = CHECK_LIMIT(shmpg);

	if (ds_total == 0) {
		ds_pve = DEF_DS;
		di_pve = DEF_DI;
	} else {
		ds_pve = ds_total / (2 * num_ve);
		di_pve = di_total / (2 * num_ve);
	}
	params[DISKSPACE].bar = ds_pve / DS_DELTA;
	params[DISKSPACE].lim = ds_pve;
	params[DISKINODES].bar = di_pve / DI_DELTA;
	params[DISKINODES].lim = di_pve;

	return 0;
}

char * get_ve_private()
{
	vps_param *param;
	char *ve_private, *veidp, *ret;

	param = init_vps_param();
	/* Parse global config file */
	vps_parse_config(0, GLOBAL_CFG, param, NULL);
	ve_private = param->res.fs.private_orig;

	if (ve_private == NULL) {
		free_vps_param(param);
		return NULL;
	}

	/* Remove $VEID and beyond from the string */
	veidp = strstr(ve_private, "$VEID");
	if (veidp == NULL)
		veidp = strstr(ve_private, "${VEID}");
	if (veidp != NULL)
		*veidp = '\0';

	ret = strdup(ve_private);
	free_vps_param(param);
	return ret;
}

int check_disk_space() {
	char * ve_private;
	int nofs = 0, noinodes = 0;
	long ve_ds, ve_di;
	int rec = 0, retval = 0;
	struct statfs statfs_buf;

	ve_private = get_ve_private();
	if (ve_private == NULL) {
		logger(-1, 0, "WARNING: unable to get VE_PRIVATE value "
				"from " GLOBAL_CFG ".");
		nofs = 1;
	}
	else {
		if (statfs(ve_private, &statfs_buf) < 0) {
			logger(-1, errno, "WARNING: statfs on %s failed",
				ve_private);
			nofs = 1;
		}
	}

	if (nofs == 1) {
		logger(-1, 0, "Default disk space values to be used.\n");
		ds_total = 0; di_total = 0;
		return retval;
	}

	ds_total = statfs_buf.f_blocks;
	/* convert to Kbytes */
	ds_total *= (statfs_buf.f_bsize / 1024);
	di_total = statfs_buf.f_files;

	if (statfs_buf.f_type == REISERFS_SUPER_MAGIC) {
		/* reiserfs does not have inodes, thus
		 * no limit on number of files */
		noinodes = 1;
	}

	if (ds_total / 2 < HOST_DS) {
		rec = 1;
		ds_total /= 2;
	} else
		ds_total -= HOST_DS;

	if (noinodes != 1) {
		if (di_total / 2 < HOST_DI) {
			rec = 1;
			di_total /= 2;
		} else
			di_total -= HOST_DI;
	}
	if (rec)
		logger(-1, 0, "WARNING: Recommended minimal size "
				"of partition holding %s is %llu Gb!",
				ve_private, HOST_DS / 1024 / 1024 * 2);

	ve_ds = ds_total / DEF_DS;
	ve_di = di_total / DEF_DI;

	if (ve_ds < num_ve) {
		retval = 1;
		ve_allowed = ve_ds;
	}

	if ((noinodes != 1) && (ve_di < num_ve) ) {
		retval = 1;
		if (ve_di < ve_ds)
			ve_allowed = ve_di;
	}
	if (retval == 1) {
		logger(-1, 0, "WARNING: partition holding %s do not "
				"have space required for %d containers\n"
				"The maximum allowed value is %d",
				ve_private, num_ve, ve_allowed);
		logger(-1, 0, "Default disk space values "
				"will be used\n");
		ds_total = 0; di_total = 0;
	}

	free(ve_private);
	return retval;
}



int main(int argc, char **argv)
{
	int len, opt, swp;
	char *tail;
	char *name = NULL;
	struct stat st;
	FILE *fd;
	char str[1024];
	unsigned long long val;
	int retval;

	num_ve = -1;
	swp = 0;
	mem_total = 0;
	low_total = 0;
	swap_total = 0;

	init_log(NULL, 0, 1, 0, 0, "vzsplit");

	while ((opt = getopt(argc, argv, "f:n:s:h")) > 0) {
		switch(opt) {
		case 'f':
			len = strlen(optarg) + strlen(VPS_CONF_DIR) +
				strlen("ve-.conf-sample");
			name = (char *)vz_malloc(len + 1);
			if (!name)
				exit(1);
			sprintf(name, VPS_CONF_DIR "ve-%s.conf-sample", optarg);
			if (!stat(name, &st)) {
				logger(-1, 0, "File %s already exist",
					name);
				exit(1);
			}
			break;
		case 'n':
			num_ve = strtol(optarg, &tail, 10);
			if (*tail != '\0') {
				logger(-1, 0, "Invalid argument "
						"for -n: %s", optarg);
				exit(1);
			}
			break;
		case 's':
			if (optarg[0] == '-') {
				logger(-1, 0, "Negative value for -s");
				exit(-1);
			}
			swp = 1;
			swap_total = strtoll(optarg, &tail, 10);
			swap_total <<= 10;
			if (*tail != '\0') {
				logger(-1, 0, "Invalid argument "
						"for -s: %s", optarg);
				exit(1);
			}
			break;
		case 'h':
			usage(0);
		default	:
			usage(1);
		}
	}
	if (optind < argc) {
		usage(1);
	}
	if (num_ve == -1) {
		printf("Enter the number of containers: ");
		if (scanf("%d", &num_ve) != 1) {
			logger(-1, 0, "Invalid value.");
			exit(1);
		}
	}
	if (num_ve < 1) {
		logger(-1, 0, "Incorrect value for number of containers.");
		exit(1);
	}

	if ((fd = fopen(PROCMEM, "r")) == NULL) {
		logger(-1, errno, "Cannot open " PROCMEM);
		exit(1);
	}

	while (fgets(str, sizeof(str), fd))
		if (sscanf(str, "MemTotal:\t %llu", &val) == 1)
			mem_total = val << 10;
		else if (sscanf(str, "LowTotal:\t %llu", &val) == 1)
			low_total = val << 10;
		else if (!swp && sscanf(str, "SwapTotal:\t %llu", &val) == 1)
			swap_total = val << 10;

	fclose(fd);
	if (low_total == 0)
		low_total = mem_total;
	if (mem_total < SYSRSRV) {
		logger(-1, 0, "At least %d Mb of RAM should be "
				"installed on Hardware Node",
				SYSRSRV / 1024 / 1024);
		exit(1);
	}

	if (swap_total > 2 * mem_total)
		logger(-1, 0, "The optimal swap space size is %llu Mb, "
				"twice bigger than the RAM size\n",
				(2 * mem_total) >> 20);
	ve_allowed = num_ve;
	retval = 0;

	if (((mem_total + swap_total - SYSRSRV) / num_ve) < MEMPERVE) {
		logger(-1, 0, "On node with %llu Mb of memory (RAM + swap) "
				"%d containers can not be allocated",
				(mem_total + swap_total) >> 20, num_ve);
		ve_allowed = (mem_total + swap_total - SYSRSRV) / MEMPERVE;
		retval = 1;
	}

	if (((low_total - SYSRSRV)/ ve_allowed) < LOWPERVE) {
		int ve_low;

		logger(-1, 0, "On node with %llu Mb of Low Memory "
				"%d containers can not be allocated",
				low_total >> 20, num_ve);
		ve_low = (low_total - SYSRSRV) / LOWPERVE;
		if (ve_low < ve_allowed)
			ve_allowed = ve_low;
		retval = 1;
	}

	if (retval != 0) {
		logger(-1, 0, "The maximum allowed value is %d",
				ve_allowed);
		exit(retval);
	}

	if ((fd = fopen(PROCTHREADS, "r")) == NULL) {
		logger(-1, errno, "Cannot open " PROCTHREADS);
		exit(1);
	}
	if (fgets(str, sizeof(str), fd))
		if (sscanf(str, "%llu", &val) == 1)
			proc_calc = val;
	fclose(fd);

	retval = check_disk_space();

	if ((pagesize = sysconf(_SC_PAGE_SIZE)) == -1)
		pagesize = 4096;

	if (low_total > 2U << 30) {
		if (proc_calc > 2 * MAX_TOTAL_PIDS)
			proc_calc = 2 * MAX_TOTAL_PIDS;
	} else if (proc_calc > MAX_TOTAL_PIDS)
			proc_calc = MAX_TOTAL_PIDS;

	if (calculate_values())
		exit(1);

	retval = lconv(name);
	exit(retval);
}
