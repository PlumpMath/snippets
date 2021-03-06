/* Simple message queue implementation using UNIX domain socket.
 * Copyright (C) 2010  Seong-Kook Shin <cinsky@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>

#include <error.h>

#include <pthread.h>

#include "msgq.h"
#include "elist.h"
#ifdef MSGQ_BROADCAST
#include "sglob.h"
#endif  /* MSGQ_BROADCAST */

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   (sizeof(struct sockaddr_un) - sizeof(sa_family_t))
#endif

#define sizeof_packet(packet)   (sizeof(*(packet)) + (packet)->size)

/*
 * All received message is packaged in struct msgq_node.  This struct
 * provides 'link' so that each struct can be wired in a doubly linked list
 * Users cannot see this struct -- users can see only the 'packet' member,
 * which is struct msgq_packet instance.
 *
 * Both struct msgq_node instance and its packet member are
 * dynamically allocated.  Once the users got the message (e.g. using
 * msgq_recv()), We do not keep a pointer to struct msgq_node
 * instance.  Instead, the 'container' member of the struct
 * msgq_packet('packet') will point the enclosing struct msgq_node.
 * See the source of msgq_pkt_delete() for more.
 */
struct msgq_node {
  struct elist link;            /* for the doubly linked list */
  char sender[UNIX_PATH_MAX];   /* sender address for 'packet */
  struct msgq_packet *packet;   /* the actual message */
};


#define MSGQ_STAT_NONE  -1
#define MSGQ_STAT_INIT  0
#define MSGQ_STAT_ALIVE 1
#define MSGQ_STAT_DEAD  2

/*
 * Currently, 'recv_mutex' is the only mutex that struct msgq_ provided.
 * Since we do not have any other queue except the 'recvq' member,  one
 * mutex is sufficient for now.
 *
 * 'recv_cond' is using together with 'recv_mutex' if needed.
 * 'recv_cond' wakes any caller that is waiting for more message,
 * specifically, msgq_recv_wait().
 */
struct msgq_ {
  int fd;
  char address[UNIX_PATH_MAX];

  unsigned char *pkbuf;         /* internal buffer to receive a message */

  int broadcast;                /* use ptheread_cond_broatcast() if nonzero */

  struct elist recvq;           /* queue for received messages */
  size_t recvs;                 /* number of packets in recvq */


  pthread_cond_t recv_cond;     /* condition for waiting for incoming msg */
  pthread_mutex_t recv_mutex;

  pthread_cond_t stat_cond;
  //pthread_mutex_t stat_mutex;
  int receiver_status;          /* receiver status, MSGQ_STAT_* */

  pthread_t receiver;           /* thread for receiving messages */
};

#define MSGQ_LOCK(msgq)        LOCK(&(msgq)->recv_mutex, "recv")
#define MSGQ_UNLOCK(msgq)      UNLOCK(&(msgq)->recv_mutex, "recv")


static int validate_packet(struct msgq_packet *packet, ssize_t len);
static int msgq_start_receiver(MSGQ *msgq);
static void *msgq_receiver(void *arg);
static int msgq_get_listener(MSGQ *msgq, const char *address);

static int bind_anonymous(int fd, char address[]);
static struct msgq_packet *msgq_copy_packet(const struct msgq_packet *packet);
static struct msgq_node *msgq_node_create(const char *sender,
                                          const struct msgq_packet *packet);

static int gettime(struct timespec *res);
static int timespec_subtract(struct timespec *result,
                             struct timespec *x, struct timespec *y);

#ifndef NDEBUG
static void verror_(const char *kind, int status, int errnum,
                    const char *fmt, va_list ap);
