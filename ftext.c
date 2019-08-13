#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * Need a large line buffer because there are cases where
 * there are actually no new line characters at all until
 * the end of the paragraph (and paragraphs can of course
 * be pretty large)
 */
#define LINE_BUF_SIZE				32368

#define PROGRESS_COLOUR			"\e[48;5;2m\e[38;5;16m"
#define DISPLAY_COLOUR			"\e[48;5;255m\e[38;5;208m"
#define FILE_STATS_COLOUR		"\e[48;5;240m\e[38;5;208m"

#define CREATION_FLAGS	(O_RDWR|O_CREAT|O_TRUNC|O_FSYNC)
#define CREATION_MASK		(S_IRWXU & ~S_IXUSR)
#define OLD_FILE_FLAGS	O_RDWR
#define OLD_FILE_MAP		MAP_SHARED
#define OLD_FILE_PROT		(PROT_READ|PROT_WRITE)

#define debug(__string, __int, __string2)						\
{											\
	if (DEBUG)									\
	  {										\
		fprintf(debug_fp,							\
		"%20s %d: %s line %d (%ld)\r\n",						\
		(__string),								\
		((__int)?(__int):0),							\
		((__string2)?(__string2):""),						\
		__LINE__,								\
		time(NULL));								\
	  }										\
}

#define p_error(s, r)									\
{											\
	if (errno == 0)									\
		errno = EINVAL;								\
	fprintf(stderr, "%s: %s: line %d\r\n", (s), strerror(errno), __LINE__);		\
	debug((s), (r), NULL);								\
	if ((r) != 0xff)								\
		return ((r));								\
	else										\
		exit((r));								\
}

#define reset_global()									\
{											\
	global_data.total_lines &= ~global_data.total_lines;				\
	global_data.done_lines &= ~global_data.done_lines;				\
	global_data.total_lines = do_line_count(filename);				\
}

#define open_map()									\
{											\
	memset(&statb, 0, sizeof(statb));						\
	lstat(filename, &statb);							\
	if ((fd_old = open(filename, OLD_FILE_FLAGS)) < 0)				\
	  {										\
		fprintf(stderr,								\
		"failed to open \"%s\": %s (line %d)\r\n",				\
		filename, strerror(errno), __LINE__);					\
	  }										\
	debug("opened file on", fd_old, NULL);						\
	map = mmap(NULL, statb.st_size, OLD_FILE_PROT, OLD_FILE_MAP, fd_old, 0);	\
	if (map == MAP_FAILED)								\
	  {										\
		fprintf(stderr,								\
		"failed to map \"%s\" into memory: %s (line %d)\r\n",			\
		filename, strerror(errno), __LINE__);					\
	  }										\
	debug("mapped file into memory", 0, filename);					\
	close(fd_old);									\
	if (mlock(map, statb.st_size) < 0)						\
	  {										\
		fprintf(stderr,								\
		"failed to lock memory (%p to %p): %s (line %d)\r\n",			\
		map, (map + (statb.st_size - 1)), strerror(errno), __LINE__);		\
	  }										\
	debug("locked region of mapped memory", 0, NULL);				\
	if ((fd_new = open(output, CREATION_FLAGS, CREATION_MASK)) < 0)			\
	  {										\
		fprintf(stderr,								\
		"failed to open \"%s\" file: %s (line %d)\r\n",				\
		output, strerror(errno), __LINE__);					\
	  }										\
	debug("opened output file on", fd_new, NULL);					\
}

#define close_unmap()									\
{											\
	if (munlock(map, statb.st_size) < 0)						\
	  {										\
		fprintf(stderr,								\
		"failed to unlock memory (%p to %p): %s (line %d)\r\n",			\
		map, (map + (statb.st_size - 1)), strerror(errno), __LINE__);		\
	  }										\
	debug("unlocked mapped memory region", 0, NULL);				\
	if (munmap(map, statb.st_size) < 0)						\
	  {										\
		fprintf(stderr,								\
		"failed to unmap memory (%p to %p): %s (line %d)\r\n",			\
		map, (map + (statb.st_size - 1)), strerror(errno), __LINE__);		\
	  }										\
	debug("unmapped file from memory", 0, filename);				\
	close(fd_new);									\
	unlink(filename);								\
	rename(output, filename);							\
}

