/**
 * collectd - src/sysevent.c
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Red Hat NFVPE
 *     Andrew Bays <abays at redhat.com>
 **/

#include "collectd.h"

#include "common.h"
#include "plugin.h"
#include "utils_complain.h"
#include "utils_ignorelist.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>

#if HAVE_YAJL_YAJL_VERSION_H
#include <yajl/yajl_version.h>
#endif
#if defined(YAJL_MAJOR) && (YAJL_MAJOR > 1)
#include <yajl/yajl_tree.h>
#define HAVE_YAJL_V2 1
#endif

#define PROFILE_MICRO 1
#define PROFILE_MILLI 1000
#define PROFILE_SCALE PROFILE_MICRO

#define SYSEVENT_DOMAIN_FIELD "domain"
#define SYSEVENT_DOMAIN_VALUE "syslog"
#define SYSEVENT_EVENT_ID_FIELD "eventId"
#define SYSEVENT_EVENT_NAME_FIELD "eventName"
#define SYSEVENT_EVENT_NAME_VALUE "syslog message"
#define SYSEVENT_LAST_EPOCH_MICROSEC_FIELD "lastEpochMicrosec"
#define SYSEVENT_PRIORITY_FIELD "priority"
#define SYSEVENT_PRIORITY_VALUE_HIGH "high"
#define SYSEVENT_PRIORITY_VALUE_LOW "low"
#define SYSEVENT_PRIORITY_VALUE_MEDIUM "medium"
#define SYSEVENT_PRIORITY_VALUE_NORMAL "normal"
#define SYSEVENT_PRIORITY_VALUE_UNKNOWN "unknown"
#define SYSEVENT_REPORTING_ENTITY_NAME_FIELD "reportingEntityName"
#define SYSEVENT_REPORTING_ENTITY_NAME_VALUE "collectd sysevent plugin"
#define SYSEVENT_SEQUENCE_FIELD "sequence"
#define SYSEVENT_SEQUENCE_VALUE 0
#define SYSEVENT_SOURCE_NAME_FIELD "sourceName"
#define SYSEVENT_SOURCE_NAME_VALUE "syslog"
#define SYSEVENT_START_EPOCH_MICROSEC_FIELD "startEpochMicrosec"
#define SYSEVENT_VERSION_FIELD "version"
#define SYSEVENT_VERSION_VALUE 1.0

#define SYSEVENT_EVENT_SOURCE_HOST_FIELD "eventSourceHost"
#define SYSEVENT_EVENT_SOURCE_TYPE_FIELD "eventSourceType"
#define SYSEVENT_EVENT_SOURCE_TYPE_VALUE "host"
#define SYSEVENT_SYSLOG_FIELDS_FIELD "syslogFields"
#define SYSEVENT_SYSLOG_FIELDS_VERSION_FIELD "syslogFieldsVersion"
#define SYSEVENT_SYSLOG_FIELDS_VERSION_VALUE 1.0
#define SYSEVENT_SYSLOG_MSG_FIELD "syslogMsg"
#define SYSEVENT_SYSLOG_PROC_FIELD "syslogProc"
#define SYSEVENT_SYSLOG_SEV_FIELD "syslogSev"
#define SYSEVENT_SYSLOG_TAG_FIELD "syslogTag"
#define SYSEVENT_SYSLOG_TAG_VALUE "NILVALUE"

/*
 * Private data types
 */

typedef struct {
  int head;
  int tail;
  int maxLen;
  char **buffer;
  long long unsigned int *timestamp;
} circbuf_t;

/*
 * Private variables
 */
static ignorelist_t *ignorelist = NULL;

static int sysevent_thread_loop = 0;
static int sysevent_thread_error = 0;
static pthread_t sysevent_thread_id;
static pthread_mutex_t sysevent_lock = PTHREAD_MUTEX_INITIALIZER;
static int sock = -1;
static int event_id = 0;
static circbuf_t ring;

static char *listen_ip;
static char *listen_port;
static int listen_buffer_size = 4096;
static int buffer_length = 10;

static int monitor_all_messages = 1;
static char profile_scale[3];

#if HAVE_YAJL_V2
static const char *rsyslog_keys[3] = {"@timestamp", "@source_host", "@message"};
static const char *rsyslog_field_keys[5] = {
    "facility", "severity", "severity-num", "program", "processid"};
#endif

