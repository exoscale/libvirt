/*
 * Copyright (C) 2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Authors:
 *     Mark McLoughlin <markmc@redhat.com>
 */

#include <config.h>

#if defined(WITH_BRIDGE)

#include "bridge.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <paths.h>
#include <sys/wait.h>

#include <linux/param.h>     /* HZ                 */
#include <linux/sockios.h>   /* SIOCBRADDBR etc.   */
#include <linux/if_bridge.h> /* SYSFS_BRIDGE_ATTR  */
#include <linux/if_tun.h>    /* IFF_TUN, IFF_NO_PI */
#include <net/if_arp.h>    /* ARPHRD_ETHER */

#include "internal.h"
#include "memory.h"
#include "util.h"

#define MAX_BRIDGE_ID 256

#define JIFFIES_TO_MS(j) (((j)*1000)/HZ)
#define MS_TO_JIFFIES(ms) (((ms)*HZ)/1000)

struct _brControl {
    int fd;
};

/**
 * brInit:
 * @ctlp: pointer to bridge control return value
 *
 * Initialize a new bridge layer. In case of success
 * @ctlp will contain a pointer to the new bridge structure.
 *
 * Returns 0 in case of success, an error code otherwise.
 */
int
brInit(brControl **ctlp)
{
    int fd;
    int flags;

    if (!ctlp || *ctlp)
        return EINVAL;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return errno;

    if ((flags = fcntl(fd, F_GETFD)) < 0 ||
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        int err = errno;
        close(fd);
        return err;
    }

    if (VIR_ALLOC(*ctlp) < 0) {
        close(fd);
        return ENOMEM;
    }

    (*ctlp)->fd = fd;

    return 0;
}

/**
 * brShutdown:
 * @ctl: pointer to a bridge control
 *
 * Shutdown the bridge layer and deallocate the associated structures
 */
void
brShutdown(brControl *ctl)
{
    if (!ctl)
        return;

    close(ctl->fd);
    ctl->fd = 0;

    VIR_FREE(ctl);
}

/**
 * brAddBridge:
 * @ctl: bridge control pointer
 * @name: the bridge name
 *
 * This function register a new bridge
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */
#ifdef SIOCBRADDBR
int
brAddBridge(brControl *ctl,
            char **name)
{
    if (!ctl || !ctl->fd || !name)
        return EINVAL;

    if (*name) {
        if (ioctl(ctl->fd, SIOCBRADDBR, *name) == 0)
            return 0;
    } else {
        int id = 0;
        do {
            char try[50];

            snprintf(try, sizeof(try), "virbr%d", id);

            if (ioctl(ctl->fd, SIOCBRADDBR, try) == 0) {
                if (!(*name = strdup(try))) {
                    ioctl(ctl->fd, SIOCBRDELBR, name);
                    return ENOMEM;
                }
                return 0;
            }

            id++;
        } while (id < MAX_BRIDGE_ID);
    }

    return errno;
}
#else
int brAddBridge (brControl *ctl ATTRIBUTE_UNUSED,
                 char **name ATTRIBUTE_UNUSED)
{
    return EINVAL;
}
#endif

#ifdef SIOCBRDELBR
int
brHasBridge(brControl *ctl,
            const char *name)
{
    struct ifreq ifr;
    int len;

    if (!ctl || !name) {
        errno = EINVAL;
        return -1;
    }

    if ((len = strlen(name)) >= BR_IFNAME_MAXLEN) {
        errno = EINVAL;
        return -1;
    }

    memset(&ifr, 0, sizeof(struct ifreq));

    strncpy(ifr.ifr_name, name, len);
    ifr.ifr_name[len] = '\0';

    if (ioctl(ctl->fd, SIOCGIFFLAGS, &ifr))
        return -1;

    return 0;
}
#else
int
brHasBridge(brControl *ctl,
            const char *name)
{
    return EINVAL;
}
#endif

