/*
 * Copyright (C) 2000, 2001  Internet Software Consortium.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM
 * DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
 * INTERNET SOFTWARE CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
 * FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id: socket.c,v 1.5.2.1 2001/09/17 22:28:57 gson Exp $ */


#define MAKE_EXTERNAL 1
#include <config.h>

#include <sys/types.h>

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_   /* Prevent inclusion of winsock.h in windows.h */
#endif

#include "errno2result.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <io.h>
#include <fcntl.h>

#include <isc/buffer.h>
#include <isc/bufferlist.h>
#include <isc/condition.h>
#include <isc/list.h>
#include <isc/log.h>
#include <isc/mem.h>
#include <isc/msgs.h>
#include <isc/mutex.h>
#include <isc/net.h>
#include <isc/platform.h>
#include <isc/print.h>
#include <isc/region.h>
#include <isc/socket.h>
#include <isc/task.h>
#include <isc/thread.h>
#include <isc/util.h>

/*
 * Some systems define the socket length argument as an int, some as size_t,
 * some as socklen_t.  This is here so it can be easily changed if needed.
 */
#ifndef ISC_SOCKADDR_LEN_T
#define ISC_SOCKADDR_LEN_T unsigned int
#endif

/*
 * Define what the possible "soft" errors can be.  These are non-fatal returns
 * of various network related functions, like recv() and so on.
 *
 * For some reason, BSDI (and perhaps others) will sometimes return <0
 * from recv() but will have errno==0.  This is broken, but we have to
 * work around it here.
 */
#define SOFT_ERROR(e)	((e) == EAGAIN || \
			 (e) == WSAEWOULDBLOCK || \
			 (e) == WSAEINTR || \
			 (e) == 0)

#define DLVL(x) ISC_LOGCATEGORY_GENERAL, ISC_LOGMODULE_SOCKET, ISC_LOG_DEBUG(x)

/*
 * DLVL(90)  --  Function entry/exit and other tracing.
 * DLVL(70)  --  Socket "correctness" -- including returning of events, etc.
 * DLVL(60)  --  Socket data send/receive
 * DLVL(50)  --  Event tracing, including receiving/sending completion events.
 * DLVL(20)  --  Socket creation/destruction.
 */
#define TRACE_LEVEL		90
#define CORRECTNESS_LEVEL	70
#define IOEVENT_LEVEL		60
#define EVENT_LEVEL		50
#define CREATION_LEVEL		20

#define TRACE		DLVL(TRACE_LEVEL)
#define CORRECTNESS	DLVL(CORRECTNESS_LEVEL)
#define IOEVENT		DLVL(IOEVENT_LEVEL)
#define EVENT		DLVL(EVENT_LEVEL)
#define CREATION	DLVL(CREATION_LEVEL)

typedef isc_event_t intev_t;

#define SOCKET_MAGIC		0x494f696fU	/* IOio */
#define VALID_SOCKET(t)		((t) != NULL && (t)->magic == SOCKET_MAGIC)

/*
 * IPv6 control information.  If the socket is an IPv6 socket we want
 * to collect the destination address and interface so the client can
 * set them on outgoing packets.
 */
#ifdef ISC_PLATFORM_HAVEIPV6
#ifndef USE_CMSG
#define USE_CMSG	1
#endif
#endif

/*
 * NetBSD and FreeBSD can timestamp packets.  XXXMLG Should we have
 * a setsockopt() like interface to request timestamps, and if the OS
 * doesn't do it for us, call gettimeofday() on every UDP receive?
 */
#ifdef SO_TIMESTAMP
#ifndef USE_CMSG
#define USE_CMSG	1
#endif
#endif

/*
 * Check to see if we have even basic support for cracking messages from
 * the control data returned from/sent via recvmsg()/sendmsg().
 */
#if defined(USE_CMSG) && (!defined(CMSG_LEN) || !defined(CMSG_SPACE))
#undef USE_CMSG
#endif

/*
 * We really  don't want to try and use these control messages. Win32
 * doesn't have this mechanism
 */
#undef USE_CMSG

/*
 * Message header for recvmsg and sendmsg calls.
 * Used value-result for recvmsg, value only for sendmsg.
 */
struct iovec {
	void	*iov_base; 	/* starting address of buffer */
	size_t	iov_len; 	/* size of buffer */
};

struct msghdr {
        void	*msg_name;              /* optional address */
        u_int   msg_namelen;            /* size of address */
        WSABUF  *msg_iov;		/* scatter/gather array */
        u_int   msg_iovlen;             /* # elements in msg_iov */
        void	*msg_control;           /* ancillary data, see below */
        u_int   msg_controllen;         /* ancillary data buffer len */
        int     msg_flags;              /* flags on received message */
} msghdr;

struct isc_socket {
	/* Not locked. */
	unsigned int		magic;
	isc_socketmgr_t	       *manager;
	isc_mutex_t		lock;
	isc_sockettype_t	type;

	/* Locked by socket lock. */
	ISC_LINK(isc_socket_t)	link;
	unsigned int		references;
	int			fd;
	int			pf;

	ISC_LIST(isc_socketevent_t)		send_list;
	ISC_LIST(isc_socketevent_t)		recv_list;
	ISC_LIST(isc_socket_newconnev_t)	accept_list;
	isc_socket_connev_t		       *connect_ev;

	/*
	 * Internal events.  Posted when a descriptor is readable or
	 * writable.  These are statically allocated and never freed.
	 * They will be set to non-purgable before use.
	 */
	intev_t			readable_ev;
	intev_t			writable_ev;

	isc_sockaddr_t		address;  /* remote address */

	unsigned int		pending_recv : 1,
				pending_send : 1,
				pending_accept : 1,
				listener : 1, /* listener socket */
				connected : 1,
				connecting : 1, /* connect pending */
				bound : 1; /* bound to local addr */

#ifdef ISC_NET_RECVOVERFLOW
	unsigned char		overflow; /* used for MSG_TRUNC fake */
#endif
};

#define SOCKET_MANAGER_MAGIC	0x494f6d67U	/* IOmg */
#define VALID_MANAGER(m)	((m) != NULL && \
				 (m)->magic == SOCKET_MANAGER_MAGIC)
struct isc_socketmgr {
	/* Not locked. */
	unsigned int		magic;
	isc_mem_t	       *mctx;
	isc_mutex_t		lock;
	/* Locked by manager lock. */
	ISC_LIST(isc_socket_t)	socklist;
	fd_set			read_fds;
	fd_set			write_fds;
	isc_socket_t	       *fds[FD_SETSIZE];
	int			fdstate[FD_SETSIZE];
	int			maxfd;
#ifdef ISC_PLATFORM_USETHREADS
	isc_thread_t		watcher;
	isc_condition_t		shutdown_ok;
	int			pipe_fds[2];
#endif /* ISC_PLATFORM_USETHREADS */
};

#define CLOSED		0	/* this one must be zero */
#define MANAGED		1
#define CLOSE_PENDING	2

/*
 * send() and recv() iovec counts
 */
#define MAXSCATTERGATHER_SEND	(ISC_SOCKET_MAXSCATTERGATHER)
#ifdef ISC_NET_RECVOVERFLOW
# define MAXSCATTERGATHER_RECV	(ISC_SOCKET_MAXSCATTERGATHER + 1)
#else
# define MAXSCATTERGATHER_RECV	(ISC_SOCKET_MAXSCATTERGATHER)
#endif

static void send_recvdone_event(isc_socket_t *, isc_socketevent_t **);
static void send_senddone_event(isc_socket_t *, isc_socketevent_t **);
static void free_socket(isc_socket_t **);
static isc_result_t allocate_socket(isc_socketmgr_t *, isc_sockettype_t,
				    isc_socket_t **);
static void destroy(isc_socket_t **);
static void internal_accept(isc_task_t *, isc_event_t *);
static void internal_connect(isc_task_t *, isc_event_t *);
static void internal_recv(isc_task_t *, isc_event_t *);
static void internal_send(isc_task_t *, isc_event_t *);
static void process_cmsg(isc_socket_t *, struct msghdr *, isc_socketevent_t *);
static void build_msghdr_send(isc_socket_t *, isc_socketevent_t *,
			      struct msghdr *, char *cmsg,
			      WSABUF *, size_t *);
static void build_msghdr_recv(isc_socket_t *, isc_socketevent_t *,
			      struct msghdr *, char *cmsg,
			      WSABUF *, size_t *);

#define SELECT_POKE_SHUTDOWN		(-1)
#define SELECT_POKE_NOTHING		(-2)
#define SELECT_POKE_READ		(-3)
#define SELECT_POKE_ACCEPT		(-3) /* Same as _READ */
#define SELECT_POKE_WRITE		(-4)
#define SELECT_POKE_CONNECT		(-4) /* Same as _WRITE */
#define SELECT_POKE_CLOSE		(-5)

long bpipe_written = 0;

#define SOCK_DEAD(s)			((s)->references == 0)

/*
 * The following routines are here to handle Unix emulation until we
 * can rewrite the the routines in Winsock2 style.
 */

/*
 * Initialize socket services
 */
BOOL InitSockets() {
	WORD wVersionRequested;
	WSADATA wsaData;
	int err;

	/* Need Winsock 2.0 or better */
	wVersionRequested = MAKEWORD(2, 0);
 
	err = WSAStartup(wVersionRequested, &wsaData);
	if ( err != 0 ) {
		/* Tell the user that we could not find a usable Winsock DLL */
		return (FALSE);
	}
 
	return (TRUE);
}

int
internal_pipe(int filedes[2]) {
	int status;
	unsigned int pipesize = 65535;
	int mode = _O_TEXT;

	status = _pipe(filedes, pipesize, mode);
	return (status);
	InterlockedExchange(&bpipe_written, 0);
}

int
internal_sendmsg(int sock, const struct msghdr *msg, int flags) {
	int Error;
	DWORD BytesSent;
	DWORD Flags = flags;

	Error = WSASendTo((SOCKET) sock, 
                    msg->msg_iov, 
                    msg->msg_iovlen, 
                    &BytesSent, 
                    Flags, 
		    msg->msg_name,
		    msg->msg_namelen,
                    NULL,
                    NULL);
    
    if (Error == SOCKET_ERROR) {
	    BytesSent = -1;
	    /* There is an error... */
	    Error = WSAGetLastError();
	    if (Error == WSA_IO_PENDING) {
		    /* Overlapped send successfully initiated. */
		    errno = EAGAIN;
	    } else {
		    /* An unexpected error occurred. */
		    errno = Error;
	    }
    }

    /* No error -- the I/O request was completed immediately... */
    return (BytesSent);
}

int
internal_recvmsg(int sock, struct msghdr *msg, int flags) {
	DWORD Flags = flags;
	DWORD NumBytes;
	int Result;
	int Error;

	Result = WSARecvFrom((SOCKET) sock,
			     msg->msg_iov,
			     msg->msg_iovlen,
			     &NumBytes,
			     &Flags,
			     msg->msg_name,
			     (int *)&(msg->msg_namelen),
			     NULL,
			     NULL);


	/* Check for errors. */
	if (Result == SOCKET_ERROR) {
		Error = WSAGetLastError();
		NumBytes = -1;
        
		switch (Error) {
		case WSAEWOULDBLOCK:
			/*
			 * No data received; return to wait for another
			 * read event.
			 */
			errno = EAGAIN;
			break;

		default:
			/* Some other error... hit the panic button. */
			errno = Error;
			break;
		}
	}
	msg->msg_flags = Flags;	/* Return the flags received in header */
	
	return (NumBytes);
}

static void
manager_log(isc_socketmgr_t *sockmgr,
	    isc_logcategory_t *category, isc_logmodule_t *module, int level,
	    const char *fmt, ...)
{
	char msgbuf[2048];
	va_list ap;

	if (! isc_log_wouldlog(isc_lctx, level))
		return;

	va_start(ap, fmt);
	vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
	va_end(ap);

	isc_log_write(isc_lctx, category, module, level,
		      "sockmgr %p: %s", sockmgr, msgbuf);
}

static void
socket_log(isc_socket_t *sock, isc_sockaddr_t *address,
	   isc_logcategory_t *category, isc_logmodule_t *module, int level,
	   isc_msgcat_t *msgcat, int msgset, int message,
	   const char *fmt, ...)
{
	char msgbuf[2048];
	char peerbuf[256];
	va_list ap;

	if (! isc_log_wouldlog(isc_lctx, level))
		return;

	va_start(ap, fmt);
	vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
	va_end(ap);

	if (address == NULL) {
		isc_log_iwrite(isc_lctx, category, module, level,
			       msgcat, msgset, message,
			       "socket %p: %s", sock, msgbuf);
	} else {
		isc_sockaddr_format(address, peerbuf, sizeof peerbuf);
		isc_log_iwrite(isc_lctx, category, module, level,
			       msgcat, msgset, message,
			       "socket %p %s: %s", sock, peerbuf, msgbuf);
	}
}

