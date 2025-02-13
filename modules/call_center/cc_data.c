/*
 * call center module - call queuing and distributio
 *
 * Copyright (C) 2014-2020 OpenSIPS Solutions
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */



#include <stdio.h>

#include "../../mem/shm_mem.h"
#include "../../parser/parse_uri.h"
#include "../../locking.h"
#include "../../ut.h"
#include "../../trim.h"
#include "../../timer.h"
#include "../../evi/evi.h"
#include "../b2b_logic/b2b_load.h"
#include "cc_data.h"


/* b2b logic API */
extern b2bl_api_t b2b_api;

extern unsigned int wrapup_time;
extern int msrp_dispatch_policy;

/* events */
static str agent_event = str_init("E_CALLCENTER_AGENT_REPORT");
static event_id_t agent_evi_id;

static void free_cc_flow( struct cc_flow *flow);
static void free_cc_agent( struct cc_agent *agent);
unsigned long cc_flow_free_agents( void *flow);


unsigned int get_skill_id(struct cc_data *data, str *name)
{
	struct cc_skill *skill;

	/* search to see if exists */
	for ( skill=data->skills_map ; skill ; skill=skill->next ) {
		if ( (skill->name.len==name->len) &&
		(memcmp(skill->name.s,name->s,name->len)==0) )
			return skill->id;
	}

	/* none found, allocate a new one */
	skill = (struct cc_skill*)shm_malloc( sizeof(struct cc_skill)+name->len );
	if (skill==NULL) {
		LM_ERR("no enough shm mem for a new skill map\n");
		return 0;
	}

	skill->is_new = 1;
	skill->name.s = (char*)(skill+1);
	skill->name.len = name->len;
	memcpy( skill->name.s , name->s, name->len);

	skill->id = ++(data->last_skill_id);

	/* link it */
	skill->next = data->skills_map;
	data->skills_map = skill;

	return skill->id;
}


str* get_skill_by_id(struct cc_data *data, unsigned int id)
{
	struct cc_skill *skill;

	/* search to see if exists */
	for ( skill=data->skills_map ; skill ; skill=skill->next ) {
		if (skill->id==id)
			return &skill->name;
	}

	return NULL;
}


void free_cc_skill(struct cc_skill *skill)
{
	shm_free(skill);
}


struct cc_data* init_cc_data(void)
{
	struct cc_data *data;

	data = (struct cc_data*) shm_malloc( sizeof(struct cc_data) );
	if (data==NULL) {
		LM_ERR("failed to allocate shm mem\n");
		return NULL;
	}
	memset( data, 0, sizeof(struct cc_data));

	/* create & init lock */
	if ( (data->lock=lock_alloc())==0) {
		LM_CRIT("failed to alloc lock\n");
		goto error;
	}
	if (lock_init(data->lock)==0 ) {
		LM_CRIT("failed to init lock\n");
		goto error;
	}

	/* set of locks for calls */
	if ( (data->call_locks=lock_set_alloc(512))==0) {
		LM_CRIT("failed to alloc set of call locks\n");
		goto error;
	}
	if (lock_set_init(data->call_locks)==0 ) {
		LM_CRIT("failed to init set of call locks\n");
		goto error;
	}

	agent_evi_id = evi_publish_event(agent_event);
	if (agent_evi_id == EVI_ERROR) {
		LM_ERR("cannot register %.*s event\n", agent_event.len, agent_event.s);
		goto error;
	}

	return data;
error:
	free_cc_data(data);
	return NULL;
}


void free_cc_data(struct cc_data *data)
{
	struct cc_flow *flow, *f_flow;
	struct cc_agent *agent,*f_agent;
	int i;

	if (data) {
		/* lock */
		if (data->lock) {
			lock_destroy( data->lock );
			lock_dealloc( data->lock );
		}
		if (data->call_locks) {
			lock_set_destroy( data->call_locks );
			lock_set_dealloc( data->call_locks );
		}
		/* flows */
		for( flow=data->flows ; flow ; ) {
			f_flow = flow;
			flow = flow->next;
			free_cc_flow( f_flow );
		}
		/* agents */
		for(i = 0; i< 2; i++) {
			for( agent=data->agents[i] ; agent ; ) {
				f_agent = agent;
				agent = agent->next;
				free_cc_agent( f_agent );
			}
		}
		shm_free(data);
	}
}


struct cc_flow *get_flow_by_name(struct cc_data *data, str *name)
{
	struct cc_flow *flow;

	for( flow=data->flows ; flow ; flow=flow->next ) {
		if (name->len==flow->id.len && 
		memcmp( name->s, flow->id.s, name->len)==0)
			return flow;
	}

	return NULL;
}


