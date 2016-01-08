/*
 * (C) Copyright 2000-2013
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * (C) Copyright 2001 Sysgo Real-Time Solutions, GmbH <www.elinos.com>
 * Andreas Heppel <aheppel@sysgo.de>
 *
 * Copyright 2011 Freescale Semiconductor, Inc.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

/*
 * Support for persistent environment data
 *
 * The "environment" is stored on external storage as a list of '\0'
 * terminated "name=value" strings. The end of the list is marked by
 * a double '\0'. The environment is preceeded by a 32 bit CRC over
 * the data part and, in case of redundant environment, a byte of
 * flags.
 *
 * This linearized representation will also be used before
 * relocation, i. e. as long as we don't have a full C runtime
 * environment. After that, we use a hash table.
 */

#include <common.h>
#include <command.h>
#include <environment.h>
#include <search.h>
#include <errno.h>
#include <malloc.h>
#include <watchdog.h>
#include <linux/stddef.h>
#include <asm/byteorder.h>

DECLARE_GLOBAL_DATA_PTR;

#if	!defined(CONFIG_ENV_IS_IN_EEPROM)	&& \
	!defined(CONFIG_ENV_IS_IN_FLASH)	&& \
	!defined(CONFIG_ENV_IS_IN_DATAFLASH)	&& \
	!defined(CONFIG_ENV_IS_IN_MMC)		&& \
	!defined(CONFIG_ENV_IS_IN_FAT)		&& \
	!defined(CONFIG_ENV_IS_IN_NAND)		&& \
	!defined(CONFIG_ENV_IS_IN_NVRAM)	&& \
	!defined(CONFIG_ENV_IS_IN_ONENAND)	&& \
	!defined(CONFIG_ENV_IS_IN_SPI_FLASH)	&& \
	!defined(CONFIG_ENV_IS_IN_REMOTE)	&& \
	!defined(CONFIG_ENV_IS_IN_UBI)		&& \
	!defined(CONFIG_ENV_IS_IN_RAM)          && \
	!defined(CONFIG_ENV_IS_NOWHERE)
# error Define one of CONFIG_ENV_IS_IN_{EEPROM|FLASH|DATAFLASH|ONENAND|\
SPI_FLASH|NVRAM|MMC|FAT|REMOTE|UBI|RAM} or CONFIG_ENV_IS_NOWHERE
#endif

/*
 * Maximum expected input data size for import command
 */
#define	MAX_ENV_SIZE	(1 << 20)	/* 1 MiB */

/*
 * This variable is incremented on each do_env_set(), so it can
 * be used via get_env_id() as an indication, if the environment
 * has changed or not. So it is possible to reread an environment
 * variable only if the environment was changed ... done so for
 * example in NetInitLoop()
 */
static int env_id = 1;

int get_env_id(void)
{
	return env_id;
}

#ifndef CONFIG_SPL_BUILD
/*
 * Command interface: print one or all environment variables
 *
 * Returns 0 in case of error, or length of printed string
 */
static int env_print(char *name, int flag)
{
	char *res = NULL;
	ssize_t len;

	if (name) {		/* print a single name */
		ENTRY e, *ep;

		e.key = name;
		e.data = NULL;
		hsearch_r(e, FIND, &ep, &env_htab, flag);
		if (ep == NULL)
			return 0;
		len = printf("%s=%s\n", ep->key, ep->data);
		return len;
	}

	/* print whole list */
	len = hexport_r(&env_htab, '\n', flag, &res, 0, 0, NULL);

	if (len > 0) {
		puts(res);
		free(res);
		return len;
	}

	/* should never happen */
	printf("## Error: cannot export environment\n");
	return 0;
}

static int do_env_print(cmd_tbl_t *cmdtp, int flag, int argc,
			char * const argv[])
{
	int i;
	int rcode = 0;
	int env_flag = H_HIDE_DOT;

	if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'a') {
		argc--;
		argv++;
		env_flag &= ~H_HIDE_DOT;
	}

	if (argc == 1) {
		/* print all env vars */
		rcode = env_print(NULL, env_flag);
		if (!rcode)
			return 1;
		printf("\nEnvironment size: %d/%ld bytes\n",
			rcode, (ulong)ENV_SIZE);
		return 0;
	}

	/* print selected env vars */
	env_flag &= ~H_HIDE_DOT;
	for (i = 1; i < argc; ++i) {
		int rc = env_print(argv[i], env_flag);
		if (!rc) {
			printf("## Error: \"%s\" not defined\n", argv[i]);
			++rcode;
		}
	}

	return rcode;
}