static void warning_(int errnum, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));
static void debug_(int errnum, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

# define DEBUG(err, ...)        debug_(err, __VA_ARGS__)
# define WARN(err, ...)         warning_(err, __VA_ARGS__)
#else
# define DEBUG(err, ...)        ((void)0)
# define WARN(err, ...)         ((void)0)
#endif  /* NDEBUG */


static __inline__ void
LOCK(pthread_mutex_t *mutex, const char *id)
{
  int ret = pthread_mutex_lock(mutex);
  if (ret != 0) {
    DEBUG(ret, "pthread_mutex_lock[%s] failed!", id);
    abort();
  }
}


static __inline__ void
UNLOCK(pthread_mutex_t *mutex, const char *id)
{
  int ret = pthread_mutex_unlock(mutex);
  if (ret != 0) {
    DEBUG(ret, "pthread_mutex_unlock[%s] failed!", id);
    abort();
  }
}


static int
gettime(struct timespec *res)
{
  int ret;
#if _POSIX_C_SOURCE < 199309L
  struct timeval tv;
  if ((ret = gettimeofday(&tv, NULL)) == 0) {
    res.tv_sec = tv.tv_sec;
    res.tv_nsec = tv.tv_usec * 1000;
  }
#else
  ret = clock_gettime(CLOCK_REALTIME, res);
#endif  /* _POSIX_C_SOURCE */

  return ret;
}

/*
 * If nonzero, block all signals (using sigfillset()) before creating
 * the receiver thread.  The reason is that I do not know that the
 * exact meaning of 'all signals' in sigfillset context.  Is sigfillset()
 * fill *really* all signals?   I remembered that pthread uses some real-time
 * signals internally (don't remember the name of the signal though).
 *
 * Frankly, I don't think that calling sigfillset() for blocking
 * signals will cause trouble later.  Anyway, if 'block_all_signals'
 * is zero, the receiver thread will block only certain signals.  See
 * msgq_start_receiver() for more.
 */
static int block_all_signals = 1;

int
msgq_send_string(MSGQ *msgq, const char *receiver, const char *fmt, ...)
{
  va_list ap;
  int ret;
  struct msgq_packet *p;

  va_start(ap, fmt);
  ret = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  p = malloc(sizeof(*p) + ret + 1);
  if (!p)
    return -1;

  va_start(ap, fmt);
  vsnprintf(p->data, ret + 1, fmt, ap);
  va_end(ap);
  p->size = ret + 1;
  p->container = NULL;

  ret = msgq_send_(msgq, receiver, p);
  free(p);
  return ret;
}


int
msgq_message_count(MSGQ *msgq)
{
  int ret;

  MSGQ_LOCK(msgq);
  ret = msgq->recvs;
  MSGQ_UNLOCK(msgq);

  return ret;
}


const char *
msgq_pkt_sender(struct msgq_packet *packet)
{
  struct msgq_node *np;
  if (!packet->container)
    return NULL;
  np = (struct msgq_node *)packet->container;
  return np->sender;
}


int
msgq_pkt_delete(struct msgq_packet *packet)
{
  struct msgq_node *np;
  if (!packet || !packet->container)
    return -1;
  np = (struct msgq_node *)packet->container;

  assert(ELIST_NEXT(np->link) == 0);
  assert(ELIST_PREV(np->link) == 0);

  free(packet);
  free(np);
  return 0;
}


/*
 * Wait for the status change of the listener thread.
 *
 * The internal listener thread changes its status from
 * MSGQ_STAT_INIT, then MSGQ_STAT_ALIVE to MSGQ_STAT_DEAD.  This
 * function blocks the caller until the listener status is reached to
 * STATUS.
 *
 * If the current listener status is the same as STATUS or, STATUS is
 * already passed, this function returns immediately.
 *
 * This function returns the current status of the listener thread.
 * On error, it returns -1, and set 'errno' accordingly.
 *
 * If MSGQ_STAT_NONE is used for STATUS, this function returns
 * immediately with the current status without blocking.
 */
static int
msgq_wait(MSGQ *msgq, int status)
{
  int saved_errno = 0;
  int ret;

  assert(status >= MSGQ_STAT_NONE && status <= MSGQ_STAT_DEAD);

  MSGQ_LOCK(msgq);
  if (status == MSGQ_STAT_NONE)
    status = msgq->receiver_status;
  else if (status <= msgq->receiver_status)
    status = msgq->receiver_status;
  else {
    while (msgq->receiver_status < status) {
      ret = pthread_cond_wait(&msgq->stat_cond, &msgq->recv_mutex);
      if (ret != 0) {
        saved_errno = ret;
        break;
      }
    }
    status = msgq->receiver_status;
  }
  MSGQ_UNLOCK(msgq);

  if (saved_errno) {
    errno = saved_errno;
    return -1;
  }

  return status;
}


struct msgq_packet *
msgq_recv_wait(MSGQ *msgq)
{
  return msgq_recv_timedwait(msgq, NULL);
}


struct msgq_packet *
msgq_recv_timedwait(MSGQ *msgq, struct timespec *abstime)
{
  struct elist *p;
  struct msgq_node *np;
  struct timespec now, diff;
  int ret;

  MSGQ_LOCK(msgq);

  while (1) {
    p = edque_pop_front(&msgq->recvq);
    if (p)
      break;

    if (msgq->receiver_status == MSGQ_STAT_DEAD) {
      WARN(0, "msgq_recv_wait: lister is dead. no more packet available!");
      errno = EADDRNOTAVAIL;
      goto just_end;
    }

    DEBUG(0, "msgq_recv_wait: waiting...");
    if (abstime) {
      ret = pthread_cond_timedwait(&msgq->recv_cond, &msgq->recv_mutex,
                                   abstime);
      if (ret != 0)
        DEBUG(ret, "msgq_recv_wait: pthread_cond_timedwait(3) failed");
    }
    else {
      ret = pthread_cond_wait(&msgq->recv_cond, &msgq->recv_mutex);
      if (ret != 0)
        DEBUG(ret, "msgq_recv_wait: pthread_cond_wait(3) failed");
    }
    DEBUG(0, "msgq_recv_wait: awaken!");

    /* The mutex is locked state. */

    if (ret != 0) {
      errno = ret;
      goto just_end;            /* error or timeout */
    }

    if (abstime) {
      /* On spurious wakeup, if ABSTIME is given, and it is timed out,
       * then escape the loop. */
      gettime(&now);
      if (timespec_subtract(&diff, &now, abstime) == 0)
        goto just_end;          /* timeout */
    }
  }

  msgq->recvs--;
  MSGQ_UNLOCK(msgq);

  np = ELIST_ENTRY(p, struct msgq_node, link);
  return np->packet;

 just_end:
  /* You should not call any function that possibly modify 'errno' here. */

  /* flow comes here if conditional waiting function failed, or if
   * timed-out. */

  MSGQ_UNLOCK(msgq);
  return NULL;
}


#if 0
/* Subtract the `struct timeval' values X and Y, storing the result in
 * RESULT.  Return 1 if the difference is negative, otherwise 0.  */
static int
timeval_subtract(struct timeval *result, struct timeval *x, struct timeval *y)
{
  /* Perform the carry for the later subtraction by updating y. */
  if (x->tv_usec < y->tv_usec) {
    int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
    y->tv_usec -= 1000000 * nsec;
    y->tv_sec += nsec;
  }
  if (x->tv_usec - y->tv_usec > 1000000) {
    int nsec = (x->tv_usec - y->tv_usec) / 1000000;
    y->tv_usec += 1000000 * nsec;
    y->tv_sec -= nsec;
  }

  /* Compute the time remaining to wait.
     tv_usec is certainly positive. */
  result->tv_sec = x->tv_sec - y->tv_sec;
  result->tv_usec = x->tv_usec - y->tv_usec;

  /* Return 1 if result is negative. */
  return x->tv_sec < y->tv_sec;
}
#endif  /* 0 */

/*
 * Subtract the `struct timespec' values X and Y, storing the result in
 * RESULT.  Return 1 if the difference is negative, otherwise 0.
 *
 * Stealed from GLIBC manual.
 */
static int
timespec_subtract(struct timespec *result,
                  struct timespec *x, struct timespec *y)
{
  /* Perform the carry for the later subtraction by updating y. */
  if (x->tv_nsec < y->tv_nsec) {
    int nsec = (y->tv_nsec - x->tv_nsec) / 1000000000 + 1;
    y->tv_nsec -= 1000000000 * nsec;
    y->tv_sec += nsec;
  }
  if (x->tv_nsec - y->tv_nsec > 1000000000) {
    int nsec = (x->tv_nsec - y->tv_nsec) / 1000000000;
    y->tv_nsec += 1000000000 * nsec;
    y->tv_sec -= nsec;
  }

  /* Compute the time remaining to wait.
     tv_usec is certainly positive. */
  result->tv_sec = x->tv_sec - y->tv_sec;
  result->tv_nsec = x->tv_nsec - y->tv_nsec;

  /* Return 1 if result is negative. */
  return x->tv_sec < y->tv_sec;
}

struct msgq_packet *
msgq_recv(MSGQ *msgq)
{
  struct elist *p;
  struct msgq_node *np;

  MSGQ_LOCK(msgq);
  p = edque_pop_front(&msgq->recvq);
  if (!p) {
    MSGQ_UNLOCK(msgq);
    return NULL;
  }
  msgq->recvs--;
  MSGQ_UNLOCK(msgq);

  np = ELIST_ENTRY(p, struct msgq_node, link);
  return np->packet;
}


int
msgq_send(MSGQ *msgq, const char *receiver,
          const void *packet, size_t size)
{
  struct msgq_packet *p;
  int ret;

  p = malloc(sizeof(*p) + size);
  if (!p)
    return -1;

  p->container = 0;
  p->size = size;
  memcpy(p->data, packet, size);

  ret = msgq_send_(msgq, receiver, p);
  free(p);

  return ret;
}


int
msgq_send_(MSGQ *msgq, const char *receiver, const struct msgq_packet *packet)
{
  struct sockaddr_un addr;
  ssize_t ret;

  addr.sun_family = AF_LOCAL;
  strncpy(addr.sun_path, receiver, sizeof(addr.sun_path) - 1);

  /* TODO: lock?? */
  ret = sendto(msgq->fd, packet, sizeof_packet(packet), MSG_NOSIGNAL,
               (struct sockaddr *)&addr, sizeof(addr));
  /* TODO: unlock?? */

  if (ret < 0) {
    WARN(errno, "sendto(2) failed");
    return -1;
  }

  return 0;
}


#ifdef MSGQ_BROADCAST
int
msgq_broadcast_string_wildcard(MSGQ *msgq, const char *pattern,
                               const char *fmt, ...)
{
  va_list ap;
  int ret;
  struct msgq_packet *p;

  va_start(ap, fmt);
  ret = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  p = malloc(sizeof(*p) + ret + 1);
  if (!p)
    return -1;
  va_start(ap, fmt);
  vsnprintf(p->data, ret + 1, fmt, ap);
  va_end(ap);
  p->size = ret + 1;
  p->container = NULL;

  msgq_broadcast_wildcard(msgq, pattern, p);

  free(p);
  return ret;
}


int
msgq_broadcast_wildcard(MSGQ *msgq, const char *pattern,
                         const struct msgq_packet *packet)
{
  sglob_t gbuf;
  int i;

  gbuf.mask = S_IFSOCK;

  if (sglob(pattern, SGLOB_MASK, &gbuf) < 0)
    return -1;

  for (i = 0; i < gbuf.pathc; i++) {
    DEBUG(0, "msgq_broadcast_wildcard: to |%s|...", gbuf.pathv[i]);
    msgq_send_(msgq, gbuf.pathv[i], packet);
  }

  sglobfree(&gbuf);
  return 0;
}
#endif  /* MSGQ_BROADCAST */


MSGQ *
msgq_open(const char *address)
{
  MSGQ *p;
  pthread_mutexattr_t attr;
  int saved_errno = 0;

  p = malloc(sizeof(*p));
  if (!p)
    return NULL;

  memset(p, 0, sizeof(*p));

  p->pkbuf = malloc(MSGQ_MSG_MAX);
  if (!p->pkbuf) {
    saved_errno = errno;
    free(p);
    errno = saved_errno;
    return NULL;
  }

  p->fd = -1;
  ELIST_INIT(p->recvq);
  p->recvs = 0;
  p->broadcast = 0;

  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);

  if ((saved_errno = pthread_mutex_init(&p->recv_mutex, &attr)) != 0) {
    WARN(errno, "pthread_mutex_init(3) failed");
    pthread_mutexattr_destroy(&attr);
    goto err_free_livemutex;
  }
  pthread_mutexattr_destroy(&attr);

  if ((saved_errno = pthread_cond_init(&p->recv_cond, NULL)) != 0) {
    WARN(errno, "pthread_cond_init(3) failed");
    goto err_cond_recv;
  }

  if ((saved_errno = pthread_cond_init(&p->stat_cond, NULL)) != 0) {
    WARN(errno, "pthread_cond_init(3) failed");
    goto err_cond;
  }

  MSGQ_LOCK(p);
  p->receiver_status = MSGQ_STAT_INIT;

  if (msgq_get_listener(p, address) < 0) {
    saved_errno = errno;
    goto err;
  }
  if (msgq_start_receiver(p) < 0) {
    saved_errno = errno;
    goto err;
  }
  MSGQ_UNLOCK(p);

  msgq_wait(p, MSGQ_STAT_ALIVE);
  return p;

 err:
  pthread_cond_destroy(&p->stat_cond);
 err_cond:
  pthread_cond_destroy(&p->recv_cond);
 err_cond_recv:
  MSGQ_UNLOCK(p);
  if (p->fd >= 0)
    close(p->fd);
  pthread_mutex_destroy(&p->recv_mutex);
 err_free_livemutex:
  if (p->pkbuf)
    free(p->pkbuf);
  if (p)
    free(p);
  errno = saved_errno;
  return NULL;
}