static void
wakeup_socket(isc_socketmgr_t *manager, int fd, int msg) {
	isc_socket_t *sock;

	/*
	 * This is a wakeup on a socket.  If the socket is not in the
	 * process of being closed, start watching it for either reads
	 * or writes.
	 */
	INSIST(fd < FD_SETSIZE);

	if (manager->fdstate[fd] == CLOSE_PENDING) {
		manager->fdstate[fd] = CLOSED;
		FD_CLR(fd, &manager->read_fds);
		FD_CLR(fd, &manager->write_fds);
		closesocket(fd);
		return;
	}
	if (manager->fdstate[fd] != MANAGED)
		return;

	sock = manager->fds[fd];

	/*
	 * If there are no events, or there is an event but we
	 * have already queued up the internal event on a task's
	 * queue, clear the bit.  Otherwise, set it.
	 */
	if (msg == SELECT_POKE_READ)
		FD_SET(sock->fd, &manager->read_fds);
	if (msg == SELECT_POKE_WRITE)
		FD_SET(sock->fd, &manager->write_fds);
}

#ifdef ISC_PLATFORM_USETHREADS
/*
 * Poke the select loop when there is something for us to do.
 * The write is required (by POSIX) to complete.  That is, we
 * will not get partial writes.
 */
static void
select_poke(isc_socketmgr_t *mgr, int fd, int msg) {
	int cc;
	int buf[2];
	int errval;

	buf[0] = fd;
	buf[1] = msg;

	if (msg == SELECT_POKE_SHUTDOWN) {
		do {
			cc = _write(mgr->pipe_fds[1], buf, sizeof(buf));
			errval = errno;
		} while (cc < 0 && SOFT_ERROR(errno));
			        
		if (cc < 0)
			FATAL_ERROR(__FILE__, __LINE__,
				    isc_msgcat_get(isc_msgcat,
						   ISC_MSGSET_SOCKET,
						   ISC_MSG_WRITEFAILED,
						   "_write() failed "
						   "during watcher poke: %s"),
				    strerror(errno));

		INSIST(cc == sizeof(buf));

		InterlockedIncrement(&bpipe_written);
	} else {
		wakeup_socket(mgr, fd, msg);
	}
}

/*
 * Read a message on the internal fd.
 */
static void
select_readmsg(isc_socketmgr_t *mgr, int *fd, int *msg) {
	int buf[2];
	int cc;

	cc = _read(mgr->pipe_fds[0], buf, sizeof(buf));
	if (cc < 0) {
		*msg = SELECT_POKE_NOTHING;
		if (SOFT_ERROR(errno))
			return;

		FATAL_ERROR(__FILE__, __LINE__,
			    isc_msgcat_get(isc_msgcat, ISC_MSGSET_SOCKET,
					   ISC_MSG_READFAILED,
					   "_read() failed "
					   "during watcher poke: %s"),
			    strerror(errno));
		return;
	}
	INSIST(cc == sizeof(buf));
	*fd  = buf[0];
	*msg = buf[1];

}
#else /* ISC_PLATFORM_USETHREADS */
/*
 * Update the state of the socketmgr when something changes.
 */
static void
select_poke(isc_socketmgr_t *manager, int fd, int msg) {
	if (msg == SELECT_POKE_SHUTDOWN)
		return;
	else if (fd >= 0)
		wakeup_socket(manager, fd, msg);
	return;
}
#endif /* ISC_PLATFORM_USETHREADS */

/*
 * Make a fd non-blocking.
 */
static isc_result_t
make_nonblock(int fd) {
	int ret;
	unsigned long flags = 1;

	/* Set the socket to non-blocking */
	ret = ioctlsocket((SOCKET) fd, FIONBIO, &flags);

	if (ret == -1) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "ioctlsocket(%d, FIOBIO, %d): %s",
				 fd, flags, strerror(errno));

		return (ISC_R_UNEXPECTED);
	}

	return (ISC_R_SUCCESS);
}

/*
 * Process control messages received on a socket.
 */
static void
process_cmsg(isc_socket_t *sock, struct msghdr *msg, isc_socketevent_t *dev) {
#ifdef USE_CMSG
	struct cmsghdr *cmsgp;
#ifdef ISC_PLATFORM_HAVEIPV6
	struct in6_pktinfo *pktinfop;
#endif
#ifdef SO_TIMESTAMP
	struct timeval *timevalp;
#endif
#endif

	/*
	 * sock is used only when ISC_NET_BSD44MSGHDR and USE_CMSG are defined.
	 * msg and dev are used only when ISC_NET_BSD44MSGHDR is defined.
	 * They are all here, outside of the CPP tests, because it is
	 * more consistent with the usual ISC coding style.
	 */
	UNUSED(sock);
	UNUSED(msg);
	UNUSED(dev);

#ifndef ISC_NET_BSD44MSGHDR
	return;

#else  /* defined ISC_NET_BSD44MSGHDR */

#ifdef MSG_TRUNC
	if ((msg->msg_flags & MSG_TRUNC) == MSG_TRUNC)
		dev->attributes |= ISC_SOCKEVENTATTR_TRUNC;
#endif

#ifdef MSG_CTRUNC
	if ((msg->msg_flags & MSG_CTRUNC) == MSG_CTRUNC)
		dev->attributes |= ISC_SOCKEVENTATTR_CTRUNC;
#endif

	/*
	 * Check for multicast.
	 */
	if (isc_sockaddr_ismulticast(&dev->address))
		dev->attributes |= ISC_SOCKEVENTATTR_MULTICAST;

#ifndef USE_CMSG
	return;
#else
	if (msg->msg_controllen == 0 || msg->msg_control == NULL)
		return;

#ifdef SO_TIMESTAMP
	timevalp = NULL;
#endif
#ifdef ISC_PLATFORM_HAVEIPV6
	pktinfop = NULL;
#endif

	cmsgp = CMSG_FIRSTHDR(msg);
	while (cmsgp != NULL) {
		socket_log(sock, NULL, TRACE,
			   isc_msgcat, ISC_MSGSET_SOCKET, ISC_MSG_PROCESSCMSG,
			   "processing cmsg %p", cmsgp);

#ifdef ISC_PLATFORM_HAVEIPV6
		if (cmsgp->cmsg_level == IPPROTO_IPV6
		    && cmsgp->cmsg_type == IPV6_PKTINFO) {

			pktinfop = (struct in6_pktinfo *)CMSG_DATA(cmsgp);
			memcpy(&dev->pktinfo, pktinfop,
			       sizeof(struct in6_pktinfo));
			dev->attributes |= ISC_SOCKEVENTATTR_PKTINFO;
			socket_log(sock, NULL, TRACE,
				   isc_msgcat, ISC_MSGSET_SOCKET,
				   ISC_MSG_IFRECEIVED,
				   "interface received on ifindex %u",
				   dev->pktinfo.ipi6_ifindex);
			goto next;
		}
#endif

#ifdef SO_TIMESTAMP
		if (cmsgp->cmsg_level == SOL_SOCKET
		    && cmsgp->cmsg_type == SCM_TIMESTAMP) {
			timevalp = (struct timeval *)CMSG_DATA(cmsgp);
			dev->timestamp.seconds = timevalp->tv_sec;
			dev->timestamp.nanoseconds = timevalp->tv_usec * 1000;
			dev->attributes |= ISC_SOCKEVENTATTR_TIMESTAMP;
			goto next;
		}
#endif

	next:
		cmsgp = CMSG_NXTHDR(msg, cmsgp);
	}
#endif /* USE_CMSG */

#endif /* ISC_NET_BSD44MSGHDR */

}

/*
 * Construct an iov array and attach it to the msghdr passed in.  This is
 * the SEND constructor, which will use the used region of the buffer
 * (if using a buffer list) or will use the internal region (if a single
 * buffer I/O is requested).
 *
 * Nothing can be NULL, and the done event must list at least one buffer
 * on the buffer linked list for this function to be meaningful.
 *
 * If write_countp != NULL, *write_countp will hold the number of bytes
 * this transaction can send.
 */
static void
build_msghdr_send(isc_socket_t *sock, isc_socketevent_t *dev,
		  struct msghdr *msg, char *cmsg,
		  WSABUF *iov, size_t *write_countp)
{
	unsigned int iovcount;
	isc_buffer_t *buffer;
	isc_region_t used;
	size_t write_count;
	size_t skip_count;

#ifndef USE_CMSG
	UNUSED(cmsg);
#endif

	memset(msg, 0, sizeof (*msg));

	if (sock->type == isc_sockettype_udp) {
		msg->msg_name = (void *)&dev->address.type.sa;
		msg->msg_namelen = dev->address.length;
	} else {
		msg->msg_name = NULL;
		msg->msg_namelen = 0;
	}

	buffer = ISC_LIST_HEAD(dev->bufferlist);
	write_count = 0;
	iovcount = 0;

	/*
	 * Single buffer I/O?  Skip what we've done so far in this region.
	 */
	if (buffer == NULL) {
		write_count = dev->region.length - dev->n;
		iov[0].buf = (void *)(dev->region.base + dev->n);
		iov[0].len = write_count;
		iovcount = 1;

		goto config;
	}

	/*
	 * Multibuffer I/O.
	 * Skip the data in the buffer list that we have already written.
	 */
	skip_count = dev->n;
	while (buffer != NULL) {
		REQUIRE(ISC_BUFFER_VALID(buffer));
		if (skip_count < isc_buffer_usedlength(buffer))
			break;
		skip_count -= isc_buffer_usedlength(buffer);
		buffer = ISC_LIST_NEXT(buffer, link);
	}

	while (buffer != NULL) {
		INSIST(iovcount < MAXSCATTERGATHER_SEND);

		isc_buffer_usedregion(buffer, &used);

		if (used.length > 0) {
			iov[iovcount].buf = (void *)(used.base
						   + skip_count);
			iov[iovcount].len = used.length - skip_count;
			write_count += (used.length - skip_count);
			skip_count = 0;
			iovcount++;
		}
		buffer = ISC_LIST_NEXT(buffer, link);
	}

	INSIST(skip_count == 0);

 config:
	msg->msg_iov = iov;
	msg->msg_iovlen = iovcount;

#ifdef ISC_NET_BSD44MSGHDR
	msg->msg_control = NULL;
	msg->msg_controllen = 0;
	msg->msg_flags = 0;
#ifdef USE_CMSG
	if ((sock->type == isc_sockettype_udp)
	    && ((dev->attributes & ISC_SOCKEVENTATTR_PKTINFO) != 0)) {
		struct cmsghdr *cmsgp;
		struct in6_pktinfo *pktinfop;

		socket_log(sock, NULL, TRACE,
			   isc_msgcat, ISC_MSGSET_SOCKET, ISC_MSG_SENDTODATA,
			   "sendto pktinfo data, ifindex %u",
			   dev->pktinfo.ipi6_ifindex);

		msg->msg_controllen = CMSG_SPACE(sizeof(struct in6_pktinfo));
		msg->msg_control = (void *)cmsg;

		cmsgp = (struct cmsghdr *)cmsg;
		cmsgp->cmsg_level = IPPROTO_IPV6;
		cmsgp->cmsg_type = IPV6_PKTINFO;
		cmsgp->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
		pktinfop = (struct in6_pktinfo *)CMSG_DATA(cmsgp);
		memcpy(pktinfop, &dev->pktinfo, sizeof(struct in6_pktinfo));
	}
#endif /* USE_CMSG */
#endif /* ISC_NET_BSD44MSGHDR */

	if (write_countp != NULL)
		*write_countp = write_count;
}

/*
 * Construct an iov array and attach it to the msghdr passed in.  This is
 * the RECV constructor, which will use the available region of the buffer
 * (if using a buffer list) or will use the internal region (if a single
 * buffer I/O is requested).
 *
 * Nothing can be NULL, and the done event must list at least one buffer
 * on the buffer linked list for this function to be meaningful.
 *
 * If read_countp != NULL, *read_countp will hold the number of bytes
 * this transaction can receive.
 */