#ifdef CONFIG_CMD_GREPENV
static int do_env_grep(cmd_tbl_t *cmdtp, int flag,
		       int argc, char * const argv[])
{
	char *res = NULL;
	int len, grep_how, grep_what;

	if (argc < 2)
		return CMD_RET_USAGE;

	grep_how  = H_MATCH_SUBSTR;	/* default: substring search	*/
	grep_what = H_MATCH_BOTH;	/* default: grep names and values */

	while (argc > 1 && **(argv + 1) == '-') {
		char *arg = *++argv;

		--argc;
		while (*++arg) {
			switch (*arg) {
#ifdef CONFIG_REGEX
			case 'e':		/* use regex matching */
				grep_how  = H_MATCH_REGEX;
				break;
#endif
			case 'n':		/* grep for name */
				grep_what = H_MATCH_KEY;
				break;
			case 'v':		/* grep for value */
				grep_what = H_MATCH_DATA;
				break;
			case 'b':		/* grep for both */
				grep_what = H_MATCH_BOTH;
				break;
			case '-':
				goto DONE;
			default:
				return CMD_RET_USAGE;
			}
		}
	}

DONE:
	len = hexport_r(&env_htab, '\n',
			flag | grep_what | grep_how,
			&res, 0, argc, argv);

	if (len > 0) {
		puts(res);
		free(res);
	}

	if (len < 2)
		return 1;

	return 0;
}
#endif
#endif /* CONFIG_SPL_BUILD */

/*
 * Set a new environment variable,
 * or replace or delete an existing one.
 */
static int _do_env_set(int flag, int argc, char * const argv[])
{
	int   i, len;
	char  *name, *value, *s;
	ENTRY e, *ep;
	int env_flag = H_INTERACTIVE;

	debug("Initial value for argc=%d\n", argc);
	while (argc > 1 && **(argv + 1) == '-') {
		char *arg = *++argv;

		--argc;
		while (*++arg) {
			switch (*arg) {
			case 'f':		/* force */
				env_flag |= H_FORCE;
				break;
			default:
				return CMD_RET_USAGE;
			}
		}
	}
	debug("Final value for argc=%d\n", argc);
	name = argv[1];
	value = argv[2];

	if (strchr(name, '=')) {
		printf("## Error: illegal character '='"
		       "in variable name \"%s\"\n", name);
		return 1;
	}

	env_id++;

	/* Delete only ? */
	if (argc < 3 || argv[2] == NULL) {
		int rc = hdelete_r(name, &env_htab, env_flag);
		return !rc;
	}

	/*
	 * Insert / replace new value
	 */
	for (i = 2, len = 0; i < argc; ++i)
		len += strlen(argv[i]) + 1;

	value = malloc(len);
	if (value == NULL) {
		printf("## Can't malloc %d bytes\n", len);
		return 1;
	}
	for (i = 2, s = value; i < argc; ++i) {
		char *v = argv[i];

		while ((*s++ = *v++) != '\0')
			;
		*(s - 1) = ' ';
	}
	if (s != value)
		*--s = '\0';

	e.key	= name;
	e.data	= value;
	hsearch_r(e, ENTER, &ep, &env_htab, env_flag);
	free(value);
	if (!ep) {
		printf("## Error inserting \"%s\" variable, errno=%d\n",
			name, errno);
		return 1;
	}

	return 0;
}

int setenv(const char *varname, const char *varvalue)
{
	const char * const argv[4] = { "setenv", varname, varvalue, NULL };

	/* before import into hashtable */
	if (!(gd->flags & GD_FLG_ENV_READY))
		return 1;

	if (varvalue == NULL || varvalue[0] == '\0')
		return _do_env_set(0, 2, (char * const *)argv);
	else
		return _do_env_set(0, 3, (char * const *)argv);
}

int setenv_force(const char *varname, const char *varvalue)
{
	const char * const argv[5] = { "setenv", "-f", varname, varvalue, NULL };

	/* before import into hashtable */
	if (!(gd->flags & GD_FLG_ENV_READY))
		return 1;

	if (varvalue == NULL || varvalue[0] == '\0')
		return _do_env_set(0, 3, (char * const *)argv);
	else
		return _do_env_set(0, 4, (char * const *)argv);
}
/**
 * Set an environment variable to an integer value
 *
 * @param varname	Environment variable to set
 * @param value		Value to set it to
 * @return 0 if ok, 1 on error
 */
int setenv_ulong(const char *varname, ulong value)
{
	/* TODO: this should be unsigned */
	char *str = simple_itoa(value);

	return setenv(varname, str);
}

/**
 * Set an environment variable to an value in hex
 *
 * @param varname	Environment variable to set
 * @param value		Value to set it to
 * @return 0 if ok, 1 on error
 */