int
msgq_close(MSGQ *msgq)
{
  void *retval;
  struct elist *p;
  struct msgq_node *np;
  int saved_errno;

#if 0
  if ((saved_errno = pthread_cancel(msgq->receiver)) != 0) {
    WARN(saved_errno, "invalid thread id, MSGQ corrupted!");
    errno = saved_errno;
    return -1;
  }
#endif  /* 0 */

  msgq_send_string(msgq, msgq->address, "shutdown");

  if ((saved_errno = pthread_join(msgq->receiver, &retval)) != 0) {
    WARN(saved_errno, "pthread_join() failed");
    errno = saved_errno;
    return -1;
  }

  MSGQ_LOCK(msgq);

  if (msgq->fd >= 0) {
    close(msgq->fd);
    msgq->fd = -1;
  }

  if (msgq->pkbuf) {
    free(msgq->pkbuf);
    msgq->pkbuf = NULL;
  }

  /* TODO: delete all remaining packets??? */
  DEBUG(0, "%u packet(s) will be destroyed", msgq->recvs);

  while ((p = edque_pop_front(&msgq->recvq)) != NULL) {
    np = ELIST_ENTRY(p, struct msgq_node, link);
    msgq->recvs--;

    DEBUG(0, "\tdestroying packet from %s...", np->sender);
    free(np->packet);
    free(np);
  }

  MSGQ_UNLOCK(msgq);

  /* TODO: possible race condition? */
  pthread_mutex_destroy(&msgq->recv_mutex);
  free(msgq);

  return 0;
}


