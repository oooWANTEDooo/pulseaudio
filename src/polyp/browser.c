/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <assert.h>
#include <howl.h>

#include <polypcore/xmalloc.h>
#include <polypcore/log.h>
#include <polypcore/util.h>

#include "browser.h"

#define SERVICE_NAME_SINK "_polypaudio-sink._tcp."
#define SERVICE_NAME_SOURCE "_polypaudio-source._tcp."
#define SERVICE_NAME_SERVER "_polypaudio-server._tcp."

struct pa_browser {
    int ref;
    pa_mainloop_api *mainloop;

    pa_browse_cb_t callback;
    void *userdata;
    
    sw_discovery discovery;
    pa_io_event *io_event;
};

static void io_callback(pa_mainloop_api*a, pa_io_event*e, int fd, pa_io_event_flags_t events, void *userdata) {
    pa_browser *b = userdata;
    assert(a && b && b->mainloop == a);

    if (events != PA_IO_EVENT_INPUT || sw_discovery_read_socket(b->discovery) != SW_OKAY) {
        pa_log(__FILE__": connection to HOWL daemon failed.");
        b->mainloop->io_free(b->io_event);
        b->io_event = NULL;
        return;
    }
}

static int type_equal(const char *a, const char *b) {
    size_t la, lb;
    
    if (strcasecmp(a, b) == 0)
        return 1;

    la = strlen(a);
    lb = strlen(b);

    if (la > 0 && a[la-1] == '.' && la == lb+1 && strncasecmp(a, b, la-1) == 0)
        return 1;
                                            
    if (lb > 0 && b[lb-1] == '.' && lb == la+1 && strncasecmp(a, b, lb-1) == 0)
        return 1;

    return 0;
}

static int map_to_opcode(const char *type, int new) {
    if (type_equal(type, SERVICE_NAME_SINK))
        return new ? PA_BROWSE_NEW_SINK : PA_BROWSE_REMOVE_SINK;
    else if (type_equal(type, SERVICE_NAME_SOURCE))
        return new ? PA_BROWSE_NEW_SOURCE : PA_BROWSE_REMOVE_SOURCE;
    else if (type_equal(type, SERVICE_NAME_SERVER))
        return new ? PA_BROWSE_NEW_SERVER : PA_BROWSE_REMOVE_SERVER;

    return -1;
}

static sw_result resolve_reply(
        sw_discovery discovery,
        sw_discovery_oid oid,
        sw_uint32 interface_index,
        sw_const_string name,
        sw_const_string type,
        sw_const_string domain,
        sw_ipv4_address address,
        sw_port port,
        sw_octets text_record,
        sw_ulong text_record_len,
        sw_opaque extra) {
    
    pa_browser *b = extra;
    pa_browse_info i;
    char ip[256], a[256];
    int opcode;
    int device_found = 0;
    uint32_t cookie;
    pa_sample_spec ss;
    int ss_valid = 0;
    sw_text_record_iterator iterator;
    int free_iterator = 0;
    char *c = NULL;
    
    assert(b);

    sw_discovery_cancel(discovery, oid);

    memset(&i, 0, sizeof(i));
    i.name = name;
        
    if (!b->callback)
        goto fail;

    opcode = map_to_opcode(type, 1);
    assert(opcode >= 0);
    
    snprintf(a, sizeof(a), "tcp:%s:%u", sw_ipv4_address_name(address, ip, sizeof(ip)), port);
    i.server = a;
    
    if (text_record && text_record_len) {
        char key[SW_TEXT_RECORD_MAX_LEN];
        uint8_t val[SW_TEXT_RECORD_MAX_LEN];
        uint32_t val_len;
  
        if (sw_text_record_iterator_init(&iterator, text_record, text_record_len) != SW_OKAY) {
            pa_log_error(__FILE__": sw_text_record_string_iterator_init() failed.");
            goto fail;
        }

        free_iterator = 1;
        
        while (sw_text_record_iterator_next(iterator, key, val, &val_len) == SW_OKAY) {
            c = pa_xstrndup((char*) val, val_len);
            
            if (!strcmp(key, "device")) {
                device_found = 1;
                pa_xfree((char*) i.device);
                i.device = c;
                c = NULL;
            } else if (!strcmp(key, "server-version")) {
                pa_xfree((char*) i.server_version);
                i.server_version = c;
                c = NULL;
            } else if (!strcmp(key, "user-name")) {
                pa_xfree((char*) i.user_name);
                i.user_name = c;
                c = NULL;
            } else if (!strcmp(key, "fqdn")) {
                size_t l;
                
                pa_xfree((char*) i.fqdn);
                i.fqdn = c;
                c = NULL;
                
                l = strlen(a);
                assert(l+1 <= sizeof(a));
                strncat(a, " ", sizeof(a)-l-1);
                strncat(a, i.fqdn, sizeof(a)-l-2);
            } else if (!strcmp(key, "cookie")) {

                if (pa_atou(c, &cookie) < 0)
                    goto fail;
                
                i.cookie = &cookie;
            } else if (!strcmp(key, "description")) {
                pa_xfree((char*) i.description);
                i.description = c;
                c = NULL;
            } else if (!strcmp(key, "channels")) {
                uint32_t ch;
                
                if (pa_atou(c, &ch) < 0 || ch <= 0 || ch > 255)
                    goto fail;

                ss.channels = (uint8_t) ch;
                ss_valid |= 1;

            } else if (!strcmp(key, "rate")) {
                if (pa_atou(c, &ss.rate) < 0)
                    goto fail;
                ss_valid |= 2;
            } else if (!strcmp(key, "format")) {

                if ((ss.format = pa_parse_sample_format(c)) == PA_SAMPLE_INVALID)
                    goto fail;
                
                ss_valid |= 4;
            }

            pa_xfree(c);
            c = NULL;
        }
        
    }

    /* No device txt record was sent for a sink or source service */
    if (opcode != PA_BROWSE_NEW_SERVER && !device_found) 
        goto fail;

    if (ss_valid == 7)
        i.sample_spec = &ss;
    

    b->callback(b, opcode, &i, b->userdata);

fail:
    pa_xfree((void*) i.device);
    pa_xfree((void*) i.fqdn);
    pa_xfree((void*) i.server_version);
    pa_xfree((void*) i.user_name);
    pa_xfree((void*) i.description);
    pa_xfree(c);

    if (free_iterator)
        sw_text_record_iterator_fina(iterator);

    
    return SW_OKAY;
}