/*
 * Private functions
 */

static int gen_metadata_payload(const char *msg, char *sev, int sev_num,
                                char *process, char *host,
                                long long unsigned int timestamp,
                                notification_t *n) {
  char tmp_str[DATA_MAX_NAME_LEN];
  notification_meta_t *header = NULL;
  notification_meta_t *domain = NULL;

  // *** BEGIN common event header ***

  // Add the object as "ves" to the notification's meta (the notification's meta
  // will be created by this call, and it will be the VES header)

  if (plugin_notification_meta_add_nested(n, "ves") != 0)
    goto err;

  // Now populate the VES header, but first we need to acquire it
  if (plugin_notification_meta_get_meta_tail(n, &header) != 0)
    goto err;

  if (header == NULL) {
    ERROR(
        "sysevent plugin: gen_metadata_payload could not acquire VES header.");
    goto err;
  }

  // domain
  if (plugin_notification_meta_append_string(header, SYSEVENT_DOMAIN_FIELD,
                                             SYSEVENT_DOMAIN_VALUE) != 0)
    goto err;

  // eventId
  event_id = event_id + 1;

  if (plugin_notification_meta_append_unsigned_int(
          header, SYSEVENT_EVENT_ID_FIELD, event_id) != 0)
    goto err;

  // eventName
  int event_name_len = 0;
  event_name_len = event_name_len + strlen(host); // host name
  event_name_len =
      event_name_len +
      22; // "host", "rsyslog", "message", 3 spaces and null-terminator
  memset(tmp_str, '\0', DATA_MAX_NAME_LEN);
  snprintf(tmp_str, event_name_len, "host %s rsyslog message", host);

  if (plugin_notification_meta_append_string(header, SYSEVENT_EVENT_NAME_FIELD,
                                             tmp_str) != 0)
    goto err;

  // lastEpochMicrosec
  if (plugin_notification_meta_append_unsigned_int(
          header, SYSEVENT_LAST_EPOCH_MICROSEC_FIELD,
          (long long unsigned int)CDTIME_T_TO_US(cdtime())) != 0)
    goto err;

  // priority
  memset(tmp_str, '\0', DATA_MAX_NAME_LEN);

  switch (sev_num) {
  case 4:
    snprintf(tmp_str, strlen(SYSEVENT_PRIORITY_VALUE_MEDIUM), "%s",
             SYSEVENT_PRIORITY_VALUE_MEDIUM);
    break;
  case 5:
    snprintf(tmp_str, strlen(SYSEVENT_PRIORITY_VALUE_NORMAL), "%s",
             SYSEVENT_PRIORITY_VALUE_NORMAL);
    break;
  case 6:
  case 7:
    snprintf(tmp_str, strlen(SYSEVENT_PRIORITY_VALUE_LOW), "%s",
             SYSEVENT_PRIORITY_VALUE_LOW);
    break;
  default:
    snprintf(tmp_str, strlen(SYSEVENT_PRIORITY_VALUE_UNKNOWN), "%s",
             SYSEVENT_PRIORITY_VALUE_UNKNOWN);
    break;
  }

  if (plugin_notification_meta_append_string(header, SYSEVENT_PRIORITY_FIELD,
                                             tmp_str) != 0)
    goto err;

  // reportingEntityName
  if (plugin_notification_meta_append_string(
          header, SYSEVENT_REPORTING_ENTITY_NAME_FIELD,
          SYSEVENT_REPORTING_ENTITY_NAME_VALUE) != 0)

    // sequence
    if (plugin_notification_meta_append_unsigned_int(
            header, SYSEVENT_SEQUENCE_FIELD,
            (unsigned int)SYSEVENT_SEQUENCE_VALUE) != 0)
      goto err;

  // sourceName
  if (plugin_notification_meta_append_string(header, SYSEVENT_SOURCE_NAME_FIELD,
                                             process) != 0)

    // startEpochMicrosec
    if (plugin_notification_meta_append_unsigned_int(
            header, SYSEVENT_START_EPOCH_MICROSEC_FIELD,
            (long long unsigned int)timestamp) != 0)
      goto err;

  // version
  if (plugin_notification_meta_append_double(header, SYSEVENT_VERSION_FIELD,
                                             SYSEVENT_VERSION_VALUE) != 0)
    goto err;

  // *** END common event header ***

  // *** BEGIN syslog fields ***

  // Append a nested metadata object to header, with key as "syslogFields",
  // and then find it.  We will then append children data to it.

  if (plugin_notification_meta_append_nested(header,
                                             SYSEVENT_SYSLOG_FIELDS_FIELD) != 0)
    goto err;

  if (plugin_notification_meta_get_nested_tail(header, &domain) != 0)
    goto err;

  if (domain == NULL) {
    ERROR(
        "sysevent plugin: gen_metadata_payload could not acquire VES domain.");
    goto err;
  }

  // eventSourceHost
  if (plugin_notification_meta_append_string(
          domain, SYSEVENT_EVENT_SOURCE_HOST_FIELD, host) != 0)
    goto err;

  // eventSourceType
  if (plugin_notification_meta_append_string(
          domain, SYSEVENT_EVENT_SOURCE_TYPE_FIELD,
          SYSEVENT_EVENT_SOURCE_TYPE_VALUE) != 0)
    goto err;

  // syslogFieldsVersion
  if (plugin_notification_meta_append_double(
          domain, SYSEVENT_SYSLOG_FIELDS_VERSION_FIELD,
          SYSEVENT_SYSLOG_FIELDS_VERSION_VALUE) != 0)
    goto err;

  // syslogMsg
  if (plugin_notification_meta_append_string(domain, SYSEVENT_SYSLOG_MSG_FIELD,
                                             msg) != 0)
    goto err;

  // syslogProc
  if (plugin_notification_meta_append_string(domain, SYSEVENT_SYSLOG_PROC_FIELD,
                                             process) != 0)
    goto err;

  // syslogSev
  if (sev != NULL) {
    if (plugin_notification_meta_append_string(
            domain, SYSEVENT_SYSLOG_SEV_FIELD, sev) != 0)
      goto err;
  }

  // syslogTag
  if (plugin_notification_meta_append_string(domain, SYSEVENT_SYSLOG_TAG_FIELD,
                                             SYSEVENT_SYSLOG_TAG_VALUE) != 0)
    goto err;

  // *** END syslog fields ***

  return 0;

err:
  ERROR("sysevent plugin: gen_metadata_payload failed to generate JSON");
  return -1;
}

