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

#include <stdint.h>
#include <stdbool.h>

#include <czmq.h>

#define FILE_READ_BUF_SIZE 1024

static zsock_t *pub_socket;
static zsock_t *sub_socket;

static int fd;

static int socket_reader_fn(zloop_t *loop, zsock_t *reader, void *arg)
{
  printf("socket_reader_fn()\n");

  zmsg_t *msg = zmsg_recv(sub_socket);
  assert(msg);

  zframe_t *frame = zmsg_first(msg);
  while (frame != NULL) {
    const void *buf = zframe_data(frame);
    size_t buf_len = zframe_size(frame);
    ssize_t write_len = write(fd, buf, buf_len);
    frame = zmsg_next(msg);
  }

  return 0;
}

static int fd_reader_fn(zloop_t *loop, zmq_pollitem_t *item, void *arg)
{
  printf("fd_reader_fn()\n");

  int result;

  uint8_t buf[FILE_READ_BUF_SIZE];
  ssize_t buf_len = read(fd, buf, sizeof(buf));

  if (buf_len >= 0) {
    zmsg_t *msg = zmsg_new();
    result = zmsg_addmem(msg, buf, buf_len);
    assert(result == 0);
    result = zmsg_send(&msg, pub_socket);
    assert(result == 0);
  }

  return 0;
}

int main (int argc, char *argv[])
{
  if (argc < 4) {
    printf("usage: %s <zmq_sub_connect> <zmq_pub_connect> <filename>\n", argv[0]);
    exit(1);
  }

  const char *sub_addr = argv[1];
  const char *pub_addr = argv[2];
  const char *filename = argv[3];

  int result;

  pub_socket = zsock_new_pub(pub_addr);
  assert(pub_socket);
  sub_socket = zsock_new_sub(sub_addr, "");
  assert(sub_socket);

  zloop_t *loop = zloop_new();
  assert(loop);
zloop_set_verbose(loop, true);


  result = zloop_reader(loop, sub_socket, socket_reader_fn, NULL);
  assert(result == 0);

  fd = open(filename, O_RDWR | O_NONBLOCK | O_CREAT);
  assert(fd >= 0);

  zmq_pollitem_t fd_pollitem = {
    .socket = NULL,
    .fd = fd,
    .events = ZMQ_POLLIN
  };

  result = zloop_poller(loop, &fd_pollitem, fd_reader_fn, NULL);
  assert(result == 0);

  zloop_start(loop);

  return 0;
}
