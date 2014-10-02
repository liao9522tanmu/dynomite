/*
 * Dynomite - A thin, distributed replication layer for multi non-distributed storages.
 * Copyright (C) 2014 Netflix, Inc.
 */


#include <stdlib.h>
#include <unistd.h>
#include <unistd.h>
#include <ctype.h>

#include "dyn_core.h"
#include "dyn_server.h"
#include "dyn_gossip.h"
#include "dyn_dnode_peer.h"
#include "dyn_util.h"
#include "dyn_string.h"
#include "dyn_ring_queue.h"
#include "dyn_node_snitch.h"
#include "dyn_token.h"
#include "seedsprovider/dyn_seeds_provider.h"


static void gossip_debug(void);
static struct gossip_node_pool gn_pool;

static rstatus_t
gossip_process_messages(void)
{
	//TODOs: fix this to process an array of nodes
	while (!CBUF_IsEmpty(C2G_InQ)) {
		struct ring_message *msg = (struct ring_message *) CBUF_Pop(C2G_InQ);
		if (msg != NULL && msg->cb != NULL) {
			struct node *rnode = (struct node *) array_get(&msg->nodes, 0);
			msg->cb(msg->sp, rnode);
			//msg->cb(msg->sp, msg->node);
			//core_debug(msg->sp->ctx);
			ring_message_deinit(msg);
		}
	}

	return DN_OK;
}


static rstatus_t
gossip_msg_to_core(struct server_pool *sp, struct node *node, void *cb)
{
	struct ring_message *msg = create_ring_message();
	struct node *rnode = (struct node *) array_get(&msg->nodes, 0);
	//node_copy(node, msg->node);
	node_copy(node, rnode);

	msg->cb = cb;
	msg->sp = sp;
	CBUF_Push(C2G_OutQ, msg);

	return DN_OK;
}

static rstatus_t
gossip_announce_joining(struct server_pool *sp)
{
	return gossip_msg_to_core(sp, NULL, dnode_peer_handshake_announcing);
}

static rstatus_t
parse_seeds(struct string *seeds, struct string *rack_name, struct string *region_name,
		struct string *port_str, struct string *address, struct string *name,
		struct array * ptokens)
{
	rstatus_t status;
	uint8_t *p, *q, *start;
	uint8_t *pname, *port, *rack, *region, *tokens, *addr;
	uint32_t k, delimlen, pnamelen, portlen, racklen, regionlen, tokenslen, addrlen;
	char delim[] = "::::";

	/* parse "hostname:port:rack:region:tokens" */
	p = seeds->data + seeds->len - 1;
	start = seeds->data;
	rack = NULL;
	region = NULL;
	racklen = 0;
	regionlen = 0;
	tokens = NULL;
	tokenslen = 0;
	port = NULL;
	portlen = 0;
	delimlen = 4;

	for (k = 0; k < sizeof(delim)-1; k++) {
		q = dn_strrchr(p, start, delim[k]);

		switch (k) {
		case 0:
			tokens = q + 1;
			tokenslen = (uint32_t)(p - tokens + 1);
			break;
		case 1:
			region = q + 1;
			regionlen = (uint32_t)(p - region + 1);
			string_copy(region_name, region, regionlen);
			break;
		case 2:
			rack = q + 1;
			racklen = (uint32_t)(p - rack + 1);
			string_copy(rack_name, rack, racklen);
			break;

		case 3:
			port = q + 1;
			portlen = (uint32_t)(p - port + 1);
			string_copy(port_str, port, portlen);
			break;

		default:
			NOT_REACHED();
		}

		p = q - 1;
	}

	if (k != delimlen) {
		return GOS_ERROR;
	}

	//pname = hostname:port
	pname = seeds->data;
	pnamelen = seeds->len - (tokenslen + racklen + regionlen + 3);
	status = string_copy(address, pname, pnamelen);


	//addr = hostname or ip only
	addr = start;
	addrlen = (uint32_t)(p - start + 1);
	//if it is a dns name, convert to IP or otherwise keep that IP
	if (!isdigit( (char) addr[0])) {
		addr[addrlen] = '\0';
		char *local_ip4 = hostname_to_private_ip4( (char *) addr);
		if (local_ip4 != NULL) {
		    status = string_copy_c(name, local_ip4);
		} else
			status = string_copy(name, addr, addrlen);
	} else {
	    status = string_copy(name, addr, addrlen);
	}
	if (status != DN_OK) {
		return GOS_ERROR;
	}