struct cc_agent* get_agent_by_name(struct cc_data *data, str *name, struct cc_agent **prev_agent)
{
	struct cc_agent *agent;
	int i;

	for(i = 0; i< 2; i++) {
		*prev_agent = data->agents[i];
		for( agent=data->agents[i] ; agent ; agent=agent->next ) {
			if (name->len==agent->id.len && 
				memcmp( name->s, agent->id.s, name->len)==0)
				return agent;
			*prev_agent = agent;
		}
	}
	return NULL;
}


void update_cc_flow_awt(struct cc_flow *flow, unsigned long duration)
{
	flow->avg_waittime_no ++;
	flow->avg_waittime =
		( ((float)duration + (flow->avg_waittime * (float)(flow->avg_waittime_no-1))) ) /
		(float)flow->avg_waittime_no;
}

#ifdef STATISTICS
static unsigned long cc_flow_get_etw( void *flow_p)
{
	struct cc_flow *flow = (struct cc_flow*)flow_p;

	return flow->logged_agents? (unsigned long)(( flow->avg_call_duration * get_stat_val(flow->st_queued_calls) ) /
		(float)flow->logged_agents):0;
}


static unsigned long cc_flow_get_awt( void *flow_p)
{
	return (unsigned long)((struct cc_flow*)flow_p)->avg_waittime;
}


static unsigned long cc_flow_get_load( void *flow_p)
{
	struct cc_flow *flow = (struct cc_flow*)flow_p;

	return (flow->logged_agents==0) ? 0 :
	(100*(get_stat_val(flow->st_onhold_calls)+flow->logged_agents-cc_flow_free_agents(flow))/flow->logged_agents);
}
#endif


int add_cc_flow( struct cc_data *data, str *id, int priority, str *skill,
		str *cid, int max_wrapup, int diss_hangup, int diss_ewt_th, 
		int diss_qsize_th, int diss_onhold_th, str *recordings )
{
	struct cc_flow *flow, *prev_flow;
	unsigned int i;
	unsigned int skill_id;
#ifdef STATISTICS
	char *name;
	str s;
#endif

	/* is the flow a new one? - search by ID */
	flow = get_flow_by_name( data, id);

	if (flow==NULL) {
		/* new flow -> create and populate one */
		flow = (struct cc_flow*)shm_malloc(sizeof(struct cc_flow)+id->len);
		if (flow==NULL) {
			LM_ERR("not enough shmem for a new flow\n");
			goto error;
		}
		memset( flow, 0, sizeof(struct cc_flow) );
		/* id */
		flow->id.s = (char*)(flow+1);
		memcpy( flow->id.s, id->s, id->len);
		flow->id.len = id->len;
		/* priority */
		flow->priority = priority;
		/* max wrapup time */
		flow->max_wrapup = max_wrapup;
		/* dissuading related options */
		flow->diss_hangup = diss_hangup;
		flow->diss_ewt_th = diss_ewt_th;
		flow->diss_qsize_th = diss_qsize_th;
		flow->diss_onhold_th = diss_onhold_th;
		/* skill */
		flow->skill = get_skill_id( data, skill );
		if (flow->skill==0) {
			LM_ERR("cannot get skill id\n");
			goto error;
		}
		/* cid */
		if (cid && cid->s && cid->len) {
			flow->cid.s = (char*)shm_malloc(cid->len);
			if (flow->cid.s==NULL) {
				LM_ERR("not enough shmem for the cid of the flow\n");
				goto error;
			}
			memcpy( flow->cid.s, cid->s, cid->len);
			flow->cid.len = cid->len;
		}
		/* audio messages */
		for( i=0 ; i<MAX_AUDIO ; i++ ) {
			if (recordings[i].s && recordings[i].len) {
				flow->recordings[i].s = (char*)shm_malloc(recordings[i].len);
				if (flow->recordings[i].s==NULL) {
					LM_ERR("not enough shmem for the message %d of the flow\n",
						i);
					goto error;
				}
				memcpy( flow->recordings[i].s, recordings[i].s,
					recordings[i].len);
				flow->recordings[i].len = recordings[i].len;
			}
		}
#ifdef STATISTICS
		/* statistics */
		s.s = "ccf_incalls";s.len = 11 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat("call_center",
		name, &flow->st_incalls, STAT_SHM_NAME)!=0 ) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "ccf_dist_incalls";s.len = 16 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat("call_center",
		name, &flow->st_dist_incalls, STAT_SHM_NAME)!=0 ) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "ccf_answ_incalls";s.len = 16 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat("call_center",
		name, &flow->st_answ_incalls, STAT_SHM_NAME)!=0 ) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "ccf_answ_inchats";s.len = 16 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat("call_center",
		name, &flow->st_answ_inchats, STAT_SHM_NAME)!=0 ) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "ccf_aban_incalls";s.len = 16 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat("call_center",
		name, &flow->st_aban_incalls, STAT_SHM_NAME)!=0 ) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "ccf_onhold_calls";s.len = 16 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat("call_center",
		name, &flow->st_onhold_calls, STAT_SHM_NAME|STAT_NO_RESET)!=0 ) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "ccf_onhold_chats";s.len = 16 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat("call_center",
		name, &flow->st_onhold_chats, STAT_SHM_NAME|STAT_NO_RESET)!=0 ) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "ccf_queued_calls";s.len = 16 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat("call_center",
		name, &flow->st_queued_calls, STAT_SHM_NAME|STAT_NO_RESET)!=0 ) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "ccf_etw";s.len = 7 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat2("call_center",
		name, (stat_var **)cc_flow_get_etw, STAT_SHM_NAME|STAT_IS_FUNC,
		(void*)flow, 0)!=0) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "ccf_awt";s.len = 7 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat2("call_center",
		name, (stat_var **)cc_flow_get_awt, STAT_SHM_NAME|STAT_IS_FUNC,
		(void*)flow, 0)!=0) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "ccf_load";s.len = 8 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat2("call_center",
		name, (stat_var **)cc_flow_get_load, STAT_SHM_NAME|STAT_IS_FUNC,
		(void*)flow, 0)!=0) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "ccf_free_agents";s.len = 15 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat2("call_center",
		name, (stat_var **)cc_flow_free_agents, STAT_SHM_NAME|STAT_IS_FUNC,
		(void*)flow, 0)!=0) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
