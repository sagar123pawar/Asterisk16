/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Asterisk Logger
 *
 * Logging routines
 *
 * \author Mark Spencer <markster@digium.com>
 */

/*! \li \ref logger.c uses the configuration file \ref logger.conf
 * \addtogroup configuration_file Configuration Files
 */

/*!
 * \page logger.conf logger.conf
 * \verbinclude logger.conf.sample
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

/* When we include logger.h again it will trample on some stuff in syslog.h, but
 * nothing we care about in here. */
#include <syslog.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "asterisk/_private.h"
#include "asterisk/module.h"
#include "asterisk/paths.h"	/* use ast_config_AST_LOG_DIR */
#include "asterisk/logger.h"
#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/term.h"
#include "asterisk/cli.h"
#include "asterisk/utils.h"
#include "asterisk/manager.h"
#include "asterisk/astobj2.h"
#include "asterisk/threadstorage.h"
#include "asterisk/strings.h"
#include "asterisk/pbx.h"
#include "asterisk/app.h"
#include "asterisk/syslog.h"
#include "asterisk/buildinfo.h"
#include "asterisk/ast_version.h"
#include "asterisk/backtrace.h"
#include "asterisk/json.h"

/*** DOCUMENTATION
 ***/

static char dateformat[256] = "%b %e %T";		/* Original Asterisk Format */

static char queue_log_name[256] = QUEUELOG;
static char exec_after_rotate[256] = "";

static int filesize_reload_needed;
static unsigned int global_logmask = 0xFFFF;
static int queuelog_init;
static int logger_initialized;
static volatile int next_unique_callid = 1; /* Used to assign unique call_ids to calls */
static int display_callids;

AST_THREADSTORAGE(unique_callid);

static int logger_queue_size;
static int logger_queue_limit = 1000;
static int logger_messages_discarded;
static unsigned int high_water_alert;

static enum rotatestrategy {
	NONE = 0,                /* Do not rotate log files at all, instead rely on external mechanisms */
	SEQUENTIAL = 1 << 0,     /* Original method - create a new file, in order */
	ROTATE = 1 << 1,         /* Rotate all files, such that the oldest file has the highest suffix */
	TIMESTAMP = 1 << 2,      /* Append the epoch timestamp onto the end of the archived file */
} rotatestrategy = SEQUENTIAL;

static struct {
	unsigned int queue_log:1;
	unsigned int queue_log_to_file:1;
	unsigned int queue_adaptive_realtime:1;
	unsigned int queue_log_realtime_use_gmt:1;
} logfiles = { 1 };

static char hostname[MAXHOSTNAMELEN];
AST_THREADSTORAGE_RAW(in_safe_log);

struct logchannel;
struct logmsg;

struct logformatter {
	/* The name of the log formatter */
	const char *name;
	/* Pointer to the function that will format the log */
	int (* const format_log)(struct logchannel *channel, struct logmsg *msg, char *buf, size_t size);
};

enum logtypes {
	LOGTYPE_SYSLOG,
	LOGTYPE_FILE,
	LOGTYPE_CONSOLE,
};

struct logchannel {
	/*! How the logs sent to this channel will be formatted */
	struct logformatter formatter;
	/*! What to log to this channel */
	unsigned int logmask;
	/*! If this channel is disabled or not */
	int disabled;
	/*! syslog facility */
	int facility;
	/*! Verbosity level. (-1 if use option_verbose for the level.) */
	int verbosity;
	/*! Type of log channel */
	enum logtypes type;
	/*! logfile logging file pointer */
	FILE *fileptr;
	/*! Filename */
	char filename[PATH_MAX];
	/*! field for linking to list */
	AST_LIST_ENTRY(logchannel) list;
	/*! Line number from configuration file */
	int lineno;
	/*! Whether this log channel was created dynamically */
	int dynamic;
	/*! Components (levels) from last config load */
	char components[0];
};

static AST_RWLIST_HEAD_STATIC(logchannels, logchannel);

enum logmsgtypes {
	LOGMSG_NORMAL = 0,
	LOGMSG_VERBOSE,
};

struct logmsg {
	enum logmsgtypes type;
	int level;
	int sublevel;
	int line;
	int lwp;
	ast_callid callid;
	AST_DECLARE_STRING_FIELDS(
		AST_STRING_FIELD(date);
		AST_STRING_FIELD(file);
		AST_STRING_FIELD(function);
		AST_STRING_FIELD(message);
		AST_STRING_FIELD(level_name);
	);
	AST_LIST_ENTRY(logmsg) list;
};

static void logmsg_free(struct logmsg *msg)
{
	ast_string_field_free_memory(msg);
	ast_free(msg);
}

static AST_LIST_HEAD_STATIC(logmsgs, logmsg);
static pthread_t logthread = AST_PTHREADT_NULL;
static ast_cond_t logcond;
static int close_logger_thread = 0;

static FILE *qlog;

/*! \brief Logging channels used in the Asterisk logging system
 *
 * The first 16 levels are reserved for system usage, and the remaining
 * levels are reserved for usage by dynamic levels registered via
 * ast_logger_register_level.
 */

/* Modifications to this array are protected by the rwlock in the
 * logchannels list.
 */

static char *levels[NUMLOGLEVELS] = {
	"DEBUG",
	"---EVENT---",		/* no longer used */
	"NOTICE",
	"WARNING",
	"ERROR",
	"VERBOSE",
	"DTMF",
};

/*! \brief Colors used in the console for logging */
static const int colors[NUMLOGLEVELS] = {
	COLOR_BRGREEN,
	COLOR_BRBLUE,		/* no longer used */
	COLOR_YELLOW,
	COLOR_BRRED,
	COLOR_RED,
	COLOR_GREEN,
	COLOR_BRGREEN,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
	COLOR_BRBLUE,
};

AST_THREADSTORAGE(verbose_buf);
AST_THREADSTORAGE(verbose_build_buf);
#define VERBOSE_BUF_INIT_SIZE   256

AST_THREADSTORAGE(log_buf);
#define LOG_BUF_INIT_SIZE       256

#ifdef GRANDSTREAM_NETWORKS
#include <sqlite3.h>

static int ast_log_index = 1;
static int ast_log_indexs[65535] = {0};

#define LOGGER_MAX_BUCKETS 53
struct ao2_container *log_container = NULL;

struct ast_log_modules {
	int index;
	char *modname;
};

sqlite3 *logger_db = NULL;
const char *logger_sql = "CREATE TABLE IF NOT EXISTS logger(key VARCHAR(256), value VARCHAR(256), PRIMARY KEY(key))";

static int ast_logger_db_insert(void)
{
	struct ao2_iterator iter;
	struct ast_log_modules *mod = NULL;
	char sql[256] = {0};
	char *zerrmsg = NULL;
	int status = 0;

	ao2_lock(log_container);
	iter = ao2_iterator_init(log_container, 0);
	while ((mod = ao2_t_iterator_next(&iter, "Iterate through queues"))) {
		snprintf(sql, sizeof(sql) - 1, "REPLACE INTO logger(key, value) values ('%s', '%d')", mod->modname, ast_log_indexs[mod->index]);
		status = sqlite3_exec(logger_db, sql, NULL, NULL, &zerrmsg);
		if (SQLITE_OK != status) {
			sqlite3_free(zerrmsg);
		}
		ao2_ref(mod, -1);
	}
	ao2_iterator_destroy(&iter);
	ao2_unlock(log_container);

	return 0;
}

static int ast_logger_db_select(void)
{
	char sql[256] = {0};
	char **result = NULL;
	char *zerrmsg = NULL;
	int nrow = 0, ncolumn = 0;
	struct ast_log_modules *mod = NULL;
	struct ao2_iterator iter;
	int status = 0;

	ao2_lock(log_container);
	iter = ao2_iterator_init(log_container, 0);
	while ((mod = ao2_t_iterator_next(&iter, "Iterate through queues"))) {
		snprintf(sql, sizeof(sql) - 1, "SELECT value FROM logger WHERE key = '%s'", mod->modname);
		status = sqlite3_get_table(logger_db, sql, &result, &nrow, &ncolumn, &zerrmsg);
		if (SQLITE_OK != status) {
			sqlite3_free(zerrmsg);
		} else if (nrow > 0 && ncolumn > 0 && '\0' != result[1]) {
			ast_log_indexs[mod->index] = atoi(result[1]);
		}
		ao2_ref(mod, -1);
	}
	ao2_iterator_destroy(&iter);
	ao2_unlock(log_container);

	return 0;
}

static int ast_logger_db_init(void)
{
	int status = 0;
	char logger_dbname[512] = {0};
	char *zerrmsg = NULL;

	snprintf(logger_dbname, sizeof(logger_dbname) - 1, "%s_logger.sqlite3", ast_config_AST_DB);

	if (!logger_db) {
		status = sqlite3_open(logger_dbname, &logger_db);
		if (SQLITE_OK != status) {
			ast_log(LOG_ERROR, "Can't open database: %s\n", sqlite3_errmsg(logger_db));
			sqlite3_close(logger_db);
			logger_db = NULL;
			return - 1;
		}

		status = sqlite3_exec(logger_db, logger_sql, NULL, NULL, &zerrmsg);
		if (SQLITE_OK != status) {
			sqlite3_free(zerrmsg);
		}
	}

	return 0;
}

static void ast_log_modules_dtor(void *obj)
{
	struct ast_log_modules *mod = obj;

	if (mod) {
		ast_free(mod->modname);
	}
}

static int ast_log_hash_cb(const void *obj, const int flags)
{
	const struct ast_log_modules *mod = obj;

	return ast_str_case_hash(mod->modname);
}

static int ast_log_cmp_cb(void *obj, void *arg, int flags)
{
	struct ast_log_modules *mod = obj;

	return (0 == strcasecmp(mod->modname, arg)) ? CMP_MATCH | CMP_STOP : 0;
}

void ast_log_module_register(const char *modname, int *index, const char *file, int line)
{
	struct ast_log_modules *mod = NULL;

	if (!log_container) {
		log_container = ao2_container_alloc(LOGGER_MAX_BUCKETS, ast_log_hash_cb, ast_log_cmp_cb);
	}

	ao2_lock(log_container);
	mod = ao2_find(log_container, modname, 0);
	if (mod) {
		*index = mod->index;
		ao2_ref(mod, -1);
		ao2_unlock(log_container);
		return;
	}

	mod = ao2_alloc(sizeof(*mod), ast_log_modules_dtor);
	if (mod) {
		mod->modname = ast_strdup(modname);
		*index = mod->index = ast_log_index++;
	}
	ast_log_indexs[mod->index] = AST_ALL_LOG_ENUM;

	if (!ast_log_indexs[0]) {
		struct ast_log_modules *all = ao2_alloc(sizeof(*all), ast_log_modules_dtor);
		if (all) {
			all->modname = ast_strdup("ALL");
			all->index = 0;
			ast_log_indexs[all->index] = AST_ALL_LOG_ENUM;

			ao2_link(log_container, all);
		}
	}
	ao2_link(log_container, mod);
	ao2_unlock(log_container);
}

void ast_log_module_unregister(const char *modname, const char *file, int line)
{
}

void ast_log_modules_destroy(void)
{
	struct ao2_iterator iter;
	struct ast_log_modules *mod = NULL;

	ao2_lock(log_container);
	iter = ao2_iterator_init(log_container, 0);
	while ((mod = ao2_t_iterator_next(&iter, "Iterate through queues"))) {
		ao2_unlink(log_container, mod);
		ao2_ref(mod, -2);
	}
	ao2_iterator_destroy(&iter);
	ao2_unlock(log_container);
	ao2_cleanup(log_container);
}

