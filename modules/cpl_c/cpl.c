/*
 * Copyright (C) 2001-2003 FhG Fokus
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 * History:
 * -------
 * 2003-03-11: New module interface (janakj)
 * 2003-03-16: flags export parameter added (janakj)
 * 2003-11-11: build_lump_rpl() removed, add_lump_rpl() has flags (bogdan)
 * 2004-06-06  updated to the new DB api (andrei)
 * 2004-06-14: all global variables merged into cpl_env and cpl_fct;
 *             case_sensitive and realm_prefix added for building AORs - see
 *             build_userhost (bogdan)
 * 2004-10-09: added process_register_norpl to allow register processing
 *             without sending the reply(bogdan) - based on a patch sent by
 *             Christopher Crawford
 */


#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include "../../sr_module.h"
#include "../../mem/shm_mem.h"
#include "../../mem/mem.h"
#include "../../str.h"
#include "../../ut.h"
#include "../../dprint.h"
#include "../../data_lump_rpl.h"
#include "../../pvar.h"
#include "../../parser/parse_uri.h"
#include "../../parser/parse_from.h"
#include "../../parser/parse_content.h"
#include "../../parser/parse_disposition.h"
#include "../../db/db.h"
#include "../../mi/mi.h"
#include "../sl/sl_api.h"
#include "cpl_run.h"
#include "cpl_env.h"
#include "cpl_db.h"
#include "cpl_loader.h"
#include "cpl_parser.h"
#include "cpl_nonsig.h"
#include "loc_set.h"


#define MAX_PROXY_RECURSE  10
#define MAX_USERHOST_LEN    256


/* modules param variables */
static str db_url          = {NULL, 0}; /* database url */
static str db_table        = str_init("cpl");  /* database table */
static char *dtd_file      = 0;  /* name of the DTD file for CPL parser */
static char *lookup_domain = 0;
static char* proxy_route   = NULL;


struct cpl_enviroment    cpl_env = {
		0, /* no cpl logging */
		0, /* recurse proxy level is 0 */
		0, /* no script route to be run before proxy */
		0, /* user part is not case sensitive */
		{0,0},   /* no domain prefix to be ignored */
		{-1,-1}, /* communication pipe to aux_process */
		{0,0},   /* original TZ \0 terminated "TZ=value" format */
		0,   /* udomain */
		0,   /* no branches on lookup */
		0    /* use_domain */
};

struct cpl_functions  cpl_fct;
static str cpl_ok_rpl = str_init("OK");


static int cpl_invoke_script (struct sip_msg* msg, void* type, void* mode);
static int w_process_register(struct sip_msg* msg, char* str, char* str2);
static int w_process_register_norpl(struct sip_msg* msg, char* str,char* str2);
static int cpl_process_register(struct sip_msg* msg, int no_rpl);
static int fixup_cpl_run_script_1(void** param);
static int fixup_cpl_run_script_2(void** param);
static int cpl_init(void);
static int cpl_child_init(int rank);
static int cpl_exit(void);
static void cpl_process(int rank);


/*
 * Exported processes
 */
static const proc_export_t cpl_procs[] = {
	{"CPL Aux",  0,  0,  cpl_process, 1, 0 },
	{0,0,0,0,0,0}
};


/*
 * Exported functions
 */

static const cmd_export_t cmds[] = {
	{"cpl_run_script", (cmd_function)cpl_invoke_script, {
		{CMD_PARAM_STR,fixup_cpl_run_script_1,0},
		{CMD_PARAM_STR,fixup_cpl_run_script_2,0}, {0,0,0}},
		REQUEST_ROUTE},
	{"cpl_process_register",(cmd_function)w_process_register, {{0,0,0}},
		REQUEST_ROUTE},
	{"cpl_process_register_norpl",(cmd_function)w_process_register_norpl, {{0,0,0}},
		REQUEST_ROUTE},
	{0,0,{{0,0,0}},0}
};