static void
build_msghdr_recv(isc_socket_t *sock, isc_socketevent_t *dev,
		  struct msghdr *msg, char *cmsg,
		  WSABUF *iov, size_t *read_countp)
{
	unsigned int iovcount;
	isc_buffer_t *buffer;
	isc_region_t available;
	size_t read_count;

#ifndef USE_CMSG
	UNUSED(cmsg);
#endif

	memset(msg, 0, sizeof (struct msghdr));

	if (sock->type == isc_sockettype_udp) {
		memset(&dev->address, 0, sizeof(dev->address));
		msg->msg_name = (void *)&dev->address.type.sa;
		msg->msg_namelen = sizeof(dev->address.type);
#ifdef ISC_NET_RECVOVERFLOW
		/* If needed, steal one iovec for overflow detection. */
		maxiov--;
#endif
	} else { /* TCP */
		msg->msg_name = NULL;
		msg->msg_namelen = 0;
		dev->address = sock->address;
	}

	buffer = ISC_LIST_HEAD(dev->bufferlist);
	read_count = 0;

	/*
	 * Single buffer I/O?  Skip what we've done so far in this region.
	 */
	if (buffer == NULL) {
		read_count = dev->region.length - dev->n;
		iov[0].buf = (void *)(dev->region.base + dev->n);
		iov[0].len = read_count;
		iovcount = 1;

	}
	else {

	/*
	 * Multibuffer I/O.
	 * Skip empty buffers.
	 */
		while (buffer != NULL) {
			REQUIRE(ISC_BUFFER_VALID(buffer));
			if (isc_buffer_availablelength(buffer) != 0)
				break;
			buffer = ISC_LIST_NEXT(buffer, link);
		}

		iovcount = 0;
		while (buffer != NULL) {
			INSIST(iovcount < MAXSCATTERGATHER_RECV);

			isc_buffer_availableregion(buffer, &available);

			if (available.length > 0) {
				iov[iovcount].buf = (void *)(available.base);
				iov[iovcount].len = available.length;
				read_count += available.length;
				iovcount++;
			}
			buffer = ISC_LIST_NEXT(buffer, link);
		}
	}

	/*
	 * If needed, set up to receive that one extra byte.  Note that
	 * we know there is at least one iov left, since we stole it
	 * at the top of this function.
	 */
#ifdef ISC_NET_RECVOVERFLOW
	if (sock->type == isc_sockettype_udp) {
		iov[iovcount].iov_base = (void *)(&sock->overflow);
		iov[iovcount].iov_len = 1;
		iovcount++;
	}
#endif

	msg->msg_iov = iov;
	msg->msg_iovlen = iovcount;

#ifdef ISC_NET_BSD44MSGHDR
	msg->msg_control = NULL;
	msg->msg_controllen = 0;
	msg->msg_flags = 0;
#if defined(USE_CMSG)
	if (sock->type == isc_sockettype_udp) {
		msg->msg_control = cmsg;
		msg->msg_controllen = CMSG_BUF_SIZE;
	}
#endif /* USE_CMSG */
#endif /* ISC_NET_BSD44MSGHDR */

	if (read_countp != NULL)
		*read_countp = read_count;
}

static void
set_dev_address(isc_sockaddr_t *address, isc_socket_t *sock,
		isc_socketevent_t *dev)
{
	if (sock->type == isc_sockettype_udp) {
		if (address != NULL)
			dev->address = *address;
		else
			dev->address = sock->address;
	} else if (sock->type == isc_sockettype_tcp) {
		INSIST(address == NULL);
		dev->address = sock->address;
	}
}

static isc_socketevent_t *
allocate_socketevent(isc_socket_t *sock, isc_eventtype_t eventtype,
		     isc_taskaction_t action, const void *arg)
{
	isc_socketevent_t *ev;

	ev = (isc_socketevent_t *)isc_event_allocate(sock->manager->mctx,
						     sock, eventtype,
						     action, arg,
						     sizeof (*ev));

	if (ev == NULL)
		return (NULL);

	ev->result = ISC_R_UNEXPECTED;
	ISC_LINK_INIT(ev, ev_link);
	ISC_LIST_INIT(ev->bufferlist);
	ev->region.base = NULL;
	ev->n = 0;
	ev->offset = 0;
	ev->attributes = 0;

	return (ev);
}

#if defined(ISC_SOCKET_DEBUG)
static void
dump_msg(struct msghdr *msg, isc_socket_t *sock) {
	unsigned int i;

	printf("MSGHDR %p, Socket #: %d\n", msg, sock->fd);
	printf("\tname %p, namelen %d\n", msg->msg_name, msg->msg_namelen);
	printf("\tiov %p, iovlen %d\n", msg->msg_iov, msg->msg_iovlen);
	for (i = 0 ; i < (unsigned int)msg->msg_iovlen ; i++)
		printf("\t\t%d\tbase %p, len %d\n", i,
		       msg->msg_iov[i].buf,
		       msg->msg_iov[i].len);
#ifdef ISC_NET_BSD44MSGHDR
	printf("\tcontrol %p, controllen %d\n", msg->msg_control,
	       msg->msg_controllen);
#endif
}
#endif

#define DOIO_SUCCESS		0	/* i/o ok, event sent */
#define DOIO_SOFT		1	/* i/o ok, soft error, no event sent */
#define DOIO_HARD		2	/* i/o error, event sent */
#define DOIO_EOF		3	/* EOF, no event sent */

static int
doio_recv(isc_socket_t *sock, isc_socketevent_t *dev) {
	int cc;
	WSABUF iov[MAXSCATTERGATHER_RECV];
	size_t read_count;
	size_t actual_count;
	struct msghdr msghdr;
	isc_buffer_t *buffer;
#if USE_CMSG
	char cmsg[CMSG_BUF_SIZE];
#else
	char *cmsg = NULL;
#endif

	build_msghdr_recv(sock, dev, &msghdr, cmsg, iov, &read_count);

#if defined(ISC_SOCKET_DEBUG)
	dump_msg(&msghdr,sock);
#endif

	cc = internal_recvmsg(sock->fd, &msghdr, 0);

	if (cc < 0) {
		if (SOFT_ERROR(errno))
			return (DOIO_SOFT);

		if (isc_log_wouldlog(isc_lctx, IOEVENT_LEVEL))
			socket_log(sock, NULL, IOEVENT,
				   isc_msgcat, ISC_MSGSET_SOCKET,
				   ISC_MSG_DOIORECV, 
				   "doio_recv: internal_recvmsg(%d) %d bytes, "
				   "err %d/%s",
				   sock->fd, cc, errno, NTstrerror(errno));

#define SOFT_OR_HARD(_system, _isc) \
	if (errno == _system) { \
		if (sock->connected) { \
			dev->result = _isc; \
			return (DOIO_HARD); \
		} \
		return (DOIO_SOFT); \
	}
#define ALWAYS_HARD(_system, _isc) \
	if (errno == _system) { \
		dev->result = _isc; \
		return (DOIO_HARD); \
	}

		SOFT_OR_HARD(WSAECONNREFUSED, ISC_R_CONNREFUSED);
		SOFT_OR_HARD(WSAENETUNREACH, ISC_R_NETUNREACH);
		SOFT_OR_HARD(WSAEHOSTUNREACH, ISC_R_HOSTUNREACH);
		SOFT_OR_HARD(WSAECONNRESET, ISC_R_CONNECTIONRESET);
		SOFT_OR_HARD(WSAENETRESET, ISC_R_CONNECTIONRESET);
		SOFT_OR_HARD(WSAEDISCON, ISC_R_CONNECTIONRESET);
		SOFT_OR_HARD(WSAENETDOWN, ISC_R_NETDOWN);
		ALWAYS_HARD(WSAENOBUFS, ISC_R_NORESOURCES);

#undef SOFT_OR_HARD
#undef ALWAYS_HARD

		dev->result = ISC_R_UNEXPECTED;
		return (DOIO_HARD);
	}

	/*
	 * On TCP, zero length reads indicate EOF, while on
	 * UDP, zero length reads are perfectly valid, although
	 * strange.
	 */
	if ((sock->type == isc_sockettype_tcp) && (cc == 0))
		return (DOIO_EOF);

	if (sock->type == isc_sockettype_udp)
		dev->address.length = msghdr.msg_namelen;

	socket_log(sock, &dev->address, IOEVENT,
		   isc_msgcat, ISC_MSGSET_SOCKET, ISC_MSG_PKTRECV,
		   "packet received correctly");

	/*
	 * Overflow bit detection.  If we received MORE bytes than we should,
	 * this indicates an overflow situation.  Set the flag in the
	 * dev entry and adjust how much we read by one.
	 */
#ifdef ISC_NET_RECVOVERFLOW
	if ((sock->type == isc_sockettype_udp) && ((size_t)cc > read_count)) {
		dev->attributes |= ISC_SOCKEVENTATTR_TRUNC;
		cc--;
	}
#endif

	/*
	 * If there are control messages attached, run through them and pull
	 * out the interesting bits.
	 */
	if (sock->type == isc_sockettype_udp)
		process_cmsg(sock, &msghdr, dev);

	/*
	 * update the buffers (if any) and the i/o count
	 */
	dev->n += cc;
	actual_count = cc;
	buffer = ISC_LIST_HEAD(dev->bufferlist);
	while (buffer != NULL && actual_count > 0) {
		REQUIRE(ISC_BUFFER_VALID(buffer));
		if (isc_buffer_availablelength(buffer) <= actual_count) {
			actual_count -= isc_buffer_availablelength(buffer);
			isc_buffer_add(buffer,
				       isc_buffer_availablelength(buffer));
		} else {
			isc_buffer_add(buffer, actual_count);
			actual_count = 0;
			break;
		}
		buffer = ISC_LIST_NEXT(buffer, link);
		if (buffer == NULL) {
			INSIST(actual_count == 0);
		}
	}

	/*
	 * If we read less than we expected, update counters,
	 * and let the upper layer poke the descriptor.
	 */
	if (((size_t)cc != read_count) && (dev->n < dev->minimum))
		return (DOIO_SOFT);

	/*
	 * Full reads are posted, or partials if partials are ok.
	 */
	dev->result = ISC_R_SUCCESS;
	return (DOIO_SUCCESS);
}

/*
 * Returns:
 *	DOIO_SUCCESS	The operation succeeded.  dev->result contains
 *			ISC_R_SUCCESS.
 *
 *	DOIO_HARD	A hard or unexpected I/O error was encountered.
 *			dev->result contains the appropriate error.
 *
 *	DOIO_SOFT	A soft I/O error was encountered.  No senddone
 *			event was sent.  The operation should be retried.
 *
 *	No other return values are possible.
 */
static int
doio_send(isc_socket_t *sock, isc_socketevent_t *dev) {
	int cc;
	WSABUF iov[MAXSCATTERGATHER_SEND];
	size_t write_count;
	struct msghdr msghdr;
	char addrbuf[ISC_SOCKADDR_FORMATSIZE];
#if USE_CMSG
	char cmsg[CMSG_BUF_SIZE];
#else
	char *cmsg = NULL;
#endif

	build_msghdr_send(sock, dev, &msghdr, cmsg, iov, &write_count);

	cc = internal_sendmsg(sock->fd, &msghdr, 0);

	/*
	 * Check for error or block condition.
	 */
	if (cc < 0) {
		if (SOFT_ERROR(errno))
			return (DOIO_SOFT);

#define SOFT_OR_HARD(_system, _isc) \
	if (errno == _system) { \
		if (sock->connected) { \
			dev->result = _isc; \
			return (DOIO_HARD); \
		} \
		return (DOIO_SOFT); \
	}
#define ALWAYS_HARD(_system, _isc) \
	if (errno == _system) { \
		dev->result = _isc; \
		return (DOIO_HARD); \
	}

		SOFT_OR_HARD(WSAEACCES, ISC_R_NOPERM);
		SOFT_OR_HARD(WSAEAFNOSUPPORT, ISC_R_ADDRNOTAVAIL);
		SOFT_OR_HARD(WSAECONNREFUSED, ISC_R_CONNREFUSED);
		SOFT_OR_HARD(WSAECONNRESET, ISC_R_CONNECTIONRESET);
		SOFT_OR_HARD(WSAENETRESET, ISC_R_CONNECTIONRESET);
		SOFT_OR_HARD(WSAEDISCON, ISC_R_CONNECTIONRESET);
		SOFT_OR_HARD(WSAENETDOWN, ISC_R_NETDOWN);
		ALWAYS_HARD(WSAEADDRNOTAVAIL, ISC_R_ADDRNOTAVAIL);
		ALWAYS_HARD(WSAEHOSTUNREACH, ISC_R_HOSTUNREACH);
		ALWAYS_HARD(WSAEHOSTDOWN, ISC_R_HOSTUNREACH);
		ALWAYS_HARD(WSAENETUNREACH, ISC_R_NETUNREACH);
		ALWAYS_HARD(WSAENOBUFS, ISC_R_NORESOURCES);
		ALWAYS_HARD(EPERM, ISC_R_HOSTUNREACH);
		ALWAYS_HARD(EPIPE, ISC_R_NOTCONNECTED);

#undef SOFT_OR_HARD
#undef ALWAYS_HARD

		/*
		 * The other error types depend on whether or not the
		 * socket is UDP or TCP.  If it is UDP, some errors
		 * that we expect to be fatal under TCP are merely
		 * annoying, and are really soft errors.
		 *
		 * However, these soft errors are still returned as
		 * a status.
		 */
		isc_sockaddr_format(&dev->address, addrbuf, sizeof(addrbuf));
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "internal_send: %s: %s",
				 addrbuf, strerror(errno));
		dev->result = ISC_R_UNEXPECTED;
		return (DOIO_HARD);
	}

	if (cc == 0)
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "internal_send: send() %s 0",
				 isc_msgcat_get(isc_msgcat, ISC_MSGSET_GENERAL,
						ISC_MSG_RETURNED, "returned"));

	/*
	 * If we write less than we expected, update counters, poke.
	 */
	dev->n += cc;
	if ((size_t)cc != write_count)
		return (DOIO_SOFT);

	/*
	 * Exactly what we wanted to write.  We're done with this
	 * entry.  Post its completion event.
	 */
	dev->result = ISC_R_SUCCESS;
	return (DOIO_SUCCESS);
}