#endif

		flow->is_new = 1;
		/* insert the new flow in the list */
		flow->next = data->flows;
		data->flows = flow;
	} else {
		/* flow already exists -> update */
		/* priority */
		flow->priority = priority;
		/* max wrapup time */
		flow->max_wrapup = max_wrapup;
		/* dissuading related options */
		flow->diss_hangup = diss_hangup;
		flow->diss_ewt_th = diss_ewt_th;
		flow->diss_qsize_th = diss_qsize_th;
		flow->diss_onhold_th = diss_onhold_th;
		/* skill - needs to be changed ? */
		skill_id = get_skill_id(data,skill);
		if (skill_id==0) {
			LM_ERR("cannot get skill id\n");
			goto error1;
		}
		flow->skill = skill_id;
		/* cid - needs to be changed ? */
		if ( flow->cid.len && ( cid->len==0 ||
		cid->len>flow->cid.len || memcmp(flow->cid.s,cid->s,cid->len)!=0) ) {
			shm_free(flow->cid.s); flow->cid.s = NULL; flow->cid.len = 0 ;
		}
		if (flow->cid.s==NULL && cid->len!=0) {
			flow->cid.s = (char*)shm_malloc(cid->len);
			if (flow->cid.s==NULL) {
				LM_ERR("not enough shmem for the cid of the flow\n");
				goto error1;
			}
		}
		if (flow->cid.s) {
			memcpy( flow->cid.s, cid->s, cid->len);
			flow->cid.len = cid->len;
		}
		/* audio messages */
		for( i=0 ; i<MAX_AUDIO ; i++ ) {
			if ( flow->recordings[i].len && ( recordings[i].len==0 ||
			recordings[i].len>flow->recordings[i].len ||
			memcmp(flow->recordings[i].s,recordings[i].s,recordings[i].len)
			) ) {
				shm_free(flow->recordings[i].s); flow->recordings[i].s = NULL;
				flow->recordings[i].len = 0 ;
			}
			if (flow->recordings[i].s==NULL && recordings[i].len!=0) {
				flow->recordings[i].s = (char*)shm_malloc(recordings[i].len);
				if (flow->recordings[i].s==NULL) {
					LM_ERR("not enough shmem for the message of the flow\n");
					goto error1;
				}
			}
			if (flow->recordings[i].s) {
				memcpy( flow->recordings[i].s, recordings[i].s,
					recordings[i].len);
				flow->recordings[i].len = recordings[i].len;
			}
		}
		flow->is_new = 1;

	}

	return 0;

error1:
	if(data->flows == flow)
		data->flows = flow->next;
	else
	for(prev_flow=data->flows; prev_flow; prev_flow=prev_flow->next)
		if(prev_flow->next == flow) {
			prev_flow->next = flow->next;
			break;
		}
error:
	if (flow)
		free_cc_flow(flow);
	return -1;
}


static void free_cc_flow( struct cc_flow *flow)
{
	int i;

	if (flow->cid.s)
		shm_free(flow->cid.s);
	for( i=0 ; i<MAX_AUDIO ; i++ ) {
		if (flow->recordings[i].s)
			shm_free(flow->recordings[i].s);
	}
	shm_free(flow);
}