static int
msgq_get_listener(MSGQ *msgq, const char *address)
{
  struct stat sbuf;
  struct sockaddr_un addr;
  int fd = -1;
  mode_t oldmask;
  socklen_t size;
  int saved_errno;

  assert(msgq->fd == -1);

  fd = socket(AF_LOCAL, SOCK_DGRAM, 0);
  if (fd < 0) {
    WARN(errno, "socket(2) failed");
    goto err;
  }

  if (!address) {
    if (bind_anonymous(fd, msgq->address) < 0) {
      goto err;
    }
  }
  else {
    if (stat(address, &sbuf) == 0) {
      if (!S_ISSOCK(sbuf.st_mode)) {
        WARN(0, "file (%s) already exists", address);
        goto err;
      }
      unlink(address);
    }

    addr.sun_family = AF_LOCAL;
    strncpy(addr.sun_path, address, UNIX_PATH_MAX - 1);
    size = offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      WARN(errno, "bind(2) failed");
      goto err;
    }
    strncpy(msgq->address, address, UNIX_PATH_MAX - 1);

    /*
     * I don't know why, but on my Linux system(Gentoo 2.6.32, glib
     * 2.11.2), fchmod() on socket fd has no effect without any error.
     * chmod(2) seems to work though.
     */
    oldmask = umask(0);
    if (chmod(address, MSGQ_PERM_DEFAULT) < 0) {
    //if (fchmod(fd, MSGQ_PERM_DEFAULT) < 0) {
      ;          /* ignore any error */
    }
    umask(oldmask);
  }

  msgq->fd = fd;
  fsync(fd);
  return 0;

 err:
  saved_errno = errno;
  if (fd >= 0)
    close(fd);
  errno = saved_errno;
  return -1;
}