/*
 * Kill.
 *
 * Caller must ensure that the socket is not locked and no external
 * references exist.
 */
static void
destroy(isc_socket_t **sockp) {
	isc_socket_t *sock = *sockp;
	isc_socketmgr_t *manager = sock->manager;

	socket_log(sock, NULL, CREATION, isc_msgcat, ISC_MSGSET_SOCKET,
		   ISC_MSG_DESTROYING, "destroying");

	INSIST(ISC_LIST_EMPTY(sock->accept_list));
	INSIST(ISC_LIST_EMPTY(sock->recv_list));
	INSIST(ISC_LIST_EMPTY(sock->send_list));
	INSIST(sock->connect_ev == NULL);

	LOCK(&manager->lock);

	/*
	 * No one has this socket open, so the watcher doesn't have to be
	 * poked, and the socket doesn't have to be locked.
	 */
	manager->fds[sock->fd] = NULL;
	manager->fdstate[sock->fd] = CLOSE_PENDING;
	select_poke(manager, sock->fd, SELECT_POKE_CLOSE);
	ISC_LIST_UNLINK(manager->socklist, sock, link);

#ifdef ISC_PLATFORM_USETHREADS
	if (ISC_LIST_EMPTY(manager->socklist))
		SIGNAL(&manager->shutdown_ok);
#endif /* ISC_PLATFORM_USETHREADS */

	/*
	 * XXX should reset manager->maxfd here
	 */

	UNLOCK(&manager->lock);

	free_socket(sockp);
}

static isc_result_t
allocate_socket(isc_socketmgr_t *manager, isc_sockettype_t type,
		isc_socket_t **socketp)
{
	isc_socket_t *sock;
	isc_result_t ret;

	sock = isc_mem_get(manager->mctx, sizeof *sock);

	if (sock == NULL)
		return (ISC_R_NOMEMORY);

	ret = ISC_R_UNEXPECTED;

	sock->magic = 0;
	sock->references = 0;

	sock->manager = manager;
	sock->type = type;
	sock->fd = -1;

	ISC_LINK_INIT(sock, link);

	/*
	 * set up list of readers and writers to be initially empty
	 */
	ISC_LIST_INIT(sock->recv_list);
	ISC_LIST_INIT(sock->send_list);
	ISC_LIST_INIT(sock->accept_list);
	sock->connect_ev = NULL;
	sock->pending_recv = 0;
	sock->pending_send = 0;
	sock->pending_accept = 0;
	sock->listener = 0;
	sock->connected = 0;
	sock->connecting = 0;
	sock->bound = 0;

	/*
	 * initialize the lock
	 */
	if (isc_mutex_init(&sock->lock) != ISC_R_SUCCESS) {
		sock->magic = 0;
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_mutex_init() %s",
				 isc_msgcat_get(isc_msgcat, ISC_MSGSET_GENERAL,
						ISC_MSG_FAILED, "failed"));
		ret = ISC_R_UNEXPECTED;
		goto error;
	}

	/*
	 * Initialize readable and writable events
	 */
	ISC_EVENT_INIT(&sock->readable_ev, sizeof(intev_t),
		       ISC_EVENTATTR_NOPURGE, NULL, ISC_SOCKEVENT_INTR,
		       NULL, sock, sock, NULL, NULL);
	ISC_EVENT_INIT(&sock->writable_ev, sizeof(intev_t),
		       ISC_EVENTATTR_NOPURGE, NULL, ISC_SOCKEVENT_INTW,
		       NULL, sock, sock, NULL, NULL);

	sock->magic = SOCKET_MAGIC;
	*socketp = sock;

	return (ISC_R_SUCCESS);

 error: /* socket allocated */

	return (ret);
}

/*
 * This event requires that the various lists be empty, that the reference
 * count be 1, and that the magic number is valid.  The other socket bits,
 * like the lock, must be initialized as well.  The fd associated must be
 * marked as closed, by setting it to -1 on close, or this routine will
 * also close the socket.
 */
static void
free_socket(isc_socket_t **socketp) {
	isc_socket_t *sock = *socketp;

	INSIST(sock->references == 0);
	INSIST(VALID_SOCKET(sock));
	INSIST(!sock->connecting);
	INSIST(!sock->pending_recv);
	INSIST(!sock->pending_send);
	INSIST(!sock->pending_accept);
	INSIST(ISC_LIST_EMPTY(sock->recv_list));
	INSIST(ISC_LIST_EMPTY(sock->send_list));
	INSIST(ISC_LIST_EMPTY(sock->accept_list));
	INSIST(!ISC_LINK_LINKED(sock, link));

	sock->magic = 0;

	DESTROYLOCK(&sock->lock);

	isc_mem_put(sock->manager->mctx, sock, sizeof *sock);

	*socketp = NULL;
}

/*
 * Create a new 'type' socket managed by 'manager'.  Events
 * will be posted to 'task' and when dispatched 'action' will be
 * called with 'arg' as the arg value.  The new socket is returned
 * in 'socketp'.
 */
isc_result_t
isc_socket_create(isc_socketmgr_t *manager, int pf, isc_sockettype_t type,
		  isc_socket_t **socketp)
{
	isc_socket_t *sock = NULL;
	isc_result_t ret;
#if defined(USE_CMSG) || defined(SO_BSDCOMPAT)
	int on = 1;
#endif

	REQUIRE(VALID_MANAGER(manager));
	REQUIRE(socketp != NULL && *socketp == NULL);

	ret = allocate_socket(manager, type, &sock);
	if (ret != ISC_R_SUCCESS)
		return (ret);

	sock->pf = pf;
	switch (type) {
	case isc_sockettype_udp:
		sock->fd = socket(pf, SOCK_DGRAM, IPPROTO_UDP);
		break;
	case isc_sockettype_tcp:
		sock->fd = socket(pf, SOCK_STREAM, IPPROTO_TCP);
		break;
	}
	if (sock->fd < 0) {
		free_socket(&sock);

		switch (errno) {
		case EMFILE:
		case ENFILE:
		case ENOBUFS:
			return (ISC_R_NORESOURCES);

		case EPROTONOSUPPORT:
		case EPFNOSUPPORT:
		case EAFNOSUPPORT:
			return (ISC_R_FAMILYNOSUPPORT);

		default:
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "socket() %s: %s",
					 isc_msgcat_get(isc_msgcat,
							ISC_MSGSET_GENERAL,
							ISC_MSG_FAILED,
							"failed"),
					 strerror(errno));
			return (ISC_R_UNEXPECTED);
		}
	}

	if (make_nonblock(sock->fd) != ISC_R_SUCCESS) {
		free_socket(&sock);
		return (ISC_R_UNEXPECTED);
	}

#ifdef SO_BSDCOMPAT
	if (setsockopt(sock->fd, SOL_SOCKET, SO_BSDCOMPAT,
		       (void *)&on, sizeof on) < 0) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "setsockopt(%d, SO_BSDCOMPAT) %s: %s",
				 sock->fd,
				 isc_msgcat_get(isc_msgcat, ISC_MSGSET_GENERAL,
						ISC_MSG_FAILED, "failed"),
				 strerror(errno));
		/* Press on... */
	}
#endif

#if defined(USE_CMSG)
	if (type == isc_sockettype_udp) {

#if defined(SO_TIMESTAMP)
		if (setsockopt(sock->fd, SOL_SOCKET, SO_TIMESTAMP,
			       (void *)&on, sizeof on) < 0
		    && errno != ENOPROTOOPT) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "setsockopt(%d, SO_TIMESTAMP) %s: %s",
					 sock->fd, 
					 isc_msgcat_get(isc_msgcat,
							ISC_MSGSET_GENERAL,
							ISC_MSG_FAILED,
							"failed"),
					 strerror(errno));
			/* Press on... */
		}
#endif /* SO_TIMESTAMP */

#if defined(ISC_PLATFORM_HAVEIPV6)
#ifdef IPV6_RECVPKTINFO
		/* 2292bis */
		if ((pf == AF_INET6)
		    && (setsockopt(sock->fd, IPPROTO_IPV6, IPV6_RECVPKTINFO,
				   (void *)&on, sizeof (on)) < 0)) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "setsockopt(%d, IPV6_RECVPKTINFO) "
					 "%s: %s", sock->fd,
					 isc_msgcat_get(isc_msgcat,
							ISC_MSGSET_GENERAL,
							ISC_MSG_FAILED,
							"failed"),
					 strerror(errno));
		}
#else
		/* 2292 */
		if ((pf == AF_INET6)
		    && (setsockopt(sock->fd, IPPROTO_IPV6, IPV6_PKTINFO,
				   (void *)&on, sizeof (on)) < 0)) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "setsockopt(%d, IPV6_PKTINFO) %s: %s",
					 sock->fd,
					 isc_msgcat_get(isc_msgcat,
							ISC_MSGSET_GENERAL,
							ISC_MSG_FAILED,
							"failed"),
					 strerror(errno));
		}
#endif /* IPV6_RECVPKTINFO */
#ifdef IPV6_USE_MIN_MTU        /*2292bis, not too common yet*/
		/* use minimum MTU */
		if (pf == AF_INET6) {
			(void)setsockopt(sock->fd, IPPROTO_IPV6,
					 IPV6_USE_MIN_MTU,
					 (void *)&on, sizeof (on));
		}
#endif
#endif /* ISC_PLATFORM_HAVEIPV6 */

	}
#endif /* USE_CMSG */

	sock->references = 1;
	*socketp = sock;

	LOCK(&manager->lock);

	/*
	 * Note we don't have to lock the socket like we normally would because
	 * there are no external references to it yet.
	 */

	manager->fds[sock->fd] = sock;
	manager->fdstate[sock->fd] = MANAGED;
	ISC_LIST_APPEND(manager->socklist, sock, link);
	if (manager->maxfd < sock->fd)
		manager->maxfd = sock->fd;

	UNLOCK(&manager->lock);

	socket_log(sock, NULL, CREATION, isc_msgcat, ISC_MSGSET_SOCKET,
		   ISC_MSG_CREATED, "created");

	return (ISC_R_SUCCESS);
}

/*
 * Attach to a socket.  Caller must explicitly detach when it is done.
 */
void
isc_socket_attach(isc_socket_t *sock, isc_socket_t **socketp) {
	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(socketp != NULL && *socketp == NULL);

	LOCK(&sock->lock);
	sock->references++;
	UNLOCK(&sock->lock);

	*socketp = sock;
}

/*
 * Dereference a socket.  If this is the last reference to it, clean things
 * up by destroying the socket.
 */
void
isc_socket_detach(isc_socket_t **socketp) {
	isc_socket_t *sock;
	isc_boolean_t kill_socket = ISC_FALSE;

	REQUIRE(socketp != NULL);
	sock = *socketp;
	REQUIRE(VALID_SOCKET(sock));

	LOCK(&sock->lock);
	REQUIRE(sock->references > 0);
	sock->references--;
	if (sock->references == 0)
		kill_socket = ISC_TRUE;
	UNLOCK(&sock->lock);

	if (kill_socket)
		destroy(&sock);

	*socketp = NULL;
}

/*
 * I/O is possible on a given socket.  Schedule an event to this task that
 * will call an internal function to do the I/O.  This will charge the
 * task with the I/O operation and let our select loop handler get back
 * to doing something real as fast as possible.
 *
 * The socket and manager must be locked before calling this function.
 */
static void
dispatch_recv(isc_socket_t *sock) {
	intev_t *iev;
	isc_socketevent_t *ev;

	INSIST(!sock->pending_recv);

	ev = ISC_LIST_HEAD(sock->recv_list);
	if (ev == NULL)
		return;

	sock->pending_recv = 1;
	iev = &sock->readable_ev;

	socket_log(sock, NULL, EVENT, NULL, 0, 0,
		   "dispatch_recv:  event %p -> task %p", ev, ev->ev_sender);

	sock->references++;
	iev->ev_sender = sock;
	iev->ev_action = internal_recv;
	iev->ev_arg = sock;

 	isc_task_send(ev->ev_sender, (isc_event_t **)&iev);
}

static void
dispatch_send(isc_socket_t *sock) {
	intev_t *iev;
	isc_socketevent_t *ev;

	INSIST(!sock->pending_send);

	ev = ISC_LIST_HEAD(sock->send_list);
	if (ev == NULL)
		return;

	sock->pending_send = 1;
	iev = &sock->writable_ev;

	socket_log(sock, NULL, EVENT, NULL, 0, 0,
		   "dispatch_send:  event %p -> task %p", ev, ev->ev_sender);

	sock->references++;
	iev->ev_sender = sock;
	iev->ev_action = internal_send;
	iev->ev_arg = sock;

	isc_task_send(ev->ev_sender, (isc_event_t **)&iev);
}

