/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include "elliptics.h"
#include "dnet/interface.h"

static struct dnet_node *dnet_node_alloc(struct dnet_config *cfg)
{
	struct dnet_node *n;
	int err;

	n = malloc(sizeof(struct dnet_node));
	if (!n)
		return NULL;

	memset(n, 0, sizeof(struct dnet_node));

	n->trans = 0;
	n->trans_root = RB_ROOT;

	err = dnet_log_init(n, cfg->log_private, cfg->log_mask, cfg->log, cfg->log_append);
	if (err)
		goto err_out_free;

	err = pthread_mutex_init(&n->state_lock, NULL);
	if (err) {
		dnet_log_err(n, "Failed to initialize state lock: err: %d", err);
		goto err_out_free;
	}

	err = pthread_mutex_init(&n->trans_lock, NULL);
	if (err) {
		dnet_log_err(n, "Failed to initialize transaction lock: err: %d", err);
		goto err_out_destroy_state;
	}

	err = pthread_mutex_init(&n->tlock, NULL);
	if (err) {
		dnet_log_err(n, "Failed to initialize transformation lock: err: %d", err);
		goto err_out_destroy_trans;
	}

	n->wait = dnet_wait_alloc(0);
	if (!n->wait) {
		dnet_log(n, DNET_LOG_ERROR, "Failed to allocate wait structure.\n");
		goto err_out_destroy_tlock;
	}

	INIT_LIST_HEAD(&n->tlist);
	INIT_LIST_HEAD(&n->state_list);
	INIT_LIST_HEAD(&n->empty_state_list);

	return n;

err_out_destroy_tlock:
	pthread_mutex_destroy(&n->tlock);
err_out_destroy_trans:
	pthread_mutex_destroy(&n->trans_lock);
err_out_destroy_state:
	pthread_mutex_destroy(&n->state_lock);
err_out_free:
	free(n);
	return NULL;
}

void dnet_state_remove(struct dnet_net_state *st)
{
	struct dnet_node *n = st->n;

	pthread_mutex_lock(&n->state_lock);
	list_del(&st->state_entry);
	pthread_mutex_unlock(&n->state_lock);
}

int dnet_state_insert(struct dnet_net_state *new)
{
	struct dnet_node *n = new->n;
	struct dnet_net_state *st;
	int err = 1;

	pthread_mutex_lock(&n->state_lock);

	list_for_each_entry(st, &n->state_list, state_entry) {
		err = dnet_id_cmp(st->id, new->id);

		dnet_log(n, DNET_LOG_NOTICE, "st: %s, ", dnet_dump_id(st->id));
		dnet_log_append(n, DNET_LOG_NOTICE, "new: %s, cmp: %d.\n", dnet_dump_id(new->id), err);

		if (!err) {
			dnet_log(n, DNET_LOG_ERROR, "%s: state exists: old: %s, ", dnet_dump_id(new->id),
				dnet_server_convert_dnet_addr(&st->addr));
			dnet_log_append(n, DNET_LOG_ERROR, "new: %s.\n", dnet_server_convert_dnet_addr(&new->addr));
			break;
		}

		if (err < 0) {
			dnet_log(n, DNET_LOG_NOTICE, "adding %s before %s.\n", dnet_server_convert_dnet_addr(&new->addr), dnet_dump_id(st->id));
			list_add_tail(&new->state_entry, &st->state_entry);
			break;
		}
	}

	if (err > 0) {
		dnet_log(n, DNET_LOG_NOTICE, "adding %s to the end.\n", dnet_server_convert_dnet_addr(&new->addr));
		list_add_tail(&new->state_entry, &n->state_list);
	}

	if (err) {
		dnet_log(n, DNET_LOG_NOTICE, "%s: node list dump:\n", dnet_dump_id(new->id));
		list_for_each_entry(st, &n->state_list, state_entry) {
			dnet_log(n, DNET_LOG_NOTICE, "      id: %s [%02x], addr: %s.\n", dnet_dump_id(st->id), st->id[0],
				dnet_server_convert_dnet_addr(&st->addr));
		}
		dnet_log_append(n, DNET_LOG_NOTICE, "\n");
	}

	pthread_mutex_unlock(&n->state_lock);

	if (!err)
		err = -EEXIST;
	else
		err = 0;

	return err;
}

static struct dnet_net_state *__dnet_state_search(struct dnet_node *n, unsigned char *id, struct dnet_net_state *self)
{
	struct dnet_net_state *st = NULL;
	int err = 1;

	list_for_each_entry(st, &n->state_list, state_entry) {
		if (st == self)
			continue;

		err = dnet_id_cmp(st->id, id);

		//dnet_log(n, DNET_LOG_INFO, "id: %02x, state: %02x, err: %d.\n", id[0], st->id[0], err);

		if (err <= 0) {
			dnet_state_get(st);
			break;
		}
	}