int setenv_hex(const char *varname, ulong value)
{
	char str[17];

	sprintf(str, "%lx", value);
	return setenv(varname, str);
}

ulong getenv_hex(const char *varname, ulong default_val)
{
	const char *s;
	ulong value;
	char *endp;

	s = getenv(varname);
	if (s)
		value = simple_strtoul(s, &endp, 16);
	if (!s || endp == s)
		return default_val;

	return value;
}

#ifndef CONFIG_SPL_BUILD
static int do_env_set(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	if (argc < 2)
		return CMD_RET_USAGE;

	return _do_env_set(flag, argc, argv);
}

/*
 * Prompt for environment variable
 */
#if defined(CONFIG_CMD_ASKENV)
int do_env_ask(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	char message[CONFIG_SYS_CBSIZE];
	int i, len, pos, size;
	char *local_args[4];
	char *endptr;

	local_args[0] = argv[0];
	local_args[1] = argv[1];
	local_args[2] = NULL;
	local_args[3] = NULL;

	/*
	 * Check the syntax:
	 *
	 * env_ask envname [message1 ...] [size]
	 */
	if (argc == 1)
		return CMD_RET_USAGE;

	/*
	 * We test the last argument if it can be converted
	 * into a decimal number.  If yes, we assume it's
	 * the size.  Otherwise we echo it as part of the
	 * message.
	 */
	i = simple_strtoul(argv[argc - 1], &endptr, 10);
	if (*endptr != '\0') {			/* no size */
		size = CONFIG_SYS_CBSIZE - 1;
	} else {				/* size given */
		size = i;
		--argc;
	}

	if (argc <= 2) {
		sprintf(message, "Please enter '%s': ", argv[1]);
	} else {
		/* env_ask envname message1 ... messagen [size] */
		for (i = 2, pos = 0; i < argc; i++) {
			if (pos)
				message[pos++] = ' ';

			strcpy(message + pos, argv[i]);
			pos += strlen(argv[i]);
		}
		message[pos++] = ' ';
		message[pos] = '\0';
	}

	if (size >= CONFIG_SYS_CBSIZE)
		size = CONFIG_SYS_CBSIZE - 1;

	if (size <= 0)
		return 1;

	/* prompt for input */
	len = readline(message);

	if (size < len)
		console_buffer[size] = '\0';

	len = 2;
	if (console_buffer[0] != '\0') {
		local_args[2] = console_buffer;
		len = 3;
	}

	/* Continue calling setenv code */
	return _do_env_set(flag, len, local_args);
}
#endif

#if defined(CONFIG_CMD_ENV_CALLBACK)
static int print_static_binding(const char *var_name, const char *callback_name)
{
	printf("\t%-20s %-20s\n", var_name, callback_name);

	return 0;
}

static int print_active_callback(ENTRY *entry)
{
	struct env_clbk_tbl *clbkp;
	int i;
	int num_callbacks;

	if (entry->callback == NULL)
		return 0;

	/* look up the callback in the linker-list */
	num_callbacks = ll_entry_count(struct env_clbk_tbl, env_clbk);
	for (i = 0, clbkp = ll_entry_start(struct env_clbk_tbl, env_clbk);
	     i < num_callbacks;
	     i++, clbkp++) {
#if defined(CONFIG_NEEDS_MANUAL_RELOC)
		if (entry->callback == clbkp->callback + gd->reloc_off)
#else
		if (entry->callback == clbkp->callback)
#endif
			break;
	}

	if (i == num_callbacks)
		/* this should probably never happen, but just in case... */
		printf("\t%-20s %p\n", entry->key, entry->callback);
	else
		printf("\t%-20s %-20s\n", entry->key, clbkp->name);

	return 0;
}

/*
 * Print the callbacks available and what they are bound to
 */
int do_env_callback(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	struct env_clbk_tbl *clbkp;
	int i;
	int num_callbacks;

	/* Print the available callbacks */
	puts("Available callbacks:\n");
	puts("\tCallback Name\n");
	puts("\t-------------\n");
	num_callbacks = ll_entry_count(struct env_clbk_tbl, env_clbk);
	for (i = 0, clbkp = ll_entry_start(struct env_clbk_tbl, env_clbk);
	     i < num_callbacks;
	     i++, clbkp++)
		printf("\t%s\n", clbkp->name);
	puts("\n");

	/* Print the static bindings that may exist */
	puts("Static callback bindings:\n");
	printf("\t%-20s %-20s\n", "Variable Name", "Callback Name");
	printf("\t%-20s %-20s\n", "-------------", "-------------");
	env_attr_walk(ENV_CALLBACK_LIST_STATIC, print_static_binding);
	puts("\n");

	/* walk through each variable and print the callback if it has one */
	puts("Active callback bindings:\n");
	printf("\t%-20s %-20s\n", "Variable Name", "Callback Name");
	printf("\t%-20s %-20s\n", "-------------", "-------------");
	hwalk_r(&env_htab, print_active_callback);
	return 0;
}
#endif