void ast_log_module_update(void)
{
	int index;

	for (index = 0; index <= ast_log_index && index < 65535; index++) {
		ast_log_indexs[index] = 0;
	}

	ast_logger_db_select();
}

static char *ast_log_find_modname(const char *modname, int skip_count)
{
	struct ast_log_modules *mod = NULL;
	char *modulename = NULL;
	struct ao2_iterator iter;
	int count = 0;
	int modnamelen = 0;

	if (!modname) {
		ao2_lock(log_container);
		iter = ao2_iterator_init(log_container, 0);
		while ((mod = ao2_t_iterator_next(&iter, "Iterate through queues"))) {
			if (++count > skip_count) {
				modulename = ast_strdup(mod->modname);
				ao2_ref(mod, -1);
				break;
			}
			ao2_ref(mod, -1);
		}
		ao2_iterator_destroy(&iter);
		ao2_unlock(log_container);
	} else {
		ao2_lock(log_container);
		modnamelen = strlen(modname);
		iter = ao2_iterator_init(log_container, 0);
		while ((mod = ao2_t_iterator_next(&iter, "Iterate through queues"))) {
			if (!strncasecmp(mod->modname, modname, modnamelen) && ++count > skip_count) {
				modulename = ast_strdup(mod->modname);
				ao2_ref(mod, -1);
				break;
			}
			ao2_ref(mod, -1);
		}
		ao2_iterator_destroy(&iter);
		ao2_unlock(log_container);
	}

	return modulename;
}

static char *ast_log_find_level(const char *name, int skip_count)
{
	int i, count = 0, namelen = 0;
	char *levelname = NULL;
	char *levelsname[] = {
		"DEBUG",
		"NOTICE",
		"WARNING",
		"ERROR",
		"VERBOSE",
		"DTMF",
		"ALL",
	};

	if (!name) {
		for (i = 0; i < ARRAY_LEN(levelsname); i++) {
			if (++count > skip_count) {
				levelname = ast_strdup(levelsname[i]);
				break;
			}
		}
	} else {
		namelen = strlen(name);
		for (i = 0; i < ARRAY_LEN(levelsname); i++) {
			if (levelsname[i] && !strncasecmp(levelsname[i], name, namelen) && ++count > skip_count) {
				levelname = ast_strdup(levelsname[i]);
				break;
			}
		}
	}

	return levelname;
}

static char *ast_log_find_switch(const char *name, int skip_count)
{
	int i, count = 0;
	char *sname = NULL;
	char *switchs[] = {"on", "off"};

	if (!name) {
		for (i = 0; i < ARRAY_LEN(switchs); i++) {
			if (++count > skip_count) {
				sname = ast_strdup(switchs[i]);
				break;
			}
		}
	} else {
		for (i = 0; i < ARRAY_LEN(switchs); i++) {
			if (switchs[i] && strcasestr(switchs[i], name) && ++count > skip_count) {
				sname = ast_strdup(switchs[i]);
				break;
			}
		}
	}

	return sname;
}
#endif

static int format_log_json(struct logchannel *channel, struct logmsg *msg, char *buf, size_t size)
{
	struct ast_json *json;
	char *str;
	char call_identifier_str[13];
	size_t json_str_len;

	if (msg->callid) {
		snprintf(call_identifier_str, sizeof(call_identifier_str), "[C-%08x]", msg->callid);
	} else {
		call_identifier_str[0] = '\0';
	}

	json = ast_json_pack("{s: s, s: s, "
		"s: {s: i, s: s} "
		"s: {s: {s: s, s: s, s: i}, "
		"s: s, s: s} }",
		"hostname", ast_config_AST_SYSTEM_NAME,
		"timestamp", msg->date,
		"identifiers",
		"lwp", msg->lwp,
		"callid", S_OR(call_identifier_str, ""),
		"logmsg",
		"location",
		"filename", msg->file,
		"function", msg->function,
		"line", msg->line,
		"level", msg->level_name,
		"message", msg->message);
	if (!json) {
		return -1;
	}

	str = ast_json_dump_string(json);
	if (!str) {
		ast_json_unref(json);
		return -1;
	}

	ast_copy_string(buf, str, size);
	json_str_len = strlen(str);
	if (json_str_len > size - 1) {
		json_str_len = size - 1;
	}
	buf[json_str_len] = '\n';
	buf[json_str_len + 1] = '\0';

	term_strip(buf, buf, size);

	ast_json_free(str);
	ast_json_unref(json);

	return 0;
}

static struct logformatter logformatter_json = {
	.name = "json",
	.format_log = format_log_json
};

static int logger_add_verbose_magic(struct logmsg *logmsg, char *buf, size_t size)
{
#ifdef GRANDSTREAM_NETWORKS
	int has_file = !ast_strlen_zero(logmsg->file);
	int has_line = (logmsg->line > 0);
	char linestr[32] = {0};

	snprintf(linestr, sizeof(linestr), "%d", logmsg->line);

	snprintf(buf, size, "[%s] " COLORIZE_FMT "[%d]: " COLORIZE_FMT "%s" COLORIZE_FMT " " "%s\n",
		logmsg->date,
		COLORIZE(colors[logmsg->level], 0, logmsg->level_name),
		logmsg->lwp,
		COLORIZE(COLOR_BRWHITE, 0, has_file ? logmsg->file : ""),
		has_file ? ":" : "",
		COLORIZE(COLOR_BRWHITE, 0, has_line ? linestr : ""),
		logmsg->message);
#else
	const char *p;
	const char *fmt;
	struct ast_str *prefixed;
	signed char magic = logmsg->sublevel > 9 ? -10 : -logmsg->sublevel - 1; /* 0 => -1, 1 => -2, etc.  Can't pass NUL, as it is EOS-delimiter */

	/* For compatibility with modules still calling ast_verbose() directly instead of using ast_verb() */
	if (logmsg->sublevel < 0) {
		if (!strncmp(logmsg->message, VERBOSE_PREFIX_4, strlen(VERBOSE_PREFIX_4))) {
			magic = -5;
		} else if (!strncmp(logmsg->message, VERBOSE_PREFIX_3, strlen(VERBOSE_PREFIX_3))) {
			magic = -4;
		} else if (!strncmp(logmsg->message, VERBOSE_PREFIX_2, strlen(VERBOSE_PREFIX_2))) {
			magic = -3;
		} else if (!strncmp(logmsg->message, VERBOSE_PREFIX_1, strlen(VERBOSE_PREFIX_1))) {
			magic = -2;
		} else {
			magic = -1;
		}
	}

	if (!(prefixed = ast_str_thread_get(&verbose_buf, VERBOSE_BUF_INIT_SIZE))) {
		return -1;
	}

	ast_str_reset(prefixed);

	/* for every newline found in the buffer add verbose prefix data */
	fmt = logmsg->message;
	do {
		if (!(p = strchr(fmt, '\n'))) {
			p = strchr(fmt, '\0') - 1;
		}
		++p;

		ast_str_append(&prefixed, 0, "%c", (char)magic);
		ast_str_append_substr(&prefixed, 0, fmt, p - fmt);
		fmt = p;
	} while (p && *p);

	snprintf(buf, size, "%s", ast_str_buffer(prefixed));
#endif

	return 0;
}

static int format_log_default(struct logchannel *chan, struct logmsg *msg, char *buf, size_t size)
{
	char call_identifier_str[13];

	if (msg->callid) {
		snprintf(call_identifier_str, sizeof(call_identifier_str), "[C-%08x]", msg->callid);
	} else {
		call_identifier_str[0] = '\0';
	}

	switch (chan->type) {
	case LOGTYPE_SYSLOG:
		snprintf(buf, size, "%s[%d]%s: %s:%d in %s: %s",
		     levels[msg->level], msg->lwp, call_identifier_str, msg->file,
		     msg->line, msg->function, msg->message);
		term_strip(buf, buf, size);
		break;
	case LOGTYPE_FILE:
		snprintf(buf, size, "[%s] %s[%d]%s %s: %s",
		      msg->date, msg->level_name, msg->lwp, call_identifier_str,
		      msg->file, msg->message);
		term_strip(buf, buf, size);
		break;
	case LOGTYPE_CONSOLE:
		{
			char linestr[32];
			int has_file = !ast_strlen_zero(msg->file);
			int has_line = (msg->line > 0);
#ifndef GRANDSTREAM_NETWORKS
			int has_func = !ast_strlen_zero(msg->function);
#endif
			/*
			 * Verbose messages are interpreted by console channels in their own
			 * special way
			 */
			if (msg->level == __LOG_VERBOSE) {
				return logger_add_verbose_magic(msg, buf, size);
			}

			/* Turn the numerical line number into a string */
			snprintf(linestr, sizeof(linestr), "%d", msg->line);
			/* Build string to print out */
#ifdef GRANDSTREAM_NETWORKS
			snprintf(buf, size, "[%s] " COLORIZE_FMT "[%d]%s: " COLORIZE_FMT "%s" COLORIZE_FMT " " "%s\n",
				msg->date,
				COLORIZE(colors[msg->level], 0, msg->level_name),
				msg->lwp,
				call_identifier_str,
				COLORIZE(COLOR_BRWHITE, 0, has_file ? msg->file : ""),
				has_file ? ":" : "",
				COLORIZE(COLOR_BRWHITE, 0, has_line ? linestr : ""),
				msg->message);
#else
			snprintf(buf, size, "[%s] " COLORIZE_FMT "[%d]%s: " COLORIZE_FMT "%s" COLORIZE_FMT " " COLORIZE_FMT "%s %s",
				msg->date,
				COLORIZE(colors[msg->level], 0, msg->level_name),
				msg->lwp,
				call_identifier_str,
				COLORIZE(COLOR_BRWHITE, 0, has_file ? msg->file : ""),
				has_file ? ":" : "",
				COLORIZE(COLOR_BRWHITE, 0, has_line ? linestr : ""),
				COLORIZE(COLOR_BRWHITE, 0, has_func ? msg->function : ""),
				has_func ? ":" : "",
				msg->message);
#endif
		}
		break;
	}

	return 0;
}

static struct logformatter logformatter_default = {
	.name = "default",
	.format_log = format_log_default,
};

static void make_components(struct logchannel *chan)
{
	char *w;
	unsigned int logmask = 0;
	char *stringp = ast_strdupa(chan->components);
	unsigned int x;
	unsigned int verb_level;

	/* Default to using option_verbose as the verbosity level of the logging channel.  */
	verb_level = -1;

	w = strchr(stringp, '[');
	if (w) {
		char *end = strchr(w + 1, ']');
		if (!end) {
			fprintf(stderr, "Logger Warning: bad formatter definition for %s in logger.conf\n", chan->filename);
		} else {
			char *formatter_name = w + 1;

			*end = '\0';
			stringp = end + 1;

			if (!strcasecmp(formatter_name, "json")) {
				memcpy(&chan->formatter, &logformatter_json, sizeof(chan->formatter));
			} else if (!strcasecmp(formatter_name, "default")) {
				memcpy(&chan->formatter, &logformatter_default, sizeof(chan->formatter));
			} else {
				fprintf(stderr, "Logger Warning: Unknown formatter definition %s for %s in logger.conf; using 'default'\n",
					formatter_name, chan->filename);
				memcpy(&chan->formatter, &logformatter_default, sizeof(chan->formatter));
			}
		}
	}

	if (!chan->formatter.name) {
		memcpy(&chan->formatter, &logformatter_default, sizeof(chan->formatter));
	}

	while ((w = strsep(&stringp, ","))) {
		w = ast_strip(w);
		if (ast_strlen_zero(w)) {
			continue;
		}
		if (!strcmp(w, "*")) {
			logmask = 0xFFFFFFFF;
		} else if (!strncasecmp(w, "verbose(", 8)) {
			if (levels[__LOG_VERBOSE] && sscanf(w + 8, "%30u)", &verb_level) == 1) {
				logmask |= (1 << __LOG_VERBOSE);
			}
		} else {
			for (x = 0; x < ARRAY_LEN(levels); ++x) {
				if (levels[x] && !strcasecmp(w, levels[x])) {
					logmask |= (1 << x);
					break;
				}
			}
		}
	}
	if (chan->type == LOGTYPE_CONSOLE) {
		/*
		 * Force to use the root console verbose level so if the
		 * user specified any verbose level then it does not interfere
		 * with calculating the ast_verb_sys_level value.
		 */
		chan->verbosity = -1;
		logmask |= (1 << __LOG_VERBOSE);
	} else {
		chan->verbosity = verb_level;
	}
	chan->logmask = logmask;
}