/*
 * Exported parameters
 */
static const param_export_t params[] = {
	{"db_url",         STR_PARAM, &db_url.s                          },
	{"db_table",       STR_PARAM, &db_table.s                        },
	{"cpl_dtd_file",   STR_PARAM, &dtd_file                          },
	{"proxy_recurse",  INT_PARAM, &cpl_env.proxy_recurse             },
	{"proxy_route",    STR_PARAM, &proxy_route                       },
	{"log_dir",        STR_PARAM, &cpl_env.log_dir                   },
	{"case_sensitive", INT_PARAM, &cpl_env.case_sensitive            },
	{"realm_prefix",   STR_PARAM, &cpl_env.realm_prefix.s            },
	{"lookup_domain",  STR_PARAM, &lookup_domain                     },
	{"lookup_append_branches", INT_PARAM, &cpl_env.lu_append_branches},
	{"username_column",STR_PARAM, &cpl_username_col.s                },
	{"domain_column",  STR_PARAM, &cpl_domain_col.s                  },
	{"cpl_xml_column", STR_PARAM, &cpl_xml_col.s                     },
	{"cpl_bin_column", STR_PARAM, &cpl_bin_col.s                     },
	{"use_domain",     INT_PARAM, &cpl_env.use_domain                },
	{0, 0, 0}
};


/*
 * Exported MI functions
 */
static const mi_export_t mi_cmds[] = {
	{ "LOAD_CPL", 0, 0, 0, {
		{mi_cpl_load, {"username", "cpl_filename", 0}},
		{EMPTY_MI_RECIPE}}
	},
	{ "REMOVE_CPL", 0, 0, 0, {
		{mi_cpl_remove, {"username", 0}},
		{EMPTY_MI_RECIPE}}
	},
	{ "GET_CPL", 0, 0, 0, {
		{mi_cpl_get, {"username", 0}},
		{EMPTY_MI_RECIPE}}
	},
	{EMPTY_MI_EXPORT}
};

static module_dependency_t *get_deps_lookup_domain(const param_export_t *param)
{
	char *domain = *(char **)param->param_pointer;
	if (!domain || strlen(domain) == 0)
		return NULL;

	return alloc_module_dep(MOD_TYPE_DEFAULT, "usrloc", DEP_ABORT);
}

static const dep_export_t deps = {
	{ /* OpenSIPS module dependencies */
		{ MOD_TYPE_DEFAULT, "tm",        DEP_ABORT },
		{ MOD_TYPE_DEFAULT, "signaling", DEP_ABORT },
		{ MOD_TYPE_SQLDB,   NULL,        DEP_ABORT },
		{ MOD_TYPE_NULL, NULL, 0 },
	},
	{ /* modparam dependencies */
		{ "lookup_domain", get_deps_lookup_domain },
		{ NULL, NULL },
	},
};

struct module_exports exports = {
	"cpl_c",
	MOD_TYPE_DEFAULT,/* class of this module */
	MODULE_VERSION,  /* module version */
	DEFAULT_DLFLAGS, /* dlopen flags */
	0,				 /* load function */
	&deps,           /* OpenSIPS module dependencies */
	cmds,     /* Exported functions */
	0,        /* Exported async functions */
	params,   /* Exported parameters */
	0,        /* exported statistics */
	mi_cmds,  /* exported MI functions */
	0,        /* exported pseudo-variables */
	0,		  /* exported transformations */
	cpl_procs,/* extra processes */
	0,        /* Module pre-initialization function */
	cpl_init, /* Module initialization function */
	(response_function) 0,
	(destroy_function) cpl_exit,
	(child_init_function) cpl_child_init, /* per-child init function */
	0         /* reload confirm function */
};