#if defined(CONFIG_CMD_ENV_FLAGS)
static int print_static_flags(const char *var_name, const char *flags)
{
	enum env_flags_vartype type = env_flags_parse_vartype(flags);
	enum env_flags_varaccess access = env_flags_parse_varaccess(flags);

	printf("\t%-20s %-20s %-20s\n", var_name,
		env_flags_get_vartype_name(type),
		env_flags_get_varaccess_name(access));

	return 0;
}

static int print_active_flags(ENTRY *entry)
{
	enum env_flags_vartype type;
	enum env_flags_varaccess access;

	if (entry->flags == 0)
		return 0;

	type = (enum env_flags_vartype)
		(entry->flags & ENV_FLAGS_VARTYPE_BIN_MASK);
	access = env_flags_parse_varaccess_from_binflags(entry->flags);
	printf("\t%-20s %-20s %-20s\n", entry->key,
		env_flags_get_vartype_name(type),
		env_flags_get_varaccess_name(access));

	return 0;
}

/*
 * Print the flags available and what variables have flags
 */
int do_env_flags(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	/* Print the available variable types */
	printf("Available variable type flags (position %d):\n",
		ENV_FLAGS_VARTYPE_LOC);
	puts("\tFlag\tVariable Type Name\n");
	puts("\t----\t------------------\n");
	env_flags_print_vartypes();
	puts("\n");

	/* Print the available variable access types */
	printf("Available variable access flags (position %d):\n",
		ENV_FLAGS_VARACCESS_LOC);
	puts("\tFlag\tVariable Access Name\n");
	puts("\t----\t--------------------\n");
	env_flags_print_varaccess();
	puts("\n");

	/* Print the static flags that may exist */
	puts("Static flags:\n");
	printf("\t%-20s %-20s %-20s\n", "Variable Name", "Variable Type",
		"Variable Access");
	printf("\t%-20s %-20s %-20s\n", "-------------", "-------------",
		"---------------");
	env_attr_walk(ENV_FLAGS_LIST_STATIC, print_static_flags);
	puts("\n");

	/* walk through each variable and print the flags if non-default */
	puts("Active flags:\n");
	printf("\t%-20s %-20s %-20s\n", "Variable Name", "Variable Type",
		"Variable Access");
	printf("\t%-20s %-20s %-20s\n", "-------------", "-------------",
		"---------------");
	hwalk_r(&env_htab, print_active_flags);
	return 0;
}
#endif

/*
 * Interactively edit an environment variable
 */
#if defined(CONFIG_CMD_EDITENV)
static int do_env_edit(cmd_tbl_t *cmdtp, int flag, int argc,
		       char * const argv[])
{
	char buffer[CONFIG_SYS_CBSIZE];
	char *init_val;

	if (argc < 2)
		return CMD_RET_USAGE;

	/* Set read buffer to initial value or empty sting */
	init_val = getenv(argv[1]);
	if (init_val)
		sprintf(buffer, "%s", init_val);
	else
		buffer[0] = '\0';

	if (readline_into_buffer("edit: ", buffer, 0) < 0)
		return 1;

	return setenv(argv[1], buffer);
}
#endif /* CONFIG_CMD_EDITENV */
#endif /* CONFIG_SPL_BUILD */

/*
 * Look up variable from environment,
 * return address of storage for that variable,
 * or NULL if not found
 */
char *getenv(const char *name)
{
	if (gd->flags & GD_FLG_ENV_READY) { /* after import into hashtable */
		ENTRY e, *ep;

		WATCHDOG_RESET();

		e.key	= name;
		e.data	= NULL;
		hsearch_r(e, FIND, &ep, &env_htab, 0);

		return ep ? ep->data : NULL;
	}

	/* restricted capabilities before import */
	if (getenv_f(name, (char *)(gd->env_buf), sizeof(gd->env_buf)) > 0)
		return (char *)(gd->env_buf);

	return NULL;
}

/*
 * Look up variable from environment for restricted C runtime env.
 */
int getenv_f(const char *name, char *buf, unsigned len)
{
	int i, nxt;

	for (i = 0; env_get_char(i) != '\0'; i = nxt + 1) {
		int val, n;

		for (nxt = i; env_get_char(nxt) != '\0'; ++nxt) {
			if (nxt >= CONFIG_ENV_SIZE)
				return -1;
		}

		val = envmatch((uchar *)name, i);
		if (val < 0)
			continue;

		/* found; copy out */
		for (n = 0; n < len; ++n, ++buf) {
			*buf = env_get_char(val++);
			if (*buf == '\0')
				return n;
		}

		if (n)
			*--buf = '\0';

		printf("env_buf [%d bytes] too small for value of \"%s\"\n",
			len, name);

		return n;
	}

	return -1;
}