/**
 * brDeleteBridge:
 * @ctl: bridge control pointer
 * @name: the bridge name
 *
 * Remove a bridge from the layer.
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */
#ifdef SIOCBRDELBR
int
brDeleteBridge(brControl *ctl,
               const char *name)
{
    if (!ctl || !ctl->fd || !name)
        return EINVAL;

    return ioctl(ctl->fd, SIOCBRDELBR, name) == 0 ? 0 : errno;
}
#else
int
brDeleteBridge(brControl *ctl ATTRIBUTE_UNUSED,
               const char *name ATTRIBUTE_UNUSED)
{
    return EINVAL;
}
#endif

#if defined(SIOCBRADDIF) && defined(SIOCBRDELIF)
static int
brAddDelInterface(brControl *ctl,
                  int cmd,
                  const char *bridge,
                  const char *iface)
{
    struct ifreq ifr;
    int len;

    if (!ctl || !ctl->fd || !bridge || !iface)
        return EINVAL;

    if ((len = strlen(bridge)) >= BR_IFNAME_MAXLEN)
        return EINVAL;

    memset(&ifr, 0, sizeof(struct ifreq));

    strncpy(ifr.ifr_name, bridge, len);
    ifr.ifr_name[len] = '\0';

    if (!(ifr.ifr_ifindex = if_nametoindex(iface)))
        return ENODEV;

    return ioctl(ctl->fd, cmd, &ifr) == 0 ? 0 : errno;
}
#endif

/**
 * brAddInterface:
 * @ctl: bridge control pointer
 * @bridge: the bridge name
 * @iface: the network interface name
 *
 * Adds an interface to a bridge
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */
#ifdef SIOCBRADDIF
int
brAddInterface(brControl *ctl,
               const char *bridge,
               const char *iface)
{
    return brAddDelInterface(ctl, SIOCBRADDIF, bridge, iface);
}
#else
int
brAddInterface(brControl *ctl ATTRIBUTE_UNUSED,
               const char *bridge ATTRIBUTE_UNUSED,
               const char *iface ATTRIBUTE_UNUSED)
{
    return EINVAL;
}
#endif

/**
 * brDeleteInterface:
 * @ctl: bridge control pointer
 * @bridge: the bridge name
 * @iface: the network interface name
 *
 * Removes an interface from a bridge
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */
#ifdef SIOCBRDELIF
int
brDeleteInterface(brControl *ctl,
                  const char *bridge,
                  const char *iface)
{
    return brAddDelInterface(ctl, SIOCBRDELIF, bridge, iface);
}
#else
int
brDeleteInterface(brControl *ctl ATTRIBUTE_UNUSED,
                  const char *bridge ATTRIBUTE_UNUSED,
                  const char *iface ATTRIBUTE_UNUSED)
{
    return EINVAL;
}
#endif

/**
 * ifGetMtu
 * @ctl: bridge control pointer
 * @ifname: interface name get MTU for
 *
 * This function gets the @mtu value set for a given interface @ifname.
 *
 * Returns the MTU value in case of success.
 * On error, returns -1 and sets errno accordingly
 */
static int ifGetMtu(brControl *ctl, const char *ifname)
{
    struct ifreq ifr;
    int len;

    if (!ctl || !ifname) {
        errno = EINVAL;
        return -1;
    }

    if ((len = strlen(ifname)) >=  BR_IFNAME_MAXLEN) {
        errno = EINVAL;
        return -1;
    }

    memset(&ifr, 0, sizeof(struct ifreq));

    strncpy(ifr.ifr_name, ifname, len);
    ifr.ifr_name[len] = '\0';

    if (ioctl(ctl->fd, SIOCGIFMTU, &ifr))
        return -1;

    return ifr.ifr_mtu;

}

