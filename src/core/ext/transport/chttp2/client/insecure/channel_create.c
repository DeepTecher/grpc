/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <grpc/grpc.h>

#include <stdlib.h>
#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/slice.h>
#include <grpc/support/slice_buffer.h>

#include "src/core/ext/client_channel/client_channel.h"
#include "src/core/ext/client_channel/http_connect_handshaker.h"
#include "src/core/ext/client_channel/resolver_registry.h"
#include "src/core/ext/transport/chttp2/transport/chttp2_transport.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/compress_filter.h"
#include "src/core/lib/channel/handshaker.h"
#include "src/core/lib/channel/http_client_filter.h"
#include "src/core/lib/iomgr/tcp_client.h"
#include "src/core/lib/surface/api_trace.h"
#include "src/core/lib/surface/channel.h"

//
// connector
//

typedef struct {
  grpc_connector base;
  gpr_refcount refs;

  grpc_closure *notify;
  grpc_connect_in_args args;
  grpc_connect_out_args *result;
  grpc_closure initial_string_sent;
  gpr_slice_buffer initial_string_buffer;

  grpc_endpoint *tcp;

  grpc_closure connected;

  grpc_handshake_manager *handshake_mgr;
} connector;

static void connector_ref(grpc_connector *con) {
  connector *c = (connector *)con;
  gpr_ref(&c->refs);
}

static void connector_unref(grpc_exec_ctx *exec_ctx, grpc_connector *con) {
  connector *c = (connector *)con;
  if (gpr_unref(&c->refs)) {
    /* c->initial_string_buffer does not need to be destroyed */
    grpc_handshake_manager_destroy(exec_ctx, c->handshake_mgr);
    gpr_free(c);
  }
}

static void on_initial_connect_string_sent(grpc_exec_ctx *exec_ctx, void *arg,
                                           grpc_error *error) {
  connector_unref(exec_ctx, arg);
}

static void on_handshake_done(grpc_exec_ctx *exec_ctx, grpc_endpoint *endpoint,
                              grpc_channel_args *args,
                              gpr_slice_buffer *read_buffer, void *user_data,
                              grpc_error *error) {
  connector *c = user_data;
  if (error != GRPC_ERROR_NONE) {
    grpc_channel_args_destroy(args);
    gpr_free(read_buffer);
  } else {
    c->result->transport =
        grpc_create_chttp2_transport(exec_ctx, args, endpoint, 1);
    GPR_ASSERT(c->result->transport);
    grpc_chttp2_transport_start_reading(exec_ctx, c->result->transport,
                                        read_buffer);
    c->result->channel_args = args;
  }
  grpc_closure *notify = c->notify;
  c->notify = NULL;
  grpc_exec_ctx_sched(exec_ctx, notify, error, NULL);
}

static void connected(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error) {
  connector *c = arg;
  grpc_endpoint *tcp = c->tcp;
  if (tcp != NULL) {
    if (!GPR_SLICE_IS_EMPTY(c->args.initial_connect_string)) {
      grpc_closure_init(&c->initial_string_sent, on_initial_connect_string_sent,
                        c);
      gpr_slice_buffer_init(&c->initial_string_buffer);
      gpr_slice_buffer_add(&c->initial_string_buffer,
                           c->args.initial_connect_string);
      connector_ref(arg);
      grpc_endpoint_write(exec_ctx, tcp, &c->initial_string_buffer,
                          &c->initial_string_sent);
    } else {
      grpc_handshake_manager_do_handshake(
          exec_ctx, c->handshake_mgr, tcp, c->args.channel_args,
          c->args.deadline, NULL /* acceptor */, on_handshake_done, c);
    }
  } else {
    memset(c->result, 0, sizeof(*c->result));
    grpc_closure *notify = c->notify;
    c->notify = NULL;
    grpc_exec_ctx_sched(exec_ctx, notify, GRPC_ERROR_REF(error), NULL);
  }
}

static void connector_shutdown(grpc_exec_ctx *exec_ctx, grpc_connector *con) {}