	uint8_t *t_end = tokens + tokenslen;
	status = derive_tokens(ptokens, tokens, t_end);
	if (status != DN_OK) {
		return GOS_ERROR;
	}

	//status = dn_resolve(&address, field->port, &field->info);
	//if (status != DN_OK) {
	//    string_deinit(&address);
	//    return CONF_ERROR;
	//}

	return GOS_OK;
}



static rstatus_t
gossip_rack_init(struct gossip_rack *g_rack, struct string *rack)
{
	rstatus_t status;
	string_copy(&g_rack->name, rack->data, rack->len);
	g_rack->nnodes = 0;
	g_rack->nlive_nodes = 0;
	status = array_init(&g_rack->nodes, 1, sizeof(struct node));

	return status;
}


static struct node *
gossip_add_node_to_rack(struct server_pool *sp, struct gossip_rack *g_rack,
		struct string *address, struct string *ip, struct string *port, struct array *tokens)
{
	rstatus_t status;
	log_debug(LOG_VERB, "gossip_add_node_to_rack : rack[%.*s] address[%.*s] ip[%.*s] port[%.*s]",
			g_rack->name, address->len, address->data, ip->len, ip->data, port->len, port->data);

	int port_i = dn_atoi(port->data, port->len);
	if (port_i == 0) {
		return NULL; //bad data
	}

	struct node *gnode = (struct node *) array_push(&g_rack->nodes);
	node_init(gnode);
	status = string_copy(&gnode->rack, g_rack->name.data, g_rack->name.len);
	status = string_copy(&gnode->name, ip->data, ip->len);
	status = string_copy(&gnode->pname, address->data, address->len); //ignore the port for now
	gnode->port = port_i;

	//only use one token
	struct dyn_token * token = (struct dyn_token *) array_get(tokens, 0);
	struct dyn_token * gtoken = &gnode->token;
	copy_dyn_token(token, gtoken);

	g_rack->nnodes++;

	return gnode;
}


static rstatus_t
gossip_add_node(struct server_pool *sp, struct gossip_rack *g_rack,
		struct string *address, struct string *ip, struct string *port, struct array *tokens)
{
	rstatus_t status;
	log_debug(LOG_VERB, "gossip_add_node : rack[%.*s] address[%.*s] ip[%.*s] port[%.*s]",
			g_rack->name.len, g_rack->name.data, address->len, address->data, ip->len, ip->data, port->len, port->data);

	struct node *gnode = gossip_add_node_to_rack(sp, g_rack, address, ip, port, tokens);
	if (gnode == NULL) {
		return DN_ENOMEM;
	}

	status = gossip_msg_to_core(sp, gnode, dnode_peer_add);
	return status;
}

static rstatus_t
gossip_replace_node(struct server_pool *sp, struct node *node,
		struct string *new_address, struct string *new_ip)
{
	rstatus_t status;
	log_debug(LOG_VERB, "gossip_replace_node : rack[%.*s] oldaddr[%.*s] newaddr[%.*s] newip[%.*s]",
			node->rack, node->name, new_address->len, new_address->data, new_ip->len, new_ip->data);

	string_deinit(&node->name);
	string_deinit(&node->pname);
	status = string_copy(&node->name, new_ip->data, new_ip->len);
	status = string_copy(&node->pname, new_address->data, new_address->len);
	//port is supposed to be the same

	gossip_msg_to_core(sp, node, dnode_peer_replace);

	//should check for status
	return status;
}


static rstatus_t
gossip_add_rack(struct server_pool *sp, struct string *rack,
		struct string *address, struct string *ip,
		struct string *port, struct array * tokens)
{
	rstatus_t status;
	log_debug(LOG_VERB, "gossip_add_rack : rack[%.*s] address[%.*s] ip[%.*s] port[%.*s]",
			rack->len, rack->data, address->len, address->data, ip->len, ip->data, port->len, port->data);
	//add rack
	struct gossip_rack *g_rack = (struct gossip_rack *)  array_push(&gn_pool.racks);
	status = gossip_rack_init(g_rack, rack);