#define clear_struct(s) memset((s), 0, sizeof(*(s)))
		
// file manipulation functions
ssize_t	check_file(char *) __THROW __nonnull ((1)) __wur;
ssize_t change_length(char *) __THROW __nonnull ((1)) __wur;
ssize_t	change_line_length(char *) __THROW __nonnull ((1)) __wur;
ssize_t	justify_text(char *) __THROW __nonnull ((1)) __wur;
ssize_t unjustify_text(char *) __THROW __nonnull ((1)) __wur;
ssize_t left_align_text(char *) __THROW __nonnull ((1)) __wur;
ssize_t right_align_text(char *) __THROW __nonnull ((1)) __wur;
ssize_t centre_align_text(char *) __THROW __nonnull ((1)) __wur;

// screen-drawing functions
void clear(void);
void deleteline(void);
void h_centre(int, int, int);
void v_centre(int, int, int);
void up(int);
void down(int);
void left(int);
void right(int);
void fill_line(char *) __THROW __nonnull ((1));
void fill(void);

// data-related functions
int do_line_count(char *) __THROW __nonnull ((1));
void print_fileinfo(char *) __THROW __nonnull ((1));

// thread-related functions
void *show_progress(void *);

// misc
void usage(void) __attribute__ ((__noreturn__));

			/* GLOBAL VARIABLES */
struct GLOBAL_DATA
{
	int		total_lines;
	int		done_lines;
};

typedef struct GLOBAL_DATA		GLOBAL_DATA;

static GLOBAL_DATA		global_data;
static int		MAX_LENGTH = 0;
//static int				TOTAL_LINES;
//static int				NEW_TOTAL;
static struct winsize			WINSIZE;
static int		POSITION;

static FILE		*debug_fp = NULL;
static int		debug_fd;
static char		*debug_log = "debug.log";

// tmp files
static char		*output = "output.tmp";
// global flags
static int		JUSTIFY = 0;
static int		UNJUSTIFY = 0;
static int		LENGTH = 0;
static int		LALIGN = 0;
static int		RALIGN = 0;
static int		CALIGN = 0;
static int		DEBUG = 0;

		/* Thread-related variables */
//static pthread_mutex_t			MUTEX;
static pthread_attr_t			tATTR;
static pthread_t			TID_SP;
//static sigjmp_buf			__jb_sp;
static char				*__sp_str_format =    "[ Changing line length ]";
static char				*__sp_str_justify =   "[   Justifying lines   ]";
static char				*__sp_str_unjustify = "[  Unjustifying lines  ]";
static char				*__sp_str_lalign =    "[  Left aligning lines ]";
static char				*__sp_str_ralign =    "[ Right aligning lines ]";
static char				*__sp_str_calign =    "[    Centering lines   ]";