/**
 * Decode the integer value of an environment variable and return it.
 *
 * @param name		Name of environemnt variable
 * @param base		Number base to use (normally 10, or 16 for hex)
 * @param default_val	Default value to return if the variable is not
 *			found
 * @return the decoded value, or default_val if not found
 */
ulong getenv_ulong(const char *name, int base, ulong default_val)
{
	/*
	 * We can use getenv() here, even before relocation, since the
	 * environment variable value is an integer and thus short.
	 */
	const char *str = getenv(name);

	return str ? simple_strtoul(str, NULL, base) : default_val;
}

#ifndef CONFIG_SPL_BUILD
#if defined(CONFIG_CMD_SAVEENV) && !defined(CONFIG_ENV_IS_NOWHERE)
static int do_env_save(cmd_tbl_t *cmdtp, int flag, int argc,
		       char * const argv[])
{
	printf("Saving Environment to %s...\n", env_name_spec);

	return saveenv() ? 1 : 0;
}

U_BOOT_CMD(
	saveenv, 1, 0,	do_env_save,
	"save environment variables to persistent storage",
	""
);
#endif
#endif /* CONFIG_SPL_BUILD */


/*
 * Match a name / name=value pair
 *
 * s1 is either a simple 'name', or a 'name=value' pair.
 * i2 is the environment index for a 'name2=value2' pair.
 * If the names match, return the index for the value2, else -1.
 */
int envmatch(uchar *s1, int i2)
{
	if (s1 == NULL)
		return -1;

	while (*s1 == env_get_char(i2++))
		if (*s1++ == '=')
			return i2;

	if (*s1 == '\0' && env_get_char(i2-1) == '=')
		return i2;

	return -1;
}

#ifndef CONFIG_SPL_BUILD
static int do_env_default(cmd_tbl_t *cmdtp, int __flag,
			  int argc, char * const argv[])
{
	int all = 0, flag = 0;

	debug("Initial value for argc=%d\n", argc);
	while (--argc > 0 && **++argv == '-') {
		char *arg = *argv;

		while (*++arg) {
			switch (*arg) {
			case 'a':		/* default all */
				all = 1;
				break;
			case 'f':		/* force */
				flag |= H_FORCE;
				break;
			default:
				return cmd_usage(cmdtp);
			}
		}
	}
	debug("Final value for argc=%d\n", argc);
	if (all && (argc == 0)) {
		/* Reset the whole environment */
		set_default_env("## Resetting to default environment\n");
		return 0;
	}
	if (!all && (argc > 0)) {
		/* Reset individual variables */
		set_default_vars(argc, argv);
		return 0;
	}

	return cmd_usage(cmdtp);
}

static int do_env_delete(cmd_tbl_t *cmdtp, int flag,
			 int argc, char * const argv[])
{
	int env_flag = H_INTERACTIVE;
	int ret = 0;

	debug("Initial value for argc=%d\n", argc);
	while (argc > 1 && **(argv + 1) == '-') {
		char *arg = *++argv;

		--argc;
		while (*++arg) {
			switch (*arg) {
			case 'f':		/* force */
				env_flag |= H_FORCE;
				break;
			default:
				return CMD_RET_USAGE;
			}
		}
	}
	debug("Final value for argc=%d\n", argc);

	env_id++;

	while (--argc > 0) {
		char *name = *++argv;

		if (!hdelete_r(name, &env_htab, env_flag))
			ret = 1;
	}

	return ret;
}