void update_cc_agent_att(struct cc_agent *agent, unsigned long duration)
{
	agent->avg_talktime_no ++;
	agent->avg_talktime =
		( ((float)duration + (agent->avg_talktime * (float)(agent->avg_talktime_no-1))) ) /
		(float)agent->avg_talktime_no;
}


#ifdef STATISTICS
static unsigned long cc_agent_get_att( void *agent_p)
{
	return (unsigned long)((struct cc_agent*)agent_p)->avg_talktime;
}
#endif

int add_cc_agent( struct cc_data *data, str *id, struct media_info *media,
				str *skills, unsigned int logstate, unsigned int own_wrapup,
												unsigned int wrapup_end_time)
{
	struct cc_agent *agent, *prev_agent= 0;
	struct sip_uri uri;
	str skill;
	char *p;
	unsigned int i, n,skill_id;
#ifdef STATISTICS
	char *name;
	str s;
#endif

	/* is the agent a new one? - search by ID */
	agent = get_agent_by_name( data, id, &prev_agent);
	logstate = logstate ? 1 : 0;

	if (agent==NULL) {
		/* new agent -> create and populate one */
		agent = (struct cc_agent*)shm_malloc(sizeof(struct cc_agent)+id->len);
		if (agent==NULL) {
			LM_ERR("not enough shmem for a new agent\n");
			goto error;
		}
		memset( agent, 0, sizeof(struct cc_agent) );
		/* id */
		agent->id.s = (char*)(agent+1);
		memcpy( agent->id.s, id->s, id->len);
		agent->id.len = id->len;
		/* locations */
		if (media[CC_MEDIA_RTP].location.s==NULL && 
		media[CC_MEDIA_MSRP].location.s==NULL){
			LM_ERR("agent has neither RTP, nor MSRP locations\n");
			goto error;
		}
		for (i=0 ; i<CC_MEDIA_NO ; i++ ) {
			if (media[i].location.s==NULL)
				continue;
			agent->media[i].location.s =
				(char*)shm_malloc(media[i].location.len);
			if (agent->media[i].location.s==NULL) {
				LM_ERR("not enough shmem for the %s location of the agent\n",
					(i==CC_MEDIA_RTP)?"RTP":"MSRP");
				goto error;
			}
			memcpy( agent->media[i].location.s, media[i].location.s,
				media[i].location.len);
			agent->media[i].location.len = media[i].location.len ;
			if (parse_uri( agent->media[i].location.s,
			agent->media[i].location.len, &uri)<0) {
				LM_ERR("%s location [%.*s] of the agent is not a SIP URI\n",
					(i==CC_MEDIA_RTP)?"RTP":"MSRP",
					media[i].location.len, media[i].location.s);
				goto error;
			}
			agent->media[i].did = uri.user;
			agent->media[i].sessions = media[i].sessions;
		}
		/* LOG STATE */
		agent->logged_in = logstate;
		/* WRAPUP TIME */
		agent->wrapup_time = (own_wrapup==0)? wrapup_time : own_wrapup;
		/* set of skills */
		if (skills && skills->len) {
			p = skills->s;
			while (p) {
				if (agent->no_skills==MAX_SKILLS_PER_AGENT) {
					LM_WARN("too many skills (%d) for the agent <%.*s>, "
						"discarding <%.*s>\n",
						agent->no_skills, agent->id.len, agent->id.s,
						(int)(skills->s+skills->len-p), p);
					break;
				}
				skill.s = p;
				p = q_memchr(skill.s, ',', skills->s+skills->len-skill.s);
				skill.len = p?(p-skill.s):(skills->s+skills->len-skill.s);
				trim(&skill);
				if (skill.len) {
					skill_id = get_skill_id(data,&skill);
					if (skill_id==0) {
						LM_WARN("unknown skill <%.*s>  for the agent <%.*s>,"
							"discarding\n",
							skill.len, skill.s, agent->id.len, agent->id.s);
					} else {
						n = agent->no_skills++;
						agent->skills[n] = skill_id;
					}
				}
				if(p)
					p++;
			}
		}
		/* statistics */
#ifdef STATISTICS
		s.s = "cca_dist_incalls";s.len = 16 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat("call_center",
		name, &agent->st_dist_incalls, STAT_SHM_NAME)!=0 ) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "cca_answ_incalls";s.len = 16 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat("call_center",
		name, &agent->st_answ_incalls, STAT_SHM_NAME)!=0 ) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "cca_answ_inchats";s.len = 16 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat("call_center",
		name, &agent->st_answ_inchats, STAT_SHM_NAME)!=0 ) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "cca_aban_incalls";s.len = 16 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat("call_center",
		name, &agent->st_aban_incalls, STAT_SHM_NAME)!=0 ) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
		s.s = "cca_att";s.len = 7 ;
		if ( (name=build_stat_name( &s, id->s))==0 || register_stat2("call_center",
		name, (stat_var **)cc_agent_get_att, STAT_SHM_NAME|STAT_IS_FUNC,
		(void*)agent, 0)!=0) {
			LM_ERR("failed to add stat variable\n");
			goto error;
		}