	if (err >= 0)
		st = NULL;

	return st;
}

struct dnet_net_state *dnet_state_search(struct dnet_node *n, unsigned char *id, struct dnet_net_state *self)
{
	struct dnet_net_state *st;

	pthread_mutex_lock(&n->state_lock);
	st = __dnet_state_search(n, id, self);
	pthread_mutex_unlock(&n->state_lock);

	return st;
}

struct dnet_net_state *dnet_state_get_first(struct dnet_node *n, unsigned char *id, struct dnet_net_state *self)
{
	struct dnet_net_state *st = NULL;
	int err = 0;

	pthread_mutex_lock(&n->state_lock);
	st = __dnet_state_search(n, id, self);

	if (!st) {
		err = -ENOENT;
		list_for_each_entry(st, &n->state_list, state_entry) {
			if (st == self)
				continue;

			dnet_state_get(st);
			err = 0;
			break;
		}
	}
	pthread_mutex_unlock(&n->state_lock);

	if (err)
		return NULL;

	return st;
}

static void *dnet_server_func(void *data)
{
	struct dnet_net_state *main_st = data;
	struct dnet_net_state *st;
	struct dnet_node *n = main_st->n;
	int cs;
	struct dnet_addr addr;

	while (!n->need_exit) {
		addr.addr_len = sizeof(addr.addr);
		cs = accept(n->listen_socket, (struct sockaddr *)&addr.addr, &addr.addr_len);
		if (cs <= 0) {
			dnet_log_err(n, "%s: failed to accept new client", dnet_dump_id(n->id));
			continue;
		}

		dnet_log(n, DNET_LOG_INFO, "%s: accepted client %s.\n", dnet_dump_id(n->id),
				dnet_server_convert_dnet_addr(&addr));

		fcntl(cs, F_SETFL, O_NONBLOCK);

		st = dnet_state_create(n, NULL, &addr, cs, dnet_state_process);
		if (!st) {
			close(cs);
			dnet_log(n, DNET_LOG_INFO, "%s: disconnected client %s.\n", dnet_dump_id(n->id),
				dnet_server_convert_dnet_addr(&addr));
		}
	}

	return NULL;
}

struct dnet_node *dnet_node_create(struct dnet_config *cfg)
{
	struct dnet_node *n;
	int err = -ENOMEM;

	if (cfg->join && !cfg->command_handler) {
		err = -EINVAL;
		if (cfg->log)
			cfg->log(cfg->log_private, DNET_LOG_ERROR, "%s: joining node has to register "
					"a comamnd handler.\n",
					dnet_dump_id(cfg->id));
		goto err_out_exit;
	}

	n = dnet_node_alloc(cfg);
	if (!n) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	memcpy(n->id, cfg->id, DNET_ID_SIZE);
	n->proto = cfg->proto;
	n->sock_type = cfg->sock_type;
	n->wait_ts.tv_sec = cfg->wait_timeout;
	n->command_handler = cfg->command_handler;
	n->command_private = cfg->command_private;

	n->addr.addr_len = sizeof(n->addr.addr);

	err = dnet_socket_create(n, cfg, (struct sockaddr *)&n->addr.addr, &n->addr.addr_len, 1);
	if (err < 0)
		goto err_out_free;

	n->listen_socket = err;

	n->st = dnet_state_create(n, (cfg->join)?n->id:NULL, &n->addr, n->listen_socket, dnet_server_func);
	if (!n->st)
		goto err_out_sock_close;

	dnet_log(n, DNET_LOG_INFO, "%s: new node has been created at %s, id_size: %u.\n",
			dnet_dump_id(n->id), dnet_dump_node(n), DNET_ID_SIZE);
	return n;

err_out_sock_close:
	close(n->listen_socket);
err_out_free:
	free(n);
err_out_exit:
	return NULL;
}

void dnet_node_destroy(struct dnet_node *n)
{
	struct dnet_net_state *st, *tmp;

	dnet_log(n, DNET_LOG_INFO, "%s: destroying node at %s.\n", dnet_dump_id(n->id), dnet_dump_node(n));

	pthread_mutex_lock(&n->state_lock);
	list_for_each_entry_safe(st, tmp, &n->state_list, state_entry) {
		list_del(&st->state_entry);
		pthread_join(st->tid, NULL);

		dnet_state_put(st);
	}
	pthread_mutex_unlock(&n->state_lock);

	close(n->listen_socket);

	pthread_mutex_destroy(&n->state_lock);
	pthread_mutex_destroy(&n->trans_lock);
	pthread_mutex_destroy(&n->tlock);

	dnet_wait_put(n->wait);

	free(n);
}