static void *sysevent_thread(void *arg) /* {{{ */
{
  pthread_mutex_lock(&sysevent_lock);

  while (sysevent_thread_loop > 0) {
    int status = 0;

    pthread_mutex_unlock(&sysevent_lock);

    if (sock == -1)
      return ((void *)0);

    char buffer[listen_buffer_size];
    struct sockaddr_storage src_addr;
    socklen_t src_addr_len = sizeof(src_addr);

    memset(buffer, '\0', listen_buffer_size);

    ssize_t count = recvfrom(sock, buffer, sizeof(buffer), 0,
                             (struct sockaddr *)&src_addr, &src_addr_len);

    if (count == -1) {
      ERROR("sysevent plugin: failed to receive data: %s", strerror(errno));
      status = -1;
    } else if (count >= sizeof(buffer)) {
      WARNING("sysevent plugin: datagram too large for buffer: truncated");
    } else {
      // 1. Acquire lock
      // 2. Push to buffer if there is room, otherwise raise warning

      pthread_mutex_lock(&sysevent_lock);

      int next = ring.head + 1;
      if (next >= ring.maxLen)
        next = 0;

      if (next == ring.tail) {
        WARNING("sysevent plugin: ring buffer full");
      } else {
        DEBUG("sysevent plugin: writing %s", buffer);

        strncpy(ring.buffer[ring.head], buffer, sizeof(buffer));
        ring.timestamp[ring.head] =
            (long long unsigned int)CDTIME_T_TO_US(cdtime());
        ring.head = next;
      }

      pthread_mutex_unlock(&sysevent_lock);
    }

    usleep(1000);

    pthread_mutex_lock(&sysevent_lock);

    if (status < 0) {
      WARNING("sysevent plugin: problem with thread status: %d", status);
      sysevent_thread_error = 1;
      break;
    }

    if (sysevent_thread_loop <= 0)
      break;
  } /* while (sysevent_thread_loop > 0) */

  pthread_mutex_unlock(&sysevent_lock);

  // pthread_exit instead of return?
  return ((void *)0);
} /* }}} void *sysevent_thread */