int
main(int argc, char *argv[])
{
	static char		c;

	if (argc == 1)
		p_error("need to specify path to a file", 0xff);

	/* initialize variables */
	memset(&WINSIZE, 0, sizeof(WINSIZE));
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &WINSIZE);
	setvbuf(stdout, NULL, _IONBF, 0);
	memset(&global_data, 0, sizeof(global_data));
	pthread_attr_setdetachstate(&tATTR, PTHREAD_CREATE_DETACHED);

	opterr = 0;
	while ((c = getopt(argc, argv, "L:lrcjuDh")) != -1)
	{
		switch(c)
		{
			case(0x44):
			DEBUG = 1;
			debug_fd = open(debug_log, O_RDWR|O_CREAT|O_TRUNC|O_FSYNC, S_IRUSR|S_IWUSR);
			debug_fp = fdopen(debug_fd, "r+");
			debug("main: opened log for debugging", 0, NULL);
			break;
			case(0x6c):
			LALIGN = 1;
			break;
			case(0x72):
			RALIGN = 1;
			break;
			case(0x63):
			CALIGN = 1;
			break;
			case(0x4c):
			MAX_LENGTH = atoi(optarg);
			LENGTH = 1;
			break;
			case(0x6a):
			JUSTIFY = 1;
			break;
			case(0x75):
			UNJUSTIFY = 1;
			break;
			case(0x68):
			usage();
			break;
			case(0x3f):
			p_error("invalid option specified", 0xff);
			break;
			default:
			p_error("invalid option specified", 0xff);
		}
	}

	if (UNJUSTIFY && JUSTIFY)
		p_error("cannot have both -j and -u", 0xff);

	if (JUSTIFY && (LALIGN || RALIGN || CALIGN))
		p_error("cannot have -j with -r or -c", 0xff);

	if ((RALIGN && (CALIGN || LALIGN)) || (CALIGN && (RALIGN || LALIGN)))
		p_error("options -l, -r and -c are mutually exclusive", 0xff);

	if (LENGTH && UNJUSTIFY)
	  {
		UNJUSTIFY = 0;
		debug("main: switched off UNJUSTIFY flag (unncessary used with -L)", 0, NULL);
	  }

	if (check_file(argv[optind]) == -1)
		p_error("main", 0xff);

	clear();
	fill();
	up(6);
	POSITION = 6;
	print_fileinfo(argv[optind]);
	down(POSITION-2);

	if (LENGTH)
	{
		pthread_create(&TID_SP, NULL, show_progress, (void *)__sp_str_format);
		debug("main: created thread", (int)TID_SP, NULL);
		if (change_line_length(argv[optind]) == -1)
			goto __err;
	}

	if (JUSTIFY)
	{
		pthread_create(&TID_SP, NULL, show_progress, (void *)__sp_str_justify);
		debug("main: created thread", (int)TID_SP, NULL);
		if (justify_text(argv[optind]) == -1)
			goto __err;
	}
	
	if (UNJUSTIFY)
	{
		pthread_create(&TID_SP, NULL, show_progress, (void *)__sp_str_unjustify);
		debug("main: created thread", (int)TID_SP, NULL);
		if (unjustify_text(argv[optind]) == -1)
			goto __err;
	}

	if (LALIGN)
	{
		pthread_create(&TID_SP, NULL, show_progress, (void *)__sp_str_lalign);
		debug("main: created thread", (int)TID_SP, NULL);
		if (left_align_text(argv[optind]) == -1)
			goto __err;
	}

	if (RALIGN)
	{
		pthread_create(&TID_SP, NULL, show_progress, (void *)__sp_str_ralign);
		debug("main: created thread", (int)TID_SP, NULL);
		if (right_align_text(argv[optind]) == -1)
			goto __err;
	}

	if (CALIGN)
	{
		pthread_create(&TID_SP, NULL, show_progress, (void *)__sp_str_calign);
		debug("main: created thread", (int)TID_SP, NULL);
		if (centre_align_text(argv[optind]) == -1)
			goto __err;
	}

	exit(0);

	__err:
	exit(0xff);
}

ssize_t
check_file(char *filename)
{
	struct stat		STATB;

	lstat(filename, &STATB);
	if (!S_ISREG(STATB.st_mode))
		p_error("not regular file", -1);
	if (access(filename, W_OK|R_OK) != 0)
		p_error("no read/write permissions for file", -1);
	return(0);
}

