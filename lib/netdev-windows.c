/*
 * Copyright (c) 2014 VMware, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <config.h>
#include <errno.h>

#include <net/if.h>

#include "coverage.h"
#include "fatal-signal.h"
#include "netdev-provider.h"
#include "ofpbuf.h"
#include "poll-loop.h"
#include "shash.h"
#include "svec.h"
#include "vlog.h"
#include "odp-netlink.h"
#include "netlink-socket.h"
#include "netlink.h"

VLOG_DEFINE_THIS_MODULE(netdev_windows);
static struct vlog_rate_limit error_rl = VLOG_RATE_LIMIT_INIT(9999, 5);

enum {
    VALID_ETHERADDR         = 1 << 0,
    VALID_MTU               = 1 << 1,
    VALID_IFFLAG            = 1 << 5,
};

/* Caches the information of a netdev. */
struct netdev_windows {
    struct netdev up;
    int32_t dev_type;
    uint32_t port_no;

    unsigned int change_seq;

    unsigned int cache_valid;
    int ifindex;
    uint8_t mac[ETH_ADDR_LEN];
    uint32_t mtu;
    unsigned int ifi_flags;
};

/* Utility structure for netdev commands. */
struct netdev_windows_netdev_info {
    /* Generic Netlink header. */
    uint8_t cmd;

    /* Information that is relevant to ovs. */
    uint32_t dp_ifindex;
    uint32_t port_no;
    uint32_t ovs_type;

    /* General information of a network device. */
    const char *name;
    uint8_t mac_address[ETH_ADDR_LEN];
    uint32_t mtu;
    uint32_t ifi_flags;
};

static int refresh_port_status(struct netdev_windows *netdev);
static int query_netdev(const char *devname,
                        struct netdev_windows_netdev_info *reply,
                        struct ofpbuf **bufp);
static int netdev_windows_init(void);

/* Generic Netlink family numbers for OVS.
 *
 * Initialized by netdev_windows_init(). */
static int ovs_win_netdev_family;
struct nl_sock *ovs_win_netdev_sock;


static bool
is_netdev_windows_class(const struct netdev_class *netdev_class)
{
    return netdev_class->init == netdev_windows_init;
}

static struct netdev_windows *
netdev_windows_cast(const struct netdev *netdev_)
{
    ovs_assert(is_netdev_windows_class(netdev_get_class(netdev_)));
    return CONTAINER_OF(netdev_, struct netdev_windows, up);
}

static int
netdev_windows_init(void)
{
    int error = 0;
    static struct ovsthread_once once = OVSTHREAD_ONCE_INITIALIZER;

    if (ovsthread_once_start(&once)) {
        error = nl_lookup_genl_family(OVS_WIN_NETDEV_FAMILY,
                                      &ovs_win_netdev_family);
        if (error) {
            VLOG_ERR("Generic Netlink family '%s' does not exist. "
                     "The Open vSwitch kernel module is probably not loaded.",
                     OVS_WIN_NETDEV_FAMILY);
        }
        if (!error) {
            /* XXX: Where to close this socket? */
            error = nl_sock_create(NETLINK_GENERIC, &ovs_win_netdev_sock);
        }

        ovsthread_once_done(&once);
    }

    return error;
}

static struct netdev *
netdev_windows_alloc(void)
{
    struct netdev_windows *netdev = xzalloc(sizeof *netdev);
    return netdev ? &netdev->up : NULL;
}

static int
netdev_windows_system_construct(struct netdev *netdev_)
{
    struct netdev_windows *netdev = netdev_windows_cast(netdev_);
    uint8_t mac[ETH_ADDR_LEN];
    struct netdev_windows_netdev_info info;
    struct ofpbuf *buf;
    int ret;

    /* Query the attributes and runtime status of the netdev. */
    ret = query_netdev(netdev_get_name(&netdev->up), &info, &buf);
    if (ret) {
        return ret;
    }
    ofpbuf_delete(buf);

    netdev->change_seq = 1;
    netdev->dev_type = info.ovs_type;
    netdev->port_no = info.port_no;

    memcpy(netdev->mac, info.mac_address, ETH_ADDR_LEN);
    netdev->cache_valid = VALID_ETHERADDR;
    netdev->ifindex = -EOPNOTSUPP;

    netdev->mtu = info.mtu;
    netdev->cache_valid |= VALID_MTU;

    netdev->ifi_flags = info.ifi_flags;
    netdev->cache_valid |= VALID_IFFLAG;

    VLOG_DBG("construct device %s, ovs_type: %u.",
             netdev_get_name(&netdev->up), info.ovs_type);
    return 0;
}

static int
netdev_windows_netdev_to_ofpbuf(struct netdev_windows_netdev_info *info,
                                struct ofpbuf *buf)
{
    struct ovs_header *ovs_header;
    int error = EINVAL;

    nl_msg_put_genlmsghdr(buf, 0, ovs_win_netdev_family,
                          NLM_F_REQUEST | NLM_F_ECHO,
                          info->cmd, OVS_WIN_NETDEV_VERSION);

    ovs_header = ofpbuf_put_uninit(buf, sizeof *ovs_header);
    ovs_header->dp_ifindex = info->dp_ifindex;

    if (info->name) {
        nl_msg_put_string(buf, OVS_WIN_NETDEV_ATTR_NAME, info->name);
        error = 0;
    }

    return error;
}

static void
netdev_windows_info_init(struct netdev_windows_netdev_info *info)
{
    memset(info, 0, sizeof *info);
}