	struct node *gnode = gossip_add_node_to_rack(sp, g_rack, address, ip, port, tokens);
	if (gnode == NULL) {
		array_pop(&gn_pool.racks);
		return DN_ENOMEM;
	}

	status = gossip_msg_to_core(sp, gnode, dnode_peer_add_rack);

	return status;
}

//we should use hash table to help out here for O(1) check.
static rstatus_t
gossip_add_seed_if_absent(struct server_pool *sp, struct string *rack,
		struct string *address, struct string *ip,
		struct string *port, struct array * tokens)
{
	rstatus_t status;
	bool rack_existed = false;

	log_debug(LOG_VERB, "gossip_add_seed_if_absent          : '%.*s'", address->len, address->data);

	uint32_t i, nelem;
	for (i = 0, nelem = array_n(&gn_pool.racks); i < nelem; i++) {
		struct gossip_rack *g_rack = (struct gossip_rack *)  array_get(&gn_pool.racks, i);

		if (string_compare(rack, &g_rack->name) == 0) { //an existed RACK
			rack_existed = true;
			if (g_rack->nnodes == 0) {
				gossip_add_node(sp, g_rack, address, ip, port, tokens);
				break;
			} else {
				uint32_t j;
				bool exist = false;
				for(j = 0; j < g_rack->nnodes; j++) {
					struct node * g_node = (struct node *) array_get(&g_rack->nodes, j);
					log_debug(LOG_VERB, "\t\tg_node->name          : '%.*s'", g_node->name.len, g_node->name.data);
					log_debug(LOG_VERB, "\t\tip         : '%.*s'", ip->len, ip->data);

					if (string_compare(&g_node->name, ip) == 0) {
						exist = true;
						break;
					} else {  //name is different. Now compare tokens
						//TODOs: only compare the 1st token for now.  Support Vnode later
						//struct dyn_token * node_token = (struct dyn_token *) array_get(&g_node->tokens, 0);
						struct dyn_token * seed_token = (struct dyn_token *) array_get(tokens, 0);
						struct dyn_token * node_token = &g_node->token;
						if (node_token != NULL && cmp_dyn_token(node_token, seed_token) == 0) {
							//replace node
							gossip_replace_node(sp, g_node, address, ip);
							exist = true;
							break;
						}
					}

				}

				if (!exist) {
					gossip_add_node(sp, g_rack, address, ip, port, tokens);
					break;
				}
			}
		}
	}

	if (!rack_existed) {
		gossip_add_rack(sp, rack, address, ip, port, tokens);
	}

	return 0;
}


static rstatus_t
gossip_update_seeds(struct server_pool *sp, struct string *seeds)
{
	struct string rack_name;
	struct string region_name; //TODOs: need to process region name
	struct string port_str;
	struct string address;
	struct string ip;
	struct array tokens;

	struct string temp;

	string_init(&rack_name);
	string_init(&region_name);
	string_init(&port_str);
	string_init(&address);
	string_init(&ip);

	uint8_t *p, *q, *start;
	start = seeds->data;
	p = seeds->data + seeds->len - 1;
	q = dn_strrchr(p, start, '|');

	uint8_t *seed_node;
	uint32_t seed_node_len;

	while (q > start) {
		seed_node = q + 1;
		seed_node_len = (uint32_t)(p - seed_node + 1);
		string_copy(&temp, seed_node, seed_node_len);
		array_init(&tokens, 1, sizeof(struct dyn_token));
		parse_seeds(&temp, &rack_name, &region_name, &port_str, &address, &ip,  &tokens);
		//log_debug(LOG_VERB, "address          : '%.*s'", address.len, address.data);
		//log_debug(LOG_VERB, "rack_name         : '%.*s'", rack_name.len, rack_name.data);
		//log_debug(LOG_VERB, "region_name        : '%.*s'", region_name.len, region_name.data);
		//log_debug(LOG_VERB, "ip         : '%.*s'", ip.len, ip.data);


		gossip_add_seed_if_absent(sp, &rack_name, &address, &ip, &port_str, &tokens);

		p = q - 1;
		q = dn_strrchr(p, start, '|');
		string_deinit(&temp);
		array_deinit(&tokens);
		string_deinit(&rack_name);
		string_deinit(&region_name);
		string_deinit(&port_str);
		string_deinit(&address);
		string_deinit(&ip);
	}

	if (q == NULL) {
		seed_node_len = (uint32_t)(p - start + 1);
		seed_node = start;

		string_copy(&temp, seed_node, seed_node_len);
		array_init(&tokens, 1, sizeof(struct dyn_token));
		parse_seeds(&temp, &rack_name, &region_name, &port_str, &address, &ip, &tokens);
		gossip_add_seed_if_absent(sp, &rack_name, &address, &ip, &port_str, &tokens);
	}

	string_deinit(&temp);
	array_deinit(&tokens);
	string_deinit(&rack_name);
	string_deinit(&region_name);
	string_deinit(&port_str);
	string_deinit(&address);
	string_deinit(&ip);

	gossip_debug();
	return DN_OK;
}