static int fixup_cpl_run_script_1(void** param)
{
	long flag;
	static str incoming_s = str_init("incoming");
	static str outgoing_s = str_init("outgoing");

	if (!str_strcasecmp( &incoming_s, (str*)*param))
		flag = CPL_RUN_INCOMING;
	else if (!str_strcasecmp( &outgoing_s, (str*)*param))
		flag = CPL_RUN_OUTGOING;
	else {
		LM_ERR("script directive \"%.*s\" unknown!\n",
			((str*)*param)->len, ((str*)*param)->s);
		return E_UNSPEC;
	}

	*param=(void*)flag;
	return 0;
}

static int fixup_cpl_run_script_2(void** param)
{
	long flag;
	static str is_stateless_s = str_init("is_stateless");
	static str is_stateful_s = str_init("is_stateful");
	static str force_stateful_s = str_init("force_stateful");

	if ( !str_strcasecmp(&is_stateless_s, (str*)*param) ) {
		flag = 0;
	} else if ( !str_strcasecmp(&is_stateful_s, (str*)*param) ) {
		flag = CPL_IS_STATEFUL;
	} else if ( !str_strcasecmp(&force_stateful_s, (str*)*param) ) {
		flag = CPL_FORCE_STATEFUL;
	} else {
		LM_ERR("flag \"%.*s\" (second param) unknown!\n",
			((str*)*param)->len, ((str*)*param)->s);
		return E_UNSPEC;
	}

	*param=(void*)flag;

	return 0;
}


