/*
 * Copyright (C) 2017 Swift Navigation Inc.
 * Contact: Swift Navigation <dev@swiftnav.com>
 *
 * This source is subject to the license found in the file 'LICENSE' which must
 * be be distributed together with this source. All other rights reserved.
 *
 * THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND,
 * EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <curl/curl.h>
#include <czmq.h>
#include <float.h>
#include <getopt.h>
#include <libnetwork.h>
#include <libpiksi/sbp_zmq_pubsub.h>
#include <libpiksi/sbp_zmq_rx.h>
#include <libpiksi/util.h>
#include <libpiksi/logging.h>
#include <libsbp/navigation.h>
#include <libsbp/system.h>
#include <libsbp/sbp.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sbp.h"

#define PROGRAM_NAME "geoid_daemon"

#define SBP_SUB_ENDPOINT    ">tcp://127.0.0.1:43090"
#define SBP_PUB_ENDPOINT    ">tcp://127.0.0.1:43091"

static bool debug = false;

static void usage(char *command)
{
  printf("Usage: %s\n", command);

  puts("\nMain options");

  puts("\nMisc options");
  puts("\t--debug");
}

static int parse_options(int argc, char *argv[])
{
  enum {
    OPT_ID_FILE = 1,
    OPT_ID_URL,
    OPT_ID_DEBUG,
  };

  const struct option long_opts[] = {
    {"debug", no_argument,       0, OPT_ID_DEBUG},
    {0, 0, 0, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
    switch (opt) {
      case OPT_ID_DEBUG: {
        debug = true;
      }
      break;

      default: {
        puts("Invalid option");
        return -1;
      }
      break;
    }
  }

  return 0;
}

struct string {
  char *ptr;
  size_t len;
};

static void init_string(struct string *s) {
  s->len = 0;
  s->ptr = malloc(s->len+1);
  if (s->ptr == NULL) {
    fputs("malloc() failed\n", stderr);
    exit(EXIT_FAILURE);
  }
  s->ptr[0] = '\0';
}

static void heartbeat_callback(u16 sender_id, u8 len, u8 msg[], void *context)
{
  (void) context;
  (void) sender_id;
  (void) len;
  (void) msg;

  puts("heartbeat\n");
}

struct thread_info {
  pthread_mutex_t mutex;
  msg_pos_llh_t pos;
  double offset;
  bool stop;
};

static void pos_llh_callback(u16 sender_id, u8 len, u8 msg[], void *context)
{
  (void) sender_id;
  (void) len;
  (void) context;
  struct thread_info *ti = (struct thread_info *)context;
  msg_pos_llh_t *pos = (msg_pos_llh_t*)msg;

  pthread_mutex_lock(&ti->mutex);
  double adjusted = pos->height - ti->offset;
  memcpy(&ti->pos, pos, sizeof(*pos));
  pthread_mutex_unlock(&ti->mutex);

  printf("pos llh %f %f %f\nadjusted height %f\n", pos->lat, pos->lon, pos->height, adjusted);

  unsigned int mode = pos->flags & 0x07;
  if (mode != 0) {
    pos->height = adjusted;
  }

  sbp_message_send(SBP_MSG_POS_LLH, sizeof(*pos), (u8 *)pos);
}

static size_t curl_write_func(void *ptr, size_t size, size_t nmemb, struct string *s)
{
  size_t new_len = s->len + size * nmemb;
  s->ptr = realloc(s->ptr, new_len+1);
  if (s->ptr == NULL) {
    fputs("realloc() failed\n", stderr);
    exit(EXIT_FAILURE);
  }
  memcpy(s->ptr+s->len, ptr, size*nmemb);
  s->ptr[new_len] = '\0';
  s->len = new_len;

  return size * nmemb;
}

static void *curl_thread_func(void *arg) {
  struct thread_info *ti = (struct thread_info *)arg;

  CURL *curl;
  CURLcode res;
  char errbuf[CURL_ERROR_SIZE];

  curl = curl_easy_init();
  if (curl == NULL) {
    exit(EXIT_FAILURE);
  }

  while (true) {

    pthread_mutex_lock(&ti->mutex);

    if (ti->stop) {
      pthread_mutex_unlock(&ti->mutex);
      break;
    }

    unsigned int mode = ti->pos.flags & 0x07;
    if (mode == 0) {
      ti->offset = 0;
      pthread_mutex_unlock(&ti->mutex);
      sleep(1);
      continue;
    }

    struct string response;
    init_string(&response);

    char url[256];
    sprintf(url,
      "http://skylark-geoid-web-2rkjl64-496683068.us-west-2.elb.amazonaws.com/geoid?lat=%f&lon=%f",
      ti->pos.lat, ti->pos.lon);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_func);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      fputs("http failure\n", stderr);
      size_t len = strlen(errbuf);
      if(len) {
        fprintf(stderr, "%s%s", errbuf,
                ((errbuf[len - 1] != '\n') ? "\n" : ""));
      }
      else {
        fprintf(stderr, "%s\n", curl_easy_strerror(res));
      }

      exit(EXIT_FAILURE);
    }

    ti->offset = atof(response.ptr);
    printf("offset %f\n", ti->offset);

    pthread_mutex_unlock(&ti->mutex);

    sleep(1);
  }

  curl_easy_cleanup(curl);

  return NULL;
}

int main(int argc, char *argv[])
{
  if (parse_options(argc, argv) != 0) {
    usage(argv[0]);
    exit(EXIT_FAILURE);
  }

  puts("Hello!\n");

  /* Prevent czmq from catching signals */
  zsys_handler_set(NULL);

  sbp_zmq_pubsub_ctx_t *ctx = sbp_zmq_pubsub_create(SBP_PUB_ENDPOINT,
                                                    SBP_SUB_ENDPOINT);
  if (ctx == NULL) {
    exit(EXIT_FAILURE);
  }

  struct thread_info ti;
  memset(&ti, 0, sizeof(ti));
  pthread_mutex_init(&ti.mutex, NULL);

  pthread_t curl_thread;
  if (pthread_create(&curl_thread, NULL, curl_thread_func, &ti) != 0) {
      fputs("thread create failure\n", stderr);
  }


  if (sbp_init(sbp_zmq_pubsub_rx_ctx_get(ctx),
               sbp_zmq_pubsub_tx_ctx_get(ctx)) != 0) {
    piksi_log(LOG_ERR, "error initializing SBP");
    exit(EXIT_FAILURE);
  }

  if (sbp_callback_register(SBP_MSG_HEARTBEAT, heartbeat_callback, NULL) != 0) {
    piksi_log(LOG_ERR, "error setting MSG_HEARTBEAT callback");
    exit(EXIT_FAILURE);
  }

  if (sbp_callback_register(SBP_MSG_POS_LLH, pos_llh_callback, &ti) != 0) {
    piksi_log(LOG_ERR, "error setting MSG_POS_LLH callback");
    exit(EXIT_FAILURE);
  }

  puts("Ready!\n");
  zmq_simple_loop(sbp_zmq_pubsub_zloop_get(ctx));

  pthread_mutex_lock(&ti.mutex);
  ti.stop = true;
  pthread_mutex_unlock(&ti.mutex);

  pthread_join(curl_thread, NULL);

  pthread_mutex_destroy(&ti.mutex);

  sbp_zmq_pubsub_destroy(&ctx);

  exit(EXIT_SUCCESS);
}
