/*
 * Unix Domain Sockets Implementation (PF_UNIX, PF_LOCAL)
 * This code handles requests generated by operations on /dev/uds
 *
 * The interface to UNIX domain sockets is similar to the interface to network
 * sockets. There is a character device (/dev/uds) and this server is a
 * 'driver' for that device.
 */

#include "uds.h"

static ssize_t uds_perform_write(devminor_t, endpoint_t, cp_grant_id_t, size_t,
	int);

static int uds_open(devminor_t, int, endpoint_t);
static int uds_close(devminor_t);
static ssize_t uds_read(devminor_t, u64_t, endpoint_t, cp_grant_id_t, size_t,
	int, cdev_id_t);
static ssize_t uds_write(devminor_t, u64_t, endpoint_t, cp_grant_id_t, size_t,
	int, cdev_id_t);
static int uds_ioctl(devminor_t, unsigned long, endpoint_t, cp_grant_id_t, int,
	endpoint_t, cdev_id_t);
static int uds_cancel(devminor_t, endpoint_t, cdev_id_t);
static int uds_select(devminor_t, unsigned int, endpoint_t);

static struct chardriver uds_tab = {
	.cdr_open	= uds_open,
	.cdr_close	= uds_close,
	.cdr_read	= uds_read,
	.cdr_write	= uds_write,
	.cdr_ioctl	= uds_ioctl,
	.cdr_cancel	= uds_cancel,
	.cdr_select	= uds_select
};

/* File Descriptor Table */
uds_fd_t uds_fd_table[NR_FDS];

static unsigned int uds_exit_left;

static int
uds_open(devminor_t UNUSED(orig_minor), int access,
	endpoint_t user_endpt)
{
	devminor_t minor;
	char *buf;
	int i;

	dprintf(("UDS: uds_open() from %d\n", user_endpt));

	/*
	 * Find a slot in the descriptor table for the new descriptor.
	 * The index of the descriptor in the table will be returned.
	 * Subsequent calls to read/write/close/ioctl/etc will use this
	 * minor number.  The minor number must be different from the
	 * the /dev/uds device's minor number (0).
	 */
	for (minor = 1; minor < NR_FDS; minor++)
		if (uds_fd_table[minor].state == UDS_FREE)
			break;

	if (minor == NR_FDS)
		return ENFILE;

	/*
	 * Allocate memory for the ringer buffer.  In order to save on memory
	 * in the common case, the buffer is allocated only when the socket is
	 * in use.  We use mmap instead of malloc to allow the memory to be
	 * actually freed later.
	 */
	if ((buf = mmap(NULL, UDS_BUF, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0)) == MAP_FAILED)
		return ENOMEM;

	/*
	 * Allocate the socket, and set its initial parameters.
	 */
	uds_fd_table[minor].state = UDS_INUSE;
	uds_fd_table[minor].owner = user_endpt;
	uds_fd_table[minor].sel_endpt = NONE;
	uds_fd_table[minor].sel_ops = 0;
	uds_fd_table[minor].buf = buf;
	uds_fd_table[minor].pos = 0;
	uds_fd_table[minor].size = 0;
	uds_fd_table[minor].mode = UDS_R | UDS_W;
	uds_fd_table[minor].type = -1;

	for (i = 0; i < UDS_SOMAXCONN; i++)
		uds_fd_table[minor].backlog[i] = -1;
	uds_fd_table[minor].backlog_size = UDS_SOMAXCONN;

	memset(&uds_fd_table[minor].ancillary_data, '\0',
	    sizeof(struct ancillary));
	for (i = 0; i < OPEN_MAX; i++)
		uds_fd_table[minor].ancillary_data.fds[i] = -1;

	uds_fd_table[minor].listening = 0;
	uds_fd_table[minor].peer = -1;
	uds_fd_table[minor].child = -1;

	memset(&uds_fd_table[minor].addr, '\0', sizeof(struct sockaddr_un));
	memset(&uds_fd_table[minor].source, '\0', sizeof(struct sockaddr_un));
	memset(&uds_fd_table[minor].target, '\0', sizeof(struct sockaddr_un));

	uds_fd_table[minor].suspended = UDS_NOT_SUSPENDED;

	return CDEV_CLONED | minor;
}