#endif
		if (wrapup_end_time && (wrapup_end_time > (int)time(NULL))) {
			agent->state = CC_AGENT_WRAPUP;
			agent->wrapup_end_time = wrapup_end_time - startup_time;
		}
		agent->is_new = 1;
		/* link the agent */
		add_cc_agent_top(data, agent);
		data->totalnr_agents++;
	} else {
		/* agent already exists -> update only */
		for (i=0 ; i<CC_MEDIA_NO ; i++ ) {
			/* location - needs to be changed ? */
			if ( agent->media[i].location.len!=media[i].location.len ||
			memcmp(agent->media[i].location.s,media[i].location.s,
			media[i].location.len)!=0 ) {
				/* set new location */
				if (agent->media[i].location.len < media[i].location.len ){
					shm_free(agent->media[i].location.s);
					agent->media[i].location.s =
						(char*)shm_malloc(media[i].location.len);
					if (agent->media[i].location.s==NULL) {
						LM_ERR("not enough shmem for the %s location of "
						"the agent\n", (i==CC_MEDIA_RTP)?"RTP":"MSRP");
							goto error1;
					}
				}
				memcpy( agent->media[i].location.s, media[i].location.s,
					media[i].location.len);
				agent->media[i].location.len = media[i].location.len;
				if (parse_uri( agent->media[i].location.s,
				agent->media[i].location.len, &uri)<0) {
					LM_ERR("%s location [%.*s] of the agent is not a "
						"SIP URI\n", (i==CC_MEDIA_RTP)?"RTP":"MSRP",
						media[i].location.len, media[i].location.s);
					goto error1;
				}
				agent->media[i].did = uri.user;
			}
		}
		/* if logstate changed - move between the lists TODO */
		if(logstate != agent->logged_in) {
			agent_switch_login(data, agent, prev_agent);
		}
		/* WRAPUP TIME */
		agent->wrapup_time = (own_wrapup==0)? wrapup_time : own_wrapup;
		/* skills - needs to be changed ? */
		agent->no_skills = 0;
		if (skills && skills->len) {
			p = skills->s;
			while (p) {
				skill.s = p;
				p = q_memchr(skill.s, ',', skills->s+skills->len-skill.s);
				skill.len = p?(p-skill.s):(skills->s+skills->len-skill.s);
				trim(&skill);
				if (skill.len) {
					skill_id = get_skill_id(data,&skill);
					if (skill_id==0) {
						LM_ERR("cannot get skill id\n");
						goto error1;
					}
					n = agent->no_skills++; 
					agent->skills[n] = skill_id;
				}
				if(p)
					p++;
			}
		}
		agent->is_new = 1;
	}

	return 0;
error1:
	remove_cc_agent(data, agent, prev_agent);
error:
	if (agent)
		free_cc_agent(agent);
	return 0;
}


static void free_cc_agent( struct cc_agent *agent)
{
	unsigned int i;

	for( i=0 ; i<CC_MEDIA_NO ; i++)
		if (agent->media[i].location.s)
			shm_free(agent->media[i].location.s);
	shm_free(agent);
}


void print_call_list(struct cc_data *data)
{
	struct cc_call *call;
	
	for( call=data->list.first ; call ; call=call->next_list )
		LM_DBG("[%.*s] - %p\n", call->b2bua_id.len, call->b2bua_id.s, call);
}

void cc_list_insert_call(struct cc_data *data, struct cc_call *call)
{
	if(data->list.first)
		data->list.first->prev_list = call;
	call->next_list= data->list.first;
	data->list.first = call;
	call->prev_list = NULL;
	data->list.calls_no++;
	call->id = data->list.lid++;
	print_call_list(data);
}

void cc_list_remove_call(struct cc_data *data, struct cc_call *call)
{
	if(call->prev_list)
		call->prev_list->next_list = call->next_list;
	else
		data->list.first = call->next_list;

	if(call->next_list)
		call->next_list->prev_list = call->prev_list;
	
	data->list.calls_no--;
	print_call_list(data);
}