static int
bind_anonymous(int fd, char address[])
{
  struct sockaddr_un addr;
  char tmpname[sizeof(MSGQ_TMP_TEMPLATE)];
  int tfd;

  assert(fd >= 0);

  while (1) {
    strcpy(tmpname, MSGQ_TMP_TEMPLATE);
    tfd = mkstemp(tmpname);
    if (tfd < 0) {
      WARN(errno, "mkstemp(3) failed");
      break;
    }
    close(tfd);

    addr.sun_family = AF_LOCAL;
    strcpy(addr.sun_path, tmpname);

    unlink(tmpname);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      if (errno != EADDRINUSE) {
        WARN(errno, "bind(2) failed");
        break;
      }
    }
    else {
      strncpy(address, tmpname, UNIX_PATH_MAX);
      return 0;
    }
  }
  return -1;
}


static int
msgq_start_receiver(MSGQ *msgq)
{
  sigset_t cur, old;
  int ret = 0;
  static const int blocksigs[] = {
    SIGINT,
    SIGQUIT,
    SIGPIPE,
    SIGALRM,
    SIGTERM,
    SIGUSR1,
    SIGUSR2,
    SIGCHLD,
    SIGSTOP,
    SIGTSTP,
  };
  size_t i;
  int saved_errno;

  sigemptyset(&old);
  sigemptyset(&cur);

  pthread_sigmask(SIG_SETMASK, NULL, &old);

  if (!block_all_signals)
    for (i = 0; i < sizeof(blocksigs) / sizeof(int); i++) {
      sigaddset(&cur, blocksigs[i]);
    }
  else
    sigfillset(&cur);

  pthread_sigmask(SIG_SETMASK, &cur, NULL);

  if ((ret = pthread_create(&msgq->receiver, NULL,
                            msgq_receiver, (void *)msgq)) != 0) {
    WARN(ret, "pthread_create(3) failed");
    saved_errno = ret;
    ret = -1;
  }

  pthread_sigmask(SIG_SETMASK, &old, NULL);

  errno = saved_errno;
  return ret;
}