/*
 * Dispatch an internal accept event.
 */
static void
dispatch_accept(isc_socket_t *sock) {
	intev_t *iev;
	isc_socket_newconnev_t *ev;

	INSIST(!sock->pending_accept);

	/*
	 * Are there any done events left, or were they all canceled
	 * before the manager got the socket lock?
	 */
	ev = ISC_LIST_HEAD(sock->accept_list);
	if (ev == NULL)
		return;

	sock->pending_accept = 1;
	iev = &sock->readable_ev;

	sock->references++;  /* keep socket around for this internal event */
	iev->ev_sender = sock;
	iev->ev_action = internal_accept;
	iev->ev_arg = sock;

	isc_task_send(ev->ev_sender, (isc_event_t **)&iev);
}

static void
dispatch_connect(isc_socket_t *sock) {
	intev_t *iev;
	isc_socket_connev_t *ev;

	iev = &sock->writable_ev;

	ev = sock->connect_ev;
	INSIST(ev != NULL); /* XXX */

	INSIST(sock->connecting);

	sock->references++;  /* keep socket around for this internal event */
	iev->ev_sender = sock;
	iev->ev_action = internal_connect;
	iev->ev_arg = sock;

	isc_task_send(ev->ev_sender, (isc_event_t **)&iev);
}

/*
 * Dequeue an item off the given socket's read queue, set the result code
 * in the done event to the one provided, and send it to the task it was
 * destined for.
 *
 * If the event to be sent is on a list, remove it before sending.  If
 * asked to, send and detach from the socket as well.
 *
 * Caller must have the socket locked if the event is attached to the socket.
 */
static void
send_recvdone_event(isc_socket_t *sock, isc_socketevent_t **dev) {
	isc_task_t *task;

	task = (*dev)->ev_sender;

	(*dev)->ev_sender = sock;

	if (ISC_LINK_LINKED(*dev, ev_link))
		ISC_LIST_DEQUEUE(sock->recv_list, *dev, ev_link);

	if (((*dev)->attributes & ISC_SOCKEVENTATTR_ATTACHED)
	    == ISC_SOCKEVENTATTR_ATTACHED)
		isc_task_sendanddetach(&task, (isc_event_t **)dev);
	else
		isc_task_send(task, (isc_event_t **)dev);
}

/*
 * See comments for send_recvdone_event() above.
 *
 * Caller must have the socket locked if the event is attached to the socket.
 */
static void
send_senddone_event(isc_socket_t *sock, isc_socketevent_t **dev) {
	isc_task_t *task;

	INSIST(dev != NULL && *dev != NULL);

	task = (*dev)->ev_sender;
	(*dev)->ev_sender = sock;

	if (ISC_LINK_LINKED(*dev, ev_link))
		ISC_LIST_DEQUEUE(sock->send_list, *dev, ev_link);

	if (((*dev)->attributes & ISC_SOCKEVENTATTR_ATTACHED)
	    == ISC_SOCKEVENTATTR_ATTACHED)
		isc_task_sendanddetach(&task, (isc_event_t **)dev);
	else
		isc_task_send(task, (isc_event_t **)dev);
}

/*
 * Call accept() on a socket, to get the new file descriptor.  The listen
 * socket is used as a prototype to create a new isc_socket_t.  The new
 * socket has one outstanding reference.  The task receiving the event
 * will be detached from just after the event is delivered.
 *
 * On entry to this function, the event delivered is the internal
 * readable event, and the first item on the accept_list should be
 * the done event we want to send.  If the list is empty, this is a no-op,
 * so just unlock and return.
 */
static void
internal_accept(isc_task_t *me, isc_event_t *ev) {
	isc_socket_t *sock;
	isc_socketmgr_t *manager;
	isc_socket_newconnev_t *dev;
	isc_task_t *task;
	ISC_SOCKADDR_LEN_T addrlen;
	int fd;
	isc_result_t result = ISC_R_SUCCESS;

	UNUSED(me);

	sock = ev->ev_sender;
	INSIST(VALID_SOCKET(sock));

	LOCK(&sock->lock);
	socket_log(sock, NULL, TRACE,
		   isc_msgcat, ISC_MSGSET_SOCKET, ISC_MSG_ACCEPTLOCK,
		   "internal_accept called, locked socket");

	manager = sock->manager;
	INSIST(VALID_MANAGER(manager));

	INSIST(sock->listener);
	INSIST(sock->pending_accept == 1);
	sock->pending_accept = 0;

	INSIST(sock->references > 0);
	sock->references--;  /* the internal event is done with this socket */
	if (sock->references == 0) {
		UNLOCK(&sock->lock);
		destroy(&sock);
		return;
	}

	/*
	 * Get the first item off the accept list.
	 * If it is empty, unlock the socket and return.
	 */
	dev = ISC_LIST_HEAD(sock->accept_list);
	if (dev == NULL) {
		UNLOCK(&sock->lock);
		return;
	}

	/*
	 * Try to accept the new connection.  If the accept fails with
	 * EAGAIN or EINTR, simply poke the watcher to watch this socket
	 * again.
	 */
	addrlen = sizeof dev->newsocket->address.type;
	memset(&dev->newsocket->address.type.sa, 0, addrlen);
	fd = accept(sock->fd, &dev->newsocket->address.type.sa,
		    (void *)&addrlen);
	if (fd < 0) {
		if (SOFT_ERROR(errno)) {
			goto soft_error;
		} else {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "internal_accept: accept() %s: %s",
					 isc_msgcat_get(isc_msgcat,
							ISC_MSGSET_GENERAL,
							ISC_MSG_FAILED,
							"failed"),
					 strerror(errno));
			fd = -1;
			result = ISC_R_UNEXPECTED;
		}
	} else {
		if (addrlen == 0) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "internal_accept(): "
					 "accept() failed to return "
					 "remote address");

			(void)closesocket(fd);
			goto soft_error;
		} else if (dev->newsocket->address.type.sa.sa_family !=
			   sock->pf)
		{
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "internal_accept(): "
					 "accept() returned peer address "
					 "family %u (expected %u)", 
					 dev->newsocket->address.
					 type.sa.sa_family,
					 sock->pf);
			(void)closesocket(fd);
			goto soft_error;
		}
	}

	if (fd != -1) {
		dev->newsocket->address.length = addrlen;
		dev->newsocket->pf = sock->pf;
	}

	/*
	 * Pull off the done event.
	 */
	ISC_LIST_UNLINK(sock->accept_list, dev, ev_link);

	/*
	 * Poke watcher if there are more pending accepts.
	 */
	if (!ISC_LIST_EMPTY(sock->accept_list))
		select_poke(sock->manager, sock->fd, SELECT_POKE_ACCEPT);

	UNLOCK(&sock->lock);

	if (fd != -1 && (make_nonblock(fd) != ISC_R_SUCCESS)) {
		closesocket(fd);
		fd = -1;
		result = ISC_R_UNEXPECTED;
	}

	LOCK(&manager->lock);
	ISC_LIST_APPEND(manager->socklist, dev->newsocket, link);

	/*
	 * -1 means the new socket didn't happen.
	 */
	if (fd != -1) {
		dev->newsocket->fd = fd;
		dev->newsocket->bound = 1;
		dev->newsocket->connected = 1;

		/*
		 * Save away the remote address
		 */
		dev->address = dev->newsocket->address;

		manager->fds[fd] = dev->newsocket;
		manager->fdstate[fd] = MANAGED;
		if (manager->maxfd < fd)
			manager->maxfd = fd;

		socket_log(sock, &dev->newsocket->address, CREATION,
			   isc_msgcat, ISC_MSGSET_SOCKET, ISC_MSG_ACCEPTEDCXN,
			   "accepted connection, new socket %p",
			   dev->newsocket);
	}

	UNLOCK(&manager->lock);

	/*
	 * Fill in the done event details and send it off.
	 */
	dev->result = result;
	task = dev->ev_sender;
	dev->ev_sender = sock;

	isc_task_sendanddetach(&task, (isc_event_t **)&dev);
	return;

 soft_error:
	select_poke(sock->manager, sock->fd, SELECT_POKE_ACCEPT);
	UNLOCK(&sock->lock);
	return;
}

static void
internal_recv(isc_task_t *me, isc_event_t *ev) {
	isc_socketevent_t *dev;
	isc_socket_t *sock;

	INSIST(ev->ev_type == ISC_SOCKEVENT_INTR);

	sock = ev->ev_sender;
	INSIST(VALID_SOCKET(sock));

	LOCK(&sock->lock);
	socket_log(sock, NULL, IOEVENT,
		   isc_msgcat, ISC_MSGSET_SOCKET, ISC_MSG_INTERNALRECV,
		   "internal_recv: task %p got event %p", me, ev, sock);

	INSIST(sock->pending_recv == 1);
	sock->pending_recv = 0;

	INSIST(sock->references > 0);
	sock->references--;  /* the internal event is done with this socket */
	if (sock->references == 0) {
		UNLOCK(&sock->lock);
		destroy(&sock);
		return;
	}

	/*
	 * Try to do as much I/O as possible on this socket.  There are no
	 * limits here, currently.
	 */
	dev = ISC_LIST_HEAD(sock->recv_list);
	while (dev != NULL) {
		switch (doio_recv(sock, dev)) {
		case DOIO_SOFT:
			goto poke;

		case DOIO_EOF:
			/*
			 * read of 0 means the remote end was closed.
			 * Run through the event queue and dispatch all
			 * the events with an EOF result code.
			 */
			do {
				dev->result = ISC_R_EOF;
				send_recvdone_event(sock, &dev);
				dev = ISC_LIST_HEAD(sock->recv_list);
			} while (dev != NULL);
			goto poke;

		case DOIO_SUCCESS:
		case DOIO_HARD:
			send_recvdone_event(sock, &dev);
			break;
		}

		dev = ISC_LIST_HEAD(sock->recv_list);
	}

 poke:
	if (!ISC_LIST_EMPTY(sock->recv_list))
		select_poke(sock->manager, sock->fd, SELECT_POKE_READ);

	UNLOCK(&sock->lock);
}

static void
internal_send(isc_task_t *me, isc_event_t *ev) {
	isc_socketevent_t *dev;
	isc_socket_t *sock;

	INSIST(ev->ev_type == ISC_SOCKEVENT_INTW);

	/*
	 * Find out what socket this is and lock it.
	 */
	sock = (isc_socket_t *)ev->ev_sender;
	INSIST(VALID_SOCKET(sock));

	LOCK(&sock->lock);
	socket_log(sock, NULL, IOEVENT,
		   isc_msgcat, ISC_MSGSET_SOCKET, ISC_MSG_INTERNALSEND,
		   "internal_send: task %p got event %p", me, ev, sock);

	INSIST(sock->pending_send == 1);
	sock->pending_send = 0;

	INSIST(sock->references > 0);
	sock->references--;  /* the internal event is done with this socket */
	if (sock->references == 0) {
		UNLOCK(&sock->lock);
		destroy(&sock);
		return;
	}

	/*
	 * Try to do as much I/O as possible on this socket.  There are no
	 * limits here, currently.
	 */
	dev = ISC_LIST_HEAD(sock->send_list);
	while (dev != NULL) {
		switch (doio_send(sock, dev)) {
		case DOIO_SOFT:
			goto poke;

		case DOIO_HARD:
		case DOIO_SUCCESS:
			send_senddone_event(sock, &dev);
			break;
		}

		dev = ISC_LIST_HEAD(sock->send_list);
	}

 poke:
	if (!ISC_LIST_EMPTY(sock->send_list))
		select_poke(sock->manager, sock->fd, SELECT_POKE_WRITE);

	UNLOCK(&sock->lock);
}

static void
process_fds(isc_socketmgr_t *manager, int maxfd,
	    fd_set *readfds, fd_set *writefds)
{
	int i;
	isc_socket_t *sock;
	isc_boolean_t unlock_sock;

	/*
	 * Process read/writes on other fds here.  Avoid locking
	 * and unlocking twice if both reads and writes are possible.
	 */
	for (i = 0 ; i < maxfd ; i++) {
		if (manager->fdstate[i] == CLOSE_PENDING) {
			manager->fdstate[i] = CLOSED;
			FD_CLR(i, &manager->read_fds);
			FD_CLR(i, &manager->write_fds);

			closesocket(i);

			continue;
		}

		sock = manager->fds[i];
		unlock_sock = ISC_FALSE;
		if (FD_ISSET(i, readfds)) {
			if (sock == NULL) {
				FD_CLR(i, &manager->read_fds);
				goto check_write;
			}
			unlock_sock = ISC_TRUE;
			LOCK(&sock->lock);
			if (!SOCK_DEAD(sock)) {
				if (sock->listener)
					dispatch_accept(sock);
				else
					dispatch_recv(sock);
			}
			FD_CLR(i, &manager->read_fds);
		}
	check_write:
		if (FD_ISSET(i, writefds)) {
			if (sock == NULL) {
				FD_CLR(i, &manager->write_fds);
				continue;
			}
			if (!unlock_sock) {
				unlock_sock = ISC_TRUE;
				LOCK(&sock->lock);
			}
			if (!SOCK_DEAD(sock)) {
				if (sock->connecting)
					dispatch_connect(sock);
				else
					dispatch_send(sock);
			}
			FD_CLR(i, &manager->write_fds);
		}
		if (unlock_sock)
			UNLOCK(&sock->lock);
	}
}