struct cc_call* new_cc_call(struct cc_data *data, struct cc_flow *flow,
		str *dn, str *un, str *param)
{
	struct cc_call *call;
	char *p;

	/* new call structure */
	call = (struct cc_call*)shm_malloc( sizeof(struct cc_call) +
		(dn?dn->len:0) + (un?un->len:0) + (param?param->len:0) );
	if (call==NULL) {
		LM_ERR("no more shm mem for a new call\n");
		return NULL;
	}
	memset( call, 0, sizeof(struct cc_call) );
	p = (char*)(call+1);

	/*copy DisplayName and UserName */
	if (dn && dn->s) {
		call->caller_dn.s = p;
		call->caller_dn.len = dn->len;
		memcpy( p, dn->s, dn->len );
		p += dn->len;
	}
	if (un && un->s) {
		call->caller_un.s = p;
		call->caller_un.len = un->len;
		memcpy( p, un->s, un->len );
		p += un->len;
	}
	if (param && param->s && param->len) {
		call->script_param.s = p;
		call->script_param.len = param->len;
		memcpy( p, param->s, param->len );
		p += param->len;
	}

	call->recv_time = get_ticks();

	call->setup_time = -1;

	/* attache to flow */
	call->flow = flow;
	flow->ref_cnt++;
	LM_DBG("created call %p\n", call);

	/* attache a lock */
	call->lock_idx = data->next_lock_to_use++;
	if (data->next_lock_to_use==512)
		data->next_lock_to_use = 0;

	cc_list_insert_call( data, call );

	return call;
}


void free_cc_call(struct cc_data * data, struct cc_call *call)
{
	lock_get( data->lock );

	cc_list_remove_call( data, call );

	lock_release( data->lock );

	LM_DBG("free call %p, [%.*s]\n", call, call->b2bua_id.len, call->b2bua_id.s);
	if (call->flow)
		call->flow->ref_cnt--;

	if(call->b2bua_id.s)
		shm_free(call->b2bua_id.s);

	if (call->b2bua_agent_id.s)
		shm_free(call->b2bua_agent_id.s);

	shm_free(call);
}


struct cc_agent* get_free_agent_by_skill(struct cc_data *data,
		media_type media, unsigned int skill)
{
	struct cc_agent *agent,*prev_agent;
	unsigned int n;

	agent = data->agents[CC_AG_ONLINE];
	if (agent==NULL) return NULL;

	/* iterate all agents*/

	if (media==CC_MEDIA_RTP) {
		/* for audio calls, we need a 100% free agent */
		do {
			if(agent->state==CC_AGENT_FREE) {
				/* iterate all skills of the agent */
				for( n=0 ; n<agent->no_skills ; n++) {
					if (agent->skills[n]==skill)
						return agent;
				}
			}
			/* next agent */
			agent = agent->next;
		}while(agent);

	} else
	if (media==CC_MEDIA_MSRP) {
		/* for chat calls, we need an agent with spare sessions */
		prev_agent = agent;
		do {
			if ( can_agent_take_chats(agent) ) {
				/* iterate all skills of the agent */
				for( n=0 ; n<agent->no_skills ; n++) {
					if (agent->skills[n]==skill) {
						if (msrp_dispatch_policy == CC_MSRP_POLICY_LB)
							move_cc_agent_to_end( data, agent, prev_agent);
						return agent;
					}
				}
			}
			/* next agent */
			prev_agent = agent;
			agent = agent->next;
		}while(agent);
	}
	return NULL;
}


void log_agent_to_flows(struct cc_data *data, struct cc_agent *agent, int login)
{
	unsigned int i;
	struct cc_flow *flow;

	LM_DBG("login %d agent %.*s\n", login, agent->id.len, agent->id.s);
	/* iterate all skills of the agent */
	for( i=0 ; i<agent->no_skills ; i++) {
		//LM_DBG(" agent  skill is %d (%d)\n", agent->skills[i],i);
		/* iterate all flows */
		for( flow=data->flows ; flow ; flow=flow->next ) {
			//LM_DBG("chekcing flow %.*s with skill %d\n", flow->id.len, flow->id.s, flow->skill);
			if (agent->skills[i]==flow->skill)
				flow->logged_agents = flow->logged_agents + (login?1:-1);
		}
	}
}