#if 0
static void
msgq_receiver_cleaner(void *arg)
{
  MSGQ *msgq = (MSGQ *)arg;

  DEBUG(0, "receiver: cleanup handler started");
  pthread_mutex_unlock(&msgq->recv_mutex);
}
#endif  /* 0 */


static void *
msgq_receiver(void *arg)
{
  int fd;
  struct sockaddr_un addr;
  socklen_t addrlen;
  ssize_t len;
  struct msgq_packet *packet;
  struct msgq_node *np;
  MSGQ *msgq = (MSGQ *)arg;

  DEBUG(0, "receiver: thread started");

  MSGQ_LOCK(msgq);
  msgq->receiver_status = MSGQ_STAT_ALIVE;
  pthread_cond_broadcast(&msgq->stat_cond);

  fd = msgq->fd;
  MSGQ_UNLOCK(msgq);

  while (1) {
    //pthread_testcancel();

    DEBUG(0, "receiver: waiting for incoming packet from fd(%d)", fd);
    addrlen = sizeof(addr);
    len = recvfrom(fd, (void *)msgq->pkbuf, MSGQ_MSG_MAX, MSG_WAITALL,
                   (struct sockaddr *)&addr, &addrlen);
    if (len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* Since 'fd' is blocking socket, we will not get these errors */
        continue;
      }
      else {
        WARN(errno, "recvfrom(2) failed");
        break;
      }
    }

    packet = (struct msgq_packet *)msgq->pkbuf;
    if (validate_packet(packet, len) < 0) {
      DEBUG(0, "receiver: ignoring invalid(too short) packet from %s",
            addr.sun_path);
      continue;
    }

    if (strcmp(addr.sun_path, msgq->address) == 0) {
      /* Self-control message */
      if (strncmp(packet->data, "shutdown", 8) == 0) {
        DEBUG(0, "receiver: initiate shutdown sequence");
        break;
      }
    }

    np = msgq_node_create(addr.sun_path, packet);
    if (!np) {
      /* TODO: failed to create msgq_node struct, out of memory? */
      continue;
    }

    MSGQ_LOCK(msgq);
    edque_push_back(&msgq->recvq, &np->link);
    msgq->recvs++;
    DEBUG(0, "receiver: accepting a packet.");

    if (msgq->broadcast) {
      DEBUG(0, "receiver: broadcast!");
      pthread_cond_broadcast(&msgq->recv_cond);
    }
    else {
      DEBUG(0, "receiver: signal!");
      pthread_cond_signal(&msgq->recv_cond);
    }

    MSGQ_UNLOCK(msgq);
  }

  //pthread_cleanup_pop(1);

  shutdown(fd, SHUT_RD);

  MSGQ_LOCK(msgq);
  msgq->receiver_status = MSGQ_STAT_DEAD;
  pthread_cond_broadcast(&msgq->recv_cond);
  pthread_cond_broadcast(&msgq->stat_cond);
  MSGQ_UNLOCK(msgq);

  /* If you ever want to change from UNIX domain socket to UDP/TCP You
   * may need to change the way it calls shutdown()/close()
   * socket!! */


  return NULL;
}