/**
 * ifSetMtu:
 * @ctl: bridge control pointer
 * @ifname: interface name to set MTU for
 * @mtu: MTU value
 *
 * This function sets the @mtu for a given interface @ifname.  Typically
 * used on a tap device to set up for Jumbo Frames.
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */
static int ifSetMtu(brControl *ctl, const char *ifname, int mtu)
{
    struct ifreq ifr;
    int len;

    if (!ctl || !ifname)
        return EINVAL;

    if ((len = strlen(ifname)) >=  BR_IFNAME_MAXLEN)
        return EINVAL;

    memset(&ifr, 0, sizeof(struct ifreq));

    strncpy(ifr.ifr_name, ifname, len);
    ifr.ifr_name[len] = '\0';
    ifr.ifr_mtu = mtu;

    return ioctl(ctl->fd, SIOCSIFMTU, &ifr) == 0 ? 0 : errno;
}

/**
 * brSetInterfaceMtu
 * @ctl: bridge control pointer
 * @bridge: name of the bridge interface
 * @ifname: name of the interface whose MTU we want to set
 *
 * Sets the interface mtu to the same MTU of the bridge
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */
static int brSetInterfaceMtu(brControl *ctl,
                             const char *bridge,
                             const char *ifname)
{
    int mtu = ifGetMtu(ctl, bridge);

    if (mtu < 0)
        return errno;

    return ifSetMtu(ctl, ifname, mtu);
}

/**
 * brAddTap:
 * @ctl: bridge control pointer
 * @bridge: the bridge name
 * @ifname: the interface name (or name template)
 * @tapfd: file descriptor return value for the new tap device
 *
 * This function creates a new tap device on a bridge. @ifname can be either
 * a fixed name or a name template with '%d' for dynamic name allocation.
 * in either case the final name for the bridge will be stored in @ifname
 * and the associated file descriptor in @tapfd.
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */
int
brAddTap(brControl *ctl,
         const char *bridge,
         char **ifname,
         int *tapfd)
{
    int id, subst, fd;

    if (!ctl || !ctl->fd || !bridge || !ifname || !tapfd)
        return EINVAL;

    subst = id = 0;

    if (strstr(*ifname, "%d"))
        subst = 1;

    if ((fd = open("/dev/net/tun", O_RDWR)) < 0)
      return errno;

    do {
        struct ifreq try;
        int len;

        memset(&try, 0, sizeof(struct ifreq));

        try.ifr_flags = IFF_TAP|IFF_NO_PI;

        if (subst) {
            len = snprintf(try.ifr_name, BR_IFNAME_MAXLEN, *ifname, id);
            if (len >= BR_IFNAME_MAXLEN) {
                errno = EADDRINUSE;
                goto error;
            }
        } else {
            len = strlen(*ifname);
            if (len >= BR_IFNAME_MAXLEN - 1) {
                errno = EINVAL;
                goto error;
            }

            strncpy(try.ifr_name, *ifname, len);
            try.ifr_name[len] = '\0';
        }

        if (ioctl(fd, TUNSETIFF, &try) == 0) {
            /* We need to set the interface MTU before adding it
             * to the bridge, because the bridge will have its
             * MTU adjusted automatically when we add the new interface.
             */
            if ((errno = brSetInterfaceMtu(ctl, bridge, try.ifr_name)))
                goto error;
            if ((errno = brAddInterface(ctl, bridge, try.ifr_name)))
                goto error;
            if ((errno = brSetInterfaceUp(ctl, try.ifr_name, 1)))
                goto error;
            VIR_FREE(*ifname);
            if (!(*ifname = strdup(try.ifr_name))) {
                errno = ENOMEM;
                goto error;
            }
            *tapfd = fd;
            return 0;
        }

        id++;
    } while (subst && id <= MAX_BRIDGE_ID);

 error:
    close(fd);

    return errno;
}

