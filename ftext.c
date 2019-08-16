#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * (14/08/2019) -- Complete rewrite of how the mapped file is handled and
 * how each operation is actually carried out. Before, the operations
 * copied the data from the memory mapped region and modified it as
 * necessary. A new file was therefore created, and once complete, the old
 * file would be unlinked. So each function mapped the old file, created
 * a brand new file, wrote to it while carrying out its purpose, unmapped
 * the old file, and then unlinked. This seems like an overly expensive
 * method of formatting a .txt file. There's also the fact that some may
 * prefer that their files maintain an original creation timestamp. Now
 * we do everything on the one virtual memory area and therefore keep the
 * original file intact.
 */

#define PROGRESS_COLOUR			"\e[48;5;2m\e[38;5;16m"
#define DISPLAY_COLOUR			"\e[48;5;255m\e[38;5;208m"
#define FILE_STATS_COLOUR		"\e[48;5;240m\e[38;5;208m"

#define reset_global()					\
{																\
	global_data.total_lines = 0;	\
	global_data.done_lines = 0;		\
}

#define clear_struct(s) memset((s), 0, sizeof(*(s)))

#ifndef PATH_MAX
# define PATH_MAX 1024
#endif

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static int MAX_LENGTH = 0;

typedef struct mapped_file_t
{
	char		filename[PATH_MAX];
	int			fd;
	void		*startp;
	void		*endp;
	size_t	map_size;
	size_t	original_file_size;
	size_t	current_file_size;
	int			flags; // MAP_SHARED / MAP_PRIVATE...
} mapped_file_t;

typedef struct global_data_t
{
	int		total_lines;
	int		done_lines;
} global_data_t;

/*
 * Remove a byte range from the file and vma by taking the data
 * from [STARTP+OFFSET+RANGE,ENDP) and moving it to
 * [STARTP+OFFSET,ENDP-RANGE). Data from [ENDP-RANGE,ENDP)
 * is zeroed out and then the vma is adjusted to be
 * [STARTP,ENDP-RANGE). File is then truncated. The dirty page(s)
 * in the vma will be written back to disc later by the kernel.
 */