void clean_cc_old_data(struct cc_data *data)
{
	struct cc_skill *skill, **prv_skill;
	struct cc_agent *agent, **prv_agent;
	struct cc_flow  *flow,  **prv_flow;
	int i;

	/* clean old skills */
	skill = data->skills_map;
	prv_skill = &(data->skills_map);
	while(skill) {
		if (skill->is_new) {
			skill->is_new = 0;
			prv_skill = &(skill->next);
			skill = skill->next;
		} else {
			*prv_skill = skill->next;
			free_cc_skill(skill);
			skill = (*prv_skill);
		}
	}

	/* clean old agents */
	for(i= 0; i< 2; i++) {
		agent = data->agents[i];
		prv_agent = &data->agents[i];
		while(agent) { 
			if (agent->is_new) {
				agent->is_new = 0;
				prv_agent = &(agent->next);
				agent = agent->next;
			} else {
				*prv_agent = agent->next;
				if (agent->ref_cnt==0) {
					free_cc_agent(agent);
				} else {
					agent->next = data->old_agents;
					data->old_agents = agent;
				}
				agent = (*prv_agent);
				data->totalnr_agents--;
			}
		}
	}

	/* clean old flows */
	flow = data->flows;
	prv_flow = &(data->flows);
	while(flow) {
		flow->logged_agents = 0;
		if (flow->is_new) {
			flow->is_new = 0;
			prv_flow = &(flow->next);
			flow = flow->next;
		} else {
			*prv_flow = flow->next;
			if (flow->ref_cnt==0) {
				free_cc_flow(flow);
			} else {
				/* put in a cleanup list */
				flow->next = data->old_flows; 
				data->old_flows = flow;
			}
			flow = (*prv_flow);
		}
	}

	/* sync flows and agents (how many agents per flow are logged) */
	/* iterate all logged agents */
	data->loggedin_agents = 0;
	for( agent=data->agents[CC_AG_ONLINE] ; agent ; agent=agent->next ) {
		/* update last agent */
		data->last_online_agent = agent;

		/* log_agent_to_flows() must now the call center of the 
		 * agent to count it as logged in */
		log_agent_to_flows( data, agent, agent->logged_in);
		data->loggedin_agents++;
	}
}


void clean_cc_unref_data(struct cc_data *data)
{
	struct cc_agent *agent, **prv_agent;
	struct cc_flow  *flow,  **prv_flow;

	/* clean unref flows */
	flow = data->old_flows;
	prv_flow = &(data->old_flows);
	while(flow) {
		if (flow->ref_cnt!=0) {
			prv_flow = &(flow->next);
			flow = flow->next;
		} else {
			*prv_flow = flow->next;
			free_cc_flow(flow);
			flow = (*prv_flow);
		}
	}

	/* clean unref agents */
	agent = data->old_agents;
	prv_agent = &(data->old_agents);
	while(agent) {
		if (agent->ref_cnt!=0) {
			prv_agent = &(agent->next);
			agent = agent->next;
		} else {
			*prv_agent = agent->next;
			free_cc_agent(agent);
			agent = (*prv_agent);
		}
	}

	return;
}


void print_queue(struct cc_data *data)
{
	struct cc_call *call_it;
	LM_DBG("QUEUE:\n");
	for(call_it=data->queue.first ; call_it ; call_it=call_it->lower_in_queue)
		LM_DBG("[%p] ->\n", call_it);
	LM_DBG("0\n");
}


int cc_queue_push_call(struct cc_data *data, struct cc_call *call, int top)
{
	struct cc_call *call_it;
	int n = 0;

	LM_DBG(" QUEUE - adding call %p \n",call);
	if ( is_call_in_queue(data, call) ) {
		LM_CRIT(" QUEUE - call already in queue \n");
		abort();
	}

	if (top) {
		/* add the call in the very top of the queue */
		call_it = NULL;
	} else {
		/* search (priority based) the place in queue */
		for(call_it=data->queue.last ; call_it ; call_it=call_it->higher_in_queue){
			if (call_it->flow->priority <= call->flow->priority)
				break;
			n++;
		}
	}


	if (call_it) {
		/* add before it */
		if (call_it->lower_in_queue) {
			call_it->lower_in_queue->higher_in_queue = call;
		} else {
			data->queue.last = call;
		}
		call->lower_in_queue = call_it->lower_in_queue;
		call->higher_in_queue = call_it;
		call_it->lower_in_queue = call;
	} else {
		/* add in top of the queue */
		call->lower_in_queue = data->queue.first;
		if (data->queue.first) {
			data->queue.first->higher_in_queue = call;
		}
		else {
			data->queue.last = call;
		}
		call->higher_in_queue = NULL;
		data->queue.first = call;
	}
	data->queue.calls_no++;
	update_stat( call->flow->st_queued_calls, +1 );
	
	LM_DBG("adding call on pos %d (already %d calls), l=%p h=%p\n",
		n, data->queue.calls_no,
		call->lower_in_queue, call->higher_in_queue);
	call->ref_cnt++;

	if (call->queue_start==0)
		call->queue_start = get_ticks();

	return data->queue.calls_no-1-n;
}