/**
 * brSetInterfaceUp:
 * @ctl: bridge control pointer
 * @ifname: the interface name
 * @up: 1 for up, 0 for down
 *
 * Function to control if an interface is activated (up, 1) or not (down, 0)
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */
int
brSetInterfaceUp(brControl *ctl,
                 const char *ifname,
                 int up)
{
    struct ifreq ifr;
    int len;
    int flags;

    if (!ctl || !ifname)
        return EINVAL;

    if ((len = strlen(ifname)) >= BR_IFNAME_MAXLEN)
        return EINVAL;

    memset(&ifr, 0, sizeof(struct ifreq));

    strncpy(ifr.ifr_name, ifname, len);
    ifr.ifr_name[len] = '\0';

    if (ioctl(ctl->fd, SIOCGIFFLAGS, &ifr) < 0)
        return errno;

    flags = up ? (ifr.ifr_flags | IFF_UP) : (ifr.ifr_flags & ~IFF_UP);

    if (ifr.ifr_flags != flags) {
        ifr.ifr_flags = flags;

        if (ioctl(ctl->fd, SIOCSIFFLAGS, &ifr) < 0)
            return errno;
    }

    return 0;
}

/**
 * brGetInterfaceUp:
 * @ctl: bridge control pointer
 * @ifname: the interface name
 * @up: where to store the status
 *
 * Function to query if an interface is activated (1) or not (0)
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */
int
brGetInterfaceUp(brControl *ctl,
                 const char *ifname,
                 int *up)
{
    struct ifreq ifr;
    int len;

    if (!ctl || !ifname || !up)
        return EINVAL;

    if ((len = strlen(ifname)) >= BR_IFNAME_MAXLEN)
        return EINVAL;

    memset(&ifr, 0, sizeof(struct ifreq));

    strncpy(ifr.ifr_name, ifname, len);
    ifr.ifr_name[len] = '\0';

    if (ioctl(ctl->fd, SIOCGIFFLAGS, &ifr) < 0)
        return errno;

    *up = (ifr.ifr_flags & IFF_UP) ? 1 : 0;

    return 0;
}

static int
brSetInetAddr(brControl *ctl,
              const char *ifname,
              int cmd,
              const char *addr)
{
    struct ifreq ifr;
    struct in_addr inaddr;
    int len, ret;

    if (!ctl || !ctl->fd || !ifname || !addr)
        return EINVAL;

    if ((len = strlen(ifname)) >= BR_IFNAME_MAXLEN)
        return EINVAL;

    memset(&ifr, 0, sizeof(struct ifreq));

    strncpy(ifr.ifr_name, ifname, len);
    ifr.ifr_name[len] = '\0';

    if ((ret = inet_pton(AF_INET, addr, &inaddr)) < 0)
        return errno;
    else if (ret == 0)
        return EINVAL;

    ((struct sockaddr_in *)&ifr.ifr_data)->sin_family = AF_INET;
    ((struct sockaddr_in *)&ifr.ifr_data)->sin_addr   = inaddr;

    if (ioctl(ctl->fd, cmd, &ifr) < 0)
        return errno;

    return 0;
}

static int
brGetInetAddr(brControl *ctl,
              const char *ifname,
              int cmd,
              char *addr,
              int maxlen)
{
    struct ifreq ifr;
    struct in_addr *inaddr;
    int len;

    if (!ctl || !ctl->fd || !ifname || !addr)
        return EINVAL;

    if ((len = strlen(ifname)) >= BR_IFNAME_MAXLEN)
        return EINVAL;

    memset(&ifr, 0, sizeof(struct ifreq));

    strncpy(ifr.ifr_name, ifname, len);
    ifr.ifr_name[len] = '\0';

    if (ioctl(ctl->fd, cmd, &ifr) < 0)
        return errno;

    if (maxlen < BR_INET_ADDR_MAXLEN || ifr.ifr_addr.sa_family != AF_INET)
        return EFAULT;

    inaddr = &((struct sockaddr_in *)&ifr.ifr_data)->sin_addr;

    if (!inet_ntop(AF_INET, inaddr, addr, maxlen))
        return errno;

    return 0;
}