ssize_t
change_line_length(char *filename)
{
	// pointers for manipulating lines in the file
	char		*start = NULL;
	char		*end = NULL;
	char		*p = NULL;
	//size_t	len;
	//static char		*p1 = NULL;
	char		line_buf[LINE_BUF_SIZE];
	char		c;
	int		fd_old, fd_new, char_cnt, i;//, max;
	int		spaces;
	void	*map = NULL;
	char	*map_end = NULL;
	struct stat statb;

	reset_global();
	open_map();

	p = (char *)map;
	map_end = ((char *)map + statb.st_size);
	//len = statb.st_size;
	i = 0;

	// unjustify the text first (and automatically left-align)
	start = p;
	while (p < map_end)
	{
		while ((*p == 0x20 || *p == 0x09) && p < map_end)
			++p;

		start = p;
		while (*p != 0x0a && p < map_end)
		{
			if (*p == 0x20)
			{
				line_buf[i++] = *p++;
				while (*p == 0x20 && p < map_end)
					++p;
			}
			line_buf[i++] = *p++;
		}
		while (*p == 0x0a)
			line_buf[i++] = *p++;

		line_buf[i] = 0;
		write(fd_new, line_buf, (size_t)i);
		i = 0;
	}

	close_unmap();
	reset_global();
	open_map();

	p = (char *)map;
	map_end = ((char *)map + statb.st_size);
	//len = statb.st_size;

	start = p;
	while (p < map_end)
	{
		__main_loop_length_start:

		if (p == map_end)
			break;

		if (*p == 0x0a)
		{
			while (*p == 0x0a)
			{
				write(fd_new, p++, 1);
				++global_data.done_lines;
			}
		}

		while ((*p == 0x20 || *p == 0x09) && p < map_end)
			++p;

		start = p;
		char_cnt = 0;

		while (char_cnt < MAX_LENGTH && p < map_end)
		{
			++char_cnt;
			++p;
		}

		/* deal with new line characters; conserve paragraph structure */
		end = p;
		p = start;
		spaces = 0;

		while (p < end)
		{
			if (*p == 0x0a && (*(p+1) == 0x0a || *(p-1) == 0x0a))
			{
				end = p;
				while (*end == 0x0a && end < map_end)
				{
					++end;
					++global_data.done_lines;
				}

				p = start;
				i = 0;

				while (p < end)
					line_buf[i++] = *p++;

				line_buf[i] = 0;
				write(fd_new, line_buf, strlen(line_buf));
				start = p;
				goto __main_loop_length_start;
			}
			else
			if (*p == 0x0a && (*(p+1) != 0x0a && *(p-1) != 0x0a))
			{
				*p = 0x20;
				++global_data.done_lines;
			}

			if (*p == 0x20)
				++spaces;

			++p;
		}

		if (spaces == 0)
		{
			p = start;
			i = 0;
			while (p < end)
				line_buf[i++] = *p++;
			if (*p == 0x0a)
				line_buf[i++] = *p++;
			else
			{
				line_buf[i++] = 0x0a;
			}
			line_buf[i] = 0;
			write(fd_new, line_buf, strlen(line_buf));
			++global_data.done_lines;
			start = p;
			goto __main_loop_length_start;
		}

		/* If we're here, we found a range of MAX_LENGTH characters not including an entre-paragraph */
		if (*p == 0x0a)
		{
			while (*end == 0x0a && end < map_end)
			{
				++end;
				++global_data.done_lines;
			}

			write(fd_new, start, (end - start));
			p = end;
			start = p;
		}
		else
		{
			if (*p == 0x20)
			{
				write(fd_new, start, (end - start));
				c = 0x0a;
				write(fd_new, &c, 1);
				++end;
				p = end;
				start = p;
			}
			else
			{
				/* we don't want the end of the line to be in the middle of a word,
				 * so find the nearest space character going backwards
				 */
				while (*p != 0x20 && p > ((char *)map + 1))
					--p;
				end = p;
				write(fd_new, start, (end - start));
				c = 0x0a;
				write(fd_new, &c, 1);
				++end;
				p = end;
				start = p;
			}
		}
	}

	//__end:
	close_unmap();
	pthread_join(TID_SP, NULL);
	return(0);
}

