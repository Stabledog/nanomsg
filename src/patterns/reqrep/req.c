/*
    Copyright (c) 2012 250bpm s.r.o.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "req.h"
#include "xreq.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/random.h"
#include "../../utils/wire.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define SP_REQ_DEFAULT_RESEND_IVL 60000

#define SP_REQ_INPROGRESS 1

struct sp_req {
    struct sp_xreq xreq;
    uint32_t reqid;
    uint32_t flags;
    size_t requestlen;
    void *request;
    int resend_ivl;
    struct sp_sockbase_timer resend_timer;
};

/*  Implementation of sp_sockbase's virtual functions. */
static void sp_req_term (struct sp_sockbase *self);
static int sp_req_send (struct sp_sockbase *self, const void *buf, size_t len);
static int sp_req_recv (struct sp_sockbase *self, void *buf, size_t *len);
static int sp_req_setopt (struct sp_sockbase *self, int option,
        const void *optval, size_t optvallen);
static int sp_req_getopt (struct sp_sockbase *self, int option,
        void *optval, size_t *optvallen);

/*  Private functions. */
void sp_req_resend_routine (struct sp_sockbase_timer *self);

static const struct sp_sockbase_vfptr sp_req_sockbase_vfptr = {
    sp_req_term,
    sp_xreq_add,
    sp_xreq_rm,
    sp_xreq_in,
    sp_xreq_out,
    sp_req_send,
    sp_req_recv,
    sp_req_setopt,
    sp_req_getopt,
};

void sp_req_init (struct sp_req *self, const struct sp_sockbase_vfptr *vfptr,
    int fd)
{
    sp_xreq_init (&self->xreq, vfptr, fd);

    /*  Start assigning request IDs beginning with a random number. This way
        there should be no key clashes even if the executable is re-started.
        Keys are 31 bit unsigned integers. */
    sp_random_generate (&self->reqid, sizeof (self->reqid));
    self->reqid &= 0x7fffffff;

    self->flags = 0;
    self->requestlen = 0;
    self->request = NULL;
    self->resend_ivl = SP_REQ_DEFAULT_RESEND_IVL;
}

static void sp_req_term (struct sp_sockbase *self)
{
    struct sp_req *req;

    req = sp_cont (self, struct sp_req, xreq.sockbase);

    if (req->flags & SP_REQ_INPROGRESS) {
        sp_sockbase_timer_cancel (self, &req->resend_timer);
        sp_free (req->request);
    }

    sp_xreq_term (&req->xreq.sockbase);
}

static int sp_req_send (struct sp_sockbase *self, const void *buf, size_t len)
{
    int rc;
    struct sp_req *req;

    req = sp_cont (self, struct sp_req, xreq.sockbase);

    /*  If there's a request in progress, cancel it. */
    if (sp_slow (req->flags & SP_REQ_INPROGRESS)) {
        sp_free (req->request);
        req->requestlen = 0;
        req->request = NULL;
        req->flags &= ~SP_REQ_INPROGRESS;
        sp_sockbase_timer_cancel (self, &req->resend_timer);
    }

    /*  Generate new request ID for the new request. */
    ++req->reqid;
    req->reqid &= 0x7fffffff;

    /*  Store the message so that it can be re-send if there's no reply.
        Tag it with the request ID. */
    req->requestlen = sizeof (uint32_t) + len;
    req->request = sp_alloc (req->requestlen);
    alloc_assert (req->request);
    sp_putl (req->request, req->reqid | 0x80000000);
    memcpy (((uint32_t*) (req->request)) + 1, buf, len);

    /*  Send the message. If it cannot be sent because of the pushback we'll
        pretend it was sent anyway. Re-send mechanism will take care of the
        rest. */
    rc = sp_xreq_send (&req->xreq.sockbase, req->request, req->requestlen);
    errnum_assert (rc == 0 || rc == -EAGAIN, -rc);

    /*  Remember that we are processing a request and waiting for the reply
        at the moment. */
    req->flags |= SP_REQ_INPROGRESS;

    /*  Set up the re-send timer. */
    sp_sockbase_timer_start (self, &req->resend_timer, req->resend_ivl,
        sp_req_resend_routine);

    return 0;
}