static int
__collapse_file(mapped_file_t *f, off_t offset, size_t range)
{
	assert(f);

	if (offset < 0 || offset >= f->map_size)
		return 0;

	char	*to = ((char *)f->startp + offset);
	char	*from = (to + range);
	char	*endp = ((char *)f->endp);
	int		flags;
	size_t	map_size = f->map_size;

	memmove(to, from, (endp - from));
	to = (endp - range);
	memset(to, 0, range);

	flags = 0;
	f->flags = flags = (MAP_SHARED|MAP_FIXED);
	f->map_size = (map_size -= range);

	if ((f->startp = mmap(f->startp, map_size, PROT_READ|PROT_WRITE, flags, f->fd, 0)) == MAP_FAILED)
	{
		fprintf(stderr, "__collapse_file: mmap error (%s)\n", strerror(errno));
		return -1;
	}

	f->endp = ((char *)f->startp + map_size);

	if (ftruncate(f->fd, f->current_file_size -= range) < 0)
	{
		fprintf(stderr, "__collapse_file: ftruncate error (%s)\n", strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * Allocate BY bytes of disc space at the end of the file and
 * update the vma accordingly. We ask the kernel to keep the
 * vma at the same start address with MAP_FIXED, because we'd
 * like to be efficient and not needlessly copy the entire
 * vma to another location...
 */
static void *
__extend_file_and_map(mapped_file_t *f, off_t by)
{
	assert(f);

	size_t	map_size = f->map_size;
	int			flags = f->flags;
	
	if (by <= 0)
		return f->startp;

	if (posix_fallocate(f->fd, (size_t)((char *)f->endp - (char *)f->startp), (size_t)by) < 0)
	{
		fprintf(stderr, "__extend_file_and_map: posix_fallocate error (%s)\n", strerror(errno));
		return NULL;
	}

	f->current_file_size += (size_t)by;

	map_size += by;
	f->flags = (flags |= MAP_FIXED);

	if ((f->startp = mmap(f->startp, map_size, PROT_READ|PROT_WRITE, flags, f->fd, 0)) == MAP_FAILED)
	{
		fprintf(stderr, "__extend_file_and_map: mmap error (%s)\n", strerror(errno));
		return NULL;
	}

	f->map_size = map_size;
	f->endp = ((char *)f->startp + map_size);

	return f->startp;
}

/*
 * Shift the data from [STARTP+OFFSET,ENDP) to [STARTP+OFFSET+BY,ENDP+BY).
 * User should have already called __extend_file_and_map() so that
 * there is enough disc space and bytes at end of vma for the shift.
 * F->ENDP will be pointing BY bytes pas the old end. So what we want
 * here is to shift the range [FROM,ENDP-BY) to [TO,ENDP).
 * Data in [FROM,TO) is zeroed out.
 */
static void
__shift_file_data(mapped_file_t *f, off_t offset, size_t by)
{
	assert(f);

	char	*from = ((char *)f->startp + offset);
	char	*to = ((char *)from + by);
	char	*endp = ((char *)f->endp - by);
	size_t	bytes;
	
	bytes = (endp - from);
	memmove((void *)to, (void *)from, bytes);
	memset(from, 0, by);

	return;
}

int
__do_line_count(mapped_file_t *f)
{
	assert(f);

	char	*p = (char *)f->startp;
	char	*endp = (char *)f->endp;
	int		lines = 0;

	while (p < endp)
	{
		if (*p == 0x0a)
			++lines;

		++p;
	}

	return lines;
}

/*
 * Some formatting options require knowing the longest line
 * in the file in order to format the other lines accordingly.
 * E.g., justifying the text means adding enough spaces to
 * all lines shorter than the longest line; centre-aligning
 * means knowing the character offset of the centre of longest
 * line.
 */
static int
__get_length_longest_line(mapped_file_t *f)
{
	assert(f);

	int		max_length = 0;
	int		char_cnt = 0;
	char	*p = (char *)f->startp;
	char	*endp = (char *)f->endp;
	char	*line_start = NULL;
	char	*line_end = NULL;
	size_t	len = f->map_size;

	while (p < endp)
	{
		char_cnt = 0;
		line_start = p;

		while (*p == 0x20 || *p == 0x09)
			++p;

		len -= (p - line_start);

		line_start = p;
	
		line_end = memchr(line_start, 0x0a, len);

		if (!line_end)
			break;

		while (p < line_end)
		{
			if (*p == 0x20)
			{
				++p;
				++char_cnt;

				while (*p == 0x20)
					++p;

				continue;
			}
			else
			if (*p == 0x0a)
			{
				break;
			}

			++p;
			++char_cnt;
		}

		if (char_cnt > max_length)
			max_length = char_cnt;

		while (*p == 0x0a && p < endp)
			++p;

		len -= (p - line_start);
	}

	return max_length;
}

/*
 * Strip file of 0x0d (\r) characters.
 */
static void
__remove_cr(mapped_file_t *f)
{
	char	*prev = NULL;
	char	*p = NULL;
	char	*startp = (char *)f->startp;
	char	*endp = (char *)f->endp;
	size_t len = f->map_size;

	if (!(p = memchr(startp, 0x0d, len)))
		return;

	len -= (p - startp);

	while (p < endp)
	{
		prev = p;

		__collapse_file(f, (off_t)(p - startp), (size_t)1);
		--len;

		if (!(p = memchr(prev, 0x0d, len)))
			break;

		len -= (p - prev);
	}

	return;
}

/**
 * Remove whitespace at the start and end of a line
 * (not including new lines! -- 0x20 / 0x09).
 */
static void
__remove_extra_whitespace(mapped_file_t *f)
{
	char	*p = (char *)f->startp;
	char	*startp = (char *)f->startp;
	char	*endp = (char *)f->endp;
	char	*save_p = NULL;
	size_t	range;

	while (p < endp)
	{
		save_p = p;

		if (*p == 0x20 || *p == 0x09)
		{
			while (*p == 0x20 || *p == 0x09)
				++p;

			range = (p - save_p);

			if (range)
			{
				__collapse_file(f, (off_t)(save_p - startp), range);
				endp -= range;
				p = save_p;
				range = 0;
			}
		}

		p = memchr(save_p, 0x0a, (endp - save_p));

		if (!p)
		{
			p = endp - 1;
			if (*p == 0x20 || *p == 0x09)
			{
				save_p = endp;
				while (*p == 0x20 || *p == 0x09)
					--p;

				++p;

				range = (save_p - p);

				if (range)
				{
					__collapse_file(f, (off_t)(p - startp), range);
					endp -= range;
					range = 0;
				}
			}

			break;
		}

		save_p = p;
		--p;

		if (*p == 0x20 || *p == 0x09)
		{
			while (*p == 0x20 || *p == 0x09)
				--p;

			++p;

			range = (save_p - p);

			if (range)
			{
				__collapse_file(f, (off_t)(p - startp), range);
				endp -= range;
				range = 0;
				++p;
			}
		}

		if (*save_p == 0x0a)
			p = save_p + 1;
	}

	return;
}

static void
__unjustify_text(mapped_file_t *f)
{
	char	*p = (char *)f->startp;
	char	*startp = (char *)f->startp;
	char	*endp = (char *)f->endp;
	char	*save_p = NULL;
	size_t	range;

	while (p < endp)
	{
		save_p = p;

		p = memchr(save_p, 0x20, (endp - save_p));

		if (!p)
			break;

		++p;
		save_p = p;

		while (*p == 0x20)
			++p;

		range = (p - save_p);

		if (range)
		{
			__collapse_file(f, (off_t)(save_p - startp), range);
			endp -= range;
			p = save_p;
			range = 0;
		}
	}

	return;
}

static void
__normalise_file(mapped_file_t *f)
{
	char	*p = (char *)f->startp;
	char	*startp = (char *)f->startp;
	char	*endp = NULL;
	char	*save_p = NULL;

	__remove_cr(f);
	__remove_extra_whitespace(f);
	__unjustify_text(f);

	endp = (char *)f->endp;
	
	/*
	 * Finish off by removing any -\n sequencies.
	 */
	while (p < endp)
	{
		save_p = p;

		p = memchr(save_p, 0x2d, (endp - save_p));

		if (!p)
			break;

		if (*(p+1) == 0x0a)
		{
			__collapse_file(f, (off_t)(p - startp), (size_t)2);
			endp -= 2;

			while (*p != 0x20 && p > startp)
				--p;

			if (*p == 0x20)
				*p++ = 0x0a;
		}
		else
		{
			++p;
		}
	}
}

int	check_file(char *) __nonnull((1)) __wur;
void usage(int) __attribute__((__noreturn__));

/* Text formatting functions */
ssize_t change_length(mapped_file_t *) __nonnull((1)) __wur;
ssize_t	justify_text(mapped_file_t *) __nonnull((1)) __wur;
ssize_t unjustify_text(mapped_file_t *) __nonnull((1)) __wur;
ssize_t left_align_text(mapped_file_t *) __nonnull((1)) __wur;
ssize_t right_align_text(mapped_file_t *) __nonnull((1)) __wur;
ssize_t centre_align_text(mapped_file_t *) __nonnull((1)) __wur;
ssize_t	change_line_length(mapped_file_t *) __nonnull((1)) __wur;

// screen-drawing functions
void clear(void);
void deleteline(void);
void h_centre(int, int, int);
void v_centre(int, int, int);
void up(int);
void down(int);
void left(int);
void right(int);
void fill_line(char *) __nonnull((1));
void fill(void);

// data-related functions
int do_line_count(char *) __nonnull((1));
void print_fileinfo(char *) __nonnull((1));

/* Thread that displays progress of each formatting operation */
void *show_progress(void *);


static global_data_t	global_data;
static mapped_file_t	file;

static struct winsize			WINSIZE;
static int		POSITION;

// global flags
static uint32_t	user_options;
#define test_flag(f) (user_options & (f))
#define set_flag(f) (user_options |= (f))
#define unset_flag(f) (user_options &= ~(f))

#define LENGTH		0x1u
#define UNJUSTIFY 0x2u
#define JUSTIFY		0x4u
#define LALIGN		0x8u
#define RALIGN		0x10u
#define CALIGN		0x20u

#define ALIGNMENT_MASK	0x3eu

#define test_user_options()																\
do {																											\
	if (test_flag(UNJUSTIFY) && test_flag(JUSTIFY))					\
	{																												\
		fprintf(stderr, "main: -j and -u are mutually exclusive\n");\
		goto fail;																						\
	}																												\
	if (test_flag(JUSTIFY)																	\
			&& (test_flag(LALIGN)																\
			|| test_flag(RALIGN)																\
			|| test_flag(CALIGN)))															\
	{																												\
		fprintf(stderr, "main: can only specify one alignment type\n");\
		goto fail;																						\
	}																												\
	if ((test_flag(RALIGN)																	\
			&& (test_flag(CALIGN)																\
			|| test_flag(LALIGN)))															\
			|| (test_flag(CALIGN)																\
			&& (test_flag(RALIGN)																\
			|| test_flag(LALIGN))))															\
	{																												\
		fprintf(stderr, "main: can only specify one alignment type\n");\
		goto fail;																						\
	}																												\
	if (test_flag(LENGTH) && test_flag(UNJUSTIFY))					\
		unset_flag(UNJUSTIFY);																\
} while (0)

		/* Thread-related variables */
//static pthread_attr_t	tATTR;
static pthread_t			TID_SP;
#define STR_PROGRESS_LENGTH	 		"[ Changing line length ]"
#define STR_PROGRESS_JUSTIFY		"[   Justifying lines   ]"
#define STR_PROGRESS_UNJUSTIFY	"[  Unjustifying lines  ]"
#define STR_PROGRESS_LALIGN			"[  Left aligning lines ]"
#define STR_PROGRESS_RALIGN			"[ Right aligning lines ]"
#define STR_PROGRESS_CALIGN			"[    Centering lines   ]"

/*
 * Map file into virtual memory.
 */
static mapped_file_t *
map_file(mapped_file_t *f)
{
	assert(f);

	char		*filename = f->filename;
	int			flags;
	struct stat statb;

	clear_struct(&statb);
	if (lstat(filename, &statb) < 0)
	{
		fprintf(stderr, "map_file: lstat error (%s)\n", strerror(errno));
		return NULL;
	}

	f->map_size = f->current_file_size = f->original_file_size = statb.st_size;

	if ((f->fd = open(filename, O_RDWR)) < 0)
	{
		fprintf(stderr, "map_file: open error (%s)\n", strerror(errno));
		return NULL;
	}

	flags = 0;
	flags |= MAP_SHARED;

	if ((f->startp = mmap(NULL, f->original_file_size, PROT_READ|PROT_WRITE, flags, f->fd, 0)) == MAP_FAILED)
	{
		fprintf(stderr, "map_file: mmap error (%s)\n", strerror(errno));
		return NULL;
	}

	f->flags = flags;
	f->endp = (void *)((char *)f->startp + statb.st_size);

	return f;
}

/*
 * Unmap file from virtual memory.
 */
static void
unmap_file(mapped_file_t *f)
{
	assert(f);

	void		*startp = f->startp;

	munmap(startp, f->map_size);

	if (f->fd > 2)
		close(f->fd);

	clear_struct(f);

	return;
}

int
main(int argc, char *argv[])
{
	char		c;

	/*
	 * Minimum number of args is 3, e.g. 'ftext -j file.txt`
	 */
	if (argc < 3)
		usage(EXIT_FAILURE);

	/*
	 * Must be done here and not in some constructor function
	 * because the terminal window dimensions are not known
	 * before main() is called.
	 */
	clear_struct(&WINSIZE);
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &WINSIZE);
	setvbuf(stdout, NULL, _IONBF, 0);

	clear_struct(&global_data);
	//pthread_attr_setdetachstate(&tATTR, PTHREAD_CREATE_DETACHED);

	opterr = 0;
	while ((c = getopt(argc, argv, "L:lrcjuDh")) != -1)
	{
		switch(c)
		{
			case(0x6c):
			set_flag(LALIGN);
			break;
			case(0x72):
			set_flag(RALIGN);
			break;
			case(0x63):
			set_flag(CALIGN);
			break;
			case(0x4c):
			MAX_LENGTH = atoi(optarg);
			set_flag(LENGTH);
			break;
			case(0x6a):
			set_flag(JUSTIFY);
			break;
			case(0x75):
			set_flag(UNJUSTIFY);
			break;
			case(0x68):
			usage(EXIT_SUCCESS);
			break;
			case(0x3f):
			fprintf(stderr, "main: invalid option ('%c')\n", c);
			exit(EXIT_FAILURE);
			break;
			default:
			fprintf(stderr, "main: invalid option ('%c')\n", c);
			exit(EXIT_FAILURE);
		}
	}

	test_user_options();

	if (check_file(argv[optind]) == -1)
		goto fail;

	if (strlen(argv[optind]) >= PATH_MAX)
	{
		fprintf(stderr, "main: path length exceeds PATH_MAX\n");
		goto fail;
	}

	clear_struct(&file);
	strcpy(file.filename, argv[optind]);

	if (!(map_file(&file)))
		goto fail;

	/*
	 * Remove 0x0d's, remove "-\n"
	 */
	__normalise_file(&file);

	clear();
	fill();
	up(6);
	POSITION = 6;
	print_fileinfo(argv[optind]);
	down(POSITION-2);

	if (test_flag(LENGTH))
	{
		pthread_create(&TID_SP, NULL, show_progress, (void *)STR_PROGRESS_LENGTH);
		if (change_line_length(&file) == -1)
			goto fail;
	}

	uint32_t		alignment = user_options & ALIGNMENT_MASK;
	switch(alignment)
	{
		case JUSTIFY:
			pthread_create(&TID_SP, NULL, show_progress, (void *)STR_PROGRESS_JUSTIFY);
			if (justify_text(&file) == -1)
				goto fail;
			break;
		case UNJUSTIFY:
			pthread_create(&TID_SP, NULL, show_progress, (void *)STR_PROGRESS_UNJUSTIFY);
			if (unjustify_text(&file) == -1)
				goto fail;
			break;
		case LALIGN:
			pthread_create(&TID_SP, NULL, show_progress, (void *)STR_PROGRESS_LALIGN);
			if (left_align_text(&file) == -1)
				goto fail;
			break;
		case RALIGN:
			pthread_create(&TID_SP, NULL, show_progress, (void *)STR_PROGRESS_RALIGN);
			if (right_align_text(&file) == -1)
				goto fail;
			break;
		case CALIGN:
			pthread_create(&TID_SP, NULL, show_progress, (void *)STR_PROGRESS_CALIGN);
			if (centre_align_text(&file) == -1)
				goto fail;
			break;
	}

	unmap_file(&file);
	exit(EXIT_SUCCESS);

	fail:
	unmap_file(&file);
	exit(EXIT_FAILURE);
}

int
check_file(char *filename)
{
	struct stat	statb;

	if (access(filename, F_OK) != 0)
	{
		fprintf(stderr, "check_file: %s does not exist\n", filename);
		goto fail;
	}
	else
	if (access(filename, R_OK) != 0)
	{
		fprintf(stderr, "check_file: cannot read %s\n", filename);
		goto fail;
	}
	else
	if (access(filename, W_OK) != 0)
	{
		fprintf(stderr, "check_file: cannot write to %s\n", filename);
		goto fail;
	}

	clear_struct(&statb);
	lstat(filename, &statb);

	if (!S_ISREG(statb.st_mode))
	{
		fprintf(stderr, "check_file: %s is not a regular file\n", filename);
		goto fail;
	}

	return 0;

	fail:
	return -1;
}

ssize_t
change_line_length(mapped_file_t *file)
{
	assert(file);

	char	*p = (char *)file->startp;
	char	*startp = (char *)file->startp;
	char	*endp = (char *)file->endp;
	char	*line_start = NULL;
	char	*line_end = NULL;

	reset_global();
	global_data.total_lines = __do_line_count(file);

	p = line_start = startp;

	/*
	 * We want to preserve paragraph structure, so if
	 * we encounter two or more consecutive new lines,
	 * we just skip them and then start a new line
	 * from there.
	 */
	while (p < endp)
	{
		__begin_next_line:
		line_end = (p + MAX_LENGTH);

		if (line_end > endp)
			line_end = endp;

		line_start = p;

		if (*line_end != 0x0a && *line_end != 0x20)
		{
			while (*line_end != 0x20 && *line_end != 0x0a && line_end > (line_start+1))
				--line_end;

			if (unlikely(line_end == line_start))
			{
				line_end = (line_start + MAX_LENGTH);

				if (line_end > endp)
					line_end = endp;
			}
		}

		/*
		 * If we happen upon a new line character, then replace
		 * it with a space. If we happen upon 2+ new line characters,
		 * then we preserve them as we want to keep the correct
		 * paragraph structure of the file.
		 */
		while (p < line_end)
		{
			if (*p == 0x0a && (p+1) == endp)
			{
				++global_data.done_lines;
				break;
			}
			else
			if (*p == 0x0a && *(p+1) != 0x0a)
			{
				*p++ = 0x20;
				++global_data.done_lines;
			}
			else
			if (*p == 0x0a && *(p+1) == 0x0a)
			{
				while (*p == 0x0a)
				{
					++p;
					++global_data.done_lines;
				}

				line_end = line_start = p;
				goto __begin_next_line;
			}
			else
			{
				++p;
			}
		}

		if (p < endp)
		{
			if (likely(*p == 0x20))
			{
				*p++ = 0x0a;
			}
			else
			if (*p == 0x0a)
			{
				/*
				 * There can only be one, otherwise we would have
				 * skipped them and restarted the loop above.
				 */
				++p;
				++global_data.done_lines;
			}
			else
			if (*p != 0x20)
			{
				/*
				 * Then we have a line with no whitespace. P and LINE_END
				 * are pointing to one byte beyond the maximum number of
				 * bytes per line.
				 *
				 * Policy at this point in time for such a scenario:
				 *
				 * Point P at LINE_START + MAX_LEN-1;
				 * Shift data by 2 bytes;
				 * Add "-\n", thus breaking the word with a hyphen.
				 */

				p = (line_start + MAX_LENGTH - 1);
				size_t		shift;

				if (unlikely(*p == 0x2d))
				{
					shift = 1;
					++p;
				}
				else
				if (unlikely(*(p-1) == 0x2d))
				{
					shift = 1;
				}
				else
				{
					shift = 2;
				}

				if (!__extend_file_and_map(file, shift))
					goto fail;

				endp += shift;

				__shift_file_data(file, (off_t)(p - startp), shift);

				if (likely(shift == 2))
					strncpy(p, "-\n", 2);
				else
					*p = 0x0a;

				p += shift;

				line_end = line_start = p;
			}
		}

		line_end = line_start = p;
	}

	return 0;

	fail:
	return -1;
}

ssize_t
justify_text(mapped_file_t *file)
{
	assert(file);

	char	*p = (char *)file->startp;
	char	*startp = (char *)file->startp;
	char	*endp = (char *)file->endp;
	char	*line_start = NULL;
	char	*line_end = NULL;
	char	*left = NULL;
	char	*right = NULL;
	char	*save_p = NULL;
	int		char_cnt = 0;
	int		delta;
	int		holes;
	int		quotient;
	int		remainder;
	int		threshold;

	reset_global();
	global_data.total_lines = __do_line_count(file);
	char_cnt = 0;

	/*
	 * If user did not specify a new line length, then
	 * determine the longest line in the file, and
	 * then justify the text accordingly.
	 */
	if (!test_flag(LENGTH))
		MAX_LENGTH = __get_length_longest_line(file);

	threshold = MAX_LENGTH / 2;

	while (p < endp)
	{
		__main_loop_justify_start:

		line_start = p;
		char_cnt = 0;

		p = memchr(line_start, 0x0a, (endp - line_start));

		if (!p)
			line_end = endp;
		else
			line_end = p;

		char_cnt = (int)(line_end - line_start);

		while (*line_end == 0x0a)
		{
			++line_end;
			++global_data.done_lines;
		}

		/*
		 * Short lines with more spaces than there are
		 * letters are not aesthetically pleasing. So
		 * just leave them alone.
		 */
		if (MAX_LENGTH == char_cnt || char_cnt <= threshold)
		{
			p = line_start = line_end;
			continue;
		}
		else
		{
			delta = holes = quotient = remainder = 0;

			p = line_start;

			/*
			 * calculate (#words - 1).
			 */
			while (p < line_end)
			{
				if (*p == 0x20)
					++holes;

				++p;
			}

			/*
			 * Then we can't justify it (adding
			 * spaces to the end of the line makes
			 * no difference visually).
			 */
			if (holes == 0)
			{
				p = line_start = line_end;
				goto __main_loop_justify_start;
			}

			delta = (MAX_LENGTH - char_cnt);
			quotient = (delta / holes);
			remainder = (delta % holes);

			p = line_start;

			/*
			 * ^An example line in the file which is shorter than the max$
			 * ^__________________________________________________________--------------$ max length
			 *                                                                 delta
			 *
			 * We need to add DELTA spaces to the line to justify it. With the number
			 * of holes in the current line known, we need to go over the line
			 * QUOTIENT times, adding an additional space between each word
			 * in the line. If (DELTA % HOLES), then must also spread that number of
			 * additional spaces across the current line.
			 */

			if (!__extend_file_and_map(file, (off_t)delta))
				goto fail;

			endp += delta;

			/*
			 * Loop until we have passed over the line QUOTIENT times,
			 * adding one additional space between each word.
			 */
			while (quotient)
			{
				if (p == line_end)
				{
					p = line_start;
					--quotient;
					continue;
				}

				save_p = p;
				p = memchr(save_p, 0x20, (line_end - save_p));

				if (!p)
				{
					p = line_end;
					continue;
				}

				__shift_file_data(file, (off_t)(p - startp), (size_t)1);
				++line_end;

				*p++ = 0x20;

				while (*p == 0x20)
					++p;
			}


			/*
			 * Go from start -> space, add space; go from end -> space, add space
			 * until REMAINDER spaces have been added to the line.
			 */
			int	_switch = 0;
			if (remainder)
			{
				p = left = line_start;
				right = (line_end - 1);

				while (remainder > 0)
				{
					if (!_switch)
					{
						p = memchr(left, 0x20, (right - left));

						if (!p)
						{
							left = line_start;
							right = (line_end - 1);

							_switch = 1;
							continue;
						}

						__shift_file_data(file, (off_t)(p - startp), (size_t)1);
						++line_end;
						++right;

						*p++ = 0x20;
						--remainder;

						_switch = 1;

						left = p;

						while (*left == 0x20)
							++left;

					}
					else
					{
						p = right;

						while (*p != 0x20 && p > left)
							--p;

						if (p == left)
						{
							right = (line_end - 1);
							left = line_start;

							_switch = 0;
							continue;
						}

						__shift_file_data(file, (off_t)(p - startp), (size_t)1);
						++line_end;
						++right;

						*p-- = 0x20;
						--remainder;

						right = p;

						while (*right == 0x20)
							--right;

						_switch = 0;

					}
				}
			} // if (remainder)

			p = line_start = line_end;

		} // else (MAX_COUNT != char_cnt && char_cnt > third_max)
	} // while (p < endp)


	++global_data.done_lines;
	return 0;

	fail:
	pthread_kill(TID_SP, SIGINT);
	return -1;
}

ssize_t
unjustify_text(mapped_file_t *file)
{
	assert(file);

	/*
	 * __unjustify_text() is called in __normalise_file()
	 */
	reset_global();
	global_data.done_lines = __do_line_count(file);

	return 0;
}

ssize_t
left_align_text(mapped_file_t *file)
{
	assert(file);

	reset_global();
	global_data.done_lines = __do_line_count(file);

	return 0;
}

ssize_t
right_align_text(mapped_file_t *file)
{
	assert(file);

	char		*p = (char *)file->startp;
	char		*startp = (char *)file->startp;
	char		*endp = (char *)file->endp;
	char		*line_start = NULL;
	char		*line_end = NULL;
	int			delta;
	int			char_cnt = 0;

	reset_global();
	global_data.total_lines = __do_line_count(file);

	if (!test_flag(LENGTH))
		MAX_LENGTH = __get_length_longest_line(file);
		
	while (p < endp)
	{
		line_start = p;
		char_cnt = 0;

		p = memchr(line_start, 0x0a, (endp - line_start));

		if (!p)
			line_end = endp;
		else
			line_end = p;

		char_cnt = (int)(line_end - line_start);

		delta = (MAX_LENGTH - char_cnt);

		if (!__extend_file_and_map(file, (size_t)delta))
			goto fail;

		endp += delta;

		__shift_file_data(file, (off_t)(line_start - startp), (off_t)delta);
		line_end += delta;

		p = (line_start + delta);

		memset(line_start, 0x20, (p - line_start));

		while (*line_end == 0x0a)
		{
			++line_end;
			++global_data.done_lines;
		}

		p = line_start = line_end;
	}

	return 0;

	fail:
	pthread_kill(TID_SP, SIGINT);
	return -1;
}

ssize_t
centre_align_text(mapped_file_t *file)
{
	assert(file);

	char	*p = (char *)file->startp;
	char	*startp = (char *)file->startp;
	char	*endp = (char *)file->endp;
	char	*line_start = NULL;
	char	*line_end = NULL;
	int		char_cnt;
	int		delta;
	int		half_delta;

	reset_global();
	global_data.total_lines = __do_line_count(file);

	if (!test_flag(LENGTH))
		MAX_LENGTH = __get_length_longest_line(file);

	while (p < endp)
	{
		line_start = p;
		char_cnt = 0;

		p = memchr(line_start, 0x0a, (endp - line_start));

		if (!p)
			line_end = endp;
		else
			line_end = p;

		char_cnt = (int)(line_end - line_start);
		delta = (MAX_LENGTH - char_cnt);

		if (!__extend_file_and_map(file, (size_t)delta))
			goto fail;

		endp += delta;

		p = line_start;

		half_delta = ((delta / 2) + (delta % 2));

		__shift_file_data(file, (off_t)(p - startp), (size_t)half_delta);

		line_end += half_delta;
		p = (line_start + half_delta);

		memset(line_start, 0x20, (p - line_start));

		p = line_end;

		half_delta = (delta - half_delta);

		__shift_file_data(file, (off_t)(p - startp), (off_t)half_delta);

		line_end += half_delta;

		memset(p, 0x20, (line_end - p));

		while (*line_end == 0x0a && line_end < endp)
		{
			++line_end;
			++global_data.done_lines;
		}

		p = line_start = line_end;
	}

	return 0;

	fail:
	pthread_kill(TID_SP, SIGINT);
	return -1;
}

// screen-drawing functions
void
h_centre(int dir, int left, int right)
{
	int		i;

	if (dir == 0)
	{
		for (i = 0; i < ((WINSIZE.ws_col/2)-left+right); ++i)
			printf("\e[C");
	}
	else
	if (dir == 1)
	{
		for (i = 0; i < ((WINSIZE.ws_col/2)-left+right); ++i)
			printf("\e[D");
	}
}

void
v_centre(int dir, int up, int down)
{
	int		i;

	if (dir == 0)
	{
		for (i = 0; i < ((WINSIZE.ws_row/2)+up-down); ++i)
			printf("\e[B");
	}
	else
	if (dir == 1)
	{
		for (i = 0; i < ((WINSIZE.ws_row/2)+up-down); ++i)
			printf("\e[A");
	}
}

void
deleteline(void)
{
	int		i;

	putchar(0x0d);
	for (i = 0; i < WINSIZE.ws_col; ++i)
		putchar(0x20);
	putchar(0x0d);
}

void
clear(void)
{
	int		i;

	putchar(0x0d);
	for (i = 0; i < WINSIZE.ws_row; ++i)
		printf("\e[A");

	for (i = 0; i < WINSIZE.ws_row; ++i)
	{
		deleteline();
		if (i != (WINSIZE.ws_row-1))
			putchar(0x0a);
	}
	putchar(0x0d);
}

void
up(int UP)
{
	int		i;

	for (i = 0; i < UP; ++i)
		printf("\e[A");
}

void
down(int DOWN)
{
	int		i;

	for (i = 0; i < DOWN; ++i)
		printf("\e[B");
}

void
left(int LEFT)
{
	int		i;

	for (i = 0; i < LEFT; ++i)
		printf("\e[D");
}

void
right(int RIGHT)
{
	int		i;

	for (i = 0; i < RIGHT; ++i)
		printf("\e[C");
}

// __FILL_LINE__
void
fill_line(char *colour)
{
	int		i;

	putchar(0x0d);
	printf("%s", colour);
	for (i = 0; i < (WINSIZE.ws_col-1); ++i)
		putchar(0x20);
	putchar(0x20);
	putchar(0x0d);
	printf("\e[m");
}

void
fill(void)
{
	int		i, j;

	up(WINSIZE.ws_row-1);
	for (i = 0; i < WINSIZE.ws_row; ++i)
	{
		for (j = 0; j < WINSIZE.ws_col; ++j)
			putchar(0x20);

		if (i != (WINSIZE.ws_row-1))
			printf("\r\n");
	}
}

/* thread-related functions */
void *
show_progress(void *arg)
{
	size_t		len, to_print;
	double		float_current_progress, float_current_progress_save, period, min;
	unsigned	current_progress, current_progress_save;

	len = strlen((char *)arg);
	current_progress = current_progress_save = 0;
	float_current_progress = float_current_progress_save = 0.0;
	to_print = (WINSIZE.ws_col-len-4);

	/*
	 * For example, if there are 200 blocks to print that
	 * make up the 100% bar, then each block is worth
	 * 0.5%. So check progress as a double and only print
	 * a new block when gaining another 0.5% increase;
	 *   If we only need 50 blocks for the bar, then
	 * each block is worth 2%, etc.
	 */
	period = ((double)100/(double)to_print);
	min = period;

	printf("%s%s\e[m", DISPLAY_COLOUR, (char *)arg);
	for (;;)
	{
		while ((float_current_progress = ((double)global_data.done_lines / (double)global_data.total_lines)) == float_current_progress_save);
		float_current_progress_save = float_current_progress;
		/*
		 * Are we nearer the ceiling or the floor?
		 */
		float_current_progress *= 100;
		double delta_ceil = (double)ceil(float_current_progress) - float_current_progress;
		double delta_floor  = float_current_progress - (double)floor(float_current_progress);

		if (delta_ceil < delta_floor
			|| (delta_ceil == delta_floor))
			current_progress = (unsigned)ceil(float_current_progress);
		else
			current_progress = (unsigned)floor(float_current_progress);

		if (current_progress == 100)
		{
			while (to_print > 0)
			{
				printf("%s%c\e[m", PROGRESS_COLOUR, 0x23);
				--to_print;
			}
			right(to_print);
			printf("%s%3d%%\e[m\r\n", DISPLAY_COLOUR, (unsigned)current_progress);
			goto __show_progress_end;
		}

		if ((float_current_progress) >= min)
		{
			printf("%s%c\e[m", PROGRESS_COLOUR, 0x23);
			/*
			 * Now increment MIN until it exceeds the current
			 * progress, and that will be our next target
		   * of progress before we print a block.
			 */
			while (min < float_current_progress)
				min += period;
			--to_print;
		}

		if (current_progress_save != current_progress)
		{
			right(to_print);
			printf("%s%3u%%\e[m", DISPLAY_COLOUR, (unsigned)current_progress);
			left(to_print+3);
			current_progress_save = current_progress;
		}

		float_current_progress = float_current_progress_save = 0.0;
	}

	__show_progress_end:
	printf("\e[m");
	pthread_exit((void *)0);
}

void
print_fileinfo(char *filename)
{
	struct stat		statb;
	char			buffer[512];
	struct tm		*TIME = NULL;
	mode_t			mode;

	clear_struct(&statb);
	lstat(filename, &statb);
	fill_line(FILE_STATS_COLOUR);
	printf("%s", FILE_STATS_COLOUR);
	printf("%22s %s\r\n", "FILENAME", filename);
	--POSITION;
	fill_line(FILE_STATS_COLOUR);
	printf("%s", FILE_STATS_COLOUR);
	TIME = gmtime(&statb.st_mtime);
	strftime(buffer, 40, "%A %d %B %Y at %T %Z", TIME);
	printf("%22s %s\r\n", "MODIFIED", buffer);
	--POSITION;
	fill_line(FILE_STATS_COLOUR);
	printf("%s", FILE_STATS_COLOUR);
	printf("%22s ", "PERMISSIONS");
	mode = statb.st_mode;
	printf("-");
	if (S_IRUSR & mode)
		printf("r");
	else
		printf("-");
	if (S_IWUSR & mode)
		printf("w");
	else
		printf("-");
	if (S_IXUSR & mode && !(S_ISUID & mode))
		printf("x");
	else if (!(S_IXUSR & mode) && S_ISUID & mode)
		printf("S");
	else if (S_IXUSR & mode && S_ISUID & mode)
		printf("s");
	else
		printf("-");
	if (S_IRGRP & mode)
		printf("r");
	else
		printf("-");
	if (S_IWGRP & mode)
		printf("w");
	else
		printf("-");
	if (S_IXGRP & mode && !(S_ISGID & mode))
		printf("x");
	else if (S_IXGRP & mode && S_ISGID & mode)
		printf("s");
	else if (!(S_IXGRP & mode) && S_ISGID & mode)
		printf("S");
	else
		printf("-");
	if (S_IROTH & mode)
		printf("r");
	else
		printf("-");
	if (S_IWOTH & mode)
		printf("w");
	else
		printf("-");
	if (S_IXOTH & mode)
		printf("x");
	else
		printf("-");
	printf("\r\n");
	--POSITION;
	fill_line(FILE_STATS_COLOUR);
	printf("%s", FILE_STATS_COLOUR);
	printf("%22s %lu bytes\r\n", "SIZE", statb.st_size);
	--POSITION;
	printf("\e[m");
}

void
usage(int exit_status)
{
	fprintf(stdout,

		"change_line_length [OPTIONS] </path/to/file>\r\n"
		"\r\n"
		" -L	Specify  the  length of line (this also left-aligns the text  by  default)\r\n"
		" -j	Justify the text (cannot be used with -r, -c or -u\r\n"
		" -u	Unjustify the text (cannot be used with -j)\r\n"
		" -r	Right-align  the  text (must also use -L to specify desired  line  length)\r\n"
		" -c	Centre-align  the  text (must also use -L to specify desired line  length)\r\n"
		" -D	Writes  a  log  to  \"debug.log\" in the  current  working  directory.  This\r\n"
		"	option  has the potential to slow down the formatting of the document,  as\r\n"
		"	the log is opened O_FSYNC to try and ensure that as much debug information\r\n"
		"	is  written  to  the  file  as possible in the event  of  a  fatal  error.\r\n"
		" -h	Display this information menu\r\n"
		"\r\n"
		"\r\n"
		"Examples:\r\n"
		"\r\n"
		"change_line_length -L 72 -j /home/Documents/My_Document.txt\r\n"
		"	changes  maximum  line  length of \"My_Document.txt\" to 72  characters  and\r\n"
		"	justifies the text\r\n"
		"\r\n"
		"change_line_length -L 55 -r /home/Documents/My_Document.txt\r\n"
		"	changes  maximum  line  length of \"My_Document.txt\" to 55  characters  and\r\n"
		"	right-aligns the text\r\n"
		"\r\n"
		"To  left-align  a text document, use -L <line length> only, as changing the length\r\n"
		"of the lines left-aligns the text by default.\r\n"
		"\r\n"
		"change_line_length -u /home/Documents/My_Document.txt\r\n"
		"	Unjustifies  \"My_Document.txt\",  which will, by default,  be  left-aligned.\r\n"
		"\r\n");

	exit(exit_status);
}