static int cpl_init(void)
{
	bind_usrloc_t bind_usrloc;
	struct stat   stat_t;
	char *ptr;
	int val;

	init_db_url( db_url , 0 /*cannot be null*/);
	db_table.len = strlen(db_table.s);
	cpl_username_col.len = strlen(cpl_username_col.s);
	cpl_domain_col.len = strlen(cpl_domain_col.s);
	cpl_xml_col.len = strlen(cpl_xml_col.s);
	cpl_bin_col.len = strlen(cpl_bin_col.s);

	LM_INFO("initializing...\n");

	if (proxy_route && proxy_route[0]) {
		cpl_env.proxy_route = get_script_route_ID_by_name( proxy_route,
				sroutes->request, RT_NO);
		if (cpl_env.proxy_route==-1) {
			LM_ERR("route <%s> does not exist\n",proxy_route);
			return -1;
		}
	}

	if (cpl_env.proxy_recurse>MAX_PROXY_RECURSE) {
		LM_CRIT("value of proxy_recurse param (%d) exceeds "
			"the maximum safety value (%d)\n",
			cpl_env.proxy_recurse,MAX_PROXY_RECURSE);
		goto error;
	}

	if (dtd_file==0) {
		LM_CRIT("mandatory parameter \"cpl_dtd_file\" found empty\n");
		goto error;
	} else {
		/* check if the dtd file exists */
		if (stat( dtd_file, &stat_t)==-1) {
			LM_ERR("checking file \"%s\" status failed; stat returned %s\n",
					dtd_file,strerror(errno));
			goto error;
		}
		if ( !S_ISREG( stat_t.st_mode ) ) {
			LM_ERR("dir \"%s\" is not a regular file!\n", dtd_file);
			goto error;
		}
		if (access( dtd_file, R_OK )==-1) {
			LM_ERR("checking file \"%s\" for permissions "
				"failed; access returned %s\n",dtd_file,strerror(errno));
			goto error;
		}
	}

	if (cpl_env.log_dir==0) {
		LM_INFO("log_dir param found empty -> logging disabled!\n");
	} else {
		if ( strlen(cpl_env.log_dir)>MAX_LOG_DIR_SIZE ) {
			LM_ERR("dir \"%s\" has a too long name :-(!\n",	cpl_env.log_dir);
			goto error;
		}
		/* check if the dir exists */
		if (stat( cpl_env.log_dir, &stat_t)==-1) {
			LM_ERR("checking dir \"%s\" status failed;"
				" stat returned %s\n",cpl_env.log_dir,strerror(errno));
			goto error;
		}
		if ( !S_ISDIR( stat_t.st_mode ) ) {
			LM_ERR("dir \"%s\" is not a directory!\n", cpl_env.log_dir);
			goto error;
		}
		if (access( cpl_env.log_dir, R_OK|W_OK )==-1) {
			LM_ERR("checking dir \"%s\" for permissions failed; access "
					"returned %s\n", cpl_env.log_dir, strerror(errno));
			goto error;
		}
	}

	/* bind to the mysql module */
	if (cpl_db_bind(&db_url, &db_table)<0) goto error;

	/* load TM API */
	if (load_tm_api(&cpl_fct.tmb)!=0) {
		LM_ERR("can't load TM API\n");
		goto error;
	}
	/* load SIGNALING API */
	if(load_sig_api(&cpl_fct.sigb)< 0) {
		LM_ERR("can't load signaling functions\n");
		return -1;
	}


	/* bind to usrloc module if requested */
	if (lookup_domain) {
		/* import all usrloc functions */
		bind_usrloc = (bind_usrloc_t)find_export("ul_bind_usrloc", 0);
		if (!bind_usrloc) {
			LM_ERR("can't bind usrloc\n");
			goto error;
		}
		if (bind_usrloc( &(cpl_fct.ulb) ) < 0) {
			LM_ERR("importing usrloc failed\n");
			goto error;
		}
		/* convert lookup_domain from char* to udomain_t* pointer */
		if (cpl_fct.ulb.register_udomain( lookup_domain, &cpl_env.lu_domain)
		< 0) {
			LM_ERR("failed to register domain <%s>\n",lookup_domain);
			goto error;
		}
	} else {
		LM_NOTICE("no lookup_domain given -> disable lookup node\n");
	}

	/* build a pipe for sending commands to aux process */
	if ( pipe( cpl_env.cmd_pipe )==-1 ) {
		LM_CRIT("cannot create command pipe: %s!\n", strerror(errno) );
		goto error;
	}
	/* set the writing non blocking */
	if ( (val=fcntl(cpl_env.cmd_pipe[1], F_GETFL, 0))<0 ) {
		LM_ERR("getting flags from pipe[1] failed: fcntl said %s!\n",
				strerror(errno));
		goto error;
	}
	if ( fcntl(cpl_env.cmd_pipe[1], F_SETFL, val|O_NONBLOCK) ) {
		LM_ERR("setting flags to pipe[1] failed: fcntl said %s!\n",
				strerror(errno));
		goto error;
	}

	/* init the CPL parser */
	if (init_CPL_parser( dtd_file )!=1 ) {
		LM_ERR("init_CPL_parser failed!\n");
		goto error;
	}

	/* make a copy of the original TZ env. variable */
	ptr = getenv("TZ");
	cpl_env.orig_tz.len = 3/*"TZ="*/ + (ptr?(strlen(ptr)+1):0);
	if ( (cpl_env.orig_tz.s=shm_malloc( cpl_env.orig_tz.len ))==0 ) {
		LM_ERR("no more shm mem. for saving TZ!\n");
		goto error;
	}
	memcpy(cpl_env.orig_tz.s,"TZ=",3);
	if (ptr)
		strcpy(cpl_env.orig_tz.s+3,ptr);

	/* convert realm_prefix from string null terminated to str */
	if (cpl_env.realm_prefix.s) {
		cpl_env.realm_prefix.len = strlen(cpl_env.realm_prefix.s);
		/* convert the realm_prefix to lower cases */
		strlower( &cpl_env.realm_prefix );
	}

	return 0;
error:
	return -1;
}



static int cpl_child_init(int rank)
{
	return cpl_db_init(&db_url, &db_table);
}


static void cpl_process(int rank)
{
	cpl_aux_process( cpl_env.cmd_pipe[0], cpl_env.log_dir);
	exit(-1);
}


static int cpl_exit(void)
{
	/* free the TZ orig */
	if (cpl_env.orig_tz.s)
		shm_free(cpl_env.orig_tz.s);

	return 0;
}