#ifdef CONFIG_CMD_EXPORTENV
/*
 * env export [-t | -b | -c] [-s size] addr [var ...]
 *	-t:	export as text format; if size is given, data will be
 *		padded with '\0' bytes; if not, one terminating '\0'
 *		will be added (which is included in the "filesize"
 *		setting so you can for exmple copy this to flash and
 *		keep the termination).
 *	-b:	export as binary format (name=value pairs separated by
 *		'\0', list end marked by double "\0\0")
 *	-c:	export as checksum protected environment format as
 *		used for example by "saveenv" command
 *	-s size:
 *		size of output buffer
 *	addr:	memory address where environment gets stored
 *	var...	List of variable names that get included into the
 *		export. Without arguments, the whole environment gets
 *		exported.
 *
 * With "-c" and size is NOT given, then the export command will
 * format the data as currently used for the persistent storage,
 * i. e. it will use CONFIG_ENV_SECT_SIZE as output block size and
 * prepend a valid CRC32 checksum and, in case of resundant
 * environment, a "current" redundancy flag. If size is given, this
 * value will be used instead of CONFIG_ENV_SECT_SIZE; again, CRC32
 * checksum and redundancy flag will be inserted.
 *
 * With "-b" and "-t", always only the real data (including a
 * terminating '\0' byte) will be written; here the optional size
 * argument will be used to make sure not to overflow the user
 * provided buffer; the command will abort if the size is not
 * sufficient. Any remainign space will be '\0' padded.
 *
 * On successful return, the variable "filesize" will be set.
 * Note that filesize includes the trailing/terminating '\0' byte(s).
 *
 * Usage szenario:  create a text snapshot/backup of the current settings:
 *
 *	=> env export -t 100000
 *	=> era ${backup_addr} +${filesize}
 *	=> cp.b 100000 ${backup_addr} ${filesize}
 *
 * Re-import this snapshot, deleting all other settings:
 *
 *	=> env import -d -t ${backup_addr}
 */
static int do_env_export(cmd_tbl_t *cmdtp, int flag,
			 int argc, char * const argv[])
{
	char	buf[32];
	char	*addr, *cmd, *res;
	size_t	size = 0;
	ssize_t	len;
	env_t	*envp;
	char	sep = '\n';
	int	chk = 0;
	int	fmt = 0;

	cmd = *argv;

	while (--argc > 0 && **++argv == '-') {
		char *arg = *argv;
		while (*++arg) {
			switch (*arg) {
			case 'b':		/* raw binary format */
				if (fmt++)
					goto sep_err;
				sep = '\0';
				break;
			case 'c':		/* external checksum format */
				if (fmt++)
					goto sep_err;
				sep = '\0';
				chk = 1;
				break;
			case 's':		/* size given */
				if (--argc <= 0)
					return cmd_usage(cmdtp);
				size = simple_strtoul(*++argv, NULL, 16);
				goto NXTARG;
			case 't':		/* text format */
				if (fmt++)
					goto sep_err;
				sep = '\n';
				break;
			default:
				return CMD_RET_USAGE;
			}
		}
NXTARG:		;
	}

	if (argc < 1)
		return CMD_RET_USAGE;

	addr = (char *)simple_strtoul(argv[0], NULL, 16);

	if (size)
		memset(addr, '\0', size);

	argc--;
	argv++;

	if (sep) {		/* export as text file */
		len = hexport_r(&env_htab, sep,
				H_MATCH_KEY | H_MATCH_IDENT,
				&addr, size, argc, argv);
		if (len < 0) {
			error("Cannot export environment: errno = %d\n", errno);
			return 1;
		}
		sprintf(buf, "%zX", (size_t)len);
		setenv("filesize", buf);

		return 0;
	}

	envp = (env_t *)addr;

	if (chk)		/* export as checksum protected block */
		res = (char *)envp->data;
	else			/* export as raw binary data */
		res = addr;

	len = hexport_r(&env_htab, '\0',
			H_MATCH_KEY | H_MATCH_IDENT,
			&res, ENV_SIZE, argc, argv);
	if (len < 0) {
		error("Cannot export environment: errno = %d\n", errno);
		return 1;
	}

	if (chk) {
		envp->crc = crc32(0, envp->data, ENV_SIZE);
#ifdef CONFIG_ENV_ADDR_REDUND
		envp->flags = ACTIVE_FLAG;
#endif
	}
	setenv_hex("filesize", len + offsetof(env_t, data));

	return 0;

sep_err:
	printf("## %s: only one of \"-b\", \"-c\" or \"-t\" allowed\n",	cmd);
	return 1;
}
#endif

#ifdef CONFIG_CMD_IMPORTENV
/*
 * env import [-d] [-t | -b | -c] addr [size]
 *	-d:	delete existing environment before importing;
 *		otherwise overwrite / append to existion definitions
 *	-t:	assume text format; either "size" must be given or the
 *		text data must be '\0' terminated
 *	-b:	assume binary format ('\0' separated, "\0\0" terminated)
 *	-c:	assume checksum protected environment format
 *	addr:	memory address to read from
 *	size:	length of input data; if missing, proper '\0'
 *		termination is mandatory
 */