#ifdef ISC_PLATFORM_USETHREADS
/*
 * This is the thread that will loop forever, always in a select or poll
 * call.
 *
 * When select returns something to do, track down what thread gets to do
 * this I/O and post the event to it.
 */
static isc_threadresult_t WINAPI
watcher(void *uap) {
	isc_socketmgr_t *manager = uap;
	isc_boolean_t done;
	int ctlfd;
	int cc;
	fd_set readfds;
	fd_set writefds;
	int msg, fd;
	int maxfd;

	/* 30 Second timeout on select in case it's necesasary */
	struct timeval tv;
	tv.tv_sec = 2;
	tv.tv_usec = 0;

	/*
	 * Get the control fd here.  This will never change.
	 */
	LOCK(&manager->lock);
	ctlfd = manager->pipe_fds[0];

	done = ISC_FALSE;
	while (!done) {
		do {
			readfds = manager->read_fds;
			writefds = manager->write_fds;
			maxfd = manager->maxfd + 1;

			UNLOCK(&manager->lock);

			cc = select(maxfd, &readfds, &writefds, NULL, &tv);
			if (cc < 0) {
				if (!SOFT_ERROR(errno))
					FATAL_ERROR(__FILE__, __LINE__,
						    "select() %s: %s",
						    isc_msgcat_get(isc_msgcat,
							    ISC_MSGSET_GENERAL,
							    ISC_MSG_FAILED,
							    "failed"),
						    strerror(errno));
			}

			LOCK(&manager->lock);
		} while (cc < 0 && bpipe_written <= 0);


		/*
		 * Process reads on internal, control fd.
		 */
		while (bpipe_written > 0) {
				select_readmsg(manager, &fd, &msg);
				InterlockedDecrement(&bpipe_written);

				manager_log(manager, IOEVENT,
					    isc_msgcat_get(isc_msgcat,
						     ISC_MSGSET_SOCKET,
						     ISC_MSG_WATCHERMSG,
						     "watcher got message %d"),
						     msg);

				/*
				 * Nothing to read?
				 */
				if (msg == SELECT_POKE_NOTHING)
					break;

				/*
				 * Handle shutdown message.  We really should
				 * jump out of this loop right away, but
				 * it doesn't matter if we have to do a little
				 * more work first.
				 */
				if (msg == SELECT_POKE_SHUTDOWN) {
					done = ISC_TRUE;

					break;
				}
		}
		process_fds(manager, maxfd, &readfds, &writefds);
	}

	manager_log(manager, TRACE,
		    isc_msgcat_get(isc_msgcat, ISC_MSGSET_GENERAL,
				   ISC_MSG_EXITING, "watcher exiting"));

	UNLOCK(&manager->lock);
	return ((isc_threadresult_t)0);
}
#endif /* ISC_PLATFORM_USETHREADS */

/*
 * Create a new socket manager.
 */
isc_result_t
isc_socketmgr_create(isc_mem_t *mctx, isc_socketmgr_t **managerp) {
	isc_socketmgr_t *manager;

	REQUIRE(managerp != NULL && *managerp == NULL);

#ifndef ISC_PLATFORM_USETHREADS
	if (socketmgr != NULL) {
		socketmgr->refs++;
		*managerp = socketmgr;
		return (ISC_R_SUCCESS);
	}
#endif /* ISC_PLATFORM_USETHREADS */

	manager = isc_mem_get(mctx, sizeof *manager);
	if (manager == NULL)
		return (ISC_R_NOMEMORY);

	manager->magic = SOCKET_MANAGER_MAGIC;
	manager->mctx = NULL;
	memset(manager->fds, 0, sizeof(manager->fds));
	ISC_LIST_INIT(manager->socklist);
	if (isc_mutex_init(&manager->lock) != ISC_R_SUCCESS) {
		isc_mem_put(mctx, manager, sizeof *manager);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_mutex_init() %s",
				 isc_msgcat_get(isc_msgcat, ISC_MSGSET_GENERAL,
						ISC_MSG_FAILED, "failed"));
		return (ISC_R_UNEXPECTED);
	}
	if (isc_condition_init(&manager->shutdown_ok) != ISC_R_SUCCESS) {
		DESTROYLOCK(&manager->lock);
		isc_mem_put(mctx, manager, sizeof *manager);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_condition_init() %s",
				 isc_msgcat_get(isc_msgcat, ISC_MSGSET_GENERAL,
						ISC_MSG_FAILED, "failed"));
		return (ISC_R_UNEXPECTED);
	}

	/*
	 * Create the special fds that will be used to wake up the
	 * select/poll loop when something internal needs to be done.
	 */
	if (internal_pipe(manager->pipe_fds) != 0) {
		DESTROYLOCK(&manager->lock);
		isc_mem_put(mctx, manager, sizeof *manager);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "pipe() %s: %s",
				 isc_msgcat_get(isc_msgcat, ISC_MSGSET_GENERAL,
						ISC_MSG_FAILED, "failed"),
				 strerror(errno));

		return (ISC_R_UNEXPECTED);
	}


	/*
	 * Set up initial state for the select loop
	 */
	FD_ZERO(&manager->read_fds);
	FD_ZERO(&manager->write_fds);
	manager->maxfd = 0;
	memset(manager->fdstate, 0, sizeof(manager->fdstate));

	/*
	 * Start up the select/poll thread.
	 */
	if (isc_thread_create(watcher, manager, &manager->watcher) !=
	    ISC_R_SUCCESS) {
		DESTROYLOCK(&manager->lock);
		isc_mem_put(mctx, manager, sizeof *manager);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_thread_create() %s",
				 isc_msgcat_get(isc_msgcat, ISC_MSGSET_GENERAL,
						ISC_MSG_FAILED, "failed"));
		_close(manager->pipe_fds[0]);
		_close(manager->pipe_fds[1]);
		return (ISC_R_UNEXPECTED);
	}
	isc_mem_attach(mctx, &manager->mctx);

	*managerp = manager;

	return (ISC_R_SUCCESS);
}

void
isc_socketmgr_destroy(isc_socketmgr_t **managerp) {
	isc_socketmgr_t *manager;
	int i;
	isc_mem_t *mctx;

	/*
	 * Destroy a socket manager.
	 */

	REQUIRE(managerp != NULL);
	manager = *managerp;
	REQUIRE(VALID_MANAGER(manager));

#ifndef ISC_PLATFORM_USETHREADS
	if (manager->refs > 1) {
		manager->refs--;
		*managerp = NULL;
		return;
	}
#endif /* ISC_PLATFORM_USETHREADS */

	LOCK(&manager->lock);

#ifdef ISC_PLATFORM_USETHREADS
	/*
	 * Wait for all sockets to be destroyed.
	 */
	while (!ISC_LIST_EMPTY(manager->socklist)) {
		manager_log(manager, CREATION,
			    isc_msgcat_get(isc_msgcat, ISC_MSGSET_SOCKET,
					   ISC_MSG_SOCKETSREMAIN,
					   "sockets exist"));
		WAIT(&manager->shutdown_ok, &manager->lock);
	}
#else /* ISC_PLATFORM_USETHREADS */
	/*
	 * Hope all sockets have been destroyed.
	 */
	if (!ISC_LIST_EMPTY(manager->socklist)) {
		manager_log(manager, CREATION,
			    isc_msgcat_get(isc_msgcat, ISC_MSGSET_SOCKET,
					   ISC_MSG_SOCKETSREMAIN,
					   "sockets exist"));
		INSIST(0);
	}
#endif /* ISC_PLATFORM_USETHREADS */

	UNLOCK(&manager->lock);

	/*
	 * Here, poke our select/poll thread.  Do this by closing the write
	 * half of the pipe, which will send EOF to the read half.
	 * This is currently a no-op in the non-threaded case.
	 */
	select_poke(manager, 0, SELECT_POKE_SHUTDOWN);

#ifdef ISC_PLATFORM_USETHREADS
	/*
	 * Wait for thread to exit.
	 */
	if (isc_thread_join(manager->watcher, NULL) != ISC_R_SUCCESS)
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_thread_join() %s",
				 isc_msgcat_get(isc_msgcat, ISC_MSGSET_GENERAL,
						ISC_MSG_FAILED, "failed"));
#endif /* ISC_PLATFORM_USETHREADS */

	/*
	 * Clean up.
	 */
#ifdef ISC_PLATFORM_USETHREADS
	_close(manager->pipe_fds[0]);
	_close(manager->pipe_fds[1]);
	(void)isc_condition_destroy(&manager->shutdown_ok);
#endif /* ISC_PLATFORM_USETHREADS */

	for (i = 0 ; i < FD_SETSIZE ; i++)
		if (manager->fdstate[i] == CLOSE_PENDING)
			closesocket(i);

	DESTROYLOCK(&manager->lock);
	manager->magic = 0;
	mctx= manager->mctx;
	isc_mem_put(mctx, manager, sizeof *manager);

	isc_mem_detach(&mctx);

	*managerp = NULL;
}

static isc_result_t
socket_recv(isc_socket_t *sock, isc_socketevent_t *dev, isc_task_t *task,
	    unsigned int flags)
{
	int io_state;
	isc_boolean_t have_lock = ISC_FALSE;
	isc_task_t *ntask = NULL;
	isc_result_t result = ISC_R_SUCCESS;

	dev->ev_sender = task;

	if (sock->type == isc_sockettype_udp) {
		io_state = doio_recv(sock, dev);
	} else {
		LOCK(&sock->lock);
		have_lock = ISC_TRUE;

		if (ISC_LIST_EMPTY(sock->recv_list))
			io_state = doio_recv(sock, dev);
		else
			io_state = DOIO_SOFT;
	}

	switch (io_state) {
	case DOIO_SOFT:
		/*
		 * We couldn't read all or part of the request right now, so
		 * queue it.
		 *
		 * Attach to socket and to task
		 */
		isc_task_attach(task, &ntask);
		dev->attributes |= ISC_SOCKEVENTATTR_ATTACHED;

		if (!have_lock) {
			LOCK(&sock->lock);
			have_lock = ISC_TRUE;
		}

		/*
		 * Enqueue the request.  If the socket was previously not being
		 * watched, poke the watcher to start paying attention to it.
		 */
		if (ISC_LIST_EMPTY(sock->recv_list))
			select_poke(sock->manager, sock->fd, SELECT_POKE_READ);
		ISC_LIST_ENQUEUE(sock->recv_list, dev, ev_link);

		socket_log(sock, NULL, EVENT, NULL, 0, 0,
			   "socket_recv: event %p -> task %p",
			   dev, ntask);

		if ((flags & ISC_SOCKFLAG_IMMEDIATE) != 0)
			result = ISC_R_INPROGRESS;
		break;

	case DOIO_EOF:
		dev->result = ISC_R_EOF;
		/* fallthrough */

	case DOIO_HARD:
	case DOIO_SUCCESS:
		if ((flags & ISC_SOCKFLAG_IMMEDIATE) == 0)
			send_recvdone_event(sock, &dev);
		break;
	}

	if (have_lock)
		UNLOCK(&sock->lock);

	return (result);
}

isc_result_t
isc_socket_recvv(isc_socket_t *sock, isc_bufferlist_t *buflist,
		 unsigned int minimum, isc_task_t *task,
		 isc_taskaction_t action, const void *arg)
{
	isc_socketevent_t *dev;
	isc_socketmgr_t *manager;
	unsigned int iocount;
	isc_buffer_t *buffer;

	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(buflist != NULL);
	REQUIRE(!ISC_LIST_EMPTY(*buflist));
	REQUIRE(task != NULL);
	REQUIRE(action != NULL);

	manager = sock->manager;
	REQUIRE(VALID_MANAGER(manager));

	iocount = isc_bufferlist_availablecount(buflist);
	REQUIRE(iocount > 0);

	INSIST(sock->bound);

	dev = allocate_socketevent(sock, ISC_SOCKEVENT_RECVDONE, action, arg);
	if (dev == NULL) {
		return (ISC_R_NOMEMORY);
	}

	/*
	 * UDP sockets are always partial read
	 */
	if (sock->type == isc_sockettype_udp)
		dev->minimum = 1;
	else {
		if (minimum == 0)
			dev->minimum = iocount;
		else
			dev->minimum = minimum;
	}

	/*
	 * Move each buffer from the passed in list to our internal one.
	 */
	buffer = ISC_LIST_HEAD(*buflist);
	while (buffer != NULL) {
		ISC_LIST_DEQUEUE(*buflist, buffer, link);
		ISC_LIST_ENQUEUE(dev->bufferlist, buffer, link);
		buffer = ISC_LIST_HEAD(*buflist);
	}

	return (socket_recv(sock, dev, task, 0));
}

