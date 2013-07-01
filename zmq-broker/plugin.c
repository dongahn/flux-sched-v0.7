/* plugin.c - broker plugin interface */

#define _GNU_SOURCE
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/param.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <json/json.h>
#include <czmq.h>

#include "log.h"
#include "zmq.h"
#include "route.h"
#include "cmbd.h"
#include "cmb.h"
#include "apisrv.h"
#include "barriersrv.h"
#include "syncsrv.h"
#include "kvssrv.h"
#include "livesrv.h"
#include "logsrv.h"
#include "util.h"
#include "plugin.h"

static plugin_t plugins[] = {
    &kvssrv,
    &syncsrv,
    &barriersrv,
    &apisrv,
    &livesrv,
    &logsrv,
};
const int plugins_len = sizeof (plugins)/sizeof (plugins[0]);


void plugin_send_request_raw (plugin_ctx_t *p, zmsg_t **zmsg)
{
    if (zmsg_send (zmsg, p->zs_upreq) < 0)
        err_exit ("%s: zmsg_send", __FUNCTION__);
    p->stats.upreq_send_count++;
}

void plugin_send_response_raw (plugin_ctx_t *p, zmsg_t **zmsg)
{
    if (zmsg_send (zmsg, p->zs_dnreq) < 0)
        err_exit ("%s: zmsg_send", __FUNCTION__);
    p->stats.dnreq_send_count++;
}

void plugin_send_event_raw (plugin_ctx_t *p, zmsg_t **zmsg)
{
    if (zmsg_send (zmsg, p->zs_evout) < 0)
        err_exit ("%s: zmsg_send", __FUNCTION__);
    p->stats.event_send_count++;
}

void plugin_send_event (plugin_ctx_t *p, const char *fmt, ...)
{
    va_list ap;
    zmsg_t *zmsg;
    char *tag;
    int n;

    va_start (ap, fmt);
    n = vasprintf (&tag, fmt, ap);
    va_end (ap);
    if (n < 0)
        err_exit ("%s: vasprintf", __FUNCTION__);
    zmsg = cmb_msg_encode (tag, NULL, NULL, 0);
    free (tag);
    plugin_send_event_raw (p, &zmsg);
}

void plugin_send_request (plugin_ctx_t *p, json_object *o, const char *fmt, ...)
{
    va_list ap;
    zmsg_t *zmsg;
    char *tag;
    int n;
    json_object *empty = NULL;

    va_start (ap, fmt);
    n = vasprintf (&tag, fmt, ap);
    va_end (ap);
    if (n < 0)
        err_exit ("vasprintf");

    if (!o) {
        if (!(empty = json_object_new_object ()))
            oom ();
        o = empty;
    }
    zmsg = cmb_msg_encode (tag, o, NULL, 0);
    if (zmsg_pushmem (zmsg, NULL, 0) < 0) /* delimiter frame */
        oom ();
    plugin_send_request_raw (p, &zmsg);

    if (empty)
        json_object_put (empty);
    free (tag);
}

void plugin_send_response (plugin_ctx_t *p, zmsg_t **req, json_object *o)
{
    if (cmb_msg_rep_json (*req, o) < 0)
        err_exit ("%s: cmb_msg_rep_json", __FUNCTION__);
    plugin_send_response_raw (p, req);
}

void plugin_send_response_errnum (plugin_ctx_t *p, zmsg_t **req, int errnum)
{
    if (cmb_msg_rep_errnum (*req, errnum) < 0)
        err_exit ("%s: cmb_msg_rep_errnum", __FUNCTION__);
    plugin_send_response_raw (p, req);
}

/* <name>.ping - respond to ping request for this plugin */
static void _plugin_ping (plugin_ctx_t *p, zmsg_t **zmsg)
{
    json_object *o, *no;
    char *s = NULL;

    if (cmb_msg_decode (*zmsg, NULL, &o, NULL, NULL) < 0) {
        err ("%s: protocol error", __FUNCTION__);
        goto done;
    }
    s = cmb_route_str (*zmsg, 2);
    if (!(no = json_object_new_string (s)))
        oom ();
    json_object_object_add (o, "route", no);
    cmb_msg_rep_json (*zmsg, o);
    plugin_send_response_raw (p, zmsg);
done:
    if (s)
        free (s);
    if (*zmsg)
        zmsg_destroy (zmsg);
}