/*!
 * \brief create the filename that will be used for a logger channel.
 *
 * \param channel The name of the logger channel
 * \param[out] filename The filename for the logger channel
 * \param size The size of the filename buffer
 */
static void make_filename(const char *channel, char *filename, size_t size)
{
	const char *log_dir_prefix = "";
	const char *log_dir_separator = "";

	*filename = '\0';

	if (!strcasecmp(channel, "console")) {
		return;
	}

	if (!strncasecmp(channel, "syslog", 6)) {
		ast_copy_string(filename, channel, size);
		return;
	}

	/* It's a filename */

	if (channel[0] != '/') {
		log_dir_prefix = ast_config_AST_LOG_DIR;
		log_dir_separator = "/";
	}

	if (!ast_strlen_zero(hostname)) {
		snprintf(filename, size, "%s%s%s.%s",
			log_dir_prefix, log_dir_separator, channel, hostname);
	} else {
		snprintf(filename, size, "%s%s%s",
			log_dir_prefix, log_dir_separator, channel);
	}
}

/*!
 * \brief Find a particular logger channel by name
 *
 * \pre logchannels list is locked
 *
 * \param channel The name of the logger channel to find
 * \retval non-NULL The corresponding logger channel
 * \retval NULL Unable to find a logger channel with that particular name
 */
static struct logchannel *find_logchannel(const char *channel)
{
	char filename[PATH_MAX];
	struct logchannel *chan;

	make_filename(channel, filename, sizeof(filename));

	AST_RWLIST_TRAVERSE(&logchannels, chan, list) {
		if (!strcmp(chan->filename, filename)) {
			return chan;
		}
	}

	return NULL;
}

static struct logchannel *make_logchannel(const char *channel, const char *components, int lineno, int dynamic)
{
	struct logchannel *chan;
	char *facility;
	struct ast_tm tm;
	struct timeval now = ast_tvnow();
	char datestring[256];

	if (ast_strlen_zero(channel) || !(chan = ast_calloc(1, sizeof(*chan) + strlen(components) + 1)))
		return NULL;

	strcpy(chan->components, components);
	chan->lineno = lineno;
	chan->dynamic = dynamic;

	make_filename(channel, chan->filename, sizeof(chan->filename));

	if (!strcasecmp(channel, "console")) {
		chan->type = LOGTYPE_CONSOLE;
	} else if (!strncasecmp(channel, "syslog", 6)) {
		/*
		* syntax is:
		*  syslog.facility => level,level,level
		*/
		facility = strchr(channel, '.');
		if (!facility++ || !facility) {
			facility = "local0";
		}

		chan->facility = ast_syslog_facility(facility);

		if (chan->facility < 0) {
			fprintf(stderr, "Logger Warning: bad syslog facility in logger.conf\n");
			ast_free(chan);
			return NULL;
		}

		chan->type = LOGTYPE_SYSLOG;
		openlog("asterisk", LOG_PID, chan->facility);
	} else {
		if (!(chan->fileptr = fopen(chan->filename, "a"))) {
			/* Can't do real logging here since we're called with a lock
			 * so log to any attached consoles */
			ast_console_puts_mutable("ERROR: Unable to open log file '", __LOG_ERROR);
			ast_console_puts_mutable(chan->filename, __LOG_ERROR);
			ast_console_puts_mutable("': ", __LOG_ERROR);
			ast_console_puts_mutable(strerror(errno), __LOG_ERROR);
			ast_console_puts_mutable("'\n", __LOG_ERROR);
			ast_free(chan);
			return NULL;
		} else {
			/* Create our date/time */
			ast_localtime(&now, &tm, NULL);
			ast_strftime(datestring, sizeof(datestring), dateformat, &tm);

			fprintf(chan->fileptr, "[%s] Asterisk %s built by %s @ %s on a %s running %s on %s\n",
				datestring, ast_get_version(), ast_build_user, ast_build_hostname,
				ast_build_machine, ast_build_os, ast_build_date);
			fflush(chan->fileptr);
		}
		chan->type = LOGTYPE_FILE;
	}
	make_components(chan);

	return chan;
}

/*!
 * \brief Read config, setup channels.
 * \param altconf Alternate configuration file to read.
 *
 * \pre logchannels list is write locked
 *
 * \retval 0 Success
 * \retval -1 No config found or Failed
 */
static int init_logger_chain(const char *altconf)
{
	struct logchannel *chan;
	struct ast_config *cfg;
	struct ast_variable *var;
	const char *s;
	struct ast_flags config_flags = { 0 };

	if (!(cfg = ast_config_load2(S_OR(altconf, "logger.conf"), "logger", config_flags)) || cfg == CONFIG_STATUS_FILEINVALID) {
		cfg = NULL;
	}

	/* Set defaults */
	hostname[0] = '\0';
	display_callids = 1;
	memset(&logfiles, 0, sizeof(logfiles));
	logfiles.queue_log = 1;
	ast_copy_string(dateformat, "%b %e %T", sizeof(dateformat));
	ast_copy_string(queue_log_name, QUEUELOG, sizeof(queue_log_name));
	exec_after_rotate[0] = '\0';
	rotatestrategy = SEQUENTIAL;

	/* delete our list of log channels */
	while ((chan = AST_RWLIST_REMOVE_HEAD(&logchannels, list))) {
		ast_free(chan);
	}
	global_logmask = 0;

	errno = 0;
	/* close syslog */
	closelog();

	/* If no config file, we're fine, set default options. */
	if (!cfg) {
		chan = make_logchannel("console", "error,warning,notice,verbose", 0, 0);
		if (!chan) {
			fprintf(stderr, "ERROR: Failed to initialize default logging\n");
			return -1;
		}

		AST_RWLIST_INSERT_HEAD(&logchannels, chan, list);
		global_logmask |= chan->logmask;

		return -1;
	}

	if ((s = ast_variable_retrieve(cfg, "general", "appendhostname"))) {
		if (ast_true(s)) {
			if (gethostname(hostname, sizeof(hostname) - 1)) {
				ast_copy_string(hostname, "unknown", sizeof(hostname));
				fprintf(stderr, "What box has no hostname???\n");
			}
		}
	}
	if ((s = ast_variable_retrieve(cfg, "general", "display_callids"))) {
		display_callids = ast_true(s);
	}
	if ((s = ast_variable_retrieve(cfg, "general", "dateformat"))) {
		ast_copy_string(dateformat, s, sizeof(dateformat));
	}
	if ((s = ast_variable_retrieve(cfg, "general", "queue_log"))) {
		logfiles.queue_log = ast_true(s);
	}
	if ((s = ast_variable_retrieve(cfg, "general", "queue_log_to_file"))) {
		logfiles.queue_log_to_file = ast_true(s);
	}
	if ((s = ast_variable_retrieve(cfg, "general", "queue_log_name"))) {
		ast_copy_string(queue_log_name, s, sizeof(queue_log_name));
	}
	if ((s = ast_variable_retrieve(cfg, "general", "queue_log_realtime_use_gmt"))) {
		logfiles.queue_log_realtime_use_gmt = ast_true(s);
	}
	if ((s = ast_variable_retrieve(cfg, "general", "exec_after_rotate"))) {
		ast_copy_string(exec_after_rotate, s, sizeof(exec_after_rotate));
	}
	if ((s = ast_variable_retrieve(cfg, "general", "rotatestrategy"))) {
		if (strcasecmp(s, "timestamp") == 0) {
			rotatestrategy = TIMESTAMP;
		} else if (strcasecmp(s, "rotate") == 0) {
			rotatestrategy = ROTATE;
		} else if (strcasecmp(s, "sequential") == 0) {
			rotatestrategy = SEQUENTIAL;
		} else if (strcasecmp(s, "none") == 0) {
			rotatestrategy = NONE;
		} else {
			fprintf(stderr, "Unknown rotatestrategy: %s\n", s);
		}
	} else {
		if ((s = ast_variable_retrieve(cfg, "general", "rotatetimestamp"))) {
			rotatestrategy = ast_true(s) ? TIMESTAMP : SEQUENTIAL;
			fprintf(stderr, "rotatetimestamp option has been deprecated.  Please use rotatestrategy instead.\n");
		}
	}
	if ((s = ast_variable_retrieve(cfg, "general", "logger_queue_limit"))) {
		if (sscanf(s, "%30d", &logger_queue_limit) != 1) {
			fprintf(stderr, "logger_queue_limit has an invalid value.  Leaving at default of %d.\n",
				logger_queue_limit);
		}
		if (logger_queue_limit < 10) {
			fprintf(stderr, "logger_queue_limit must be >= 10. Setting to 10.\n");
			logger_queue_limit = 10;
		}
	}

	var = ast_variable_browse(cfg, "logfiles");
	for (; var; var = var->next) {
		chan = make_logchannel(var->name, var->value, var->lineno, 0);
		if (!chan) {
			/* Print error message directly to the consoles since the lock is held
			 * and we don't want to unlock with the list partially built */
			ast_console_puts_mutable("ERROR: Unable to create log channel '", __LOG_ERROR);
			ast_console_puts_mutable(var->name, __LOG_ERROR);
			ast_console_puts_mutable("'\n", __LOG_ERROR);
			continue;
		}
		AST_RWLIST_INSERT_HEAD(&logchannels, chan, list);
		global_logmask |= chan->logmask;
	}

	if (qlog) {
		fclose(qlog);
		qlog = NULL;
	}

	ast_config_destroy(cfg);

	return 0;
}

void ast_child_verbose(int level, const char *fmt, ...)
{
	char *msg = NULL, *emsg = NULL, *sptr, *eptr;
	va_list ap, aq;
	int size;

	va_start(ap, fmt);
	va_copy(aq, ap);
	if ((size = vsnprintf(msg, 0, fmt, ap)) < 0) {
		va_end(ap);
		va_end(aq);
		return;
	}
	va_end(ap);

	if (!(msg = ast_malloc(size + 1))) {
		va_end(aq);
		return;
	}

	vsnprintf(msg, size + 1, fmt, aq);
	va_end(aq);

	if (!(emsg = ast_malloc(size * 2 + 1))) {
		ast_free(msg);
		return;
	}

	for (sptr = msg, eptr = emsg; ; sptr++) {
		if (*sptr == '"') {
			*eptr++ = '\\';
		}
		*eptr++ = *sptr;
		if (*sptr == '\0') {
			break;
		}
	}
	ast_free(msg);

	fprintf(stdout, "verbose \"%s\" %d\n", emsg, level);
	fflush(stdout);
	ast_free(emsg);
}

