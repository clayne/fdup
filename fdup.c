/* Copyright (c) 2013, Robert Clausecker
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE. */

#define _XOPEN_SOURCE 500
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ftw.h>

#include <openssl/sha.h>

#include "btrfs.h"
#include "match.h"

static enum {
	LIST_DUPS_MODE,
	HARD_LINK_MODE,
	SOFT_LINK_MODE,
	BTRFS_COPY_MODE
} operation_mode = LIST_DUPS_MODE;

static struct matcher *m;
static off_t lower_boundary = 0;
static off_t upper_boundary = 0;
static int has_upper_boundary = 0;

static int print_walker(const char *fpath,const struct stat *sb,int tf,struct FTW *ftwbuf) {

	/* silence warnings */
	(void)tf;
	(void)ftwbuf;

	if (!S_ISREG(sb->st_mode)) return 0;
	if (sb->st_size < lower_boundary) return 0;
	if (has_upper_boundary && sb->st_size > upper_boundary) return 0;

	if (register_file(m,fpath,sb)) return 1;

	fprintf(stderr,"\r%9d files",get_file_count(m));

	return 0;
}

static int print_dups(struct matcher *m) {
	int first = 1;
	const char *file;

	while ((file = next_group(m))) {
		if (first) first = 0;
		else printf("\n");

		puts(file);

		while ((file = next_file(m))) puts(file);
	}

	return 0;
}

typedef int link_func(const char*,const char*);

static int perform_link(link_func f, const char *old, const char *new) {
	char *tmp, *new_dup;
	int rval = 0, len;

	new_dup = strdup(new);
	if (new_dup == NULL) {
		perror("malloc failed");
		return 1;
	}

	/* /fdup.##########.tmp */
	len = strlen(new_dup) + 21;

	tmp = malloc(len);
	if (tmp == NULL) {
		perror("malloc failed");
		return 1;
	}

	snprintf(tmp,len,"%s/fdup.%010d.tmp",dirname(new_dup),getpid());
	free(new_dup);

	if (f(old,tmp) == -1) {
		fprintf(stderr,"Cannot link %s to %s",old,tmp);
		perror(NULL);
		rval = 1;
	} else if (rename(tmp,new) == -1) {
		fprintf(stderr,"Cannot rename %s to %s",tmp,new);
		perror(NULL);
		rval = 1;
	}

	free(tmp);

	return rval;
}

static int make_links(struct matcher *m, link_func f) {
	const char *orig, *dup;
	int link_count = 0, pair_count = 0;

	while ((orig = next_group(m))) {
		pair_count++;
		while ((dup = next_file(m))) {
			link_count++;
			if (perform_link(f,orig,dup)) return 1;
			fprintf(stderr,"\rMade %9d links for %9d groups",link_count,pair_count);
		}
	}

	return 0;
}

static void help(const char *program) {
	printf("Usage: %s [-B | -H | -L | -S] [-hx] [-b cdglmpu] [-s n[,m]] directory...\n",program);
}

/* apply kilo, mega, giga etc. suffix */
static off_t adjust_suffix(off_t n, char suffix) {
	switch (suffix) {
	case 'K': return n << 10;
	case 'M': return n << 20;
	case 'G': return n << 30;
	case 'T': return n << 40;
	case 'P': return n << 50;
	case 'E': return n << 60;
	default: return n;
}
}

int main(int argc, char *argv[]) {
	int ok=1,i,opt;
	rlim_t maxfiles;
	struct rlimit limit;
	matcher_flags flags = 0;
	int xdev = 0;
	char *argrmdr;

	while ((opt = getopt(argc,argv,"BHLSb:hs:x")) != -1) {
		switch(opt) {
		case 'B':
			operation_mode = BTRFS_COPY_MODE;
			break;
		case 'H':
			operation_mode = HARD_LINK_MODE;
			flags |= M_DEV|M_LINK; /* avoid a quirk in rename */
			break;
		case 'L':
			operation_mode = LIST_DUPS_MODE;
			break;
		case 'S':
			operation_mode = SOFT_LINK_MODE;
			break;
		case 'x':
			xdev = 1;
			break;
		case 'b':
			optarg--;
			while (*++optarg != '\0') switch (*optarg) {
			case 'c': flags |= M_CTIME; break;
			case 'd': flags |= M_DEV; break;
			case 'g': flags |= M_GID; break;
			case 'l': flags |= M_LINK; break;
			case 'm': flags |= M_MTIME; break;
			case 'p': flags |= M_MODE; break;
			case 'u': flags |= M_UID; break;
			default:
				fprintf(stderr,"Unknown specifier %c to -b\n",*optarg);
				return 2;
			}
			break;
		case 's':
			if (*optarg == '\0') {
				help(argv[0]);
				return 2;
			}
			lower_boundary = strtoll(optarg,&argrmdr,10);
			if (strchr(",KMGTPE",*argrmdr) == NULL) {
				fprintf(stderr,"Unexpected character %c in string to -s\n",*argrmdr);
				help(argv[0]);
				return 2;
			}
			if (*argrmdr != '\0' && *argrmdr != ',')
				lower_boundary = adjust_suffix(lower_boundary,*argrmdr++);
			if (*argrmdr == ',') argrmdr++;
			if (*argrmdr == '\0') break;

			has_upper_boundary = 1;
			optarg = argrmdr;
			upper_boundary = strtoll(optarg,&argrmdr,10);

			if (strchr("KMGTPE",*argrmdr) == NULL) {
				fprintf(stderr,"Unexpected character %c in string to -s\n",*argrmdr);
				help(argv[0]);
				return 2;
			}

			if (*argrmdr != '\0') upper_boundary = adjust_suffix(upper_boundary,*argrmdr);
			break;
		case 'h':
		default:
			help(argv[0]);
			return 2;
		}
	}

	if (optind >= argc) {
		help(argv[0]);
		return 2;
	}

	m = new_matcher(flags);

	/* attempt to use as many files as possible */
	getrlimit(RLIMIT_NOFILE,&limit);
	maxfiles = limit.rlim_cur - 8; /* spare some file descriptors */

	fputs("Scanning file system...\n",stderr);

	for (i = optind; i < argc; i++) {
		ok = nftw(argv[i],print_walker,maxfiles,FTW_PHYS|(xdev?FTW_MOUNT:0));
		if (ok == -1) {
			fprintf(stderr,"\nError processing argument %s: ",argv[i]);
			perror(NULL);
		}
	}

	fputs("\nLooking for duplicates...\n",stderr);
	if (finalize_matcher(m)) return 1;

	switch (operation_mode) {
	case LIST_DUPS_MODE:  ok = print_dups(m); break;
	case HARD_LINK_MODE:  ok = make_links(m,link); break;
	case SOFT_LINK_MODE:  ok = make_links(m,symlink); break;
	case BTRFS_COPY_MODE: ok = make_links(m,btrfs_clone); break;
	}

	if (!ok) return 1;

	free_matcher(m);

	return 0;
}