/* <name>.stats - respond to stats request for this plugin */
static void _plugin_stats (plugin_ctx_t *p, zmsg_t **zmsg)
{
    json_object *no, *o = NULL;

    if (cmb_msg_decode (*zmsg, NULL, &o, NULL, NULL) < 0) {
        err ("%s: error decoding message", __FUNCTION__);
        goto done;
    }
    if (!(no = json_object_new_int (p->stats.upreq_send_count)))
        oom ();
    json_object_object_add (o, "upreq_send_count", no);

    if (!(no = json_object_new_int (p->stats.upreq_recv_count)))
        oom ();
    json_object_object_add (o, "upreq_recv_count", no);

    if (!(no = json_object_new_int (p->stats.dnreq_send_count)))
        oom ();
    json_object_object_add (o, "dnreq_send_count", no);

    if (!(no = json_object_new_int (p->stats.dnreq_recv_count)))
        oom ();
    json_object_object_add (o, "dnreq_recv_count", no);

    if (!(no = json_object_new_int (p->stats.event_send_count)))
        oom ();
    json_object_object_add (o, "event_send_count", no);

    if (!(no = json_object_new_int (p->stats.event_recv_count)))
        oom ();
    json_object_object_add (o, "event_recv_count", no);

    if (cmb_msg_rep_json (*zmsg, o) < 0) {
        err ("%s: cmb_msg_rep_json", __FUNCTION__);
        goto done;
    }
    plugin_send_response_raw (p, zmsg);
done:
    if (o)
        json_object_put (o);
    if (*zmsg)
        zmsg_destroy (zmsg);    
}


static void _plugin_poll (plugin_ctx_t *p)
{
    zmq_pollitem_t zpa[] = {
{ .socket = p->zs_upreq,     .events = ZMQ_POLLIN, .revents = 0, .fd = -1 },
{ .socket = p->zs_dnreq,     .events = ZMQ_POLLIN, .revents = 0, .fd = -1 },
{ .socket = p->zs_evin,      .events = ZMQ_POLLIN, .revents = 0, .fd = -1 },
{ .socket = p->zs_snoop,     .events = ZMQ_POLLIN, .revents = 0, .fd = -1 },
    };
    zmsg_t *zmsg;
    struct timeval t1, t2, t;
    long elapsed, msec = -1;
    char *pingtag, *statstag;
    zmsg_type_t type;

    if (asprintf (&pingtag, "%s.ping", p->plugin->name) < 0)
        err_exit ("asprintf");
    if (asprintf (&statstag, "%s.stats", p->plugin->name) < 0)
        err_exit ("asprintf");

    for (;;) {
        /* set up timeout */
        if (p->timeout > 0) {
            if (msec == -1) { 
                msec = p->timeout;
                xgettimeofday (&t1, NULL);
            }
        } else
            msec = -1;

        zpoll(zpa, sizeof (zpa) / sizeof (zpa[0]), msec);

        /* process timeout */
        if (p->timeout > 0) {
            xgettimeofday (&t2, NULL);
            timersub (&t2, &t1, &t);
            elapsed = (t.tv_sec * 1000 + t.tv_usec / 1000);
            if (elapsed < p->timeout) {
                msec -= elapsed;
            } else {
                if (p->plugin->timeoutFn)
                    p->plugin->timeoutFn (p);
                msec = -1;
            }
        }

        /* receive a message */
        if (zpa[0].revents & ZMQ_POLLIN) {   /* response on 'upreq' */
            zmsg = zmsg_recv (p->zs_upreq);
            type = ZMSG_RESPONSE;
            p->stats.upreq_recv_count++;
        } else if (zpa[1].revents & ZMQ_POLLIN) {   /* request on 'dnreq' */
            zmsg = zmsg_recv (p->zs_dnreq);
            if (!zmsg)
                err ("zmsg_recv");
            type = ZMSG_REQUEST;
            p->stats.dnreq_recv_count++;
        } else if (zpa[2].revents & ZMQ_POLLIN) {   /* event on 'in_event' */
            zmsg = zmsg_recv (p->zs_evin);
            if (!zmsg)
                err ("zmsg_recv");
            type = ZMSG_EVENT;
            p->stats.event_recv_count++;
        } else if (zpa[3].revents & ZMQ_POLLIN) {   /* debug on 'snoop' */
            zmsg = zmsg_recv (p->zs_snoop);
            type = ZMSG_SNOOP;
        } else
            zmsg = NULL;

        /* intercept and respond to ping requests for this plugin */
        if (zmsg && type == ZMSG_REQUEST && cmb_msg_match (zmsg, pingtag))
            _plugin_ping (p, &zmsg);

        /* intercept and respond to stats request for this plugin */
        if (zmsg && type == ZMSG_REQUEST && cmb_msg_match (zmsg, statstag))
            _plugin_stats (p, &zmsg);

        /* dispatch message to plugin's recvFn() */
        /*     recvFn () shouldn't free zmsg if it doesn't recognize the tag */
        if (zmsg && p->plugin->recvFn)
            p->plugin->recvFn (p, &zmsg, type);

        /* send ENOSYS response indicating plugin did not recognize tag */
        if (zmsg && type == ZMSG_REQUEST)
            plugin_send_response_errnum (p, &zmsg, ENOSYS);

        if (zmsg)
            zmsg_destroy (&zmsg);
    }

    free (pingtag);
    free (statstag);
}