static int do_env_import(cmd_tbl_t *cmdtp, int flag,
			 int argc, char * const argv[])
{
	char	*cmd, *addr;
	char	sep = '\n';
	int	chk = 0;
	int	fmt = 0;
	int	del = 0;
	size_t	size;

	cmd = *argv;

	while (--argc > 0 && **++argv == '-') {
		char *arg = *argv;
		while (*++arg) {
			switch (*arg) {
			case 'b':		/* raw binary format */
				if (fmt++)
					goto sep_err;
				sep = '\0';
				break;
			case 'c':		/* external checksum format */
				if (fmt++)
					goto sep_err;
				sep = '\0';
				chk = 1;
				break;
			case 't':		/* text format */
				if (fmt++)
					goto sep_err;
				sep = '\n';
				break;
			case 'd':
				del = 1;
				break;
			default:
				return CMD_RET_USAGE;
			}
		}
	}

	if (argc < 1)
		return CMD_RET_USAGE;

	if (!fmt)
		printf("## Warning: defaulting to text format\n");

	addr = (char *)simple_strtoul(argv[0], NULL, 16);

	if (argc == 2) {
		size = simple_strtoul(argv[1], NULL, 16);
	} else {
		char *s = addr;

		size = 0;

		while (size < MAX_ENV_SIZE) {
			if ((*s == sep) && (*(s+1) == '\0'))
				break;
			++s;
			++size;
		}
		if (size == MAX_ENV_SIZE) {
			printf("## Warning: Input data exceeds %d bytes"
				" - truncated\n", MAX_ENV_SIZE);
		}
		size += 2;
		printf("## Info: input data size = %zu = 0x%zX\n", size, size);
	}

	if (chk) {
		uint32_t crc;
		env_t *ep = (env_t *)addr;

		size -= offsetof(env_t, data);
		memcpy(&crc, &ep->crc, sizeof(crc));

		if (crc32(0, ep->data, size) != crc) {
			puts("## Error: bad CRC, import failed\n");
			return 1;
		}
		addr = (char *)ep->data;
	}

	if (himport_r(&env_htab, addr, size, sep, del ? 0 : H_NOCLEAR,
			0, NULL) == 0) {
		error("Environment import failed: errno = %d\n", errno);
		return 1;
	}
	gd->flags |= GD_FLG_ENV_READY;

	return 0;

sep_err:
	printf("## %s: only one of \"-b\", \"-c\" or \"-t\" allowed\n",
		cmd);
	return 1;
}
#endif

/*
 * New command line interface: "env" command with subcommands
 */
static cmd_tbl_t cmd_env_sub[] = {
#if defined(CONFIG_CMD_ASKENV)
	U_BOOT_CMD_MKENT(ask, CONFIG_SYS_MAXARGS, 1, do_env_ask, "", ""),
#endif
	U_BOOT_CMD_MKENT(default, 1, 0, do_env_default, "", ""),
	U_BOOT_CMD_MKENT(delete, CONFIG_SYS_MAXARGS, 0, do_env_delete, "", ""),
#if defined(CONFIG_CMD_EDITENV)
	U_BOOT_CMD_MKENT(edit, 2, 0, do_env_edit, "", ""),
#endif
#if defined(CONFIG_CMD_ENV_CALLBACK)
	U_BOOT_CMD_MKENT(callbacks, 1, 0, do_env_callback, "", ""),
#endif
#if defined(CONFIG_CMD_ENV_FLAGS)
	U_BOOT_CMD_MKENT(flags, 1, 0, do_env_flags, "", ""),
#endif
#if defined(CONFIG_CMD_EXPORTENV)
	U_BOOT_CMD_MKENT(export, 4, 0, do_env_export, "", ""),
#endif
#if defined(CONFIG_CMD_GREPENV)
	U_BOOT_CMD_MKENT(grep, CONFIG_SYS_MAXARGS, 1, do_env_grep, "", ""),
#endif
#if defined(CONFIG_CMD_IMPORTENV)
	U_BOOT_CMD_MKENT(import, 5, 0, do_env_import, "", ""),
#endif
	U_BOOT_CMD_MKENT(print, CONFIG_SYS_MAXARGS, 1, do_env_print, "", ""),
#if defined(CONFIG_CMD_RUN)
	U_BOOT_CMD_MKENT(run, CONFIG_SYS_MAXARGS, 1, do_run, "", ""),
#endif
#if defined(CONFIG_CMD_SAVEENV) && !defined(CONFIG_ENV_IS_NOWHERE)
	U_BOOT_CMD_MKENT(save, 1, 0, do_env_save, "", ""),
#endif
	U_BOOT_CMD_MKENT(set, CONFIG_SYS_MAXARGS, 0, do_env_set, "", ""),
};

#if defined(CONFIG_NEEDS_MANUAL_RELOC)
void env_reloc(void)
{
	fixup_cmdtable(cmd_env_sub, ARRAY_SIZE(cmd_env_sub));
}
#endif