/**
 * brSetInetAddress:
 * @ctl: bridge control pointer
 * @ifname: the interface name
 * @addr: the string representation of the IP address
 *
 * Function to bind the interface to an IP address, it should handle
 * IPV4 and IPv6. The string for addr would be of the form
 * "ddd.ddd.ddd.ddd" assuming the common IPv4 format.
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */

int
brSetInetAddress(brControl *ctl,
                 const char *ifname,
                 const char *addr)
{
    return brSetInetAddr(ctl, ifname, SIOCSIFADDR, addr);
}

/**
 * brGetInetAddress:
 * @ctl: bridge control pointer
 * @ifname: the interface name
 * @addr: the array for the string representation of the IP address
 * @maxlen: size of @addr in bytes
 *
 * Function to get the IP address of an interface, it should handle
 * IPV4 and IPv6. The returned string for addr would be of the form
 * "ddd.ddd.ddd.ddd" assuming the common IPv4 format.
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */

int
brGetInetAddress(brControl *ctl,
                 const char *ifname,
                 char *addr,
                 int maxlen)
{
    return brGetInetAddr(ctl, ifname, SIOCGIFADDR, addr, maxlen);
}

/**
 * brSetInetNetmask:
 * @ctl: bridge control pointer
 * @ifname: the interface name
 * @addr: the string representation of the netmask
 *
 * Function to set the netmask of an interface, it should handle
 * IPV4 and IPv6 forms. The string for addr would be of the form
 * "ddd.ddd.ddd.ddd" assuming the common IPv4 format.
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */

int
brSetInetNetmask(brControl *ctl,
                 const char *ifname,
                 const char *addr)
{
    return brSetInetAddr(ctl, ifname, SIOCSIFNETMASK, addr);
}

/**
 * brGetInetNetmask:
 * @ctl: bridge control pointer
 * @ifname: the interface name
 * @addr: the array for the string representation of the netmask
 * @maxlen: size of @addr in bytes
 *
 * Function to get the netmask of an interface, it should handle
 * IPV4 and IPv6. The returned string for addr would be of the form
 * "ddd.ddd.ddd.ddd" assuming the common IPv4 format.
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */

int
brGetInetNetmask(brControl *ctl,
                 const char *ifname,
                 char *addr,
                 int maxlen)
{
    return brGetInetAddr(ctl, ifname, SIOCGIFNETMASK, addr, maxlen);
}


/**
 * brSetForwardDelay:
 * @ctl: bridge control pointer
 * @bridge: the bridge name
 * @delay: delay in seconds
 *
 * Set the bridge forward delay
 *
 * Returns 0 in case of success or -1 on failure
 */

int
brSetForwardDelay(brControl *ctl ATTRIBUTE_UNUSED,
                  const char *bridge,
                  int delay)
{
    char delayStr[30];
    const char *const progargv[] = {
        BRCTL, "setfd", bridge, delayStr, NULL
    };

    snprintf(delayStr, sizeof(delayStr), "%d", delay);

    if (virRun(NULL, progargv, NULL) < 0)
        return -1;

    return 0;
}

/**
 * brSetEnableSTP:
 * @ctl: bridge control pointer
 * @bridge: the bridge name
 * @enable: 1 to enable, 0 to disable
 *
 * Control whether the bridge participates in the spanning tree protocol,
 * in general don't disable it without good reasons.
 *
 * Returns 0 in case of success or -1 on failure
 */
int
brSetEnableSTP(brControl *ctl ATTRIBUTE_UNUSED,
               const char *bridge,
               int enable)
{
    const char *setting = enable ? "on" : "off";
    const char *const progargv[] = {
        BRCTL, "stp", bridge, setting, NULL
    };

    if (virRun(NULL, progargv, NULL) < 0)
        return -1;

    return 0;
}

#endif /* WITH_BRIDGE */