void ast_queue_log(const char *queuename, const char *callid, const char *agent, const char *event, const char *fmt, ...)
{
	va_list ap;
	struct timeval tv;
	struct ast_tm tm;
	char qlog_msg[8192];
	int qlog_len;
	char time_str[30];

	if (!logger_initialized) {
		/* You are too early.  We are not open yet! */
		return;
	}
	if (!queuelog_init) {
		/* We must initialize now since someone is trying to log something. */
		logger_queue_start();
	}

	if (ast_check_realtime("queue_log")) {
		tv = ast_tvnow();
		ast_localtime(&tv, &tm, logfiles.queue_log_realtime_use_gmt ? "GMT" : NULL);
		ast_strftime(time_str, sizeof(time_str), "%F %T.%6q", &tm);
		va_start(ap, fmt);
		vsnprintf(qlog_msg, sizeof(qlog_msg), fmt, ap);
		va_end(ap);
		if (logfiles.queue_adaptive_realtime) {
			AST_DECLARE_APP_ARGS(args,
				AST_APP_ARG(data)[5];
			);
			AST_NONSTANDARD_APP_ARGS(args, qlog_msg, '|');
			/* Ensure fields are large enough to receive data */
			ast_realtime_require_field("queue_log",
				"data1", RQ_CHAR, strlen(S_OR(args.data[0], "")),
				"data2", RQ_CHAR, strlen(S_OR(args.data[1], "")),
				"data3", RQ_CHAR, strlen(S_OR(args.data[2], "")),
				"data4", RQ_CHAR, strlen(S_OR(args.data[3], "")),
				"data5", RQ_CHAR, strlen(S_OR(args.data[4], "")),
				SENTINEL);

			/* Store the log */
			ast_store_realtime("queue_log", "time", time_str,
				"callid", callid,
				"queuename", queuename,
				"agent", agent,
				"event", event,
				"data1", S_OR(args.data[0], ""),
				"data2", S_OR(args.data[1], ""),
				"data3", S_OR(args.data[2], ""),
				"data4", S_OR(args.data[3], ""),
				"data5", S_OR(args.data[4], ""),
				SENTINEL);
		} else {
			ast_store_realtime("queue_log", "time", time_str,
				"callid", callid,
				"queuename", queuename,
				"agent", agent,
				"event", event,
				"data", qlog_msg,
				SENTINEL);
		}

		if (!logfiles.queue_log_to_file) {
			return;
		}
	}

	if (qlog) {
		va_start(ap, fmt);
		qlog_len = snprintf(qlog_msg, sizeof(qlog_msg), "%ld|%s|%s|%s|%s|", (long)time(NULL), callid, queuename, agent, event);
		vsnprintf(qlog_msg + qlog_len, sizeof(qlog_msg) - qlog_len, fmt, ap);
		va_end(ap);
		AST_RWLIST_RDLOCK(&logchannels);
		if (qlog) {
			fprintf(qlog, "%s\n", qlog_msg);
			fflush(qlog);
		}
		AST_RWLIST_UNLOCK(&logchannels);
	}
}

static int rotate_file(const char *filename)
{
	char old[PATH_MAX];
	char new[PATH_MAX];
	int x, y, which, found, res = 0, fd;
	char *suffixes[4] = { "", ".gz", ".bz2", ".Z" };

	switch (rotatestrategy) {
	case NONE:
		/* No rotation */
		break;
	case SEQUENTIAL:
		for (x = 0; ; x++) {
			snprintf(new, sizeof(new), "%s.%d", filename, x);
			fd = open(new, O_RDONLY);
			if (fd > -1)
				close(fd);
			else
				break;
		}
		if (rename(filename, new)) {
			fprintf(stderr, "Unable to rename file '%s' to '%s'\n", filename, new);
			res = -1;
		} else {
			filename = new;
		}
		break;
	case TIMESTAMP:
		snprintf(new, sizeof(new), "%s.%ld", filename, (long)time(NULL));
		if (rename(filename, new)) {
			fprintf(stderr, "Unable to rename file '%s' to '%s'\n", filename, new);
			res = -1;
		} else {
			filename = new;
		}
		break;
	case ROTATE:
		/* Find the next empty slot, including a possible suffix */
		for (x = 0; ; x++) {
			found = 0;
			for (which = 0; which < ARRAY_LEN(suffixes); which++) {
				snprintf(new, sizeof(new), "%s.%d%s", filename, x, suffixes[which]);
				fd = open(new, O_RDONLY);
				if (fd > -1) {
					close(fd);
					found = 1;
					break;
				}
			}
			if (!found) {
				break;
			}
		}

		/* Found an empty slot */
		for (y = x; y > 0; y--) {
			for (which = 0; which < ARRAY_LEN(suffixes); which++) {
				snprintf(old, sizeof(old), "%s.%d%s", filename, y - 1, suffixes[which]);
				fd = open(old, O_RDONLY);
				if (fd > -1) {
					/* Found the right suffix */
					close(fd);
					snprintf(new, sizeof(new), "%s.%d%s", filename, y, suffixes[which]);
					if (rename(old, new)) {
						fprintf(stderr, "Unable to rename file '%s' to '%s'\n", old, new);
						res = -1;
					}
					break;
				}
			}
		}

		/* Finally, rename the current file */
		snprintf(new, sizeof(new), "%s.0", filename);
		if (rename(filename, new)) {
			fprintf(stderr, "Unable to rename file '%s' to '%s'\n", filename, new);
			res = -1;
		} else {
			filename = new;
		}
	}

	if (!ast_strlen_zero(exec_after_rotate)) {
		struct ast_channel *c = ast_dummy_channel_alloc();
		char buf[512];

		pbx_builtin_setvar_helper(c, "filename", filename);
		pbx_substitute_variables_helper(c, exec_after_rotate, buf, sizeof(buf));
		if (c) {
			c = ast_channel_unref(c);
		}
		if (ast_safe_system(buf) == -1) {
			ast_log(LOG_WARNING, "error executing '%s'\n", buf);
		}
	}
	return res;
}

/*!
 * \internal
 * \brief Start the realtime queue logging if configured.
 *
 * \retval TRUE if not to open queue log file.
 */
static int logger_queue_rt_start(void)
{
	if (ast_check_realtime("queue_log")) {
		if (!ast_realtime_require_field("queue_log",
			"time", RQ_DATETIME, 26,
			"data1", RQ_CHAR, 20,
			"data2", RQ_CHAR, 20,
			"data3", RQ_CHAR, 20,
			"data4", RQ_CHAR, 20,
			"data5", RQ_CHAR, 20,
			SENTINEL)) {
			logfiles.queue_adaptive_realtime = 1;
		} else {
			logfiles.queue_adaptive_realtime = 0;
		}

		if (!logfiles.queue_log_to_file) {
			/* Don't open the log file. */
			return 1;
		}
	}
	return 0;
}

/*!
 * \internal
 * \brief Rotate the queue log file and restart.
 *
 * \param queue_rotate Log queue rotation mode.
 *
 * \note Assumes logchannels is write locked on entry.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
static int logger_queue_restart(int queue_rotate)
{
	int res = 0;
	char qfname[PATH_MAX];

	if (logger_queue_rt_start()) {
		return res;
	}

	snprintf(qfname, sizeof(qfname), "%s/%s", ast_config_AST_LOG_DIR, queue_log_name);
	if (qlog) {
		/* Just in case it was still open. */
		fclose(qlog);
		qlog = NULL;
	}
	if (queue_rotate) {
		rotate_file(qfname);
	}

	/* Open the log file. */
	qlog = fopen(qfname, "a");
	if (!qlog) {
		ast_log(LOG_ERROR, "Unable to create queue log: %s\n", strerror(errno));
		res = -1;
	}
	return res;
}

static int reload_logger(int rotate, const char *altconf)
{
	int queue_rotate = rotate;
	struct logchannel *f;
	int res = 0;

	AST_RWLIST_WRLOCK(&logchannels);

	if (qlog) {
		if (rotate < 0) {
			/* Check filesize - this one typically doesn't need an auto-rotate */
			if (ftello(qlog) > 0x40000000) { /* Arbitrarily, 1 GB */
				fclose(qlog);
				qlog = NULL;
			} else {
				queue_rotate = 0;
			}
		} else {
			fclose(qlog);
			qlog = NULL;
		}
	} else {
		queue_rotate = 0;
	}

	ast_mkdir(ast_config_AST_LOG_DIR, 0777);

	AST_RWLIST_TRAVERSE(&logchannels, f, list) {
		if (f->disabled) {
			f->disabled = 0;	/* Re-enable logging at reload */
			/*** DOCUMENTATION
				<managerEventInstance>
					<synopsis>Raised when a logging channel is re-enabled after a reload operation.</synopsis>
					<syntax>
						<parameter name="Channel">
							<para>The name of the logging channel.</para>
						</parameter>
					</syntax>
				</managerEventInstance>
			***/
			manager_event(EVENT_FLAG_SYSTEM, "LogChannel", "Channel: %s\r\nEnabled: Yes\r\n", f->filename);
		}
		if (f->fileptr && (f->fileptr != stdout) && (f->fileptr != stderr)) {
			int rotate_this = 0;
			if (rotatestrategy != NONE && ftello(f->fileptr) > 0x40000000) { /* Arbitrarily, 1 GB */
				/* Be more proactive about rotating massive log files */
				rotate_this = 1;
			}
			fclose(f->fileptr);	/* Close file */
			f->fileptr = NULL;
			if (rotate || rotate_this) {
				rotate_file(f->filename);
			}
		}
	}

	filesize_reload_needed = 0;

	init_logger_chain(altconf);

	ast_unload_realtime("queue_log");
	if (logfiles.queue_log) {
		res = logger_queue_restart(queue_rotate);
		AST_RWLIST_UNLOCK(&logchannels);
		ast_verb_update();
		ast_queue_log("NONE", "NONE", "NONE", "CONFIGRELOAD", "%s", "");
		ast_verb(1, "Asterisk Queue Logger restarted\n");
	} else {
		AST_RWLIST_UNLOCK(&logchannels);
		ast_verb_update();
	}

	return res;
}

static char *handle_logger_reload(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "logger reload";
		e->usage =
			"Usage: logger reload [<alt-conf>]\n"
			"       Reloads the logger subsystem state.  Use after restarting syslogd(8) if you are using syslog logging.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (reload_logger(0, a->argc == 3 ? a->argv[2] : NULL)) {
		ast_cli(a->fd, "Failed to reload the logger\n");
		return CLI_FAILURE;
	}
	return CLI_SUCCESS;
}

static char *handle_logger_rotate(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "logger rotate";
		e->usage =
			"Usage: logger rotate\n"
			"       Rotates and Reopens the log files.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (reload_logger(1, NULL)) {
		ast_cli(a->fd, "Failed to reload the logger and rotate log files\n");
		return CLI_FAILURE;
	}
	return CLI_SUCCESS;
}

int ast_logger_rotate()
{
	return reload_logger(1, NULL);
}

int ast_logger_rotate_channel(const char *log_channel)
{
	struct logchannel *f;
	int success = AST_LOGGER_FAILURE;
	char filename[PATH_MAX];

	make_filename(log_channel, filename, sizeof(filename));

	AST_RWLIST_WRLOCK(&logchannels);

	ast_mkdir(ast_config_AST_LOG_DIR, 0644);

	AST_RWLIST_TRAVERSE(&logchannels, f, list) {
		if (f->disabled) {
			f->disabled = 0;	/* Re-enable logging at reload */
			manager_event(EVENT_FLAG_SYSTEM, "LogChannel", "Channel: %s\r\nEnabled: Yes\r\n",
				f->filename);
		}
		if (f->fileptr && (f->fileptr != stdout) && (f->fileptr != stderr)) {
			fclose(f->fileptr);	/* Close file */
			f->fileptr = NULL;
			if (strcmp(filename, f->filename) == 0) {
				rotate_file(f->filename);
				success = AST_LOGGER_SUCCESS;
			}
		}
	}

	init_logger_chain(NULL);

	AST_RWLIST_UNLOCK(&logchannels);

	return success;
}