ssize_t
justify_text(char *filename)
{
	static char			*p = NULL, *start = NULL, *end = NULL;
	static char			j_line[256];
	static int			char_cnt, fd_old, fd_new, i, j;
	static void			*map = NULL;
	static struct stat		statb;

	reset_global();
	open_map();
	p = (char *)map;

	char_cnt = 0;
	if (!LENGTH) // find len of longest line and justify according to that
	{
		MAX_LENGTH = 0; // au cas où
		while (p < (char *)(map + statb.st_size))
		{
			char_cnt = 0;
			while (*p == 0x20 || *p == 0x09)
				++p;
			while (*p != 0x0a && p < (char *)(map + statb.st_size))
			{
				if (*p == 0x20)
				{
					if (*(p+1) == 0x20)
					{
						fprintf(stderr,
						"Text already justified\r\n");
						goto __err;
					}
				}
				++p; ++char_cnt;
			}
			while (*p == 0x0a && p < (char *)(map + statb.st_size))
				++p;
			if (char_cnt > MAX_LENGTH)
				MAX_LENGTH = char_cnt;
		}
		p = (char *)map;
	}

	start = p;
	while (p < (char *)(map + statb.st_size))
	{
		__main_loop_justify_start:

		if (p == (char *)(map + statb.st_size))
			break;

		if (*p == 0x0a)
			while (*p == 0x0a)
			{ write(fd_new, p++, 1); ++global_data.done_lines; }

		while (*p == 0x20 || *p == 0x09)
			++p;

		start = p; char_cnt &= ~char_cnt;
		while (*p != 0x0a && p < (char *)(map + statb.st_size))
		{ ++p; ++char_cnt; }

		if (char_cnt > MAX_LENGTH)
		{
			fprintf(stderr, "counted more characters than are in the longest line\r\n"
					"count %d: longest line %d\r\n",
				char_cnt, MAX_LENGTH);
			goto __err;
		}


	// now we have the line and we'll add more spaces if necessary

		end = p;
		while (*end == 0x0a && end < (char *)(map + statb.st_size))
		{ ++end; ++global_data.done_lines; }

		if (MAX_LENGTH < char_cnt)
		{
			fprintf(stderr,
				"justify length specified too short (%d - length of line %d)\r\n",
				MAX_LENGTH, char_cnt);
			goto __err;
		}
		else if (MAX_LENGTH == char_cnt)
		{
			write(fd_new, start, (end - start));
			p = end; start = p;
		}
		else if ((MAX_LENGTH - char_cnt) > 15)
		{
			write(fd_new, start, (end - start));
			p = end; start = p;
		}
		else
		{
			static int			delta, spaces, remainder;
			static char			*lim1 = NULL;
			static char			*lim2 = NULL;
			static char			*l_end = NULL;
			static char			*l = NULL;
			static char			*l2 = NULL;

			spaces &= ~spaces;
			delta &= ~delta;
			remainder &= ~remainder;

			p = start;
			while (p < end)
			{
				if (*p == 0x20)
				{
					++spaces;
					while (*p == 0x20)
						++p;
				}
				++p;
			}

			if (spaces == 0)
			{
				write(fd_new, start, (end - start));
				p = end; start = p;
				goto __main_loop_justify_start;
			}

			delta = (MAX_LENGTH - char_cnt);
			remainder = (delta % spaces);

			p = start; i &= ~i;
			while (p < end)
			{
				if (*p == 0x20)
				{
					j_line[i++] = *p++;
					for (j = 0; j < (delta/spaces); ++j)
						j_line[i++] = 0x20;
				}
				j_line[i++] = *p++;
			}

			j_line[i] = 0;
			
			if (remainder != 0)
			{
				l_end = j_line;
				while (*l_end != 0)
					++l_end;

				if (*(l_end-2) == 0x20)
				{
					l_end -= 2;
					remainder += 2;
					while (*l_end == 0x20 && l_end > j_line)
					{ --l_end; ++remainder; }
					++l_end;
					--remainder;
					*l_end++ = 0x0a;
					--remainder;
					*l_end = 0;
				}

				lim2 = l_end;
				lim1 = j_line;

				while (remainder != 0 && remainder > -1)
				{
					l = lim1;
					while (*l != 0x20)
						++l;
					l2 = l;
					l = l_end;
					while (l > l2)
					{
						*l = *(l-1);
						--l;
					}
					while (*l2 == 0x20 && l2 < l_end)
						++l2;
					if (l2 == l_end)
					{
						if (remainder > 0)
							lim1 = l2 = j_line;
						else
							break;
					}
					lim1 = l2;

					++l_end;
					*l_end = 0;

					--remainder;
					if (remainder == 0 || remainder < 0)
						break;

					l = lim2;
					while (*l != 0x20)
						--l;
					l2 = l;
					l = l_end;
					while (l > l2)
					{
						*l = *(l-1);
						--l;
					}
					while (*l2 == 0x20 && l2 > j_line)
						--l2;
					if (l2 == j_line)
					{
						if (remainder > 0)
							lim2 = l2 = l_end;
						else
							break;
					}
					lim2 = l2;

					++l_end;
					*l_end = 0;

					--remainder;
					if (remainder == 0 || remainder < 0)
						break;
				}
			}

			write(fd_new, j_line, strlen(j_line));
			p = end; start = p;
		}
	}

	//__end:
	close_unmap();
	++global_data.done_lines;
	pthread_join(TID_SP, NULL);
	return(0);

	__err:
	pthread_kill(TID_SP, SIGINT);
	if (mlock(map, statb.st_size) < 0)
	{
		fprintf(stderr,
		"failed to unlock memory region (%p to %p): %s (line %d)\r\n",
		map, (map + (statb.st_size - 1)), strerror(errno), __LINE__);
	}
	if (munmap(map, statb.st_size) < 0)
	{
		fprintf(stderr,
		"failed to unmap \"%s\": %s (line %d)\r\n",
		filename, strerror(errno), __LINE__);
		
	}
	debug("justify_text: __err: unmapped file from memory", 0, NULL);
	close(fd_new);
	unlink(output);
	debug("justify_text: __err: unlinked output file", 0, filename);
	close(fd_old);
	return -1;
}