static int start_thread(void) /* {{{ */
{
  int status;

  pthread_mutex_lock(&sysevent_lock);

  if (sysevent_thread_loop != 0) {
    pthread_mutex_unlock(&sysevent_lock);
    return (0);
  }

  sysevent_thread_loop = 1;
  sysevent_thread_error = 0;

  DEBUG("sysevent plugin: starting thread");

  status = plugin_thread_create(&sysevent_thread_id, /* attr = */ NULL,
                                sysevent_thread,
                                /* arg = */ (void *)0, "sysevent");
  if (status != 0) {
    sysevent_thread_loop = 0;
    ERROR("sysevent plugin: starting thread failed.");
    pthread_mutex_unlock(&sysevent_lock);
    return (-1);
  }

  pthread_mutex_unlock(&sysevent_lock);
  return (0);
} /* }}} int start_thread */

static int stop_thread(int shutdown) /* {{{ */
{
  int status;

  pthread_mutex_lock(&sysevent_lock);

  if (sysevent_thread_loop == 0) {
    pthread_mutex_unlock(&sysevent_lock);
    return (-1);
  }

  sysevent_thread_loop = 0;
  pthread_mutex_unlock(&sysevent_lock);

  if (shutdown == 1) {
    // Since the thread is blocking, calling pthread_join
    // doesn't actually succeed in stopping it.  It will stick around
    // until a message is received on the socket (at which
    // it will realize that "sysevent_thread_loop" is 0 and will
    // break out of the read loop and be allowed to die).  This is
    // fine when the process isn't supposed to be exiting, but in
    // the case of a process shutdown, we don't want to have an
    // idle thread hanging around.  Calling pthread_cancel here in
    // the case of a shutdown is just assures that the thread is
    // gone and that the process has been fully terminated.

    DEBUG("sysevent plugin: Canceling thread for process shutdown");

    status = pthread_cancel(sysevent_thread_id);

    if (status != 0) {
      ERROR("sysevent plugin: Unable to cancel thread: %d (%s)", status,
            strerror(errno));
      status = -1;
    }
  } else {
    status = pthread_join(sysevent_thread_id, /* return = */ NULL);
    if (status != 0) {
      ERROR("sysevent plugin: Stopping thread failed.");
      status = -1;
    }
  }

  pthread_mutex_lock(&sysevent_lock);
  memset(&sysevent_thread_id, 0, sizeof(sysevent_thread_id));
  sysevent_thread_error = 0;
  pthread_mutex_unlock(&sysevent_lock);

  DEBUG("sysevent plugin: Finished requesting stop of thread");

  return (status);
} /* }}} int stop_thread */

static int sysevent_init(void) /* {{{ */
{
  if (PROFILE_SCALE == 1)
    sstrncpy(profile_scale, "us\0", sizeof(profile_scale));
  else if (PROFILE_SCALE == 1000)
    sstrncpy(profile_scale, "ms\0", sizeof(profile_scale));
  else
    sstrncpy(profile_scale, "??\0", sizeof(profile_scale));

  ring.head = 0;
  ring.tail = 0;
  ring.maxLen = buffer_length;
  ring.buffer = (char **)malloc(buffer_length * sizeof(char *));

  for (int i = 0; i < buffer_length; i++) {
    ring.buffer[i] = malloc(listen_buffer_size);
  }

  ring.timestamp = (long long unsigned int *)malloc(
      buffer_length * sizeof(long long unsigned int));

  if (sock == -1) {
    const char *hostname = listen_ip;
    const char *portname = listen_port;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = 0;
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
    struct addrinfo *res = 0;

    int err = getaddrinfo(hostname, portname, &hints, &res);

    if (err != 0) {
      ERROR("sysevent plugin: failed to resolve local socket address (err=%d)",
            err);
      freeaddrinfo(res);
      return (-1);
    }

    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == -1) {
      ERROR("sysevent plugin: failed to open socket: %s", strerror(errno));
      freeaddrinfo(res);
      return (-1);
    }

    if (bind(sock, res->ai_addr, res->ai_addrlen) == -1) {
      ERROR("sysevent plugin: failed to bind socket: %s", strerror(errno));
      freeaddrinfo(res);
      return (-1);
    }

    freeaddrinfo(res);
  }

  DEBUG("sysevent plugin: socket created and bound");

  return (start_thread());
} /* }}} int sysevent_init */