static void
uds_reset(devminor_t minor)
{
	/* Disconnect the socket from its peer. */
	uds_fd_table[minor].peer = -1;

	/* Set an error to pass to the caller. */
	uds_fd_table[minor].err = ECONNRESET;

	/* If a process was blocked on I/O, revive it. */
	if (uds_fd_table[minor].suspended != UDS_NOT_SUSPENDED)
		uds_unsuspend(minor);

	/* All of the peer's calls will fail immediately now. */
	if (uds_fd_table[minor].sel_ops != 0) {
		chardriver_reply_select(uds_fd_table[minor].sel_endpt, minor,
		    uds_fd_table[minor].sel_ops);
		uds_fd_table[minor].sel_ops = 0;
	}
}

static int
uds_close(devminor_t minor)
{
	int i, peer;

	dprintf(("UDS: uds_close(%d)\n", minor));

	if (minor < 0 || minor >= NR_FDS) return ENXIO;

	if (uds_fd_table[minor].state != UDS_INUSE)
		return EINVAL;

	peer = uds_fd_table[minor].peer;
	if (peer != -1 && uds_fd_table[peer].peer == -1) {
		/* Connecting socket: clear from server's backlog. */
		if (!uds_fd_table[peer].listening)
			panic("connecting socket attached to non-server");

		for (i = 0; i < uds_fd_table[peer].backlog_size; i++) {
			if (uds_fd_table[peer].backlog[i] == minor) {
				uds_fd_table[peer].backlog[i] = -1;
				break;
			}
		}
	} else if (peer != -1) {
		/* Connected socket: disconnect it. */
		uds_reset(peer);
	} else if (uds_fd_table[minor].listening) {
		/* Listening socket: disconnect all sockets in the backlog. */
		for (i = 0; i < uds_fd_table[minor].backlog_size; i++)
			if (uds_fd_table[minor].backlog[i] != -1)
				uds_reset(uds_fd_table[minor].backlog[i]);
	}

	if (uds_fd_table[minor].ancillary_data.nfiledes > 0)
		uds_clear_fds(minor, &uds_fd_table[minor].ancillary_data);

	/* Release the memory for the ring buffer. */
	munmap(uds_fd_table[minor].buf, UDS_BUF);

	/* Set the socket back to its original UDS_FREE state. */
	memset(&uds_fd_table[minor], '\0', sizeof(uds_fd_t));

	/* If terminating, and this was the last open socket, exit now. */
	if (uds_exit_left > 0) {
		if (--uds_exit_left == 0)
			chardriver_terminate();
	}

	return OK;
}

static int
uds_select(devminor_t minor, unsigned int ops, endpoint_t endpt)
{
	unsigned int ready_ops;
	int i, bytes, watch;

	dprintf(("UDS: uds_select(%d)\n", minor));

	if (minor < 0 || minor >= NR_FDS) return ENXIO;

	if (uds_fd_table[minor].state != UDS_INUSE)
		return EINVAL;

	watch = (ops & CDEV_NOTIFY);
	ops &= (CDEV_OP_RD | CDEV_OP_WR | CDEV_OP_ERR);

	ready_ops = 0;

	/* Check if there is data available to read. */
	if (ops & CDEV_OP_RD) {
		bytes = uds_perform_read(minor, NONE, GRANT_INVALID, 1, 1);
		if (bytes > 0) {
			ready_ops |= CDEV_OP_RD;	/* data available */
		} else if (uds_fd_table[minor].listening == 1) {
			/* Check for pending connections. */
			for (i = 0; i < uds_fd_table[minor].backlog_size; i++)
			{
				if (uds_fd_table[minor].backlog[i] != -1) {
					ready_ops |= CDEV_OP_RD;
					break;
				}
			}
		} else if (bytes != EDONTREPLY) {
			ready_ops |= CDEV_OP_RD;	/* error */
		}
	}

	/* Check if we can write without blocking. */
	if (ops & CDEV_OP_WR) {
		bytes = uds_perform_write(minor, NONE, GRANT_INVALID, 1, 1);
		if (bytes != 0 && bytes != EDONTREPLY)
			ready_ops |= CDEV_OP_WR;
	}

	/*
	 * If not all requested ops were ready, and the caller requests to be
	 * notified about changes, we add the remaining ops to the saved set.
	 */
	ops &= ~ready_ops;
	if (ops && watch) {
		uds_fd_table[minor].sel_endpt = endpt;
		uds_fd_table[minor].sel_ops |= ops;
	}

	return ready_ops;
}