static inline int build_user_AOR(str *username, str *domain, str *uh, int sip)
{
	unsigned char do_strip;
	char *p;
	int i;

	/* calculate the len (without terminating \0) */
	uh->len = 4*(sip!=0) + username->len;
	do_strip = 0;

	if (sip || cpl_env.use_domain) {
		/* do we need to strip realm prefix? */
		if (cpl_env.realm_prefix.len && cpl_env.realm_prefix.len<domain->len){
			for( i=cpl_env.realm_prefix.len-1 ; i>=0 ; i-- )
				if ( cpl_env.realm_prefix.s[i]!=tolower(domain->s[i]) )
					break;
			if (i==-1)
				do_strip = 1;
		}
		uh->len += 1 + domain->len - do_strip*cpl_env.realm_prefix.len;
	}

	uh->s = (char*)shm_malloc( uh->len + 1 );
	if (!uh->s) {
		LM_ERR("no more shm memory.\n");
		return -1;
	}

	/* build user@host */
	p = uh->s;
	if (sip) {
		memcpy( uh->s, "sip:", 4);
		p += 4;
	}
	/* user part */
	if (cpl_env.case_sensitive) {
		memcpy( p, username->s, username->len);
		p += username->len;
	} else {
		for(i=0;i<username->len;i++)
			*(p++) = tolower(username->s[i]);
	}
	if (sip || cpl_env.use_domain) {
		*(p++) = '@';
		/* host part in lower cases */
		for( i=do_strip*cpl_env.realm_prefix.len ; i< domain->len ; i++ )
			*(p++) = tolower(domain->s[i]);
	}
	*(p++) = 0;

	/* sanity check */
	if (p-uh->s!=uh->len+1) {
		LM_CRIT("buffer overflow l=%d,w=%ld\n", uh->len,(long)(p-uh->s));
		return -1;
	}
	return 0;
}



static inline int get_dest_user(struct sip_msg *msg, str *username, str *domain)
{
	struct sip_uri uri;

	/*  get the user_name from new_uri/RURI/To */
	LM_DBG("trying to get user from new_uri\n");
	if ( !msg->new_uri.s || parse_uri( msg->new_uri.s,msg->new_uri.len,&uri)<0
	|| !uri.user.len )
	{
		LM_DBG("trying to get user from R_uri\n");
		if ( parse_uri( msg->first_line.u.request.uri.s,
		msg->first_line.u.request.uri.len ,&uri)==-1 || !uri.user.len )
		{
			LM_DBG("trying to get user from To\n");
			if ( (!msg->to&&((parse_headers(msg,HDR_TO_F,0)==-1)||!msg->to))||
			parse_uri( get_to(msg)->uri.s, get_to(msg)->uri.len, &uri)<0
			|| !uri.user.len)
			{
				LM_ERR("unable to extract user name from RURI or To header!\n");
				return -1;
			}
		}
	}
	*username = uri.user;
	*domain = uri.host;
	return 0;
}



static inline int get_orig_user(struct sip_msg *msg, str *username, str *domain)
{
	struct to_body *from;
	struct sip_uri uri;

	/* if it's outgoing -> get the user_name from From */
	/* parsing from header */
	LM_DBG("trying to get user from From\n");
	if ( parse_from_header( msg )==-1 ) {
		LM_ERR("unable to extract URI from FROM header\n");
		return -1;
	}
	from = (struct to_body*)msg->from->parsed;
	/* parse the extracted uri from From */
	if (parse_uri( from->uri.s, from->uri.len, &uri)||!uri.user.len) {
		LM_ERR("unable to extract user name from URI (From header)\n");
		return -1;
	}
	*username = uri.user;
	*domain = uri.host;
	return 0;
}



/* Params:
 *   str1 - as unsigned int - can be CPL_RUN_INCOMING or CPL_RUN_OUTGOING
 *   str2 - as unsigned int - flags regarding state(less)|(ful)
 */