isc_result_t
isc_socket_recv(isc_socket_t *sock, isc_region_t *region, unsigned int minimum,
		isc_task_t *task, isc_taskaction_t action, const void *arg)
{
	isc_socketevent_t *dev;
	isc_socketmgr_t *manager;

	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(action != NULL);

	manager = sock->manager;
	REQUIRE(VALID_MANAGER(manager));

	INSIST(sock->bound);

	dev = allocate_socketevent(sock, ISC_SOCKEVENT_RECVDONE, action, arg);
	if (dev == NULL)
		return (ISC_R_NOMEMORY);

	return (isc_socket_recv2(sock, region, minimum, task, dev, 0));
}

isc_result_t
isc_socket_recv2(isc_socket_t *sock, isc_region_t *region,
		 unsigned int minimum, isc_task_t *task,
		 isc_socketevent_t *event, unsigned int flags)
{
	event->ev_sender = sock;
	event->result = ISC_R_UNEXPECTED;
	ISC_LIST_INIT(event->bufferlist);
	event->region = *region;
	event->n = 0;
	event->offset = 0;
	event->attributes = 0;

	/*
	 * UDP sockets are always partial read.
	 */
	if (sock->type == isc_sockettype_udp)
		event->minimum = 1;
	else {
		if (minimum == 0)
			event->minimum = region->length;
		else
			event->minimum = minimum;
	}

	return (socket_recv(sock, event, task, flags));
}

static isc_result_t
socket_send(isc_socket_t *sock, isc_socketevent_t *dev, isc_task_t *task,
	    isc_sockaddr_t *address, struct in6_pktinfo *pktinfo,
	    unsigned int flags)
{
	int io_state;
	isc_boolean_t have_lock = ISC_FALSE;
	isc_task_t *ntask = NULL;
	isc_result_t result = ISC_R_SUCCESS;

	dev->ev_sender = task;

	set_dev_address(address, sock, dev);
	if (pktinfo != NULL) {
		socket_log(sock, NULL, TRACE, isc_msgcat, ISC_MSGSET_SOCKET,
			   ISC_MSG_PKTINFOPROVIDED,
			   "pktinfo structure provided, ifindex %u (set to 0)",
			   pktinfo->ipi6_ifindex);

		dev->attributes |= ISC_SOCKEVENTATTR_PKTINFO;
		dev->pktinfo = *pktinfo;
		/*
		 * Set the pktinfo index to 0 here, to let the kernel decide
		 * what interface it should send on.
		 */
		dev->pktinfo.ipi6_ifindex = 0;
	}

	if (sock->type == isc_sockettype_udp)
		io_state = doio_send(sock, dev);
	else {
		LOCK(&sock->lock);
		have_lock = ISC_TRUE;

		if (ISC_LIST_EMPTY(sock->send_list))
			io_state = doio_send(sock, dev);
		else
			io_state = DOIO_SOFT;
	}

	switch (io_state) {
	case DOIO_SOFT:
		/*
		 * We couldn't send all or part of the request right now, so
		 * queue it.
		 */
		isc_task_attach(task, &ntask);
		dev->attributes |= ISC_SOCKEVENTATTR_ATTACHED;

		if (!have_lock) {
			LOCK(&sock->lock);
			have_lock = ISC_TRUE;
		}

		/*
		 * Enqueue the request.  If the socket was previously not being
		 * watched, poke the watcher to start paying attention to it.
		 */
		if (ISC_LIST_EMPTY(sock->send_list))
			select_poke(sock->manager, sock->fd,
				    SELECT_POKE_WRITE);
		ISC_LIST_ENQUEUE(sock->send_list, dev, ev_link);

		socket_log(sock, NULL, EVENT, NULL, 0, 0,
			   "socket_send: event %p -> task %p",
			   dev, ntask);

		if ((flags & ISC_SOCKFLAG_IMMEDIATE) != 0)
			result = ISC_R_INPROGRESS;
		break;

	case DOIO_HARD:
	case DOIO_SUCCESS:
		if ((flags & ISC_SOCKFLAG_IMMEDIATE) == 0)
			send_senddone_event(sock, &dev);
		break;
	}

	if (have_lock)
		UNLOCK(&sock->lock);

	return (result);
}

isc_result_t
isc_socket_send(isc_socket_t *sock, isc_region_t *region,
		isc_task_t *task, isc_taskaction_t action, const void *arg)
{
	/*
	 * REQUIRE() checking is performed in isc_socket_sendto().
	 */
	return (isc_socket_sendto(sock, region, task, action, arg, NULL,
				  NULL));
}

isc_result_t
isc_socket_sendto(isc_socket_t *sock, isc_region_t *region,
		  isc_task_t *task, isc_taskaction_t action, const void *arg,
		  isc_sockaddr_t *address, struct in6_pktinfo *pktinfo)
{
	isc_socketevent_t *dev;
	isc_socketmgr_t *manager;

	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(region != NULL);
	REQUIRE(task != NULL);
	REQUIRE(action != NULL);

	manager = sock->manager;
	REQUIRE(VALID_MANAGER(manager));

	INSIST(sock->bound);

	dev = allocate_socketevent(sock, ISC_SOCKEVENT_SENDDONE, action, arg);
	if (dev == NULL) {
		return (ISC_R_NOMEMORY);
	}

	dev->region = *region;

	return (socket_send(sock, dev, task, address, pktinfo, 0));
}

isc_result_t
isc_socket_sendv(isc_socket_t *sock, isc_bufferlist_t *buflist,
		 isc_task_t *task, isc_taskaction_t action, const void *arg)
{
	return (isc_socket_sendtov(sock, buflist, task, action, arg, NULL,
				   NULL));
}

isc_result_t
isc_socket_sendtov(isc_socket_t *sock, isc_bufferlist_t *buflist,
		   isc_task_t *task, isc_taskaction_t action, const void *arg,
		   isc_sockaddr_t *address, struct in6_pktinfo *pktinfo)
{
	isc_socketevent_t *dev;
	isc_socketmgr_t *manager;
	unsigned int iocount;
	isc_buffer_t *buffer;

	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(buflist != NULL);
	REQUIRE(!ISC_LIST_EMPTY(*buflist));
	REQUIRE(task != NULL);
	REQUIRE(action != NULL);

	manager = sock->manager;
	REQUIRE(VALID_MANAGER(manager));

	iocount = isc_bufferlist_usedcount(buflist);
	REQUIRE(iocount > 0);

	dev = allocate_socketevent(sock, ISC_SOCKEVENT_SENDDONE, action, arg);
	if (dev == NULL) {
		return (ISC_R_NOMEMORY);
	}

	/*
	 * Move each buffer from the passed in list to our internal one.
	 */
	buffer = ISC_LIST_HEAD(*buflist);
	while (buffer != NULL) {
		ISC_LIST_DEQUEUE(*buflist, buffer, link);
		ISC_LIST_ENQUEUE(dev->bufferlist, buffer, link);
		buffer = ISC_LIST_HEAD(*buflist);
	}

	return (socket_send(sock, dev, task, address, pktinfo, 0));
}

isc_result_t
isc_socket_sendto2(isc_socket_t *sock, isc_region_t *region,
		   isc_task_t *task,
		   isc_sockaddr_t *address, struct in6_pktinfo *pktinfo,
		   isc_socketevent_t *event, unsigned int flags)
{
	event->ev_sender = sock;
	event->result = ISC_R_UNEXPECTED;
	ISC_LIST_INIT(event->bufferlist);
	event->region = *region;
	event->n = 0;
	event->offset = 0;
	event->attributes = 0;

	return (socket_send(sock, event, task, address, pktinfo, flags));
}

isc_result_t
isc_socket_bind(isc_socket_t *sock, isc_sockaddr_t *sockaddr) {
	int on = 1;

	LOCK(&sock->lock);

	INSIST(!sock->bound);

	if (sock->pf != sockaddr->type.sa.sa_family) {
		UNLOCK(&sock->lock);
		return (ISC_R_FAMILYMISMATCH);
	}
	if (setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, (void *)&on,
		       sizeof on) < 0) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "setsockopt(%d) %s", sock->fd,
				 isc_msgcat_get(isc_msgcat, ISC_MSGSET_GENERAL,
						ISC_MSG_FAILED, "failed"));
		/* Press on... */
	}
	if (bind(sock->fd, &sockaddr->type.sa, sockaddr->length) < 0) {
		UNLOCK(&sock->lock);
		switch (errno) {
		case EACCES:
			return (ISC_R_NOPERM);
		case EADDRNOTAVAIL:
			return (ISC_R_ADDRNOTAVAIL);
		case EADDRINUSE:
			return (ISC_R_ADDRINUSE);
		case EINVAL:
			return (ISC_R_BOUND);
		default:
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "bind: %s", strerror(errno));
			return (ISC_R_UNEXPECTED);
		}
	}

	socket_log(sock, sockaddr, TRACE,
		   isc_msgcat, ISC_MSGSET_SOCKET, ISC_MSG_BOUND, "bound");
	sock->bound = 1;

	UNLOCK(&sock->lock);
	return (ISC_R_SUCCESS);
}

/*
 * Set up to listen on a given socket.  We do this by creating an internal
 * event that will be dispatched when the socket has read activity.  The
 * watcher will send the internal event to the task when there is a new
 * connection.
 *
 * Unlike in read, we don't preallocate a done event here.  Every time there
 * is a new connection we'll have to allocate a new one anyway, so we might
 * as well keep things simple rather than having to track them.
 */
isc_result_t
isc_socket_listen(isc_socket_t *sock, unsigned int backlog) {
	REQUIRE(VALID_SOCKET(sock));

	LOCK(&sock->lock);

	REQUIRE(!sock->listener);
	REQUIRE(sock->bound);
	REQUIRE(sock->type == isc_sockettype_tcp);

	if (backlog == 0)
		backlog = SOMAXCONN;

	if (listen(sock->fd, (int)backlog) < 0) {
		UNLOCK(&sock->lock);
		UNEXPECTED_ERROR(__FILE__, __LINE__, "listen: %s",
				 strerror(errno));

		return (ISC_R_UNEXPECTED);
	}

	sock->listener = 1;

	UNLOCK(&sock->lock);
	return (ISC_R_SUCCESS);
}

/*
 * This should try to do agressive accept() XXXMLG
 */
isc_result_t
isc_socket_accept(isc_socket_t *sock,
		  isc_task_t *task, isc_taskaction_t action, const void *arg)
{
	isc_socket_newconnev_t *dev;
	isc_socketmgr_t *manager;
	isc_task_t *ntask = NULL;
	isc_socket_t *nsock;
	isc_result_t ret;
	isc_boolean_t do_poke = ISC_FALSE;

	REQUIRE(VALID_SOCKET(sock));
	manager = sock->manager;
	REQUIRE(VALID_MANAGER(manager));

	LOCK(&sock->lock);

	REQUIRE(sock->listener);

	/*
	 * Sender field is overloaded here with the task we will be sending
	 * this event to.  Just before the actual event is delivered the
	 * actual ev_sender will be touched up to be the socket.
	 */
	dev = (isc_socket_newconnev_t *)
		isc_event_allocate(manager->mctx, task, ISC_SOCKEVENT_NEWCONN,
				   action, arg, sizeof (*dev));
	if (dev == NULL) {
		UNLOCK(&sock->lock);
		return (ISC_R_NOMEMORY);
	}
	ISC_LINK_INIT(dev, ev_link);

	ret = allocate_socket(manager, sock->type, &nsock);
	if (ret != ISC_R_SUCCESS) {
		isc_event_free((isc_event_t **)&dev);
		UNLOCK(&sock->lock);
		return (ret);
	}

	/*
	 * Attach to socket and to task.
	 */
	isc_task_attach(task, &ntask);
	nsock->references++;

	dev->ev_sender = ntask;
	dev->newsocket = nsock;

	/*
	 * Poke watcher here.  We still have the socket locked, so there
	 * is no race condition.  We will keep the lock for such a short
	 * bit of time waking it up now or later won't matter all that much.
	 */
	if (ISC_LIST_EMPTY(sock->accept_list))
		do_poke = ISC_TRUE;

	ISC_LIST_ENQUEUE(sock->accept_list, dev, ev_link);

	if (do_poke)
		select_poke(manager, sock->fd, SELECT_POKE_ACCEPT);

	UNLOCK(&sock->lock);
	return (ISC_R_SUCCESS);
}

