/*
 * Copyright (C) 2016 Swift Navigation Inc.
 * Contact: Jacob McNamee <jacob@swiftnav.com>
 *
 * This source is subject to the license found in the file 'LICENSE' which must
 * be be distributed together with this source. All other rights reserved.
 *
 * THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND,
 * EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <assert.h>

#include "zmq_router.h"

extern const router_t router_sbp;

static const router_t * const routers[] = {
  &router_sbp
};

static void router_setup(const router_t *router)
{
  for (int i=0; i<router->ports_count; i++) {
    port_t *port = &router->ports[i];
    port->pub_socket = zsock_new_pub(port->config.pub_addr);
    assert(port->pub_socket);
    port->sub_socket = zsock_new_sub(port->config.sub_addr, "");
    assert(port->sub_socket);
  }
}

static void routers_setup(const router_t * const routers[], int routers_count)
{
  for (int i=0; i<routers_count; i++) {
    router_setup(routers[i]);
  }
}

static void loop_add_router(zloop_t *loop, const router_t *router,
                            zloop_reader_fn reader_fn)
{
  for (int i=0; i<router->ports_count; i++) {
    port_t *port = &router->ports[i];
    int result;
    result = zloop_reader(loop, port->sub_socket, reader_fn, port);
    assert(result == 0);
  }
}

static void loop_setup(zloop_t *loop, const router_t * const routers[],
                       int routers_count, zloop_reader_fn reader_fn)
{
  for (int i=0; i<routers_count; i++) {
    loop_add_router(loop, routers[i], reader_fn);
  }
}

static int reader_fn(zloop_t *loop, zsock_t *reader, void *arg)
{
  port_t *port = (port_t *)arg;

  zmsg_t *rx_msg = zmsg_recv(port->sub_socket);
  assert(rx_msg);

  /* Get first frame for filtering */
  zframe_t *rx_frame_first = zmsg_first(rx_msg);
  const void *rx_prefix = NULL;
  int rx_prefix_len = 0;
  if (rx_frame_first != NULL) {
    rx_prefix = zframe_data(rx_frame_first);
    rx_prefix_len = zframe_size(rx_frame_first);
  }

  /* Iterate over forwarding rules */
  int rule_index = 0;
  while (1) {
    const forwarding_rule_t *forwarding_rule =
        port->config.sub_forwarding_rules[rule_index++];
    if (forwarding_rule == NULL) {
      break;
    }

    /* Iterate over filters for this rule */
    int filter_index = 0;
    while (1) {
      const filter_t *filter = forwarding_rule->filters[filter_index++];
      if (filter == NULL) {
        break;
      }

      bool match = false;

      /* Empty filter matches all */
      if (filter->len == 0) {
        match = true;
      } else if (rx_prefix != NULL) {
        if ((rx_prefix_len >= filter->len) &&
            (memcmp(rx_prefix, filter->data, filter->len) == 0)) {
          match = true;
        }
      }

      if (match) {
        switch (filter->action) {
          case FILTER_ACTION_ACCEPT: {
            zmsg_t *tx_msg = zmsg_dup(rx_msg);
            assert(tx_msg);
            int result = zmsg_send(&tx_msg, forwarding_rule->dst_port->pub_socket);
            assert(result == 0);
          }
          break;

          case FILTER_ACTION_REJECT: {

          }
          break;

          default: {
            assert(!"invalid filter action");
          }
          break;
        }

        /* Done with this rule after finding a filter match */
        break;
      }
    }
  }

  zmsg_destroy(&rx_msg);
  return 0;
}

int main (void)
{
  routers_setup(routers, sizeof(routers)/sizeof(routers[0]));

  zloop_t *loop = zloop_new();
  assert(loop);
  loop_setup(loop, routers, sizeof(routers)/sizeof(routers[0]), reader_fn);
  zloop_start(loop);

  return 0;
}