/*
 * Validate given PACKET.   The PACKET points the message buffer that
 * contains the data just received from the remote.
 *
 * LEN contains the total bytes received from the remote.
 */
static int
validate_packet(struct msgq_packet *packet, ssize_t len)
{
  ssize_t estimated;

  estimated = len - (ssize_t)offsetof(struct msgq_packet, data);

  if (estimated < 0) {          /* wrong packet? */
    /* The received bytes is too small even for struct msgq_packet itself. */
    return -1;
  }

  if (estimated < (ssize_t)packet->size) /* correct wrong sized packet */
    packet->size = estimated;

  packet->container = 0;
  return 0;
}


/*
 * Copy(duplicate) the given PACKET.
 */
static struct msgq_packet *
msgq_copy_packet(const struct msgq_packet *packet)
{
  struct msgq_packet *p;
  size_t size, newsize;

  size = sizeof(*packet) + packet->size;
  newsize = size;

  if (packet->data[packet->size - 1] != '\0') {
    /* To make easy/safe debugging, If the last byte in the packet
     * is not '\0',  Add '\0' to the copied packet.  Since 'size' member
     * of the copied will not be changed,  it is okay for the sensitive
     * receiver. */
    newsize = size + 1;
  }
  p = malloc(newsize);
  if (!p)
    return NULL;
  memcpy(p, packet, size);

  if (newsize > size) {
    p->data[p->size] = '\0';
  }

  return p;
}