static void *_plugin_thread (void *arg)
{
    plugin_ctx_t *p = arg;

    if (p->plugin->initFn)
        p->plugin->initFn (p);
    if (p->plugin->pollFn)
        p->plugin->pollFn (p);
    else
        _plugin_poll (p);
    if (p->plugin->finiFn)
        p->plugin->finiFn (p);

    return NULL;
}    

static void _plugin_destroy (void *arg)
{
    plugin_ctx_t *p = arg;
    int errnum;

    route_del (p->srv->rctx, p->plugin->name, p->plugin->name);

    /* FIXME: no mechanism to tell thread to exit yet */
    errnum = pthread_join (p->t, NULL);
    if (errnum)
        errn_exit (errnum, "pthread_join\n");

    zsocket_destroy (p->srv->zctx, p->zs_snoop);
    zsocket_destroy (p->srv->zctx, p->zs_evout);
    zsocket_destroy (p->srv->zctx, p->zs_evin);
    zsocket_destroy (p->srv->zctx, p->zs_dnreq);
    zsocket_destroy (p->srv->zctx, p->zs_upreq);

    free (p);
}

static plugin_t _lookup_plugin (char *name)
{
    int i;

    for (i = 0; i < plugins_len; i++)
        if (!strcmp (plugins[i]->name, name))
            return plugins[i];
    return NULL;
}

static int _plugin_create (char *name, server_t *srv, conf_t *conf)
{
    zctx_t *zctx = srv->zctx;
    plugin_ctx_t *p;
    int errnum;
    plugin_t plugin;

    if (!(plugin = _lookup_plugin (name))) {
        msg ("unknown plugin '%s'", name);
        return -1;
    }

    p = xzmalloc (sizeof (plugin_ctx_t));
    p->conf = conf;
    p->srv = srv;
    p->plugin = plugin;
    p->timeout = -1;

    /* connect sockets in the parent, then use them in the thread */
    zconnect (zctx, &p->zs_upreq, ZMQ_DEALER, UPREQ_URI, -1,
              (char *)plugin->name);
    zconnect (zctx, &p->zs_dnreq, ZMQ_DEALER, DNREQ_URI, -1,
              (char *)plugin->name);
    zconnect (zctx, &p->zs_evin,  ZMQ_SUB, DNEV_OUT_URI, 0, NULL);
    zconnect (zctx, &p->zs_evout, ZMQ_PUB, DNEV_IN_URI, -1, NULL);
    zconnect (zctx, &p->zs_snoop, ZMQ_SUB, SNOOP_URI, -1, NULL);

    route_add (p->srv->rctx, name, name, NULL, ROUTE_FLAGS_PRIVATE);

    errnum = pthread_create (&p->t, NULL, _plugin_thread, p);
    if (errnum)
        errn_exit (errnum, "pthread_create\n");

    zhash_insert (srv->plugins, plugin->name, p);
    zhash_freefn (srv->plugins, plugin->name, _plugin_destroy);

    return 0;
}

void plugin_init (conf_t *conf, server_t *srv)
{
    srv->plugins = zhash_new ();

    if (mapstr (conf->plugins, (mapstrfun_t)_plugin_create, srv, conf) < 0)
        exit (1);
}

void plugin_fini (conf_t *conf, server_t *srv)
{
    zhash_destroy (&srv->plugins);
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