static int cpl_invoke_script(struct sip_msg* msg, void* type, void* mode)
{
	struct cpl_interpreter  *cpl_intr;
	str  username = {0,0};
	str  domain = {0,0};
	str  loc;
	str  script;

	/* get the user_name */
	if ( ((unsigned long)type)&CPL_RUN_INCOMING ) {
		/* if it's incoming -> get the destination user name */
		if (get_dest_user( msg, &username, &domain)==-1)
			goto error0;
	} else {
		/* if it's outgoing -> get the origin user name */
		if (get_orig_user( msg, &username, &domain)==-1)
			goto error0;
	}

	/* get the script for this user */
	if (get_user_script(&username, cpl_env.use_domain?&domain:0,
	&script, &cpl_bin_col)==-1)
		goto error0;

	/* has the user a non-empty script? if not, return normally, allowing the
	 * script execution to continue */
	if ( !script.s || !script.len )
		return 2;

	/* build a new script interpreter */
	if ( (cpl_intr=new_cpl_interpreter(msg,&script))==0 )
		goto error1;
	/* set the flags */
	cpl_intr->flags =(unsigned int)((unsigned long)type)|((unsigned long)mode);
	/* build user AOR */
	if (build_user_AOR( &username, &domain, &(cpl_intr->user), 0)!=0 )
		goto error2;
	/* for OUTGOING we need also the destination user for init. with him
	 * the location set */
	if ( ((unsigned long)type)&CPL_RUN_OUTGOING ) {
		/* build user initial location -> get the destination user name */
		if (get_dest_user( msg, &username, &domain)==-1)
			goto error2;
		if (build_user_AOR( &username, &domain, &loc, 1)!=0 )
			goto error2;
		if (add_location( &(cpl_intr->loc_set), &loc, 0, 10,CPL_LOC_DUPL)==-1){
			shm_free(loc.s);
			goto error2;
		}
		shm_free(loc.s);
	}

	/* run the script */
	switch (cpl_run_script( cpl_intr )) {
		case SCRIPT_DEFAULT:
			if ( cpl_intr->flags&CPL_DO_NOT_FREE )
				cpl_intr->flags |= CPL_ENDED;
			else
				free_cpl_interpreter( cpl_intr );
			return 1; /* execution of ser's script will continue */
		case SCRIPT_END:
			if ( cpl_intr->flags&CPL_DO_NOT_FREE )
				cpl_intr->flags |= CPL_ENDED;
			else
				free_cpl_interpreter( cpl_intr );
		case SCRIPT_TO_BE_CONTINUED:
			return 0; /* break the SER script */
		case SCRIPT_RUN_ERROR:
		case SCRIPT_FORMAT_ERROR:
			goto error2;
	}

	return 1;
error2:
	if ( cpl_intr->flags&CPL_DO_NOT_FREE )
		cpl_intr->flags |= CPL_ENDED;
	else
		free_cpl_interpreter( cpl_intr );
	return -1;
error1:
	shm_free(script.s);
error0:
	return -1;
}



#define CPL_SCRIPT          "script"
#define CPL_SCRIPT_LEN      (sizeof(CPL_SCRIPT)-1)
#define ACTION_PARAM        "action"
#define ACTION_PARAM_LEN    (sizeof(ACTION_PARAM)-1)
#define STORE_ACTION        "store"
#define STORE_ACTION_LEN    (sizeof(STORE_ACTION)-1)
#define REMOVE_ACTION       "remove"
#define REMOVE_ACTION_LEN   (sizeof(REMOVE_ACTION)-1)

#define REMOVE_SCRIPT       0xcaca
#define STORE_SCRIPT        0xbebe

#define CONTENT_TYPE_HDR      ("Content-Type: application/cpl-xml"CRLF)
#define CONTENT_TYPE_HDR_LEN  (sizeof(CONTENT_TYPE_HDR)-1)

struct cpl_error {
	int   err_code;
	str   err_msg;
};