ssize_t
unjustify_text(char *filename)
{
	static int		fd_old, fd_new, i;
	static void		*map = NULL;
	static struct stat	statb;
	static char		*p = NULL;// *start = NULL, *end = NULL;
	static char		line_buf[256];

	global_data.total_lines &= ~global_data.total_lines;
	global_data.done_lines &= ~global_data.done_lines;

	global_data.total_lines = do_line_count(filename);

	memset(&statb, 0, sizeof(statb));
	if (lstat(filename, &statb) < 0)
		p_error("unjustify_text: lstat error", -1);
	if ((fd_old = open(filename, OLD_FILE_FLAGS)) < 0)
		p_error("unjustify_text: error opening input file", -1);
	if ((map = mmap(NULL, statb.st_size, OLD_FILE_PROT, OLD_FILE_MAP, fd_old, 0)) == MAP_FAILED)
		p_error("unjustify_text: error mapping file into memory", -1);
	close(fd_old);
	if ((fd_new = open(output, CREATION_FLAGS, CREATION_MASK)) < 0)
		p_error("unjustify_text: error creating output file", -1);

	p = (char *)map;

	while ((void *)p < (map + statb.st_size))
	  {
		i = 0;
		while ((*p == 0x20 || *p == 0x09) && (void *)p < (map + statb.st_size))
			++p;
		//start = p;
		while (*p != 0x0a && (void *)p < (map + statb.st_size))
		  {
			if (*p == 0x20)
			  {
				line_buf[i++] = *p++;
				while (*p == 0x20)
					++p;
			  }
			line_buf[i++] = *p++;
		  }
		while (*p == 0x0a && (void *)p < (map + statb.st_size))
		  {
			line_buf[i++] = *p++;
			++global_data.done_lines;
		  }
		line_buf[i] = 0;
		write(fd_new, line_buf, strlen(line_buf));
		if (((map + statb.st_size) - (void *)p) <= MAX_LENGTH)
		  {
			i = 0;
			while ((void *)p < (map + statb.st_size))
				line_buf[i++] = *p++;
			line_buf[i] = 0;
			write(fd_new, line_buf, strlen(line_buf));
			++global_data.done_lines;
			break;
		  }
	  }

	munmap(map, statb.st_size);
	unlink(filename);
	rename(output, filename);
	return(0);
}

ssize_t
left_align_text(char *filename)
{
	static char		*p = NULL, *start = NULL, *end = NULL;
	static char		line_buf[LINE_BUF_SIZE];//, c;
	static int		fd_old, fd_new, i;//, j;
	static void		*map = NULL;
	static struct stat	statb;

	
	reset_global();
	open_map();

	p = (char *)map;

	while (p < (char *)(map + statb.st_size))
	  {
		if (*p == 0x0a)
			while (*p == 0x0a)
				{ write(fd_new, p++, 1); ++global_data.done_lines; }

		start = p;
		while ((*p == 0x20 || *p == 0x09) && p < (char *)(map + statb.st_size))
			++p;

		if (p == (char *)(map + statb.st_size))
		  {
			i &= ~i; end = p; p = start;
			while (p < end)
			  {
				if (*p == 0x20)
				  {
					line_buf[i++] = *p++;
					while (*p == 0x20)
						++p;
				  }
				line_buf[i++] = *p++;
			  }
			line_buf[i] = 0;
			if (i > 0)
			  {
				write(fd_new, line_buf, (size_t)i);
				++global_data.done_lines;
			  }
			break;
		  }

		start = p; i &= ~i;
		while (*p != 0x0a && p < (char *)(map + statb.st_size))
		  {
			if (*p == 0x20)
			  {
				line_buf[i++] = *p++;
				while (*p == 0x20)
					++p;
			  }
			line_buf[i++] = *p++;
		  }

		if (p == (char *)(map + statb.st_size))
		  {
			line_buf[i] = 0;
			write(fd_new, line_buf, (size_t)i);
			++global_data.done_lines;
			break;
		  }


		end = p;
		while (*end == 0x0a)
		  {
			line_buf[i++] = *end++;
			++global_data.done_lines;
		  }

		line_buf[i] = 0;
		write(fd_new, line_buf, (size_t)i);

		p = end; start = p;
	  }

	close_unmap();
	return(0);
}