static int sysevent_config_add_listen(const oconfig_item_t *ci) /* {{{ */
{
  if (ci->values_num != 2 || ci->values[0].type != OCONFIG_TYPE_STRING ||
      ci->values[1].type != OCONFIG_TYPE_STRING) {
    ERROR("sysevent plugin: The `%s' config option needs "
          "two string arguments (ip and port).",
          ci->key);
    return (-1);
  }

  listen_ip = strdup(ci->values[0].value.string);
  listen_port = strdup(ci->values[1].value.string);

  return (0);
}

static int sysevent_config_add_buffer_size(const oconfig_item_t *ci) /* {{{ */
{
  int tmp = 0;

  if (cf_util_get_int(ci, &tmp) != 0)
    return (-1);
  else if ((tmp >= 1024) && (tmp <= 65535))
    listen_buffer_size = tmp;
  else {
    WARNING(
        "sysevent plugin: The `BufferSize' must be between 1024 and 65535.");
    return (-1);
  }

  return (0);
}

static int sysevent_config_add_buffer_length(const oconfig_item_t *ci) /* {{{ */
{
  int tmp = 0;

  if (cf_util_get_int(ci, &tmp) != 0)
    return (-1);
  else if ((tmp >= 3) && (tmp <= 4096))
    buffer_length = tmp;
  else {
    WARNING("sysevent plugin: The `Bufferlength' must be between 3 and 4096.");
    return (-1);
  }

  return (0);
}

static int sysevent_config_add_regex_filter(const oconfig_item_t *ci) /* {{{ */
{
  if (ci->values_num != 1 || ci->values[0].type != OCONFIG_TYPE_STRING) {
    ERROR("sysevent plugin: The `%s' config option needs "
          "one string argument, a regular expression.",
          ci->key);
    return (-1);
  }

#if HAVE_REGEX_H
  if (ignorelist == NULL)
    ignorelist = ignorelist_create(/* invert = */ 1);

  int status = ignorelist_add(ignorelist, ci->values[0].value.string);

  if (status != 0) {
    ERROR("sysevent plugin: invalid regular expression: %s",
          ci->values[0].value.string);
    return (1);
  }

  monitor_all_messages = 0;
#else
  WARNING("sysevent plugin: The plugin has been compiled without support "
          "for the \"RegexFilter\" option.");
#endif

  return (0);
}

static int sysevent_config(oconfig_item_t *ci) /* {{{ */
{
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("Listen", child->key) == 0)
      sysevent_config_add_listen(child);
    else if (strcasecmp("BufferSize", child->key) == 0)
      sysevent_config_add_buffer_size(child);
    else if (strcasecmp("BufferLength", child->key) == 0)
      sysevent_config_add_buffer_length(child);
    else if (strcasecmp("RegexFilter", child->key) == 0)
      sysevent_config_add_regex_filter(child);
    else {
      WARNING("sysevent plugin: Option `%s' is not allowed here.", child->key);
    }
  }

  return (0);
} /* }}} int sysevent_config */