#ifdef GRANDSTREAM_NETWORKS
static char *handle_logger_save_switch(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "logger save switch";
		e->usage = "Usage: logger save switch\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 3) {
		return CLI_SHOWUSAGE;
	}

	ast_logger_db_insert();
	ast_cli(a->fd, "Logger save switch successfully.\n");

	return CLI_SUCCESS;
}

static char *handle_logger_set_level(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct ast_log_modules *mod = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "logger set module";
		e->usage =
			"Usage: logger set module {module} level {DEBUG|NOTICE|WARNING|ERROR|VERBOSE|DTMF} {on|off}\n"
			"       Set a specific log level to enabled/disabled for this console.\n";
		return NULL;
	case CLI_GENERATE:
		if (3 == a->pos) {
			return ast_log_find_modname(S_OR(a->argv[3], NULL), a->n);
		} else if (4 == a->pos && 0 == a->n) {
			return ast_strdup("level");
		} else if (5 == a->pos) {
			return ast_log_find_level(S_OR(a->argv[5], NULL), a->n);
		} else if (6 == a->pos) {
			return ast_log_find_switch(S_OR(a->argv[6], NULL), a->n);
		}
		return NULL;
	}

	if (a->argc < 7)
		return CLI_SHOWUSAGE;

	ao2_lock(log_container);
	mod = ao2_find(log_container, a->argv[3], 0);
	ao2_unlock(log_container);

	if (!mod) {
		ast_cli(a->fd, "Module '%s' is not exist!\n", a->argv[3]);
	}

	if (!strcasecmp(a->argv[6], "off")) {
		int index;

		if (!strcasecmp(a->argv[3], "ALL")) {
			if (!strcasecmp(a->argv[5], "ALL")) {
				for (index = 0; index <= ast_log_index && index < 65535; index++) {
					ast_log_indexs[index] = 0;
				}
			} else if (!strcasecmp(a->argv[5], "DEBUG")) {
				for (index = 0; index <= ast_log_index && index < 65535; index++) {
					ast_log_indexs[index] &= ~AST_DEBUG_LOG_ENUM;
				}
			} else if (!strcasecmp(a->argv[5], "NOTICE")) {
				for (index = 0; index <= ast_log_index && index < 65535; index++) {
					ast_log_indexs[index] &= ~AST_NOTICE_LOG_ENUM;
				}
			} else if (!strcasecmp(a->argv[5], "WARNING")) {
				for (index = 0; index <= ast_log_index && index < 65535; index++) {
					ast_log_indexs[index] &= ~AST_WARNING_LOG_ENUM;
				}
			} else if (!strcasecmp(a->argv[5], "ERROR")) {
				for (index = 0; index <= ast_log_index && index < 65535; index++) {
					ast_log_indexs[index] &= ~AST_ERROR_LOG_ENUM;
				}
			} else if (!strcasecmp(a->argv[5], "VERBOSE")) {
				for (index = 0; index <= ast_log_index && index < 65535; index++) {
					ast_log_indexs[index] &= ~AST_VERBOSE_LOG_ENUM;
				}
			}
		} else {
			if (!strcasecmp(a->argv[5], "ALL")) {
				ast_log_indexs[mod->index] = 0;
			} else if (!strcasecmp(a->argv[5], "DEBUG")) {
				ast_log_indexs[mod->index] &= ~AST_DEBUG_LOG_ENUM;
			} else if (!strcasecmp(a->argv[5], "NOTICE")) {
				ast_log_indexs[mod->index] &= ~AST_NOTICE_LOG_ENUM;
			} else if (!strcasecmp(a->argv[5], "WARNING")) {
				ast_log_indexs[mod->index] &= ~AST_WARNING_LOG_ENUM;
			} else if (!strcasecmp(a->argv[5], "ERROR")) {
				ast_log_indexs[mod->index] &= ~AST_ERROR_LOG_ENUM;
			} else if (!strcasecmp(a->argv[5], "VERBOSE")) {
				ast_log_indexs[mod->index] &= ~AST_VERBOSE_LOG_ENUM;
			}
		}
	} else {
		if (!strcasecmp(a->argv[3], "ALL")) {
			int index;

			if (!strcasecmp(a->argv[5], "ALL")) {
				for (index = 0; index <= ast_log_index && index < 65535; index++) {
					ast_log_indexs[index] = AST_ALL_LOG_ENUM;
				}
			} else if (!strcasecmp(a->argv[5], "DEBUG")) {
				for (index = 0; index <= ast_log_index && index < 65535; index++) {
					ast_log_indexs[index] |= AST_DEBUG_LOG_ENUM;
				}
			} else if (!strcasecmp(a->argv[5], "NOTICE")) {
				for (index = 0; index <= ast_log_index && index < 65535; index++) {
					ast_log_indexs[index] |= AST_NOTICE_LOG_ENUM;
				}
			} else if (!strcasecmp(a->argv[5], "WARNING")) {
				for (index = 0; index <= ast_log_index && index < 65535; index++) {
					ast_log_indexs[index] |= AST_WARNING_LOG_ENUM;
				}
			} else if (!strcasecmp(a->argv[5], "ERROR")) {
				for (index = 0; index <= ast_log_index && index < 65535; index++) {
					ast_log_indexs[index] |= AST_ERROR_LOG_ENUM;
				}
			} else if (!strcasecmp(a->argv[5], "VERBOSE")) {
				for (index = 0; index <= ast_log_index && index < 65535; index++) {
					ast_log_indexs[index] |= AST_VERBOSE_LOG_ENUM;
				}
			}
		} else {
			if (!strcasecmp(a->argv[5], "ALL")) {
				ast_log_indexs[mod->index] = AST_ALL_LOG_ENUM;
			} else if (!strcasecmp(a->argv[5], "DEBUG")) {
				ast_log_indexs[mod->index] |= AST_DEBUG_LOG_ENUM;
			} else if (!strcasecmp(a->argv[5], "NOTICE")) {
				ast_log_indexs[mod->index] |= AST_NOTICE_LOG_ENUM;
			} else if (!strcasecmp(a->argv[5], "WARNING")) {
				ast_log_indexs[mod->index] |= AST_WARNING_LOG_ENUM;
			} else if (!strcasecmp(a->argv[5], "ERROR")) {
				ast_log_indexs[mod->index] |= AST_ERROR_LOG_ENUM;
			} else if (!strcasecmp(a->argv[5], "VERBOSE")) {
				ast_log_indexs[mod->index] |= AST_VERBOSE_LOG_ENUM;
			}
		}
	}

	ao2_ref(mod, -1);

	return CLI_SUCCESS;
}
#else
static char *handle_logger_set_level(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int x;
	int state;
	int level = -1;

	switch (cmd) {
	case CLI_INIT:
		e->command = "logger set level {DEBUG|NOTICE|WARNING|ERROR|VERBOSE|DTMF} {on|off}";
		e->usage =
			"Usage: logger set level {DEBUG|NOTICE|WARNING|ERROR|VERBOSE|DTMF} {on|off}\n"
			"       Set a specific log level to enabled/disabled for this console.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 5)
		return CLI_SHOWUSAGE;

	AST_RWLIST_WRLOCK(&logchannels);

	for (x = 0; x < ARRAY_LEN(levels); x++) {
		if (levels[x] && !strcasecmp(a->argv[3], levels[x])) {
			level = x;
			break;
		}
	}

	AST_RWLIST_UNLOCK(&logchannels);

	state = ast_true(a->argv[4]) ? 1 : 0;

	if (level != -1) {
		ast_console_toggle_loglevel(a->fd, level, state);
		ast_cli(a->fd, "Logger status for '%s' has been set to '%s'.\n", levels[level], state ? "on" : "off");
	} else
		return CLI_SHOWUSAGE;

	return CLI_SUCCESS;
}
#endif

int ast_logger_get_channels(int (*logentry)(const char *channel, const char *type,
	const char *status, const char *configuration, void *data), void *data)
{
	struct logchannel *chan;
	struct ast_str *configs = ast_str_create(64);
	int res = AST_LOGGER_SUCCESS;

	if (!configs) {
		return AST_LOGGER_ALLOC_ERROR;
	}

	AST_RWLIST_RDLOCK(&logchannels);
	AST_RWLIST_TRAVERSE(&logchannels, chan, list) {
		unsigned int level;

		ast_str_reset(configs);

		for (level = 0; level < ARRAY_LEN(levels); level++) {
			if ((chan->logmask & (1 << level)) && levels[level]) {
				ast_str_append(&configs, 0, "%s ", levels[level]);
			}
		}

		res = logentry(chan->filename, chan->type == LOGTYPE_CONSOLE ? "Console" :
			(chan->type == LOGTYPE_SYSLOG ? "Syslog" : "File"), chan->disabled ?
			"Disabled" : "Enabled", ast_str_buffer(configs), data);

		if (res) {
			AST_RWLIST_UNLOCK(&logchannels);
			ast_free(configs);
			configs = NULL;
			return AST_LOGGER_FAILURE;
		}
	}
	AST_RWLIST_UNLOCK(&logchannels);

	ast_free(configs);
	configs = NULL;

	return AST_LOGGER_SUCCESS;
}

/*! \brief CLI command to show logging system configuration */
static char *handle_logger_show_channels(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
#define FORMATL	"%-35.35s %-8.8s %-10.10s %-9.9s "
	struct logchannel *chan;
	switch (cmd) {
	case CLI_INIT:
		e->command = "logger show channels";
		e->usage =
			"Usage: logger show channels\n"
			"       List configured logger channels.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	ast_cli(a->fd, "Logger queue limit: %d\n\n", logger_queue_limit);
	ast_cli(a->fd, FORMATL, "Channel", "Type", "Formatter", "Status");
	ast_cli(a->fd, "Configuration\n");
	ast_cli(a->fd, FORMATL, "-------", "----", "---------", "------");
	ast_cli(a->fd, "-------------\n");
	AST_RWLIST_RDLOCK(&logchannels);
	AST_RWLIST_TRAVERSE(&logchannels, chan, list) {
		unsigned int level;

		ast_cli(a->fd, FORMATL, chan->filename, chan->type == LOGTYPE_CONSOLE ? "Console" : (chan->type == LOGTYPE_SYSLOG ? "Syslog" : "File"),
			chan->formatter.name,
			chan->disabled ? "Disabled" : "Enabled");
		ast_cli(a->fd, " - ");
		for (level = 0; level < ARRAY_LEN(levels); level++) {
			if ((chan->logmask & (1 << level)) && levels[level]) {
				ast_cli(a->fd, "%s ", levels[level]);
			}
		}
		ast_cli(a->fd, "\n");
	}
	AST_RWLIST_UNLOCK(&logchannels);
	ast_cli(a->fd, "\n");

	return CLI_SUCCESS;
}

int ast_logger_create_channel(const char *log_channel, const char *components)
{
	struct logchannel *chan;

	if (ast_strlen_zero(components)) {
		return AST_LOGGER_DECLINE;
	}

	AST_RWLIST_WRLOCK(&logchannels);

	chan = find_logchannel(log_channel);
	if (chan) {
		AST_RWLIST_UNLOCK(&logchannels);
		return AST_LOGGER_FAILURE;
	}

	chan = make_logchannel(log_channel, components, 0, 1);
	if (!chan) {
		AST_RWLIST_UNLOCK(&logchannels);
		return AST_LOGGER_ALLOC_ERROR;
	}

	AST_RWLIST_INSERT_HEAD(&logchannels, chan, list);
	global_logmask |= chan->logmask;

	AST_RWLIST_UNLOCK(&logchannels);

	return AST_LOGGER_SUCCESS;
}

static char *handle_logger_add_channel(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "logger add channel";
		e->usage =
			"Usage: logger add channel <name> <levels>\n"
			"       Adds a temporary logger channel. This logger channel\n"
			"       will exist until removed or until Asterisk is restarted.\n"
			"       <levels> is a comma-separated list of desired logger\n"
			"       levels such as: verbose,warning,error\n"
			"       An optional formatter may be specified with the levels;\n"
			"       valid values are '[json]' and '[default]'.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 5) {
		return CLI_SHOWUSAGE;
	}

	switch (ast_logger_create_channel(a->argv[3], a->argv[4])) {
	case AST_LOGGER_SUCCESS:
		return CLI_SUCCESS;
	case AST_LOGGER_FAILURE:
		ast_cli(a->fd, "Logger channel '%s' already exists\n", a->argv[3]);
		return CLI_SUCCESS;
	case AST_LOGGER_DECLINE:
	case AST_LOGGER_ALLOC_ERROR:
	default:
		ast_cli(a->fd, "ERROR: Unable to create log channel '%s'\n", a->argv[3]);
		return CLI_FAILURE;
	}
}

int ast_logger_remove_channel(const char *log_channel)
{
	struct logchannel *chan;

	AST_RWLIST_WRLOCK(&logchannels);

	chan = find_logchannel(log_channel);
	if (chan && chan->dynamic) {
		AST_RWLIST_REMOVE(&logchannels, chan, list);
	} else {
		AST_RWLIST_UNLOCK(&logchannels);
		return AST_LOGGER_FAILURE;
	}
	AST_RWLIST_UNLOCK(&logchannels);

	if (chan->fileptr) {
		fclose(chan->fileptr);
		chan->fileptr = NULL;
	}
	ast_free(chan);
	chan = NULL;

	return AST_LOGGER_SUCCESS;
}

static char *handle_logger_remove_channel(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct logchannel *chan;
	int gen_count = 0;
	char *gen_ret = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "logger remove channel";
		e->usage =
			"Usage: logger remove channel <name>\n"
			"       Removes a temporary logger channel.\n";
		return NULL;
	case CLI_GENERATE:
		if (a->argc > 4 || (a->argc == 4 && a->pos > 3)) {
			return NULL;
		}
		AST_RWLIST_RDLOCK(&logchannels);
		AST_RWLIST_TRAVERSE(&logchannels, chan, list) {
			if (chan->dynamic && (ast_strlen_zero(a->argv[3])
				|| !strncmp(a->argv[3], chan->filename, strlen(a->argv[3])))) {
				if (gen_count == a->n) {
					gen_ret = ast_strdup(chan->filename);
					break;
				}
				gen_count++;
			}
		}
		AST_RWLIST_UNLOCK(&logchannels);
		return gen_ret;
	}

	if (a->argc < 4) {
		return CLI_SHOWUSAGE;
	}

	switch (ast_logger_remove_channel(a->argv[3])) {
	case AST_LOGGER_SUCCESS:
		ast_cli(a->fd, "Removed dynamic logger channel '%s'\n", a->argv[3]);
		return CLI_SUCCESS;
	case AST_LOGGER_FAILURE:
		ast_cli(a->fd, "Unable to find dynamic logger channel '%s'\n", a->argv[3]);
		return CLI_SUCCESS;
	default:
		ast_cli(a->fd, "Internal failure attempting to delete dynamic logger channel '%s'\n", a->argv[3]);
		return CLI_FAILURE;
	}
}