static int do_env(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	cmd_tbl_t *cp;

	if (argc < 2)
		return CMD_RET_USAGE;

	/* drop initial "env" arg */
	argc--;
	argv++;

	cp = find_cmd_tbl(argv[0], cmd_env_sub, ARRAY_SIZE(cmd_env_sub));

	if (cp)
		return cp->cmd(cmdtp, flag, argc, argv);

	return CMD_RET_USAGE;
}

#ifdef CONFIG_SYS_LONGHELP
static char env_help_text[] =
#if defined(CONFIG_CMD_ASKENV)
	"ask name [message] [size] - ask for environment variable\nenv "
#endif
#if defined(CONFIG_CMD_ENV_CALLBACK)
	"callbacks - print callbacks and their associated variables\nenv "
#endif
	"default [-f] -a - [forcibly] reset default environment\n"
	"env default [-f] var [...] - [forcibly] reset variable(s) to their default values\n"
	"env delete [-f] var [...] - [forcibly] delete variable(s)\n"
#if defined(CONFIG_CMD_EDITENV)
	"env edit name - edit environment variable\n"
#endif
#if defined(CONFIG_CMD_EXPORTENV)
	"env export [-t | -b | -c] [-s size] addr [var ...] - export environment\n"
#endif
#if defined(CONFIG_CMD_ENV_FLAGS)
	"env flags - print variables that have non-default flags\n"
#endif
#if defined(CONFIG_CMD_GREPENV)
#ifdef CONFIG_REGEX
	"env grep [-e] [-n | -v | -b] string [...] - search environment\n"
#else
	"env grep [-n | -v | -b] string [...] - search environment\n"
#endif
#endif
#if defined(CONFIG_CMD_IMPORTENV)
	"env import [-d] [-t | -b | -c] addr [size] - import environment\n"
#endif
	"env print [-a | name ...] - print environment\n"
#if defined(CONFIG_CMD_RUN)
	"env run var [...] - run commands in an environment variable\n"
#endif
#if defined(CONFIG_CMD_SAVEENV) && !defined(CONFIG_ENV_IS_NOWHERE)
	"env save - save environment\n"
#endif
	"env set [-f] name [arg ...]\n";
#endif

U_BOOT_CMD(
	env, CONFIG_SYS_MAXARGS, 1, do_env,
	"environment handling commands", env_help_text
);

/*
 * Old command line interface, kept for compatibility
 */

#if defined(CONFIG_CMD_EDITENV)
U_BOOT_CMD_COMPLETE(
	editenv, 2, 0,	do_env_edit,
	"edit environment variable",
	"name\n"
	"    - edit environment variable 'name'",
	var_complete
);
#endif

U_BOOT_CMD_COMPLETE(
	printenv, CONFIG_SYS_MAXARGS, 1,	do_env_print,
	"print environment variables",
	"[-a]\n    - print [all] values of all environment variables\n"
	"printenv name ...\n"
	"    - print value of environment variable 'name'",
	var_complete
);

#ifdef CONFIG_CMD_GREPENV
U_BOOT_CMD_COMPLETE(
	grepenv, CONFIG_SYS_MAXARGS, 0,  do_env_grep,
	"search environment variables",
#ifdef CONFIG_REGEX
	"[-e] [-n | -v | -b] string ...\n"
#else
	"[-n | -v | -b] string ...\n"
#endif
	"    - list environment name=value pairs matching 'string'\n"
#ifdef CONFIG_REGEX
	"      \"-e\": enable regular expressions;\n"
#endif
	"      \"-n\": search variable names; \"-v\": search values;\n"
	"      \"-b\": search both names and values (default)",
	var_complete
);
#endif

U_BOOT_CMD_COMPLETE(
	setenv, CONFIG_SYS_MAXARGS, 0,	do_env_set,
	"set environment variables",
	"[-f] name value ...\n"
	"    - [forcibly] set environment variable 'name' to 'value ...'\n"
	"setenv [-f] name\n"
	"    - [forcibly] delete environment variable 'name'",
	var_complete
);

#if defined(CONFIG_CMD_ASKENV)

U_BOOT_CMD(
	askenv,	CONFIG_SYS_MAXARGS,	1,	do_env_ask,
	"get environment variables from stdin",
	"name [message] [size]\n"
	"    - get environment variable 'name' from stdin (max 'size' chars)"
);
#endif

#if defined(CONFIG_CMD_RUN)
U_BOOT_CMD_COMPLETE(
	run,	CONFIG_SYS_MAXARGS,	1,	do_run,
	"run commands in an environment variable",
	"var [...]\n"
	"    - run the commands in the environment variable(s) 'var'",
	var_complete
);
#endif
#endif /* CONFIG_SPL_BUILD */