static void connector_connect(grpc_exec_ctx *exec_ctx, grpc_connector *con,
                              const grpc_connect_in_args *args,
                              grpc_connect_out_args *result,
                              grpc_closure *notify) {
  connector *c = (connector *)con;
  GPR_ASSERT(c->notify == NULL);
  GPR_ASSERT(notify->cb);
  c->notify = notify;
  c->args = *args;
  c->result = result;
  c->tcp = NULL;
  grpc_closure_init(&c->connected, connected, c);
  grpc_tcp_client_connect(exec_ctx, &c->connected, &c->tcp,
                          args->interested_parties, args->channel_args,
                          args->addr, args->deadline);
}

static const grpc_connector_vtable connector_vtable = {
    connector_ref, connector_unref, connector_shutdown, connector_connect};

//
// client_channel_factory
//

static void client_channel_factory_ref(
    grpc_client_channel_factory *cc_factory) {}

static void client_channel_factory_unref(
    grpc_exec_ctx *exec_ctx, grpc_client_channel_factory *cc_factory) {}

static grpc_subchannel *client_channel_factory_create_subchannel(
    grpc_exec_ctx *exec_ctx, grpc_client_channel_factory *cc_factory,
    const grpc_subchannel_args *args) {
  connector *c = gpr_malloc(sizeof(*c));
  memset(c, 0, sizeof(*c));
  c->base.vtable = &connector_vtable;
  gpr_ref_init(&c->refs, 1);
  c->handshake_mgr = grpc_handshake_manager_create();
  char *proxy_name = grpc_get_http_proxy_server();
  if (proxy_name != NULL) {
    grpc_handshake_manager_add(
        c->handshake_mgr,
        grpc_http_connect_handshaker_create(proxy_name, args->server_name));
    gpr_free(proxy_name);
  }
  grpc_subchannel *s = grpc_subchannel_create(exec_ctx, &c->base, args);
  grpc_connector_unref(exec_ctx, &c->base);
  return s;
}

static grpc_channel *client_channel_factory_create_channel(
    grpc_exec_ctx *exec_ctx, grpc_client_channel_factory *cc_factory,
    const char *target, grpc_client_channel_type type,
    const grpc_channel_args *args) {
  grpc_channel *channel =
      grpc_channel_create(exec_ctx, target, args, GRPC_CLIENT_CHANNEL, NULL);
  grpc_resolver *resolver = grpc_resolver_create(target, args);
  if (!resolver) {
    GRPC_CHANNEL_INTERNAL_UNREF(exec_ctx, channel,
                                "client_channel_factory_create_channel");
    return NULL;
  }

  grpc_client_channel_finish_initialization(
      exec_ctx, grpc_channel_get_channel_stack(channel), resolver, cc_factory);
  GRPC_RESOLVER_UNREF(exec_ctx, resolver, "create_channel");

  return channel;
}

static const grpc_client_channel_factory_vtable client_channel_factory_vtable =
    {client_channel_factory_ref, client_channel_factory_unref,
     client_channel_factory_create_subchannel,
     client_channel_factory_create_channel};

static grpc_client_channel_factory client_channel_factory = {
    &client_channel_factory_vtable};

/* Create a client channel:
   Asynchronously: - resolve target
                   - connect to it (trying alternatives as presented)
                   - perform handshakes */
grpc_channel *grpc_insecure_channel_create(const char *target,
                                           const grpc_channel_args *args,
                                           void *reserved) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  GRPC_API_TRACE(
      "grpc_insecure_channel_create(target=%p, args=%p, reserved=%p)", 3,
      (target, args, reserved));
  GPR_ASSERT(!reserved);

  grpc_client_channel_factory *factory =
      (grpc_client_channel_factory *)&client_channel_factory;
  grpc_channel *channel = client_channel_factory_create_channel(
      &exec_ctx, factory, target, GRPC_CLIENT_CHANNEL_TYPE_REGULAR, args);

  grpc_client_channel_factory_unref(&exec_ctx, factory);
  grpc_exec_ctx_finish(&exec_ctx);

  return channel != NULL ? channel : grpc_lame_client_channel_create(
                                         target, GRPC_STATUS_INTERNAL,
                                         "Failed to create client channel");
}