static sw_result browse_reply(
        sw_discovery discovery,
        sw_discovery_oid id,
        sw_discovery_browse_status status,
        sw_uint32 interface_index,
        sw_const_string name,
        sw_const_string type,
        sw_const_string domain,
        sw_opaque extra) {
    
    pa_browser *b = extra;
    assert(b);

    switch (status) {
        case SW_DISCOVERY_BROWSE_ADD_SERVICE: {
            sw_discovery_oid oid;

            if (sw_discovery_resolve(b->discovery, 0, name, type, domain, resolve_reply, b, &oid) != SW_OKAY)
                pa_log_error(__FILE__": sw_discovery_resolve() failed");

            break;
        }
            
        case SW_DISCOVERY_BROWSE_REMOVE_SERVICE:
            if (b->callback) {
                pa_browse_info i;
                int opcode;
                
                memset(&i, 0, sizeof(i));
                i.name = name;

                opcode = map_to_opcode(type, 0);
                assert(opcode >= 0);
                
                b->callback(b, opcode, &i, b->userdata);
            }
            break;

        default:
            ;
    }

    return SW_OKAY;
}

pa_browser *pa_browser_new(pa_mainloop_api *mainloop) {
    pa_browser *b;
    sw_discovery_oid oid;

    b = pa_xnew(pa_browser, 1);
    b->mainloop = mainloop;
    b->ref = 1;
    b->callback = NULL;
    b->userdata = NULL;

    if (sw_discovery_init(&b->discovery) != SW_OKAY) {
        pa_log_error(__FILE__": sw_discovery_init() failed.");
        pa_xfree(b);
        return NULL;
    }
    
    if (sw_discovery_browse(b->discovery, 0, SERVICE_NAME_SERVER, NULL, browse_reply, b, &oid) != SW_OKAY ||
        sw_discovery_browse(b->discovery, 0, SERVICE_NAME_SINK, NULL, browse_reply, b, &oid) != SW_OKAY ||
        sw_discovery_browse(b->discovery, 0, SERVICE_NAME_SOURCE, NULL, browse_reply, b, &oid) != SW_OKAY) {

        pa_log_error(__FILE__": sw_discovery_browse() failed.");
        
        sw_discovery_fina(b->discovery);
        pa_xfree(b);
        return NULL;
    }
    
    b->io_event = mainloop->io_new(mainloop, sw_discovery_socket(b->discovery), PA_IO_EVENT_INPUT, io_callback, b);
    return b;
}

static void browser_free(pa_browser *b) {
    assert(b && b->mainloop);

    if (b->io_event)
        b->mainloop->io_free(b->io_event);
    
    sw_discovery_fina(b->discovery);
    pa_xfree(b);
}

pa_browser *pa_browser_ref(pa_browser *b) {
    assert(b && b->ref >= 1);
    b->ref++;
    return b;
}

void pa_browser_unref(pa_browser *b) {
    assert(b && b->ref >= 1);

    if ((-- (b->ref)) <= 0)
        browser_free(b);
}

void pa_browser_set_callback(pa_browser *b, pa_browse_cb_t cb, void *userdata) {
    assert(b);

    b->callback = cb;
    b->userdata = userdata;
}