static void sysevent_dispatch_notification(const char *message,
#if HAVE_YAJL_V2
                                           yajl_val *node,
#endif
                                           long long unsigned int timestamp) {
  long long unsigned int before = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;
  notification_t n = {NOTIF_OKAY, cdtime(), "", "",  "sysevent",
                      "",         "",       "", NULL};
  pid_t tid = syscall(__NR_gettid);
#if HAVE_YAJL_V2
  if (node != NULL) {
    // If we have a parsed-JSON node to work with, use that

    char process[listen_buffer_size];
    char severity[listen_buffer_size];
    char sev_num_str[listen_buffer_size];
    char msg[listen_buffer_size];
    char hostname_str[listen_buffer_size];
    int sev_num = -1;

    // msg
    const char *msg_path[] = {rsyslog_keys[2], (const char *)0};
    yajl_val msg_v = yajl_tree_get(*node, msg_path, yajl_t_string);

    if (msg_v != NULL) {
      memset(msg, '\0', listen_buffer_size);
      snprintf(msg, listen_buffer_size, "%s%c", YAJL_GET_STRING(msg_v), '\0');
    }

    // severity
    const char *severity_path[] = {"@fields", rsyslog_field_keys[1],
                                   (const char *)0};
    yajl_val severity_v = yajl_tree_get(*node, severity_path, yajl_t_string);

    if (severity_v != NULL) {
      memset(severity, '\0', listen_buffer_size);
      snprintf(severity, listen_buffer_size, "%s%c",
               YAJL_GET_STRING(severity_v), '\0');
    }

    // sev_num
    const char *sev_num_str_path[] = {"@fields", rsyslog_field_keys[2],
                                      (const char *)0};
    yajl_val sev_num_str_v =
        yajl_tree_get(*node, sev_num_str_path, yajl_t_string);

    if (sev_num_str_v != NULL) {
      memset(sev_num_str, '\0', listen_buffer_size);
      snprintf(sev_num_str, listen_buffer_size, "%s%c",
               YAJL_GET_STRING(sev_num_str_v), '\0');

      sev_num = atoi(sev_num_str);

      if (sev_num < 4)
        n.severity = NOTIF_FAILURE;
    }

    // process
    const char *process_path[] = {"@fields", rsyslog_field_keys[3],
                                  (const char *)0};
    yajl_val process_v = yajl_tree_get(*node, process_path, yajl_t_string);

    if (process_v != NULL) {
      memset(process, '\0', listen_buffer_size);
      snprintf(process, listen_buffer_size, "%s%c", YAJL_GET_STRING(process_v),
               '\0');
    }

    // hostname
    const char *hostname_path[] = {rsyslog_keys[1], (const char *)0};
    yajl_val hostname_v = yajl_tree_get(*node, hostname_path, yajl_t_string);

    if (hostname_v != NULL) {
      memset(hostname_str, '\0', listen_buffer_size);
      snprintf(hostname_str, listen_buffer_size, "%s%c",
               YAJL_GET_STRING(hostname_v), '\0');
    }

    gen_metadata_payload(
        (msg_v != NULL ? msg : NULL), (severity_v != NULL ? severity : NULL),
        (sev_num_str_v != NULL ? sev_num : -1),
        (process_v != NULL ? process : NULL),
        (hostname_v != NULL ? hostname_str : hostname_g), timestamp, &n);
  } else {
    // Data was not sent in JSON format, so just treat the whole log entry
    // as the message (and we'll be unable to acquire certain data, so the
    // payload
    // generated below will be less informative)

    gen_metadata_payload(message, NULL, -1, NULL, hostname_g, timestamp, &n);
  }
#else
  gen_metadata_payload(message, NULL, -1, NULL, hostname_g, timestamp, &n);
#endif

  long long unsigned int after = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;

  if (after - before > 1000)
    WARNING("AJB (%d) sysevent sysevent_dispatch_gen_payload_DIFF: %llu %s", tid, after-before, profile_scale);

  sstrncpy(n.host, hostname_g, sizeof(n.host));
  sstrncpy(n.type, "gauge", sizeof(n.type));

  DEBUG("sysevent plugin: dispatching message");

  before = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;
  plugin_dispatch_notification(&n);
  after = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;
  if (after - before > 1000)
    WARNING("AJB (%d) sysevent sysevent_dispatch_dispatch_DIFF: %llu %s", tid, after-before, profile_scale);

  plugin_notification_meta_free(n.meta);
}