ssize_t
right_align_text(char *filename)
{
	static char		*p = NULL, *start = NULL, *end = NULL, *end2 = NULL, c;
	static char		line_buf[LINE_BUF_SIZE];
	static int		i, fd_old, fd_new, delta, char_cnt;//,j;
	static void		*map = NULL;
	static struct stat	statb;

	reset_global();
	open_map();
	p = (char *)map;

	if (!LENGTH) // then find # chars in longest line
	  {
		while (p < (char *)(map + statb.st_size))
		  {
			if (*p == 0x0a)
				while (*p == 0x0a)
					++p;
			while (*p == 0x20 || *p == 0x09)
				++p;

			start = p; char_cnt &= ~char_cnt;
			while (*p != 0x0a && p < (char *)(map + statb.st_size))
			  {
				if (*p == 0x20)
				  {
					++char_cnt;
					while (*p == 0x20)
						++p;
					continue;
				  }
				++char_cnt; ++p;
			  }

			if (p == (char *)(map + statb.st_size))
			  {
				if (char_cnt > MAX_LENGTH)
					MAX_LENGTH = char_cnt;
				break;
			  }

			end = p; ++end;
			while (*end == 0x0a)
				++end;

			if (char_cnt > MAX_LENGTH)
				MAX_LENGTH = char_cnt;

			p = end; start = p;
		  }
		p = (char *)map;
	  }

	while (p < (char *)(map + statb.st_size))
	  {
		if (*p == 0x0a)
			while (*p == 0x0a)
				{ write(fd_new, p++, 1); ++global_data.done_lines; }

		while (*p == 0x20 || *p == 0x09)
			++p;

		start = p;

		char_cnt &= ~char_cnt;
		while (*p != 0x0a && p < (char *)(map + statb.st_size))
		  {
			if (*p == 0x20)
			  {
				++char_cnt;
				while (*p == 0x20)
					++p;
				continue;
			  }
			++char_cnt; ++p;
		  }

		if (p == (char *)(map + statb.st_size))
		  {
			end2 = p; --p;
			while (*p == 0x20 || *p == 0x09)
				{ --p; --char_cnt; }
			end = p; ++end;
			delta &= ~delta;
			delta = (MAX_LENGTH - char_cnt);
			i &= ~i; c = 0x20;
			while (i < delta)
				{ write(fd_new, &c, 1); ++i; }
			i &= ~i; p = start;
			while (p < end)
			  {
				if (*p == 0x20)	
				  {
					line_buf[i++] = *p++;
					while (*p == 0x20)
						++p;
				  }
				line_buf[i++] = *p++;
			  }
			line_buf[i] = 0;
			write(fd_new, line_buf, (size_t)i);
			++global_data.done_lines;
			break;
		  }

		end2 = p; --p;

		while (*p == 0x20 || *p == 0x09)
			{ --p; --char_cnt; }
		end = p; ++end; p = start;

		delta &= ~delta;
		delta = (MAX_LENGTH - char_cnt);

		i &= ~i; c = 0x20;
		while (i < delta)
			{ write(fd_new, &c, 1); ++i; }

		i &= ~i; p = start;
		while (p < end)
		  {
			if (*p == 0x20)
			  {
				line_buf[i++] = *p++;
				while (*p == 0x20)
					++p;
			  }
			line_buf[i++] = *p++;
		  }
		while (*end2 == 0x0a)
			{ line_buf[i++] = *end2++; ++global_data.done_lines; }
		line_buf[i] = 0;
		write(fd_new, line_buf, (size_t)i);
		p = end2; start = p; end = p;
	  }

	close_unmap();
	return(0);
}