static struct cpl_error bad_req = {400,str_init("Bad request")};
static struct cpl_error intern_err = {500,str_init("Internal server error")};
static struct cpl_error bad_cpl = {400,str_init("Bad CPL script")};

static struct cpl_error *cpl_err = &bad_req;


static inline int do_script_action(struct sip_msg *msg, int action)
{
	str  body = {0,0};
	str  bin  = {0,0};
	str  log  = {0,0};
	str  username = {0,0};
	str  domain   = {0,0};

	if ( get_body(msg, &body)!=0 ) {
		LM_ERR("failed to look for body!\n");
		goto error;
	}

	/* get the user name */
	if (get_dest_user( msg, &username, &domain)==-1)
		goto error;

	/* we have the script and the user */
	switch (action) {
		case STORE_SCRIPT :
			/* check the len -> it must not be 0 */
			if (body.len==0) {
				LM_ERR("0 content-len found for store\n");
				goto error_1;
			}
			/* now compile the script and place it into database */
			/* get the binary coding for the XML file */
			if ( encodeCPL( &body, &bin, &log)!=1) {
				cpl_err = &bad_cpl;
				goto error_1;
			}

			/* write both the XML and binary formats into database */
			if (write_to_db( &username, cpl_env.use_domain?&domain:0,
			&body,&bin)!=1) {
				cpl_err = &intern_err;
				goto error_1;
			}
			break;
		case REMOVE_SCRIPT:
			/* check the len -> it must be 0 */
			if (body.len!=0) {
				LM_ERR("non-0 content-len found for remove\n");
				goto error_1;
			}
			/* remove the script for the user */
			if (rmv_from_db( &username, cpl_env.use_domain?&domain:0)!=1) {
				cpl_err = &intern_err;
				goto error_1;
			}
			break;
	}

	if (log.s) pkg_free( log.s );
	return 0;
error_1:
	if (log.s) pkg_free( log.s );
error:
	return -1;
}



static inline int do_script_download(struct sip_msg *msg)
{
	str username  = {0,0};
	str domain = {0,0};
	str script = {0,0};

	/* get the destination user name */
	if (get_dest_user( msg, &username, &domain)!=0)
		goto error;

	/* get the user's xml script from the database */
	if (get_user_script( &username, cpl_env.use_domain?&domain:0,
	&script, &cpl_xml_col)==-1)
		goto error;

	/* add a lump with content-type hdr */
	if (add_lump_rpl( msg, CONTENT_TYPE_HDR, CONTENT_TYPE_HDR_LEN,
	LUMP_RPL_HDR)==0) {
		LM_ERR("cannot build hdr lump\n");
		cpl_err = &intern_err;
		goto error;
	}

	if (script.s!=0) {
		/* user has a script -> add a body lump */
		if ( add_lump_rpl( msg, script.s, script.len, LUMP_RPL_BODY)==0) {
			LM_ERR("cannot build body lump\n");
			cpl_err = &intern_err;
			goto error;
		}
		/* build_lump_rpl duplicates the added text, so free the original */
		shm_free( script.s );
	}

	return 0;
error:
	if (script.s)
		shm_free(script.s);
	return -1;
}



static int w_process_register(struct sip_msg* msg, char* str, char* str2)
{
	return cpl_process_register( msg, 0);
}



static int w_process_register_norpl(struct sip_msg* msg, char* str,char* str2)
{
	return cpl_process_register( msg, 1);
}