static int sysevent_read(void) /* {{{ */
{
  if (sysevent_thread_error != 0) {
    ERROR("sysevent plugin: The sysevent thread had a problem (%d). Restarting "
          "it.",
          sysevent_thread_error);

    stop_thread(0);

    start_thread();

    return (-1);
  } /* if (sysevent_thread_error != 0) */

  long long unsigned int before;
  long long unsigned int after;
  long long unsigned int after_lock;
  long long unsigned int after_unlock;
  long long unsigned int loop;

  pid_t tid = syscall(__NR_gettid);

  before = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;
  pthread_mutex_lock(&sysevent_lock);

  after_lock = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;

  if (after_lock - before > 1000)
    WARNING("AJB (%d) sysevent sysevent_read_ring_loop_lock_acq_DIFF: %llu %s", tid, after_lock-before, profile_scale);

  loop = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;

  while (ring.head != ring.tail) {
    long long unsigned int timestamp;
    int is_match = 1;
    char *match_str = NULL;
    int next = ring.tail + 1;

    if (next >= ring.maxLen)
      next = 0;

    DEBUG("sysevent plugin: reading from ring buffer: %s",
          ring.buffer[ring.tail]);

    timestamp = ring.timestamp[ring.tail];

#if HAVE_YAJL_V2
    // Try to parse JSON, and if it fails, fall back to plain string
    yajl_val node = NULL;
    char errbuf[1024];
    errbuf[0] = 0;
    node = yajl_tree_parse((const char *)ring.buffer[ring.tail], errbuf,
                           sizeof(errbuf));

    if (node != NULL) {
      // JSON rsyslog data

      // If we have any regex filters, we need to see if the message portion of
      // the data matches any of them (otherwise we're not interested)
      if (monitor_all_messages == 0) {
        char json_val[listen_buffer_size];
        const char *path[] = {"@message", (const char *)0};
        yajl_val v = yajl_tree_get(node, path, yajl_t_string);

        memset(json_val, '\0', listen_buffer_size);

        snprintf(json_val, listen_buffer_size, "%s%c", YAJL_GET_STRING(v),
                 '\0');

        match_str = (char *)&json_val;
      }
    } else {
      // non-JSON rsyslog data

      // If we have any regex filters, we need to see if the message data
      // matches any of them (otherwise we're not interested)
      if (monitor_all_messages == 0)
        match_str = ring.buffer[ring.tail];
    }
#else
    // If we have any regex filters, we need to see if the message data
    // matches any of them (otherwise we're not interested)
    if (monitor_all_messages == 0)
      match_str = ring.buffer[ring.tail];
#endif

    // If we care about matching, do that comparison here
    if (match_str != NULL) {
      is_match = 1;

      if (ignorelist_match(ignorelist, match_str) != 0)
        is_match = 0;
      else
        DEBUG("sysevent plugin: regex filter match");
    }

#if HAVE_YAJL_V2
    if (is_match == 1 && node != NULL) {
      long long unsigned int before2 = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;
      sysevent_dispatch_notification(NULL, &node, timestamp);
      long long unsigned int after2 = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;
      WARNING("AJB (%d) sysevent node_dispatch_DIFF: %llu %s", tid, after2-before2, profile_scale);
      yajl_tree_free(node);
    } else if (is_match == 1) {
      long long unsigned int before2 = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;
      sysevent_dispatch_notification(ring.buffer[ring.tail], NULL, timestamp);
      long long unsigned int after2 = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;
      WARNING("AJB (%d) sysevent raw_dispatch_DIFF: %llu %s", tid, after2-before2, profile_scale);
    }
#else
    if (is_match == 1)
    {
      long long unsigned int before2 = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;
      sysevent_dispatch_notification(ring.buffer[ring.tail], timestamp);
      long long unsigned int after2 = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;
      WARNING("AJB (%d) sysevent raw_dispatch_DIFF: %llu %s", tid, after2-before2, profile_scale);
    }
#endif

    ring.tail = next;
  }

  after = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;
    
  if (after - loop > 1000)
  {
    WARNING("AJB (%d) sysevent sysevent_read_ring_loop_DIFF: %llu %s", tid, after-loop, profile_scale);
  }

  pthread_mutex_unlock(&sysevent_lock);

  after_unlock = (long long unsigned int)CDTIME_T_TO_US(cdtime())/PROFILE_SCALE;

  if (after_unlock - after_lock > 1000)
  {
    WARNING("AJB (%d) sysevent sysevent_read_ring_lock_rel_DIFF: %llu %s", tid, after_unlock-after_lock, profile_scale);
  }

  return (0);
} /* }}} int sysevent_read */

static int sysevent_shutdown(void) /* {{{ */
{
  int status;

  DEBUG("sysevent plugin: Shutting down thread.");
  if (stop_thread(1) < 0)
    return (-1);

  if (sock != -1) {
    status = close(sock);
    if (status != 0) {
      ERROR("sysevent plugin: failed to close socket %d: %d (%s)", sock, status,
            strerror(errno));
      return (-1);
    } else
      sock = -1;
  }

  free(listen_ip);
  free(listen_port);

  for (int i = 0; i < buffer_length; i++) {
    free(ring.buffer[i]);
  }

  free(ring.buffer);
  free(ring.timestamp);

  return (0);
} /* }}} int sysevent_shutdown */

void module_register(void) {
  plugin_register_complex_config("sysevent", sysevent_config);
  plugin_register_init("sysevent", sysevent_init);
  plugin_register_read("sysevent", sysevent_read);
  plugin_register_shutdown("sysevent", sysevent_shutdown);
} /* void module_register */