ssize_t
uds_perform_read(devminor_t minor, endpoint_t endpt, cp_grant_id_t grant,
	size_t size, int pretend)
{
	size_t pos, subsize;
	int r, peer;

	dprintf(("UDS: uds_perform_read(%d)\n", minor));

	peer = uds_fd_table[minor].peer;

	/* Skip reads of zero bytes. */
	if (size == 0)
		return 0;

	/* Check if the socket isn't shut down for reads. */
	if (!(uds_fd_table[minor].mode & UDS_R))
		return EPIPE;

	if (uds_fd_table[minor].size == 0) {
		if (peer == -1) {
			/*
			 * We're not connected. That's only a problem when this
			 * socket is connection oriented.
			 */
			if (uds_fd_table[minor].type == SOCK_STREAM ||
			    uds_fd_table[minor].type == SOCK_SEQPACKET) {
				if (uds_fd_table[minor].err == ECONNRESET) {
					if (!pretend)
						uds_fd_table[minor].err = 0;
					return ECONNRESET;
				} else
					return ENOTCONN;
			}
		}

		/* Check if process is reading from a closed pipe. */
		if (peer != -1 && !(uds_fd_table[peer].mode & UDS_W) &&
		    uds_fd_table[minor].size == 0)
			return 0;

		if (pretend)
			return EDONTREPLY;

		if (peer != -1 &&
			uds_fd_table[peer].suspended == UDS_SUSPENDED_WRITE)
			panic("writer blocked on empty socket");

		dprintf(("UDS: suspending read request on %d\n", minor));

		/* Process is reading from an empty pipe.  Suspend it. */
		return EDONTREPLY;
	}

	/* How much can we get from the ring buffer? */
	if (size > uds_fd_table[minor].size)
		size = uds_fd_table[minor].size;

	if (pretend)
		return size;

	/* Get the data from the tail of the ring buffer. */
	pos = uds_fd_table[minor].pos;

	subsize = UDS_BUF - pos;
	if (subsize > size)
		subsize = size;

	if ((r = sys_safecopyto(endpt, grant, 0,
	    (vir_bytes) &uds_fd_table[minor].buf[pos], subsize)) != OK)
		return r;

	if (subsize < size) {
		if ((r = sys_safecopyto(endpt, grant, subsize,
		    (vir_bytes) uds_fd_table[minor].buf,
		    size - subsize)) != OK)
			return r;
	}

	/* Advance the buffer tail. */
	uds_fd_table[minor].pos = (pos + size) % UDS_BUF;
	uds_fd_table[minor].size -= size;

	/* Reset position if the buffer is empty (it may save a copy call). */
	if (uds_fd_table[minor].size == 0)
		uds_fd_table[minor].pos = 0;

	/* See if we can wake up a blocked writer. */
	if (peer != -1 && uds_fd_table[peer].suspended == UDS_SUSPENDED_WRITE)
		uds_unsuspend(peer);

	/* See if we can satisfy an ongoing select. */
	if (peer != -1 && (uds_fd_table[peer].sel_ops & CDEV_OP_WR) &&
	    uds_fd_table[minor].size < UDS_BUF) {
		/* A write on the peer is possible now. */
		chardriver_reply_select(uds_fd_table[peer].sel_endpt, peer,
		    CDEV_OP_WR);
		uds_fd_table[peer].sel_ops &= ~CDEV_OP_WR;
	}

	return size; /* number of bytes read */
}