static struct ast_cli_entry cli_logger[] = {
	AST_CLI_DEFINE(handle_logger_show_channels, "List configured log channels"),
	AST_CLI_DEFINE(handle_logger_reload, "Reopens the log files"),
	AST_CLI_DEFINE(handle_logger_rotate, "Rotates and reopens the log files"),
	AST_CLI_DEFINE(handle_logger_set_level, "Enables/Disables a specific logging level for this console"),
	AST_CLI_DEFINE(handle_logger_add_channel, "Adds a new logging channel"),
	AST_CLI_DEFINE(handle_logger_remove_channel, "Removes a logging channel"),
#ifdef GRANDSTREAM_NETWORKS
	AST_CLI_DEFINE(handle_logger_save_switch, "Save logger switch"),
#endif
};

static void _handle_SIGXFSZ(int sig)
{
	/* Indicate need to reload */
	filesize_reload_needed = 1;
}

static struct sigaction handle_SIGXFSZ = {
	.sa_handler = _handle_SIGXFSZ,
	.sa_flags = SA_RESTART,
};

/*! \brief Print a normal log message to the channels */
static void logger_print_normal(struct logmsg *logmsg)
{
	struct logchannel *chan = NULL;
	char buf[BUFSIZ];
	int level = 0;

	AST_RWLIST_RDLOCK(&logchannels);
	if (!AST_RWLIST_EMPTY(&logchannels)) {
		AST_RWLIST_TRAVERSE(&logchannels, chan, list) {

			/* If the channel is disabled, then move on to the next one */
			if (chan->disabled) {
				continue;
			}
			if (logmsg->level == __LOG_VERBOSE
				&& (((chan->verbosity < 0) ? option_verbose : chan->verbosity)) < level) {
				continue;
			}

			if (!(chan->logmask & (1 << logmsg->level))) {
				continue;
			}

			switch (chan->type) {
			case LOGTYPE_SYSLOG:
				{
					int syslog_level = ast_syslog_priority_from_loglevel(logmsg->level);

					if (syslog_level < 0) {
						/* we are locked here, so cannot ast_log() */
						fprintf(stderr, "ast_log_vsyslog called with bogus level: %d\n", logmsg->level);
						continue;
					}

					/* Don't use LOG_MAKEPRI because it's broken in glibc<2.17 */
					syslog_level = chan->facility | syslog_level; /* LOG_MAKEPRI(chan->facility, syslog_level); */
					if (!chan->formatter.format_log(chan, logmsg, buf, BUFSIZ)) {
						syslog(syslog_level, "%s", buf);
					}
				}
				break;
			case LOGTYPE_CONSOLE:
				if (!chan->formatter.format_log(chan, logmsg, buf, BUFSIZ)) {
					ast_console_puts_mutable_full(buf, logmsg->level, logmsg->sublevel);
				}
				break;
			case LOGTYPE_FILE:
				{
					int res = 0;

					if (!chan->fileptr) {
						continue;
					}

					if (chan->formatter.format_log(chan, logmsg, buf, BUFSIZ)) {
						continue;
					}

					/* Print out to the file */
					res = fprintf(chan->fileptr, "%s", buf);
					if (res > 0) {
						fflush(chan->fileptr);
					} else if (res <= 0 && !ast_strlen_zero(logmsg->message)) {
						fprintf(stderr, "**** Asterisk Logging Error: ***********\n");
						if (errno == ENOMEM || errno == ENOSPC) {
							fprintf(stderr, "Asterisk logging error: Out of disk space, can't log to log file %s\n", chan->filename);
						} else {
							fprintf(stderr, "Logger Warning: Unable to write to log file '%s': %s (disabled)\n", chan->filename, strerror(errno));
						}

						/*** DOCUMENTATION
							<managerEventInstance>
								<synopsis>Raised when a logging channel is disabled.</synopsis>
								<syntax>
									<parameter name="Channel">
										<para>The name of the logging channel.</para>
									</parameter>
								</syntax>
							</managerEventInstance>
						***/
						manager_event(EVENT_FLAG_SYSTEM, "LogChannel", "Channel: %s\r\nEnabled: No\r\nReason: %d - %s\r\n", chan->filename, errno, strerror(errno));
						chan->disabled = 1;
					}
				}
				break;
			}
		}
	} else if (logmsg->level != __LOG_VERBOSE || option_verbose >= logmsg->sublevel) {
		fputs(logmsg->message, stdout);
	}

	AST_RWLIST_UNLOCK(&logchannels);

	/* If we need to reload because of the file size, then do so */
	if (filesize_reload_needed) {
		reload_logger(-1, NULL);
		ast_verb(1, "Rotated Logs Per SIGXFSZ (Exceeded file size limit)\n");
	}

	return;
}

static struct logmsg * __attribute__((format(printf, 7, 0))) format_log_message_ap(int level,
	int sublevel, const char *file, int line, const char *function, ast_callid callid,
	const char *fmt, va_list ap)
{
	struct logmsg *logmsg = NULL;
	struct ast_str *buf = NULL;
	struct ast_tm tm;
	struct timeval now = ast_tvnow();
	int res = 0;
	char datestring[256];

	if (!(buf = ast_str_thread_get(&log_buf, LOG_BUF_INIT_SIZE))) {
		return NULL;
	}

	/* Build string */
	res = ast_str_set_va(&buf, BUFSIZ, fmt, ap);

	/* If the build failed, then abort and free this structure */
	if (res == AST_DYNSTR_BUILD_FAILED) {
		return NULL;
	}

	/* Create a new logging message */
	if (!(logmsg = ast_calloc_with_stringfields(1, struct logmsg, res + 128))) {
		return NULL;
	}

	/* Copy string over */
	ast_string_field_set(logmsg, message, ast_str_buffer(buf));

	/* Set type */
	if (level == __LOG_VERBOSE) {
		logmsg->type = LOGMSG_VERBOSE;
	} else {
		logmsg->type = LOGMSG_NORMAL;
	}

	if (display_callids && callid) {
		logmsg->callid = callid;
	}

	/* Create our date/time */
	ast_localtime(&now, &tm, NULL);
	ast_strftime(datestring, sizeof(datestring), dateformat, &tm);
	ast_string_field_set(logmsg, date, datestring);

	/* Copy over data */
	logmsg->level = level;
	logmsg->sublevel = sublevel;
	logmsg->line = line;
	ast_string_field_set(logmsg, level_name, levels[level]);
	ast_string_field_set(logmsg, file, file);
	ast_string_field_set(logmsg, function, function);
	logmsg->lwp = ast_get_tid();

	return logmsg;
}

static struct logmsg * __attribute__((format(printf, 7, 0))) format_log_message(int level,
	int sublevel, const char *file, int line, const char *function, ast_callid callid,
	const char *fmt, ...)
{
	struct logmsg *logmsg;
	va_list ap;

	va_start(ap, fmt);
	logmsg = format_log_message_ap(level, sublevel, file, line, function, callid, fmt, ap);
	va_end(ap);

	return logmsg;
}

/*! \brief Actual logging thread */
static void *logger_thread(void *data)
{
	struct logmsg *next = NULL, *msg = NULL;

	for (;;) {
		/* We lock the message list, and see if any message exists... if not we wait on the condition to be signalled */
		AST_LIST_LOCK(&logmsgs);
		if (AST_LIST_EMPTY(&logmsgs)) {
			if (close_logger_thread) {
				AST_LIST_UNLOCK(&logmsgs);
				break;
			} else {
				ast_cond_wait(&logcond, &logmsgs.lock);
			}
		}

		if (high_water_alert) {
			msg = format_log_message(__LOG_WARNING, 0, "logger", 0, "***", 0,
				"Logging resumed.  %d message%s discarded.\n",
				logger_messages_discarded, logger_messages_discarded == 1 ? "" : "s");
			if (msg) {
				AST_LIST_INSERT_TAIL(&logmsgs, msg, list);
			}
			high_water_alert = 0;
			logger_messages_discarded = 0;
		}

		next = AST_LIST_FIRST(&logmsgs);
		AST_LIST_HEAD_INIT_NOLOCK(&logmsgs);
		logger_queue_size = 0;
		AST_LIST_UNLOCK(&logmsgs);

		/* Otherwise go through and process each message in the order added */
		while ((msg = next)) {
			/* Get the next entry now so that we can free our current structure later */
			next = AST_LIST_NEXT(msg, list);

			/* Depending on the type, send it to the proper function */
			logger_print_normal(msg);

			/* Free the data since we are done */
			logmsg_free(msg);
		}
	}

	return NULL;
}