ssize_t
centre_align_text(char *filename)
{
	static char		*p = NULL, *start = NULL, *end = NULL, *end2 = NULL;
	static char		c;
	static int		fd_old, fd_new, i, char_cnt, delta;
	static void		*map = NULL;
	static struct stat	statb;

	reset_global();
	open_map();

	p = (char *)map;

	if (!LENGTH)
	  {
		MAX_LENGTH &= ~MAX_LENGTH;
		while (p < (char *)(map + statb.st_size))
		  {
			char_cnt &= ~char_cnt;
			while (*p != 0x0a)
			  {
				if (*p == 0x20)
				  {
					++char_cnt;
					while (*p == 0x20)
						++p;
					continue;
				  }
				++char_cnt;
				++p;
			  }
			if (char_cnt > MAX_LENGTH)
				MAX_LENGTH = char_cnt;
			while (*p == 0x0a)
				++p;
		  }
		p = (char *)map;
	  }

	start = p;
	while (p < (char *)(map + statb.st_size))
	  {
		if (*p == 0x0a)
		  {
			while (*p == 0x0a)
			  {
				write(fd_new, p, 1);
				++global_data.done_lines;
				++p;
			  }
		  }

		while (*p == 0x20 || *p == 0x09)
			++p;

		start = p;

		char_cnt &= ~char_cnt;
		while (*p != 0x0a && p < (char *)(map + statb.st_size))
		  {
			if (*p == 0x20)
			  {
				++char_cnt;
				while (*p == 0x20)
					++p;
				continue;
			  }
			++char_cnt;
			++p;
		  }

		end2 = p; // point to new line char

		--p;
		while (*p == 0x20 || *p == 0x09)
			--p;

		end = p;
		++end;

		delta &= ~delta;
		delta = (MAX_LENGTH - char_cnt);

		i &= ~i; c = 0x20;
		while (i < (delta/2))
			{ write(fd_new, &c, 1); ++i; }
			//line_buf[i++] = 0x20;

		p = start;
		write(fd_new, p, (end-p));
			//line_buf[i++] = *p++;
		while (*end2 == 0x0a)
		  {
			write(fd_new, end2, 1);
			//line_buf[i++] = *end2++;
			++global_data.done_lines;
			++end2;
		  }

		p = end2; start = p; end = p;
	  }

	close_unmap();
	return(0);
}

int
do_line_count(char *filename)
{
	int			fd, count;
	void		*map = NULL;
	struct	stat	statb;
	char		*p = NULL;

	debug("do_line_count: opening file", 0, filename);
	memset(&statb, 0, sizeof(statb));
	if (lstat(filename, &statb) < 0)
		p_error("do_line_count: lstat error", -1);
	if ((fd = open(filename, O_RDONLY)) < 0)
		p_error("do_line_count: error opening file", -1);
	if ((map = mmap(NULL, statb.st_size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED)
		p_error("do_line_count: error mapping file into memory", -1);
	if (mlock(map, statb.st_size) < 0)
		p_error("do_line_count: error locking mapped memory", -1);
	close(fd);

	count = 0;
	p = (char *)map;

	while (p < ((char *)map + statb.st_size))
	{
		if (*p == 0x0a)
			++count;

		++p;
	}

	if (munlock(map, statb.st_size) < 0)
		p_error("do_line_count: error unlocking mapped memory", -1);

	munmap(map, statb.st_size);
	return(count);
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
	static int		i, j;

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
	static struct stat		statb;
	static char			buffer[512];
	static struct tm		*TIME = NULL;
	static mode_t			mode;

	memset(&statb, 0, sizeof(statb));
	lstat(filename, &statb);
	fill_line(FILE_STATS_COLOUR);
	printf("%s", FILE_STATS_COLOUR);
	printf("%22s %s\r\n", "FILENAME", filename);
	--POSITION;
	fill_line(FILE_STATS_COLOUR);
	printf("%s", FILE_STATS_COLOUR);
	TIME = gmtime(&statb.st_ctime);
	strftime(buffer, 40, "%A %d %B %Y at %T %Z", TIME);
	printf("%22s %s\r\n", "CREATED", buffer);
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
usage(void)
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
	exit(0);
}