static ssize_t
uds_perform_write(devminor_t minor, endpoint_t endpt, cp_grant_id_t grant,
	size_t size, int pretend)
{
	size_t subsize, pos;
	int i, r, peer;

	dprintf(("UDS: uds_perform_write(%d)\n", minor));

	/* Skip writes of zero bytes. */
	if (size == 0)
		return 0;

	/* Check if the socket isn't shut down for writes. */
	if (!(uds_fd_table[minor].mode & UDS_W))
		return EPIPE;

	/* Datagram messages must fit in the buffer entirely. */
	if (size > UDS_BUF && uds_fd_table[minor].type != SOCK_STREAM)
		return EMSGSIZE;

	if (uds_fd_table[minor].type == SOCK_STREAM ||
	    uds_fd_table[minor].type == SOCK_SEQPACKET) {
		/*
		 * If we're writing to a connection-oriented socket, then it
		 * needs a peer to write to.  For disconnected sockets, writing
		 * is an error; for connecting sockets, writes should suspend.
		 */
		peer = uds_fd_table[minor].peer;

		if (peer == -1) {
			if (uds_fd_table[minor].err == ECONNRESET) {
				if (!pretend)
					uds_fd_table[minor].err = 0;
				return ECONNRESET;
			} else
				return ENOTCONN;
		} else if (uds_fd_table[peer].peer == -1) /* connecting */
			return EDONTREPLY;
	} else /* uds_fd_table[minor].type == SOCK_DGRAM */ {
		peer = -1;

		/* Locate the "peer" we want to write to. */
		for (i = 0; i < NR_FDS; i++) {
			/*
			 * Look for a SOCK_DGRAM socket that is bound on
			 * the target address.
			 */
			if (uds_fd_table[i].type == SOCK_DGRAM &&
			    uds_fd_table[i].addr.sun_family == AF_UNIX &&
			    !strncmp(uds_fd_table[minor].target.sun_path,
			    uds_fd_table[i].addr.sun_path, UNIX_PATH_MAX)) {
				peer = i;
				break;
			}
		}

		if (peer == -1)
			return ENOENT;
	}

	/* Check if we write to a closed pipe. */
	if (!(uds_fd_table[peer].mode & UDS_R))
		return EPIPE;

	/*
	 * We have to preserve the boundary for DGRAM.  If there's already a
	 * packet waiting, discard it silently and pretend it was written.
	 */
	if (uds_fd_table[minor].type == SOCK_DGRAM &&
	    uds_fd_table[peer].size > 0)
		return size;

	/*
	 * Check if the ring buffer is already full, and if the SEQPACKET
	 * message wouldn't write to an empty buffer.
	 */
	if (uds_fd_table[peer].size == UDS_BUF ||
	    (uds_fd_table[minor].type == SOCK_SEQPACKET &&
	    uds_fd_table[peer].size > 0)) {
		if (pretend)
			return EDONTREPLY;

		if (uds_fd_table[peer].suspended == UDS_SUSPENDED_READ)
			panic("reader blocked on full socket");

		dprintf(("UDS: suspending write request on %d\n", minor));

		/* Process is reading from an empty pipe.  Suspend it. */
		return EDONTREPLY;
	}

	/* How much can we add to the ring buffer? */
	if (size > UDS_BUF - uds_fd_table[peer].size)
		size = UDS_BUF - uds_fd_table[peer].size;

	if (pretend)
		return size;

	/* Put the data at the head of the ring buffer. */
	pos = (uds_fd_table[peer].pos + uds_fd_table[peer].size) % UDS_BUF;

	subsize = UDS_BUF - pos;
	if (subsize > size)
		subsize = size;

	if ((r = sys_safecopyfrom(endpt, grant, 0,
	    (vir_bytes) &uds_fd_table[peer].buf[pos], subsize)) != OK)
		return r;

	if (subsize < size) {
		if ((r = sys_safecopyfrom(endpt, grant, subsize,
		    (vir_bytes) uds_fd_table[peer].buf, size - subsize)) != OK)
			return r;
	}

	/* Advance the buffer head. */
	uds_fd_table[peer].size += size;

	/* Fill in the source address to be returned by recvfrom, recvmsg. */
	if (uds_fd_table[minor].type == SOCK_DGRAM)
		memcpy(&uds_fd_table[peer].source, &uds_fd_table[minor].addr,
		    sizeof(struct sockaddr_un));

	/* See if we can wake up a blocked reader. */
	if (uds_fd_table[peer].suspended == UDS_SUSPENDED_READ)
		uds_unsuspend(peer);

	/* See if we can satisfy an ongoing select. */
	if ((uds_fd_table[peer].sel_ops & CDEV_OP_RD) &&
	    uds_fd_table[peer].size > 0) {
		/* A read on the peer is possible now. */
		chardriver_reply_select(uds_fd_table[peer].sel_endpt, peer,
		    CDEV_OP_RD);
		uds_fd_table[peer].sel_ops &= ~CDEV_OP_RD;
	}

	return size; /* number of bytes written */
}