/*!
 * \internal
 * \brief Initialize the logger queue.
 *
 * \note Assumes logchannels is write locked on entry.
 *
 * \return Nothing
 */
static void logger_queue_init(void)
{
	ast_unload_realtime("queue_log");
	if (logfiles.queue_log) {
		char qfname[PATH_MAX];

		if (logger_queue_rt_start()) {
			return;
		}

		/* Open the log file. */
		snprintf(qfname, sizeof(qfname), "%s/%s", ast_config_AST_LOG_DIR,
			queue_log_name);
		if (qlog) {
			/* Just in case it was already open. */
			fclose(qlog);
		}
		qlog = fopen(qfname, "a");
		if (!qlog) {
			ast_log(LOG_ERROR, "Unable to create queue log: %s\n", strerror(errno));
		}
	}
}

int ast_is_logger_initialized(void)
{
	return logger_initialized;
}

/*!
 * \brief Start the ast_queue_log() logger.
 *
 * \note Called when the system is fully booted after startup
 * so preloaded realtime modules can get up.
 *
 * \return Nothing
 */
void logger_queue_start(void)
{
	/* Must not be called before the logger is initialized. */
	ast_assert(logger_initialized);

	AST_RWLIST_WRLOCK(&logchannels);
	if (!queuelog_init) {
		logger_queue_init();
		queuelog_init = 1;
		AST_RWLIST_UNLOCK(&logchannels);
		ast_queue_log("NONE", "NONE", "NONE", "QUEUESTART", "%s", "");
	} else {
		AST_RWLIST_UNLOCK(&logchannels);
	}
}

int init_logger(void)
{
	int res;
	/* auto rotate if sig SIGXFSZ comes a-knockin */
	sigaction(SIGXFSZ, &handle_SIGXFSZ, NULL);

	/* Re-initialize the logmsgs mutex.  The recursive mutex can be accessed prior
 	 * to Asterisk being forked into the background, which can cause the thread
 	 * ID tracked by the underlying pthread mutex to be different than the ID of
 	 * the thread that unlocks the mutex.  Since init_logger is called after the
 	 * fork, it is safe to initialize the mutex here for future accesses.
 	 */
	ast_mutex_destroy(&logmsgs.lock);
	ast_mutex_init(&logmsgs.lock);
	ast_cond_init(&logcond, NULL);

	/* start logger thread */
	if (ast_pthread_create(&logthread, NULL, logger_thread, NULL) < 0) {
		ast_cond_destroy(&logcond);
		return -1;
	}

	/* register the logger cli commands */
	ast_cli_register_multiple(cli_logger, ARRAY_LEN(cli_logger));

	ast_mkdir(ast_config_AST_LOG_DIR, 0777);

	/* create log channels */
	AST_RWLIST_WRLOCK(&logchannels);
	res = init_logger_chain(NULL);
	AST_RWLIST_UNLOCK(&logchannels);
	ast_verb_update();
	logger_initialized = 1;
	if (res) {
		ast_log(LOG_ERROR, "Errors detected in logger.conf.  Default console logging is being used.\n");
	}

#ifdef GRANDSTREAM_NETWORKS
	ast_logger_db_init();
#endif

	return 0;
}

void close_logger(void)
{
	struct logchannel *f = NULL;

	ast_cli_unregister_multiple(cli_logger, ARRAY_LEN(cli_logger));

	logger_initialized = 0;

	/* Stop logger thread */
	AST_LIST_LOCK(&logmsgs);
	close_logger_thread = 1;
	ast_cond_signal(&logcond);
	AST_LIST_UNLOCK(&logmsgs);

	if (logthread != AST_PTHREADT_NULL) {
		pthread_join(logthread, NULL);
	}

	AST_RWLIST_WRLOCK(&logchannels);

	if (qlog) {
		fclose(qlog);
		qlog = NULL;
	}

	while ((f = AST_LIST_REMOVE_HEAD(&logchannels, list))) {
		if (f->fileptr && (f->fileptr != stdout) && (f->fileptr != stderr)) {
			fclose(f->fileptr);
			f->fileptr = NULL;
		}
		ast_free(f);
	}

	closelog(); /* syslog */

	AST_RWLIST_UNLOCK(&logchannels);

	ast_log_modules_destroy();
}

void ast_callid_strnprint(char *buffer, size_t buffer_size, ast_callid callid)
{
	snprintf(buffer, buffer_size, "[C-%08x]", callid);
}

ast_callid ast_create_callid(void)
{
	return ast_atomic_fetchadd_int(&next_unique_callid, +1);
}

ast_callid ast_read_threadstorage_callid(void)
{
	ast_callid *callid;

	callid = ast_threadstorage_get(&unique_callid, sizeof(*callid));

	return callid ? *callid : 0;
}

int ast_callid_threadassoc_change(ast_callid callid)
{
	ast_callid *id = ast_threadstorage_get(&unique_callid, sizeof(*id));

	if (!id) {
		return -1;
	}

	*id = callid;

	return 0;
}

int ast_callid_threadassoc_add(ast_callid callid)
{
	ast_callid *pointing;

	pointing = ast_threadstorage_get(&unique_callid, sizeof(*pointing));
	if (!pointing) {
		return -1;
	}

	if (*pointing) {
		ast_log(LOG_ERROR, "ast_callid_threadassoc_add(C-%08x) on thread "
			"already associated with callid [C-%08x].\n", callid, *pointing);
		return 1;
	}

	*pointing = callid;
	return 0;
}

int ast_callid_threadassoc_remove(void)
{
	ast_callid *pointing;

	pointing = ast_threadstorage_get(&unique_callid, sizeof(*pointing));
	if (!pointing) {
		return -1;
	}

	if (*pointing) {
		*pointing = 0;
		return 0;
	}

	return -1;
}

int ast_callid_threadstorage_auto(ast_callid *callid)
{
	ast_callid tmp;

	/* Start by trying to see if a callid is available from thread storage */
	tmp = ast_read_threadstorage_callid();
	if (tmp) {
		*callid = tmp;
		return 0;
	}

	/* If that failed, try to create a new one and bind it. */
	*callid = ast_create_callid();
	if (*callid) {
		ast_callid_threadassoc_add(*callid);
		return 1;
	}

	/* If neither worked, then something must have gone wrong. */
	return -1;
}

void ast_callid_threadstorage_auto_clean(ast_callid callid, int callid_created)
{
	if (callid && callid_created) {
		/* If the callid was created rather than simply grabbed from the thread storage, we need to unbind here. */
		ast_callid_threadassoc_remove();
	}
}

/*!
 * \brief send log messages to syslog and/or the console
 */
#ifdef GRANDSTREAM_NETWORKS
static void __attribute__((format(printf, 8, 0))) ast_log_full(int level, int sublevel,
	const char *file, int line, const char *function, int index, ast_callid callid,
	const char *fmt, va_list ap)
#else
static void __attribute__((format(printf, 7, 0))) ast_log_full(int level, int sublevel,
	const char *file, int line, const char *function, ast_callid callid,
	const char *fmt, va_list ap)
#endif
{
	struct logmsg *logmsg = NULL;

	if (level == __LOG_VERBOSE && ast_opt_remote && ast_opt_exec) {
		return;
	}

#ifdef GRANDSTREAM_NETWORKS
	int flags = 0;
	switch (ast_log_indexs[index] & (1 << level)) {
	case AST_DEBUG_LOG_ENUM:
		flags = 1;
		break;
	case AST_EVENT_LOG_ENUM:
		flags = 1;
		break;
	case AST_NOTICE_LOG_ENUM:
		flags = 1;
		break;
	case AST_WARNING_LOG_ENUM:
		flags = 1;
		break;
	case AST_ERROR_LOG_ENUM:
		flags = 1;
		break;
	case AST_VERBOSE_LOG_ENUM:
		flags = 1;
		break;
	case AST_DTMF_LOGO_ENUM:
		flags = 1;
		break;
	case AST_ALL_LOG_ENUM:
		flags = 1;
		break;
	default:
		return;
	}

	if (!flags) {
		return;
	}
#endif

	AST_LIST_LOCK(&logmsgs);
	if (logger_queue_size >= logger_queue_limit && !close_logger_thread) {
		logger_messages_discarded++;
		if (!high_water_alert && !close_logger_thread) {
			logmsg = format_log_message(__LOG_WARNING, 0, "logger", 0, "***", 0,
				"Log queue threshold (%d) exceeded.  Discarding new messages.\n", logger_queue_limit);
			AST_LIST_INSERT_TAIL(&logmsgs, logmsg, list);
			high_water_alert = 1;
			ast_cond_signal(&logcond);
		}
		AST_LIST_UNLOCK(&logmsgs);
		return;
	}
	AST_LIST_UNLOCK(&logmsgs);

	logmsg = format_log_message_ap(level, sublevel, file, line, function, callid, fmt, ap);
	if (!logmsg) {
		return;
	}

	/* If the logger thread is active, append it to the tail end of the list - otherwise skip that step */
	if (logthread != AST_PTHREADT_NULL) {
		AST_LIST_LOCK(&logmsgs);
		if (close_logger_thread) {
			/* Logger is either closing or closed.  We cannot log this message. */
			logmsg_free(logmsg);
		} else {
			AST_LIST_INSERT_TAIL(&logmsgs, logmsg, list);
			logger_queue_size++;
			ast_cond_signal(&logcond);
		}
		AST_LIST_UNLOCK(&logmsgs);
	} else {
		logger_print_normal(logmsg);
		logmsg_free(logmsg);
	}
}

#ifdef GRANDSTREAM_NETWORKS
void __ast_log(int level, const char *file, int line, const char *function, int index, const char *fmt, ...)
#else
void ast_log(int level, const char *file, int line, const char *function, const char *fmt, ...)
#endif
{
	va_list ap;

	va_start(ap, fmt);
#ifdef GRANDSTREAM_NETWORKS
	__ast_log_ap(level, file, line, function, index, fmt, ap);
#else
	ast_log_ap(level, file, line, function, fmt, ap);
#endif
	va_end(ap);
}

#ifdef GRANDSTREAM_NETWORKS
void __ast_log_ap(int level, const char *file, int line, const char *function, int index, const char *fmt, va_list ap)
#else
void ast_log_ap(int level, const char *file, int line, const char *function, const char *fmt, va_list ap)
#endif
{
	ast_callid callid;

	callid = ast_read_threadstorage_callid();

	if (level == __LOG_VERBOSE) {
#ifdef GRANDSTREAM_NETWORKS
		__ast_verbose_ap(file, line, function, 0, index, callid, fmt, ap);
#else
		__ast_verbose_ap(file, line, function, 0, callid, fmt, ap);
#endif
	} else {
#ifdef GRANDSTREAM_NETWORKS
		ast_log_full(level, -1, file, line, function, index, callid, fmt, ap);
#else
		ast_log_full(level, -1, file, line, function, callid, fmt, ap);
#endif
	}
}

