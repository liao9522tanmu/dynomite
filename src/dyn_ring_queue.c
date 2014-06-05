/*
 * dyn_ring_queue.c
 *
 *  Created on: May 31, 2014
 *      Author: mdo
 */

#include "dyn_core.h"
#include "dyn_ring_queue.h"
#include "hashkit/dyn_token.h"


struct ring_message *
create_ring_message()
{
	struct ring_message *result = nc_alloc(sizeof(*result));
	ring_message_init(result);

	return result;
}

rstatus_t
ring_message_init(struct ring_message *msg)
{
	if (msg == NULL)
		return NC_ERROR;

	msg->node = nc_alloc(sizeof(struct node));

	return NC_OK;
}


rstatus_t
ring_message_deinit(struct ring_message *msg)
{
	if (msg == NULL)
		return NC_ERROR;

    nc_free(msg->node);
    nc_free(msg);

    return NC_OK;
}



struct node *
create_node()
{
    struct node *result = nc_alloc(sizeof(*result));
    node_init(result);

    return result;
}


rstatus_t
node_init(struct node *node)
{
     if (node == NULL)
    	 return NC_ERROR;

     array_init(&node->tokens, 1, sizeof(struct dyn_token));
     string_init(&node->dc);
     string_init(&node->name);
     string_init(&node->pname);

     node->port = 0;

     node->next_retry = 0;
     node->last_retry = 0;
     node->failure_count = 0;

     node->is_seed = false;
     node->is_local = false;
     node->status = 2;

     return NC_OK;
}


rstatus_t
node_deinit(struct node *node)
{
     if (node == NULL)
    	 return NC_ERROR;

     array_deinit(&node->tokens);
     string_deinit(&node->dc);
     string_deinit(&node->name);
     string_deinit(&node->pname);

     nc_free(node);

     return NC_OK;
}


rstatus_t
node_copy(const struct node *src, struct node *dst)
{
     if (src == NULL || dst == NULL)
    	 return NC_ERROR;

     dst->status = src->status;
     dst->is_local = src->is_local;
     dst->is_seed = src->is_seed;
     dst->failure_count = src->failure_count;
     dst->last_retry = src->last_retry;
     dst->next_retry = src->next_retry;
     dst->port = src->port;

     string_copy(&dst->pname, src->pname.data, src->pname.len);
     string_copy(&dst->name, src->name.data, src->name.len);
     string_copy(&dst->dc, src->dc.data, src->dc.len);

     uint32_t i, nelem;
     for (i = 0, nelem = array_n(&src->tokens); i < nelem; i++) {
         	struct dyn_token *src_token = (struct dyn_token *) array_get(&src->tokens, i);
         	struct dyn_token *dst_token = array_push(&dst->tokens);
         	copy_dyn_token(src_token, dst_token);
     }

}