static ssize_t
uds_read(devminor_t minor, u64_t position, endpoint_t endpt,
	cp_grant_id_t grant, size_t size, int flags, cdev_id_t id)
{
	ssize_t rc;

	dprintf(("UDS: uds_read(%d)\n", minor));

	if (minor < 0 || minor >= NR_FDS) return ENXIO;

	if (uds_fd_table[minor].state != UDS_INUSE)
		return EINVAL;

	rc = uds_perform_read(minor, endpt, grant, size, 0);

	/* If the call couldn't complete, suspend the caller. */
	if (rc == EDONTREPLY) {
		uds_fd_table[minor].suspended = UDS_SUSPENDED_READ;
		uds_fd_table[minor].susp_endpt = endpt;
		uds_fd_table[minor].susp_grant = grant;
		uds_fd_table[minor].susp_size = size;
		uds_fd_table[minor].susp_id = id;

		/* If the call wasn't supposed to block, cancel immediately. */
		if (flags & CDEV_NONBLOCK) {
			uds_cancel(minor, endpt, id);

			rc = EAGAIN;
		}
	}

	return rc;
}

static ssize_t
uds_write(devminor_t minor, u64_t position, endpoint_t endpt,
	cp_grant_id_t grant, size_t size, int flags, cdev_id_t id)
{
	ssize_t rc;

	dprintf(("UDS: uds_write(%d)\n", minor));

	if (minor < 0 || minor >= NR_FDS) return ENXIO;

	if (uds_fd_table[minor].state != UDS_INUSE)
		return EINVAL;

	rc = uds_perform_write(minor, endpt, grant, size, 0);

	/* If the call couldn't complete, suspend the caller. */
	if (rc == EDONTREPLY) {
		uds_fd_table[minor].suspended = UDS_SUSPENDED_WRITE;
		uds_fd_table[minor].susp_endpt = endpt;
		uds_fd_table[minor].susp_grant = grant;
		uds_fd_table[minor].susp_size = size;
		uds_fd_table[minor].susp_id = id;

		/* If the call wasn't supposed to block, cancel immediately. */
		if (flags & CDEV_NONBLOCK) {
			uds_cancel(minor, endpt, id);

			rc = EAGAIN;
		}
	}

	return rc;
}

static int
uds_ioctl(devminor_t minor, unsigned long request, endpoint_t endpt,
	cp_grant_id_t grant, int flags, endpoint_t user_endpt, cdev_id_t id)
{
	int rc, s;

	dprintf(("UDS: uds_ioctl(%d, %lu)\n", minor, request));

	if (minor < 0 || minor >= NR_FDS) return ENXIO;

	if (uds_fd_table[minor].state != UDS_INUSE)
		return EINVAL;

	/* Update the owner endpoint. */
	uds_fd_table[minor].owner = user_endpt;

	/* Let the UDS ioctl subsystem handle the actual request. */
	rc = uds_do_ioctl(minor, request, endpt, grant);

	/* If the call couldn't complete, suspend the caller. */
	if (rc == EDONTREPLY) {
		/* The suspension type is already set by the IOCTL handler. */
		if ((s = uds_fd_table[minor].suspended) == UDS_NOT_SUSPENDED)
			panic("IOCTL did not actually suspend?");
		uds_fd_table[minor].susp_endpt = endpt;
		uds_fd_table[minor].susp_grant = grant;
		uds_fd_table[minor].susp_size = 0; /* irrelevant */
		uds_fd_table[minor].susp_id = id;

		/* If the call wasn't supposed to block, cancel immediately. */
		if (flags & CDEV_NONBLOCK) {
			uds_cancel(minor, endpt, id);
			if (s == UDS_SUSPENDED_CONNECT)
				rc = EINPROGRESS;
			else
				rc = EAGAIN;
		}
	}

	return rc;
}