static void *
gossip_loop(void *arg)
{
	struct server_pool *sp = arg;
	struct string seeds;
	uint64_t gossip_interval = gn_pool.g_interval * 1000;

	string_init(&seeds);

	for(;;) {
		usleep(gossip_interval);
		log_debug(LOG_VERB, "Gossip is running ...");
		//log_debug(LOG_VERB, "Sp addrstr  '%.*s'", sp->addrstr.len, sp->addrstr.data);
        //struct server *peer = (struct server *) array_get(&sp->peers, 0);

		//log_debug(LOG_VERB, "peer name == '%.*s' ", peer->name.len, peer->name.data);


		if (gn_pool.seeds_provider != NULL && gn_pool.seeds_provider(NULL, &seeds) == DN_OK) {
			log_debug(LOG_VERB, "Got seed nodes  '%.*s'", seeds.len, seeds.data);
			gossip_update_seeds(sp, &seeds);
			string_deinit(&seeds);
		}

		if (gn_pool.ctx->dyn_state == JOINING) {
			log_debug(LOG_VERB, "I am still joining......");
			//aggressively contact all known nodes before changing to state NORMAL
			gossip_announce_joining(sp);
		}

		gossip_process_messages();

		//loga("From gossip thread, Length of C2G_InQ ::: %d", CBUF_Len( C2G_InQ ));

		//while (!CBUF_IsEmpty(C2G_InQ)) {
		//char* s = (char*) CBUF_Pop( C2G_InQ );
		//loga("Gossip: %s", s);
		//dn_free(s);
		//}

		//void* ss = "hello main, from gossip";
		//CBUF_Push( C2G_OutQ, ss );


	}

	return NULL;
}


rstatus_t
gossip_start(struct server_pool *sp)
{
	rstatus_t status;
	pthread_t tid;

	status = pthread_create(&tid, NULL, gossip_loop, sp);
	if (status < 0) {
		log_error("gossip service create failed: %s", strerror(status));
		return DN_ERROR;
	}

	return DN_OK;
}


static void
gossip_set_seeds_provider(struct string * seeds_provider_str)
{
	log_debug(LOG_VERB, "Seed provider :::::: '%.*s'",
			seeds_provider_str->len, seeds_provider_str->data);

	if (dn_strncmp(seeds_provider_str->data, FLORIDA_PROVIDER, 16) == 0) {
		gn_pool.seeds_provider = florida_get_seeds;
	} else {
		gn_pool.seeds_provider = NULL;
	}
}