isc_result_t
isc_socket_connect(isc_socket_t *sock, isc_sockaddr_t *addr,
		   isc_task_t *task, isc_taskaction_t action, const void *arg)
{
	isc_socket_connev_t *dev;
	isc_task_t *ntask = NULL;
	isc_socketmgr_t *manager;
	int cc;

	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(addr != NULL);
	REQUIRE(task != NULL);
	REQUIRE(action != NULL);

	manager = sock->manager;
	REQUIRE(VALID_MANAGER(manager));
	REQUIRE(addr != NULL);

	if (isc_sockaddr_ismulticast(addr))
		return (ISC_R_MULTICAST);

	LOCK(&sock->lock);

	REQUIRE(!sock->connecting);

	dev = (isc_socket_connev_t *)isc_event_allocate(manager->mctx, sock,
							ISC_SOCKEVENT_CONNECT,
							action,	arg,
							sizeof (*dev));
	if (dev == NULL) {
		UNLOCK(&sock->lock);
		return (ISC_R_NOMEMORY);
	}
	ISC_LINK_INIT(dev, ev_link);

	/*
	 * Try to do the connect right away, as there can be only one
	 * outstanding, and it might happen to complete.
	 */
	sock->address = *addr;
	cc = connect(sock->fd, &addr->type.sa, addr->length);
	if (cc < 0) {
		if (SOFT_ERROR(errno) || errno == EINPROGRESS)
			goto queue;

		switch (errno) {
#define ERROR_MATCH(a, b) case a: dev->result = b; goto err_exit;
			ERROR_MATCH(WSAEACCES, ISC_R_NOPERM);
			ERROR_MATCH(WSAEADDRNOTAVAIL, ISC_R_ADDRNOTAVAIL);
			ERROR_MATCH(WSAEAFNOSUPPORT, ISC_R_ADDRNOTAVAIL);
			ERROR_MATCH(WSAECONNREFUSED, ISC_R_CONNREFUSED);
			ERROR_MATCH(WSAEHOSTUNREACH, ISC_R_HOSTUNREACH);
			ERROR_MATCH(WSAEHOSTDOWN, ISC_R_HOSTUNREACH);
			ERROR_MATCH(WSAENETUNREACH, ISC_R_NETUNREACH);
			ERROR_MATCH(WSAENOBUFS, ISC_R_NORESOURCES);
			ERROR_MATCH(EPERM, ISC_R_HOSTUNREACH);
			ERROR_MATCH(EPIPE, ISC_R_NOTCONNECTED);
#undef ERROR_MATCH
		}

		sock->connected = 0;

		UNEXPECTED_ERROR(__FILE__, __LINE__, "%d/%s",
				 errno, strerror(errno));

		UNLOCK(&sock->lock);
		isc_event_free((isc_event_t **)&dev);
		return (ISC_R_UNEXPECTED);

	err_exit:
		sock->connected = 0;
		isc_task_send(task, (isc_event_t **)&dev);

		UNLOCK(&sock->lock);
		return (ISC_R_SUCCESS);
	}

	/*
	 * If connect completed, fire off the done event.
	 */
	if (cc == 0) {
		sock->connected = 1;
		sock->bound = 1;
		dev->result = ISC_R_SUCCESS;
		isc_task_send(task, (isc_event_t **)&dev);

		UNLOCK(&sock->lock);
		return (ISC_R_SUCCESS);
	}

 queue:

	/*
	 * Attach to task.
	 */
	isc_task_attach(task, &ntask);

	sock->connecting = 1;

	dev->ev_sender = ntask;

	/*
	 * Poke watcher here.  We still have the socket locked, so there
	 * is no race condition.  We will keep the lock for such a short
	 * bit of time waking it up now or later won't matter all that much.
	 */
	if (sock->connect_ev == NULL)
		select_poke(manager, sock->fd, SELECT_POKE_CONNECT);

	sock->connect_ev = dev;

	UNLOCK(&sock->lock);
	return (ISC_R_SUCCESS);
}

/*
 * Called when a socket with a pending connect() finishes.
 */
static void
internal_connect(isc_task_t *me, isc_event_t *ev) {
	isc_socket_t *sock;
	isc_socket_connev_t *dev;
	isc_task_t *task;
	int cc;
	ISC_SOCKADDR_LEN_T optlen;

	UNUSED(me);
	INSIST(ev->ev_type == ISC_SOCKEVENT_INTW);

	sock = ev->ev_sender;
	INSIST(VALID_SOCKET(sock));

	LOCK(&sock->lock);

	/*
	 * When the internal event was sent the reference count was bumped
	 * to keep the socket around for us.  Decrement the count here.
	 */
	INSIST(sock->references > 0);
	sock->references--;
	if (sock->references == 0) {
		UNLOCK(&sock->lock);
		destroy(&sock);
		return;
	}

	/*
	 * Has this event been canceled?
	 */
	dev = sock->connect_ev;
	if (dev == NULL) {
		INSIST(!sock->connecting);
		UNLOCK(&sock->lock);
		return;
	}

	INSIST(sock->connecting);
	sock->connecting = 0;

	/*
	 * Get any possible error status here.
	 */
	optlen = sizeof(cc);
	if (getsockopt(sock->fd, SOL_SOCKET, SO_ERROR,
		       (void *)&cc, (void *)&optlen) < 0)
		cc = errno;
	else
		errno = cc;

	if (errno != 0) {
		/*
		 * If the error is EAGAIN, just re-select on this
		 * fd and pretend nothing strange happened.
		 */
		if (SOFT_ERROR(errno) || errno == EINPROGRESS) {
			sock->connecting = 1;
			select_poke(sock->manager, sock->fd,
				    SELECT_POKE_CONNECT);
			UNLOCK(&sock->lock);

			return;
		}

		/*
		 * Translate other errors into ISC_R_* flavors.
		 */
		switch (errno) {
#define ERROR_MATCH(a, b) case a: dev->result = b; break;
			ERROR_MATCH(WSAEACCES, ISC_R_NOPERM);
			ERROR_MATCH(WSAEADDRNOTAVAIL, ISC_R_ADDRNOTAVAIL);
			ERROR_MATCH(WSAEAFNOSUPPORT, ISC_R_ADDRNOTAVAIL);
			ERROR_MATCH(WSAECONNREFUSED, ISC_R_CONNREFUSED);
			ERROR_MATCH(WSAEHOSTUNREACH, ISC_R_HOSTUNREACH);
			ERROR_MATCH(WSAEHOSTDOWN, ISC_R_HOSTUNREACH);
			ERROR_MATCH(WSAENETUNREACH, ISC_R_NETUNREACH);
			ERROR_MATCH(WSAENOBUFS, ISC_R_NORESOURCES);
			ERROR_MATCH(EPERM, ISC_R_HOSTUNREACH);
			ERROR_MATCH(EPIPE, ISC_R_NOTCONNECTED);
			ERROR_MATCH(WSAETIMEDOUT, ISC_R_TIMEDOUT);
#undef ERROR_MATCH
		default:
			dev->result = ISC_R_UNEXPECTED;
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "internal_connect: connect() %s",
					 strerror(errno));
		}
	} else {
		dev->result = ISC_R_SUCCESS;
		sock->connected = 1;
		sock->bound = 1;
	}

	sock->connect_ev = NULL;

	UNLOCK(&sock->lock);

	task = dev->ev_sender;
	dev->ev_sender = sock;
	isc_task_sendanddetach(&task, (isc_event_t **)&dev);
}

isc_result_t
isc_socket_getpeername(isc_socket_t *sock, isc_sockaddr_t *addressp) {
	isc_result_t ret;

	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(addressp != NULL);

	LOCK(&sock->lock);

	if (sock->connected) {
		*addressp = sock->address;
		ret = ISC_R_SUCCESS;
	} else {
		ret = ISC_R_NOTCONNECTED;
	}

	UNLOCK(&sock->lock);

	return (ret);
}

isc_result_t
isc_socket_getsockname(isc_socket_t *sock, isc_sockaddr_t *addressp) {
	ISC_SOCKADDR_LEN_T len;
	isc_result_t ret;

	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(addressp != NULL);

	LOCK(&sock->lock);

	if (!sock->bound) {
		ret = ISC_R_NOTBOUND;
		goto out;
	}

	ret = ISC_R_SUCCESS;

	len = sizeof addressp->type;
	if (getsockname(sock->fd, &addressp->type.sa, (void *)&len) < 0) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "getsockname: %s", strerror(errno));
		ret = ISC_R_UNEXPECTED;
		goto out;
	}
	addressp->length = (unsigned int)len;

 out:
	UNLOCK(&sock->lock);

	return (ret);
}

/*
 * Run through the list of events on this socket, and cancel the ones
 * queued for task "task" of type "how".  "how" is a bitmask.
 */
void
isc_socket_cancel(isc_socket_t *sock, isc_task_t *task, unsigned int how) {

	REQUIRE(VALID_SOCKET(sock));

	/*
	 * Quick exit if there is nothing to do.  Don't even bother locking
	 * in this case.
	 */
	if (how == 0)
		return;

	LOCK(&sock->lock);

	/*
	 * All of these do the same thing, more or less.
	 * Each will:
	 *	o If the internal event is marked as "posted" try to
	 *	  remove it from the task's queue.  If this fails, mark it
	 *	  as canceled instead, and let the task clean it up later.
	 *	o For each I/O request for that task of that type, post
	 *	  its done event with status of "ISC_R_CANCELED".
	 *	o Reset any state needed.
	 */
	if (((how & ISC_SOCKCANCEL_RECV) == ISC_SOCKCANCEL_RECV)
	    && !ISC_LIST_EMPTY(sock->recv_list)) {
		isc_socketevent_t      *dev;
		isc_socketevent_t      *next;
		isc_task_t	       *current_task;

		dev = ISC_LIST_HEAD(sock->recv_list);

		while (dev != NULL) {
			current_task = dev->ev_sender;
			next = ISC_LIST_NEXT(dev, ev_link);

			if ((task == NULL) || (task == current_task)) {
				dev->result = ISC_R_CANCELED;
				send_recvdone_event(sock, &dev);
			}
			dev = next;
		}
	}

	if (((how & ISC_SOCKCANCEL_SEND) == ISC_SOCKCANCEL_SEND)
	    && !ISC_LIST_EMPTY(sock->send_list)) {
		isc_socketevent_t      *dev;
		isc_socketevent_t      *next;
		isc_task_t	       *current_task;

		dev = ISC_LIST_HEAD(sock->send_list);

		while (dev != NULL) {
			current_task = dev->ev_sender;
			next = ISC_LIST_NEXT(dev, ev_link);

			if ((task == NULL) || (task == current_task)) {
				dev->result = ISC_R_CANCELED;
				send_senddone_event(sock, &dev);
			}
			dev = next;
		}
	}

	if (((how & ISC_SOCKCANCEL_ACCEPT) == ISC_SOCKCANCEL_ACCEPT)
	    && !ISC_LIST_EMPTY(sock->accept_list)) {
		isc_socket_newconnev_t *dev;
		isc_socket_newconnev_t *next;
		isc_task_t	       *current_task;

		dev = ISC_LIST_HEAD(sock->accept_list);
		while (dev != NULL) {
			current_task = dev->ev_sender;
			next = ISC_LIST_NEXT(dev, ev_link);

			if ((task == NULL) || (task == current_task)) {

				ISC_LIST_UNLINK(sock->accept_list, dev,
						ev_link);

				dev->newsocket->references--;
				free_socket(&dev->newsocket);

				dev->result = ISC_R_CANCELED;
				dev->ev_sender = sock;
				isc_task_sendanddetach(&current_task,
						       (isc_event_t **)&dev);
			}

			dev = next;
		}
	}

	/*
	 * Connecting is not a list.
	 */
	if (((how & ISC_SOCKCANCEL_CONNECT) == ISC_SOCKCANCEL_CONNECT)
	    && sock->connect_ev != NULL) {
		isc_socket_connev_t    *dev;
		isc_task_t	       *current_task;

		INSIST(sock->connecting);
		sock->connecting = 0;

		dev = sock->connect_ev;
		current_task = dev->ev_sender;

		if ((task == NULL) || (task == current_task)) {
			sock->connect_ev = NULL;

			dev->result = ISC_R_CANCELED;
			dev->ev_sender = sock;
			isc_task_sendanddetach(&current_task,
					       (isc_event_t **)&dev);
		}
	}

	UNLOCK(&sock->lock);
}

isc_sockettype_t
isc_socket_gettype(isc_socket_t *sock) {
	REQUIRE(VALID_SOCKET(sock));

	return (sock->type);
}

isc_boolean_t
isc_socket_isbound(isc_socket_t *sock) {
	isc_boolean_t val;

	LOCK(&sock->lock);
	val = ((sock->bound) ? ISC_TRUE : ISC_FALSE);
	UNLOCK(&sock->lock);

	return (val);
}