static int cpl_process_register(struct sip_msg* msg, int no_rpl)
{
	struct disposition *disp;
	struct disposition_param *param;
	int  ret;
	int  mime;
	int  *mimes;

	/* make sure that is a REGISTER ??? */

	/* here should be the CONTACT- hack */

	/* is there a CONTENT-TYPE hdr ? */
	mime = parse_content_type_hdr( msg );
	if (mime==-1)
		goto error;

	/* check the mime type */
	LM_DBG("Content-Type mime found %u, %u\n",
		mime>>16,mime&0x00ff);
	if ( mime && mime==(TYPE_APPLICATION<<16)+SUBTYPE_CPLXML ) {
		/* can be an upload or remove -> check for the content-purpose and
		 * content-action headers */
		LM_DBG("carrying CPL -> look at Content-Disposition\n");
		if (parse_content_disposition( msg )!=0) {
			LM_ERR("Content-Disposition missing or corrupted\n");
			goto error;
		}
		disp = get_content_disposition(msg);
		print_disposition( disp ); /* just for DEBUG */
		/* check if the type of disposition is SCRIPT */
		if (disp->type.len!=CPL_SCRIPT_LEN ||
		strncasecmp(disp->type.s,CPL_SCRIPT,CPL_SCRIPT_LEN) ) {
			LM_ERR("bogus message - Content-Type"
				"says CPL_SCRIPT, but Content-Disposition something else\n");
			goto error;
		}
		/* disposition type is OK -> look for action parameter */
		for(param=disp->params;param;param=param->next) {
			if (param->name.len==ACTION_PARAM_LEN &&
			!strncasecmp(param->name.s,ACTION_PARAM,ACTION_PARAM_LEN))
				break;
		}
		if (param==0) {
			LM_ERR("bogus message - "
				"Content-Disposition has no action param\n");
			goto error;
		}
		/* action param found -> check its value: store or remove */
		if (param->body.len==STORE_ACTION_LEN &&
		!strncasecmp( param->body.s, STORE_ACTION, STORE_ACTION_LEN)) {
			/* it's a store action -> get the script from body message and store
			 * it into database (CPL and BINARY format) */
			if (do_script_action( msg, STORE_SCRIPT)==-1)
				goto error;
		} else
		if (param->body.len==REMOVE_ACTION_LEN &&
		!strncasecmp( param->body.s, REMOVE_ACTION, REMOVE_ACTION_LEN)) {
			/* it's a remove action -> remove the script from database */
			if (do_script_action( msg, REMOVE_SCRIPT)==-1)
				goto error;
		} else {
			LM_ERR("unknown action <%.*s>\n",
				param->body.len,param->body.s);
			goto error;
		}

		/* do I have to send to reply? */
		if (no_rpl)
			goto resume_script;

		/* send a 200 OK reply back */
		cpl_fct.sigb.reply( msg, 200, &cpl_ok_rpl,NULL);
		/* I send the reply and I don't want to return to script execution, so
		 * I return 0 to do break */
		goto stop_script;
	}

	/* is there an ACCEPT hdr ? */
	if ( (ret=parse_accept_hdr(msg))<0)
		goto error;
	if (ret==0 || (mimes=get_accept(msg))==0 )
		/* accept header not present or no mimes found */
		goto resume_script;

	/* looks if the REGISTER accepts cpl-xml or * */
	while (*mimes) {
		LM_DBG("accept mime found %u, %u\n",
			(*mimes)>>16,(*mimes)&0x00ff);
		if (*mimes==(TYPE_ALL<<16)+SUBTYPE_ALL ||
		*mimes==(TYPE_APPLICATION<<16)+SUBTYPE_CPLXML )
			break;
		mimes++;
	}
	if (*mimes==0)
		/* no accept mime that matched cpl */
		goto resume_script;

	/* get the user name from msg, retrieve the script from db
	 * and appended to reply */
	if (do_script_download( msg )==-1)
		goto error;

	/* do I have to send to reply? */
	if (no_rpl)
		goto resume_script;

	/* send a 200 OK reply back */
	cpl_fct.sigb.reply( msg, 200, &cpl_ok_rpl,NULL);

stop_script:
	return 0;
resume_script:
	return 1;
error:
	/* send a error reply back */
	cpl_fct.sigb.reply( msg, cpl_err->err_code, &cpl_err->err_msg, NULL);
	/* I don't want to return to script execution, so I return 0 to do break */
	return 0;
}