static rstatus_t
gossip_pool_each_init(void *elem, void *data)
{
	rstatus_t status;
	struct server_pool *sp = elem;

	gn_pool.ctx = sp->ctx;
	gn_pool.name = &sp->name;
	gn_pool.idx = sp->idx;
	gn_pool.g_interval = sp->g_interval;

	gossip_set_seeds_provider(&sp->seed_provider);

	uint32_t n_rack = array_n(&sp->racks);
	ASSERT(n_rack != 0);

	status = array_init(&gn_pool.racks, n_rack, sizeof(struct gossip_rack));
	if (status != DN_OK) {
		return status;
	}

	uint32_t i, nelem;
	for (i = 0, nelem = array_n(&sp->racks); i < nelem; i++) {
		struct rack *rack = (struct rack *) array_get(&sp->racks, i);
		struct gossip_rack *g_rack = array_push(&gn_pool.racks);
		gossip_rack_init(g_rack, rack->name);
	}

	for (i = 0, nelem = array_n(&sp->peers); i < nelem; i++) {
		struct server *peer = (struct server *) array_get(&sp->peers, i);
		//better to have a hash table here
		uint32_t j, nrack;
		for(j = 0, nrack = array_n(&gn_pool.racks); j < nrack; j++) {
			struct gossip_rack *g_rack = (struct gossip_rack *) array_get(&gn_pool.racks, j);

			if (string_compare(&peer->rack, &g_rack->name) == 0) {
				struct node *gnode = array_push(&g_rack->nodes);
				node_init(gnode);

				string_copy(&gnode->rack, g_rack->name.data, g_rack->name.len);
				string_copy(&gnode->name, peer->name.data, peer->name.len);
				string_copy(&gnode->pname, peer->pname.data, peer->pname.len); //ignore the port for now
				gnode->port = peer->port;


				struct dyn_token *ptoken = (struct dyn_token *) array_get(&peer->tokens, 0);
				copy_dyn_token(ptoken, &gnode->token);

				//copy socket stuffs

				g_rack->nnodes++;
			}
		}
	}

	gossip_debug();

	status = gossip_start(sp);
	if (status != DN_OK) {
		goto error;
	}

	return DN_OK;

	error:
	gossip_destroy(sp);
	return DN_OK;

}


rstatus_t
gossip_pool_init(struct context *ctx)
{
	rstatus_t status;
	status = array_each(&ctx->pool, gossip_pool_each_init, NULL);
	if (status != DN_OK) {
		return status;
	}

	return DN_OK;
}


void gossip_pool_deinit(struct context *ctx)
{

}


rstatus_t
gossip_destroy(struct server_pool *sp)
{
	return DN_OK;
}


void gossip_debug(void)
{
	log_debug(LOG_VERB, "Gossip dump.................................................");
	uint32_t i, nelem;
	for (i = 0, nelem = array_n(&gn_pool.racks); i < nelem; i++) {
		log_debug(LOG_VERB, "==============================================");
		struct gossip_rack *g_rack = (struct gossip_rack *) array_get(&gn_pool.racks, i);
		log_debug(LOG_VERB, "\tRACK name         : '%.*s'", g_rack->name.len, g_rack->name.data);
		log_debug(LOG_VERB, "\tNum nodes in RACK : '%d'", array_n(&g_rack->nodes));
		uint32_t jj;
		for (jj = 0; jj < array_n(&g_rack->nodes); jj++) {
			log_debug(LOG_VERB, "-----------------------------------------");
			struct node * node = (struct node *) array_get(&g_rack->nodes, jj);
			log_debug(LOG_VERB, "\t\tNode name          : '%.*s'", node->name);
			log_debug(LOG_VERB, "\t\tNode pname         : '%.*s'", node->pname);
			log_debug(LOG_VERB, "\t\tNode port          : %"PRIu32"", node->port);
			log_debug(LOG_VERB, "\t\tNode is_local      : %"PRIu32" ", node->is_local);
			log_debug(LOG_VERB, "\t\tNode last_retry    : %"PRIu32" ", node->last_retry);
			log_debug(LOG_VERB, "\t\tNode failure_count : %"PRIu32" ", node->failure_count);
			//uint32_t k;
			//for (k = 0; k < array_n(&node->tokens); k++) {
			//	struct dyn_token *token = (struct dyn_token *) array_get(&node->tokens, k);
			//	print_dyn_token(token, 8);
			//}

			print_dyn_token(&node->token, 8);
		}
	}
	log_debug(LOG_VERB, "...........................................................");
}


rstatus_t
gossip_peer_join(struct server_pool *sp, struct node *node)
{
        rstatus_t status;

        log_debug(LOG_VVERB, "Processing msg   gossip_peer_join '%.*s'", node->name.len, node->name.data);
        //status = dnode_peer_add_node(sp, node);

        return status;
}