void
uds_unsuspend(devminor_t minor)
{
	int r;
	uds_fd_t *fdp;

	fdp = &uds_fd_table[minor];

	switch (fdp->suspended) {
	case UDS_SUSPENDED_READ:
		r = uds_perform_read(minor, fdp->susp_endpt, fdp->susp_grant,
		    fdp->susp_size, 0);

		if (r == EDONTREPLY)
			return;

		break;

	case UDS_SUSPENDED_WRITE:
		r = uds_perform_write(minor, fdp->susp_endpt, fdp->susp_grant,
		    fdp->susp_size, 0);

		if (r == EDONTREPLY)
			return;

		break;

	case UDS_SUSPENDED_CONNECT:
	case UDS_SUSPENDED_ACCEPT:
		/*
		 * In both cases, the caller already set up the connection.
		 * The only thing to do here is unblock.
		 */
		r = fdp->err;
		fdp->err = 0;

		break;

	default:
		panic("unknown suspension type %d", fdp->suspended);
	}

	chardriver_reply_task(fdp->susp_endpt, fdp->susp_id, r);

	fdp->suspended = UDS_NOT_SUSPENDED;
}

static int
uds_cancel(devminor_t minor, endpoint_t endpt, cdev_id_t id)
{
	uds_fd_t *fdp;
	int i;

	dprintf(("UDS: uds_cancel(%d)\n", minor));

	if (minor < 0 || minor >= NR_FDS) return EDONTREPLY;

	fdp = &uds_fd_table[minor];

	if (fdp->state != UDS_INUSE) {
		printf("UDS: cancel request for closed minor %d\n", minor);
		return EDONTREPLY;
	}

	/* Make sure the cancel request is for a request we're hanging on. */
	if (fdp->suspended == UDS_NOT_SUSPENDED || fdp->susp_endpt != endpt ||
	    fdp->susp_id != id)
		return EDONTREPLY;	/* this happens. */

	/*
	 * The system call was cancelled, so the socket is not suspended
	 * anymore.
	 */
	switch (fdp->suspended) {
	case UDS_SUSPENDED_ACCEPT:
		/* A partial accept() only sets the server's child. */
		for (i = 0; i < NR_FDS; i++)
			if (uds_fd_table[i].child == minor)
				uds_fd_table[i].child = -1;

		break;

	case UDS_SUSPENDED_CONNECT:
		/* Connect requests should continue asynchronously. */
		break;

	case UDS_SUSPENDED_READ:
	case UDS_SUSPENDED_WRITE:
		/* Nothing more to do. */
		break;

	default:
		panic("unknown suspension type %d", fdp->suspended);
	}

	fdp->suspended = UDS_NOT_SUSPENDED;

	return EINTR;	/* reply to the original request */
}

/*
 * Initialize the server.
 */
static int
uds_init(int UNUSED(type), sef_init_info_t *UNUSED(info))
{
	/* Setting everything to NULL implicitly sets the state to UDS_FREE. */
	memset(uds_fd_table, '\0', sizeof(uds_fd_t) * NR_FDS);

	uds_exit_left = 0;

	return(OK);
}

static void
uds_signal(int signo)
{
	int i;

	/* Only check for termination signal, ignore anything else. */
	if (signo != SIGTERM) return;

	/* Only exit once all sockets have been closed. */
	uds_exit_left = 0;
	for (i = 0; i < NR_FDS; i++)
		if (uds_fd_table[i].state == UDS_INUSE)
			uds_exit_left++;

	if (uds_exit_left == 0)
		chardriver_terminate();
}

static void
uds_startup(void)
{
	/* Register init callbacks. */
	sef_setcb_init_fresh(uds_init);

	/* No live update support for now. */

	/* Register signal callbacks. */
	sef_setcb_signal_handler(uds_signal);

	/* Let SEF perform startup. */
	sef_startup();
}

/*
 * The UNIX domain sockets driver.
 */
int
main(void)
{
	uds_startup();

	chardriver_task(&uds_tab);

	return(OK);
}