#ifdef GRANDSTREAM_NETWORKS
void __ast_log_safe(int level, const char *file, int line, const char *function, int index, const char *fmt, ...)
#else
void ast_log_safe(int level, const char *file, int line, const char *function, const char *fmt, ...)
#endif
{
	va_list ap;
	void *recursed = ast_threadstorage_get_ptr(&in_safe_log);
	ast_callid callid;

	if (recursed) {
		return;
	}

	if (ast_threadstorage_set_ptr(&in_safe_log, (void*)1)) {
		/* We've failed to set the flag that protects against
		 * recursion, so bail. */
		return;
	}

	callid = ast_read_threadstorage_callid();

	va_start(ap, fmt);
#ifdef GRANDSTREAM_NETWORKS
	ast_log_full(level, -1, file, line, function, index, callid, fmt, ap);
#else
	ast_log_full(level, -1, file, line, function, callid, fmt, ap);
#endif
	va_end(ap);

	/* Clear flag so the next allocation failure can be logged. */
	ast_threadstorage_set_ptr(&in_safe_log, NULL);
}

#ifdef GRANDSTREAM_NETWORKS
void __ast_log_callid(int level, const char *file, int line, const char *function, int index, ast_callid callid, const char *fmt, ...)
#else
void ast_log_callid(int level, const char *file, int line, const char *function, ast_callid callid, const char *fmt, ...)
#endif
{
	va_list ap;
	va_start(ap, fmt);
#ifdef GRANDSTREAM_NETWORKS
	ast_log_full(level, -1, file, line, function, index, callid, fmt, ap);
#else
	ast_log_full(level, -1, file, line, function, callid, fmt, ap);
#endif
	va_end(ap);
}


void ast_log_backtrace(void)
{
#ifdef HAVE_BKTR
	struct ast_bt *bt;
	int i = 0;
	struct ast_vector_string *strings;

	if (!(bt = ast_bt_create())) {
		ast_log(LOG_WARNING, "Unable to allocate space for backtrace structure\n");
		return;
	}

	if ((strings = ast_bt_get_symbols(bt->addresses, bt->num_frames))) {
		int count = AST_VECTOR_SIZE(strings);
		struct ast_str *buf = ast_str_create(bt->num_frames * 64);

		if (buf) {
			ast_str_append(&buf, 0, "Got %d backtrace record%c\n", count - 3, count - 3 != 1 ? 's' : ' ');
			for (i = 3; i < AST_VECTOR_SIZE(strings); i++) {
				ast_str_append(&buf, 0, "#%2d: %s\n", i - 3, AST_VECTOR_GET(strings, i));
			}
#ifdef GRANDSTREAM_NETWORKS
			ast_log_safe(__LOG_ERROR, "%s\n", ast_str_buffer(buf));
#else
			ast_log_safe(__LOG_ERROR, NULL, 0, NULL, "%s\n", ast_str_buffer(buf));
#endif
			ast_free(buf);
		}

		ast_bt_free_symbols(strings);
	} else {
		ast_log(LOG_ERROR, "Could not allocate memory for backtrace\n");
	}
	ast_bt_destroy(bt);
#else
	ast_log(LOG_WARNING, "Must run configure with '--with-execinfo' for stack backtraces.\n");
#endif /* defined(HAVE_BKTR) */
}

#ifdef GRANDSTREAM_NETWORKS
void __ast_verbose_ap(const char *file, int line, const char *func, int level, int index, ast_callid callid, const char *fmt, va_list ap)
{
	ast_log_full(__LOG_VERBOSE, level, file, line, func, index, callid, fmt, ap);
}
#else
void __ast_verbose_ap(const char *file, int line, const char *func, int level, ast_callid callid, const char *fmt, va_list ap)
{
	ast_log_full(__LOG_VERBOSE, level, file, line, func, callid, fmt, ap);
}
#endif

#ifdef GRANDSTREAM_NETWORKS
void __ast_verbose(const char *file, int line, const char *func, int level, int index, const char *fmt, ...)
#else
void __ast_verbose(const char *file, int line, const char *func, int level, const char *fmt, ...)
#endif
{
	ast_callid callid;
	va_list ap;

	callid = ast_read_threadstorage_callid();

	va_start(ap, fmt);
#ifdef GRANDSTREAM_NETWORKS
	__ast_verbose_ap(file, line, func, level, index, callid, fmt, ap);
#else
	__ast_verbose_ap(file, line, func, level, callid, fmt, ap);
#endif
	va_end(ap);
}

#ifdef GRANDSTREAM_NETWORKS
void __ast_verbose_callid(const char *file, int line, const char *func, int level, int index, ast_callid callid, const char *fmt, ...)
#else
void __ast_verbose_callid(const char *file, int line, const char *func, int level, ast_callid callid, const char *fmt, ...)
#endif
{
	va_list ap;
	va_start(ap, fmt);
#ifdef GRANDSTREAM_NETWORKS
	__ast_verbose_ap(file, line, func, level, index, callid, fmt, ap);
#else
	__ast_verbose_ap(file, line, func, level, callid, fmt, ap);
#endif
	va_end(ap);
}

/*! Console verbosity level node. */
struct verb_console {
	/*! List node link */
	AST_LIST_ENTRY(verb_console) node;
	/*! Console verbosity level. */
	int *level;
};

/*! Registered console verbosity levels */
static AST_RWLIST_HEAD_STATIC(verb_consoles, verb_console);

/*! ast_verb_update() reentrancy protection lock. */
AST_MUTEX_DEFINE_STATIC(verb_update_lock);

void ast_verb_update(void)
{
	struct logchannel *log;
	struct verb_console *console;
	int verb_level;

	ast_mutex_lock(&verb_update_lock);

	AST_RWLIST_RDLOCK(&verb_consoles);

	/* Default to the root console verbosity. */
	verb_level = option_verbose;

	/* Determine max remote console level. */
	AST_LIST_TRAVERSE(&verb_consoles, console, node) {
		if (verb_level < *console->level) {
			verb_level = *console->level;
		}
	}
	AST_RWLIST_UNLOCK(&verb_consoles);

	/* Determine max logger channel level. */
	AST_RWLIST_RDLOCK(&logchannels);
	AST_RWLIST_TRAVERSE(&logchannels, log, list) {
		if (verb_level < log->verbosity) {
			verb_level = log->verbosity;
		}
	}
	AST_RWLIST_UNLOCK(&logchannels);

	ast_verb_sys_level = verb_level;

	ast_mutex_unlock(&verb_update_lock);
}

/*!
 * \internal
 * \brief Unregister a console verbose level.
 *
 * \param console Which console to unregister.
 *
 * \return Nothing
 */
static void verb_console_unregister(struct verb_console *console)
{
	AST_RWLIST_WRLOCK(&verb_consoles);
	console = AST_RWLIST_REMOVE(&verb_consoles, console, node);
	AST_RWLIST_UNLOCK(&verb_consoles);
	if (console) {
		ast_verb_update();
	}
}

static void verb_console_free(void *v_console)
{
	struct verb_console *console = v_console;

	verb_console_unregister(console);
	ast_free(console);
}

/*! Thread specific console verbosity level node. */
AST_THREADSTORAGE_CUSTOM(my_verb_console, NULL, verb_console_free);

void ast_verb_console_register(int *level)
{
	struct verb_console *console;

	console = ast_threadstorage_get(&my_verb_console, sizeof(*console));
	if (!console || !level) {
		return;
	}
	console->level = level;

	AST_RWLIST_WRLOCK(&verb_consoles);
	AST_RWLIST_INSERT_HEAD(&verb_consoles, console, node);
	AST_RWLIST_UNLOCK(&verb_consoles);
	ast_verb_update();
}

void ast_verb_console_unregister(void)
{
	struct verb_console *console;

	console = ast_threadstorage_get(&my_verb_console, sizeof(*console));
	if (!console) {
		return;
	}
	verb_console_unregister(console);
}

int ast_verb_console_get(void)
{
	struct verb_console *console;
	int verb_level;

	console = ast_threadstorage_get(&my_verb_console, sizeof(*console));
	AST_RWLIST_RDLOCK(&verb_consoles);
	if (!console) {
		verb_level = 0;
	} else if (console->level) {
		verb_level = *console->level;
	} else {
		verb_level = option_verbose;
	}
	AST_RWLIST_UNLOCK(&verb_consoles);
	return verb_level;
}

void ast_verb_console_set(int verb_level)
{
	struct verb_console *console;

	console = ast_threadstorage_get(&my_verb_console, sizeof(*console));
	if (!console) {
		return;
	}

	AST_RWLIST_WRLOCK(&verb_consoles);
	if (console->level) {
		*console->level = verb_level;
	} else {
		option_verbose = verb_level;
	}
	AST_RWLIST_UNLOCK(&verb_consoles);
	ast_verb_update();
}

static void update_logchannels(void)
{
	struct logchannel *cur;

	AST_RWLIST_WRLOCK(&logchannels);

	global_logmask = 0;

	AST_RWLIST_TRAVERSE(&logchannels, cur, list) {
		make_components(cur);
		global_logmask |= cur->logmask;
	}

	AST_RWLIST_UNLOCK(&logchannels);
}

int ast_logger_register_level(const char *name)
{
	unsigned int level;
	unsigned int available = 0;

	AST_RWLIST_WRLOCK(&logchannels);

	for (level = 0; level < ARRAY_LEN(levels); level++) {
		if ((level >= 16) && !available && !levels[level]) {
			available = level;
			continue;
		}

		if (levels[level] && !strcasecmp(levels[level], name)) {
			ast_log(LOG_WARNING,
				"Unable to register dynamic logger level '%s': a standard logger level uses that name.\n",
				name);
			AST_RWLIST_UNLOCK(&logchannels);

			return -1;
		}
	}

	if (!available) {
		ast_log(LOG_WARNING,
			"Unable to register dynamic logger level '%s'; maximum number of levels registered.\n",
			name);
		AST_RWLIST_UNLOCK(&logchannels);

		return -1;
	}

	levels[available] = ast_strdup(name);

	AST_RWLIST_UNLOCK(&logchannels);

	ast_debug(1, "Registered dynamic logger level '%s' with index %u.\n", name, available);

	update_logchannels();

	return available;
}

void ast_logger_unregister_level(const char *name)
{
	unsigned int found = 0;
	unsigned int x;

	AST_RWLIST_WRLOCK(&logchannels);

	for (x = 16; x < ARRAY_LEN(levels); x++) {
		if (!levels[x]) {
			continue;
		}

		if (strcasecmp(levels[x], name)) {
			continue;
		}

		found = 1;
		break;
	}

	if (found) {
		/* take this level out of the global_logmask, to ensure that no new log messages
		 * will be queued for it
		 */

		global_logmask &= ~(1 << x);

		ast_free(levels[x]);
		levels[x] = NULL;
		AST_RWLIST_UNLOCK(&logchannels);

		ast_debug(1, "Unregistered dynamic logger level '%s' with index %u.\n", name, x);

		update_logchannels();
	} else {
		AST_RWLIST_UNLOCK(&logchannels);
	}
}

const char *ast_logger_get_dateformat(void)
{
	return dateformat;
}

void ast_logger_set_queue_limit(int queue_limit)
{
	logger_queue_limit = queue_limit;
}

int ast_logger_get_queue_limit(void)
{
	return logger_queue_limit;
}

static int reload_module(void)
{
	return reload_logger(0, NULL);
}

static int unload_module(void)
{
	return 0;
}

static int load_module(void)
{
	return AST_MODULE_LOAD_SUCCESS;
}

/* Logger is initialized separate from the module loader, only reload_module does anything. */
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_GLOBAL_SYMBOLS | AST_MODFLAG_LOAD_ORDER, "Logger",
	.support_level = AST_MODULE_SUPPORT_CORE,
	.load = load_module,
	.unload = unload_module,
	/* This reload does not support realtime so it does not require "extconfig". */
	.reload = reload_module,
	.load_pri = 0,
);