static int
netdev_windows_netdev_from_ofpbuf(struct netdev_windows_netdev_info *info,
                                  struct ofpbuf *buf)
{
    static const struct nl_policy ovs_netdev_policy[] = {
        [OVS_WIN_NETDEV_ATTR_PORT_NO] = { .type = NL_A_U32 },
        [OVS_WIN_NETDEV_ATTR_TYPE] = { .type = NL_A_U32 },
        [OVS_WIN_NETDEV_ATTR_NAME] = { .type = NL_A_STRING, .max_len = IFNAMSIZ },
        [OVS_WIN_NETDEV_ATTR_MAC_ADDR] = { NL_POLICY_FOR(info->mac_address) },
        [OVS_WIN_NETDEV_ATTR_MTU] = { .type = NL_A_U32 },
        [OVS_WIN_NETDEV_ATTR_IF_FLAGS] = { .type = NL_A_U32 },
    };

    struct nlattr *a[ARRAY_SIZE(ovs_netdev_policy)];
    struct ovs_header *ovs_header;
    struct nlmsghdr *nlmsg;
    struct genlmsghdr *genl;
    struct ofpbuf b;

    netdev_windows_info_init(info);

    ofpbuf_use_const(&b, ofpbuf_data(buf), ofpbuf_size(buf));
    nlmsg = ofpbuf_try_pull(&b, sizeof *nlmsg);
    genl = ofpbuf_try_pull(&b, sizeof *genl);
    ovs_header = ofpbuf_try_pull(&b, sizeof *ovs_header);
    if (!nlmsg || !genl || !ovs_header
        || nlmsg->nlmsg_type != ovs_win_netdev_family
        || !nl_policy_parse(&b, 0, ovs_netdev_policy, a,
                            ARRAY_SIZE(ovs_netdev_policy))) {
        return EINVAL;
    }

    info->cmd = genl->cmd;
    info->dp_ifindex = ovs_header->dp_ifindex;
    info->port_no = nl_attr_get_odp_port(a[OVS_WIN_NETDEV_ATTR_PORT_NO]);
    info->ovs_type = nl_attr_get_u32(a[OVS_WIN_NETDEV_ATTR_TYPE]);
    info->name = nl_attr_get_string(a[OVS_WIN_NETDEV_ATTR_NAME]);
    memcpy(info->mac_address, nl_attr_get_string(a[OVS_WIN_NETDEV_ATTR_NAME]),
           sizeof(info->mac_address));
    info->mtu = nl_attr_get_u32(a[OVS_WIN_NETDEV_ATTR_MTU]);
    info->ifi_flags = nl_attr_get_u32(a[OVS_WIN_NETDEV_ATTR_IF_FLAGS]);

    return 0;
}

static int
query_netdev(const char *devname,
             struct netdev_windows_netdev_info *info,
             struct ofpbuf **bufp)
{
    int error = 0;
    struct ofpbuf *request_buf;

    ovs_assert(info != NULL);
    netdev_windows_info_init(info);

    error = netdev_windows_init();
    if (error) {
        if (info) {
            *bufp = NULL;
            netdev_windows_info_init(info);
        }
        return error;
    }

    request_buf = ofpbuf_new(1024);
    info->cmd = OVS_WIN_NETDEV_CMD_GET;
    info->name = devname;
    error = netdev_windows_netdev_to_ofpbuf(info, request_buf);
    if (error) {
        ofpbuf_delete(request_buf);
        return error;
    }

    error = nl_transact(NETLINK_GENERIC, request_buf, bufp);
    ofpbuf_delete(request_buf);

    if (info) {
        if (!error) {
            error = netdev_windows_netdev_from_ofpbuf(info, *bufp);
        }
        if (error) {
            netdev_windows_info_init(info);
            ofpbuf_delete(*bufp);
            *bufp = NULL;
        }
    }

    return 0;
}

static void
netdev_windows_destruct(struct netdev *netdev_)
{

}

static void
netdev_windows_dealloc(struct netdev *netdev_)
{
    struct netdev_windows *netdev = netdev_windows_cast(netdev_);
    free(netdev);
}

static int
netdev_windows_get_etheraddr(const struct netdev *netdev_, uint8_t mac[6])
{
    struct netdev_windows *netdev = netdev_windows_cast(netdev_);

    ovs_assert((netdev->cache_valid & VALID_ETHERADDR) != 0);
    if (netdev->cache_valid & VALID_ETHERADDR) {
        memcpy(mac, netdev->mac, ETH_ADDR_LEN);
    } else {
        return EINVAL;
    }
    return 0;
}

static int
netdev_windows_get_mtu(const struct netdev *netdev_, int *mtup)
{
    struct netdev_windows *netdev = netdev_windows_cast(netdev_);

    ovs_assert((netdev->cache_valid & VALID_MTU) != 0);
    if (netdev->cache_valid & VALID_MTU) {
        *mtup = netdev->mtu;
    } else {
        return EINVAL;
    }
    return 0;
}


static int
netdev_windows_internal_construct(struct netdev *netdev_)
{
    return netdev_windows_system_construct(netdev_);
}


#define NETDEV_WINDOWS_CLASS(NAME, CONSTRUCT)                           \
{                                                                       \
    .type               = NAME,                                         \
    .init               = netdev_windows_init,                          \
    .alloc              = netdev_windows_alloc,                         \
    .construct          = CONSTRUCT,                                    \
    .destruct           = netdev_windows_destruct,                      \
    .dealloc            = netdev_windows_dealloc,                       \
    .get_etheraddr      = netdev_windows_get_etheraddr,                 \
}

const struct netdev_class netdev_windows_class =
    NETDEV_WINDOWS_CLASS(
        "system",
        netdev_windows_system_construct);

const struct netdev_class netdev_internal_class =
    NETDEV_WINDOWS_CLASS(
        "internal",
        netdev_windows_internal_construct);