static int sp_req_recv (struct sp_sockbase *self, void *buf, size_t *len)
{
    int rc;
    struct sp_req *req;
    size_t replylen;
    void *reply;
    uint32_t reqid;

    req = sp_cont (self, struct sp_req, xreq.sockbase);

    /*  TODO: In case of invalid replies we should try to recv again here
        instead of returning -EAGAIN. */

    /*  No request was sent. Waiting for a reply doesn't make sense. */
    if (sp_slow (!(req->flags & SP_REQ_INPROGRESS)))
        return -EFSM;

    /*  TODO: Do this using iovecs. */
    replylen = sizeof (uint32_t) + *len;
    reply = sp_alloc (replylen);
    alloc_assert (reply);
    rc = sp_xreq_recv (&req->xreq.sockbase, reply, &replylen);
    if (sp_slow (rc == -EAGAIN)) {
        sp_free (reply);
        return -EAGAIN;
    }
    errnum_assert (rc == 0, -rc);

    /*  Ignore malformed replies. */
    if (sp_slow (replylen < sizeof (uint32_t))) {
        sp_free (reply);
        return -EAGAIN;
    }

    /*  Ignore replies with incorrect request IDs. */
    reqid = sp_getl (reply);
    if (sp_slow (!(reqid & 0x80000000))) {
        sp_free (reply);
        return -EAGAIN;
    }
    reqid &= 0x7fffffff;
    if (sp_slow (reqid != req->reqid)) {
        sp_free (reply);
        return -EAGAIN;
    }

    /*  Correct reply received. Pass it to the caller. */
    memcpy (buf, ((uint32_t*) reply) + 1, *len < replylen - sizeof (uint32_t) ?
        *len : replylen - sizeof (uint32_t));
    *len = replylen - sizeof (uint32_t);

    /*  Clean-up. */
    sp_sockbase_timer_cancel (self, &req->resend_timer);
    sp_free (reply);
    sp_free (req->request);
    req->requestlen = 0;
    req->request = NULL;
    req->flags &= ~SP_REQ_INPROGRESS;

    return 0;
}

void sp_req_resend_routine (struct sp_sockbase_timer *self)
{
    int rc;
    struct sp_req *req;

    req = sp_cont (self, struct sp_req, resend_timer);
    sp_assert (req->flags & SP_REQ_INPROGRESS);

    /*  Re-send the request. */
    rc = sp_xreq_send (&req->xreq.sockbase, req->request, req->requestlen);
    errnum_assert (rc == 0 || rc == -EAGAIN, -rc);

    /*  Set up the next re-send timer. */
    sp_sockbase_timer_start (&req->xreq.sockbase, &req->resend_timer,
        req->resend_ivl, sp_req_resend_routine);
}

static int sp_req_setopt (struct sp_sockbase *self, int option,
        const void *optval, size_t optvallen)
{
    struct sp_req *req;

    req = sp_cont (self, struct sp_req, xreq.sockbase);

    if (option == SP_RESEND_IVL) {
        if (sp_slow (optvallen != sizeof (int)))
            return -EINVAL;
        req->resend_ivl = *(int*) optval;
        return 0;
    }

    return -ENOPROTOOPT;
}

static int sp_req_getopt (struct sp_sockbase *self, int option,
        void *optval, size_t *optvallen)
{
    struct sp_req *req;

    req = sp_cont (self, struct sp_req, xreq.sockbase);

    if (option == SP_RESEND_IVL) {
        if (sp_slow (*optvallen < sizeof (int)))
            return -EINVAL;
        *(int*) optval = req->resend_ivl;
        *optvallen = sizeof (int);
        return 0;
    }

    return -ENOPROTOOPT;
}

static struct sp_sockbase *sp_req_create (int fd)
{
    struct sp_req *self;

    self = sp_alloc (sizeof (struct sp_req));
    alloc_assert (self);
    sp_req_init (self, &sp_req_sockbase_vfptr, fd);
    return &self->xreq.sockbase;
}

static struct sp_socktype sp_req_socktype_struct = {
    AF_SP,
    SP_REQ,
    sp_req_create
};

struct sp_socktype *sp_req_socktype = &sp_req_socktype_struct;