/*
 * Create new struct msgq_node instance using given PACKET's duplicate.
 */
static struct msgq_node *
msgq_node_create(const char *sender, const struct msgq_packet *packet)
{
  struct msgq_node *p;

  p = malloc(sizeof(*p));
  if (!p)
    return NULL;

  p->packet = msgq_copy_packet(packet);
  if (!p->packet) {
    free(p);
    return NULL;
  }
  p->packet->container = p;
  strncpy(p->sender, sender, UNIX_PATH_MAX - 1);
  ELIST_INIT(p->link);

  return p;
}


#ifndef NDEBUG
static void
verror_(const char *kind, int status, int errnum, const char *fmt, va_list ap)
{
  int saved_errno = errno;

  flockfile(stderr);
  fprintf(stderr, "%s: ", kind);

  vfprintf(stderr, fmt, ap);

  if (errnum > 0)
    fprintf(stderr, ": %s", strerror(errnum));

  fputc('\n', stderr);
  funlockfile(stderr);

  errno = saved_errno;

  if (status)
    exit(status);
}


static void
warning_(int errnum, const char *fmt, ...)
{
  va_list ap;
  int saved_errno = errno;

  va_start(ap, fmt);
  verror_("warning", 0, errnum, fmt, ap);
  va_end(ap);
  errno = saved_errno;
}

static void
debug_(int errnum, const char *fmt, ...)
{
  va_list ap;
  int saved_errno = errno;

  va_start(ap, fmt);
  verror_("debug", 0, errnum, fmt, ap);
  va_end(ap);
  errno = saved_errno;
}
#endif  /* NDEBUG */


#ifdef TEST_MSGQ
/*
 * This is the server side sample.
 *
 * You can send a message to this server using socat(1).
 * Each line by socat(1) will be a packet to the server.
 *
 * Since the first 8 bytes are used for maintaing packet header,
 * you may pass any byte for the first 8 bytes.  The server will
 * ignore that data.
 *
 * If the line(packet) is shorter than 8, the server will ignore that
 * packet.
 *
 * $ socat UNIX-SENDTO:/tmp/msgq,bind=/tmp/msgq-cli STDIO
 * 00000000hello, world
 */
int
main(int argc, char *argv[])
{
  MSGQ *msgq;
  struct msgq_packet *packet;
  int cond = 1;
  struct timespec ts;

  msgq = msgq_open("/tmp/msgq");

  while (cond) {
    packet = NULL;
    if (argc > 1) {
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += atoi(argv[1]);
      ts.tv_nsec = 0;

      printf("Waiting for %d second(s)...\n", atoi(argv[1]));

      packet = msgq_recv_timedwait(msgq, &ts);
    }
    else
      packet = msgq_recv_wait(msgq);

    if (packet) {
      int len = strlen(packet->data);
      if (packet->data[len - 1] == '\n')
        packet->data[len - 1] = '\0';

      printf("packet(%s): |%s|\n",
             msgq_pkt_sender(packet), packet->data);

#ifdef MSGQ_BROADCAST
      printf("broadcasting to /tmp/cli*...\n");
      msgq_broadcast_wildcard(msgq, "/tmp/cli*", packet);
#endif  /* MSGQ_BROADCAST */

      if (strcmp(packet->data, "quit") == 0)
        cond = 0;

      msgq_pkt_delete(packet);
    }
  }

  msgq_close(msgq);
  return 0;
}
#endif  /* TEST_MSGQ */