void cc_queue_rmv_call( struct cc_data *data, struct cc_call *call)
{
	LM_DBG(" QUEUE - removing call %p \n",call);
	if ( !is_call_in_queue(data, call) ) { 
		LM_CRIT(" QUEUE - call not in queue l=%p, h=%p\n",
				call->lower_in_queue, call->higher_in_queue);
		abort();
	}

	if (call->lower_in_queue) {
		call->lower_in_queue->higher_in_queue =
			call->higher_in_queue;
	} else {
		data->queue.last = call->higher_in_queue;
	}
	if (call->higher_in_queue) {
		call->higher_in_queue->lower_in_queue =
			call->lower_in_queue;
	} else {
		data->queue.first = call->lower_in_queue;
	}
	call->lower_in_queue = call->higher_in_queue = NULL;
	data->queue.calls_no--;
	update_stat( call->flow->st_queued_calls, -1 );
}



struct cc_call *cc_queue_pop_call_for_agent(struct cc_data *data,
									struct cc_agent *agent, media_type media)
{
	struct cc_call *call_it;
	unsigned int i;

	/* interate all the queued calls and see *
	 * if they mathe the agent (as skills)*/
	for(call_it=data->queue.first ; call_it ; call_it=call_it->lower_in_queue){
		/* before taking a call out, be sure that call is fully initialized
		 * from b2bua point of view (to avoid races) -> check the b2bua id */
		if ( call_it->media!=media || call_it->b2bua_id.len==0)
			continue;
		/* check the call skill against the agent skills */
		for(i=0 ; i<agent->no_skills ; i++) {
			if (call_it->flow->skill==agent->skills[i]) {
				LM_DBG("found call %p/%d for agent %p(%.*s) with skill %d \n",
					call_it, call_it->media,
					agent, agent->id.len, agent->id.s,
					call_it->flow->skill);
				/* remove the call from queue */
				cc_queue_rmv_call( data, call_it);
				return call_it;
			}
		}
	}

	return NULL;
}


void agent_raise_event(struct cc_agent *agent, struct cc_call *call)
{
	static str agent_id_str = str_init("agent_id");
	static str status_str = str_init("status");
	static str status_offline_str = str_init("offline");
	static str status_free_str = str_init("free");
	static str status_incall_str = str_init("incall");
	static str status_inchat_str = str_init("inchat");
	static str status_wrapup_str = str_init("wrapup");
	static str wrapup_ends_str = str_init("wrapup_ends");
	static str flow_id_str = str_init("flow_id");

	evi_params_p list;
	str *txt = NULL;
	int ts;

	if (agent_evi_id == EVI_ERROR || !evi_probe_event(agent_evi_id))
		return;

	list = evi_get_params();
	if (!list) {
		LM_ERR("cannot create event params\n");
		return;
	}

	if (evi_param_add_str(list, &agent_id_str, &agent->id) < 0) {
		LM_ERR("cannot add agent_id\n");
		goto error;
	}

	if (!agent->logged_in) {
		txt = &status_offline_str;
	} else {
		switch (agent->state) {
			case CC_AGENT_FREE:
				txt = &status_free_str;
				break;
			case CC_AGENT_INCALL:
				txt = &status_incall_str;
				break;
			case CC_AGENT_INCHAT:
				txt = &status_inchat_str;
				break;
			case CC_AGENT_WRAPUP:
				txt = &status_wrapup_str;
				break;
		}
	}

	if (evi_param_add_str(list, &status_str, txt) < 0) {
		LM_ERR("cannot add state\n");
		goto error;
	}

	if (agent->state==CC_AGENT_WRAPUP) {
		ts = (int)time(NULL)+agent->wrapup_end_time-get_ticks();
		if (evi_param_add_int(list, &wrapup_ends_str, &ts) < 0) {
			LM_ERR("cannot add wrapup time\n");
			goto error;
		}
	}

	if (agent->state==CC_AGENT_INCALL && call) {
		if (evi_param_add_str(list, &flow_id_str, &call->flow->id) < 0) {
			LM_ERR("cannot add flow ID time\n");
			goto error;
		}
	}

	if (agent->state==CC_AGENT_INCHAT && call) {
		static str sessions_str = str_init("ongoing_sessions");
		ts = agent->ongoing_sessions[CC_MEDIA_MSRP];
		if (evi_param_add_int(list, &sessions_str, &ts) < 0) {
			LM_ERR("cannot add ongoing calls\n");
			goto error;
		}
		if ( agent->wrapup_end_time>get_ticks() ) {
			ts = (int)time(NULL)+agent->wrapup_end_time-get_ticks();
			if (evi_param_add_int(list, &wrapup_ends_str, &ts) < 0) {
				LM_ERR("cannot add wrapup time\n");
				goto error;
			}
		}
		if (call && evi_param_add_str(list, &flow_id_str, &call->flow->id)<0) {
			LM_ERR("cannot add flow ID time\n");
			goto error;
		}
	}


	if (evi_raise_event(agent_evi_id, list)) {
		LM_ERR("unable to send agent report event\n");
	}
	return;

error:
	evi_free_params(list);
}


