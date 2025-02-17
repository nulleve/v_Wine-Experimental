/*
 * nsiproxy.sys udp module
 *
 * Copyright 2003, 2006, 2011 Juan Lang
 * Copyright 2021 Huw Davies
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#if 0
#pragma makedep unix
#endif

#include "config.h"
#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif

#ifdef HAVE_SYS_SOCKETVAR_H
#include <sys/socketvar.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_NETINET_IP_H
#include <netinet/ip.h>
#endif

#ifdef HAVE_NETINET_IP_VAR_H
#include <netinet/ip_var.h>
#endif

#ifdef HAVE_NETINET_IN_PCB_H
#include <netinet/in_pcb.h>
#endif

#ifdef HAVE_NETINET_UDP_H
#include <netinet/udp.h>
#endif

#ifdef HAVE_NETINET_UDP_VAR_H
#include <netinet/udp_var.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#define USE_WS_PREFIX
#include "winsock2.h"
#include "ifdef.h"
#include "netiodef.h"
#include "ws2ipdef.h"
#include "udpmib.h"
#include "wine/nsi.h"
#include "wine/debug.h"
#include "wine/server.h"

#include "unix_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(nsi);

static UINT udp_num_addrs( USHORT family )
{
    UINT endpoint_count = 0;

    nsi_enumerate_all( 1, 0, &NPI_MS_UDP_MODULEID, NSI_UDP_ENDPOINT_TABLE,
                       NULL, 0, NULL, 0, NULL, 0, NULL, 0, &endpoint_count );
    /* FIXME: actually retrieve the keys and only count endpoints which match family */
    return endpoint_count;
}

static NTSTATUS udp_stats_get_all_parameters( const void *key, UINT key_size, void *rw_data, UINT rw_size,
                                              void *dynamic_data, UINT dynamic_size, void *static_data, UINT static_size )
{
    struct nsi_udp_stats_dynamic dyn;
    const USHORT *family = key;

    TRACE( "%p %d %p %d %p %d %p %d\n", key, key_size, rw_data, rw_size, dynamic_data, dynamic_size,
           static_data, static_size );

    if (*family != WS_AF_INET && *family != WS_AF_INET6) return STATUS_NOT_SUPPORTED;

    memset( &dyn, 0, sizeof(dyn) );

    dyn.num_addrs = udp_num_addrs( *family );

#ifdef __linux__
    if (*family == WS_AF_INET)
    {
        NTSTATUS status = STATUS_NOT_SUPPORTED;
        static const char hdr[] = "Udp:";
        char buf[512], *ptr;
        FILE *fp;

        if (!(fp = fopen( "/proc/net/snmp", "r" ))) return STATUS_NOT_SUPPORTED;

        while ((ptr = fgets( buf, sizeof(buf), fp )))
        {
            if (ascii_strncasecmp( buf, hdr, sizeof(hdr) - 1 )) continue;
            /* last line was a header, get another */
            if (!(ptr = fgets( buf, sizeof(buf), fp ))) break;
            if (!ascii_strncasecmp( buf, hdr, sizeof(hdr) - 1 ))
            {
                unsigned int in_dgrams, out_dgrams;
                ptr += sizeof(hdr);
                sscanf( ptr, "%u %u %u %u %u",
                        &in_dgrams, &dyn.no_ports, &dyn.in_errs, &out_dgrams, &dyn.num_addrs );
                dyn.in_dgrams = in_dgrams;
                dyn.out_dgrams = out_dgrams;
                if (dynamic_data) *(struct nsi_udp_stats_dynamic *)dynamic_data = dyn;
                status = STATUS_SUCCESS;
                break;
            }
        }
        fclose( fp );
        return status;
    }
    else
    {
        unsigned int in_dgrams = 0, out_dgrams = 0;
        struct
        {
            const char *name;
            UINT *elem;
        } udp_stat_list[] =
        {
            { "Udp6InDatagrams",  &in_dgrams },
            { "Udp6NoPorts",      &dyn.no_ports },
            { "Udp6InErrors",     &dyn.in_errs },
            { "Udp6OutDatagrams", &out_dgrams },
        };
        char buf[512], *ptr, *value;
        UINT res, i;
        FILE *fp;

        if (!(fp = fopen( "/proc/net/snmp6", "r" ))) return STATUS_NOT_SUPPORTED;

        while ((ptr = fgets( buf, sizeof(buf), fp )))
        {
            if (!(value = strchr( buf, ' ' ))) continue;

            /* terminate the valuename */
            ptr = value - 1;
            *(ptr + 1) = '\0';

            /* and strip leading spaces from value */
            value += 1;
            while (*value==' ') value++;
            if ((ptr = strchr( value, '\n' ))) *ptr='\0';

            for (i = 0; i < ARRAY_SIZE(udp_stat_list); i++)
                if (!ascii_strcasecmp( buf, udp_stat_list[i].name ) && sscanf( value, "%d", &res ))
                    *udp_stat_list[i].elem = res;
        }
        dyn.in_dgrams = in_dgrams;
        dyn.out_dgrams = out_dgrams;
        if (dynamic_data) *(struct nsi_udp_stats_dynamic *)dynamic_data = dyn;
        fclose( fp );
        return STATUS_SUCCESS;
    }
#elif defined(HAVE_SYS_SYSCTL_H) && defined(UDPCTL_STATS) && defined(HAVE_STRUCT_UDPSTAT_UDPS_IPACKETS)
    {
        int mib[] = { CTL_NET, PF_INET, IPPROTO_UDP, UDPCTL_STATS };
        struct udpstat udp_stat;
        size_t needed = sizeof(udp_stat);

        if (sysctl( mib, ARRAY_SIZE(mib), &udp_stat, &needed, NULL, 0 ) == -1) return STATUS_NOT_SUPPORTED;

        dyn.in_dgrams = udp_stat.udps_ipackets;
        dyn.out_dgrams = udp_stat.udps_opackets;
        dyn.no_ports = udp_stat.udps_noport;
        dyn.in_errs = udp_stat.udps_hdrops + udp_stat.udps_badsum + udp_stat.udps_fullsock + udp_stat.udps_badlen;
        if (dynamic_data) *(struct nsi_udp_stats_dynamic *)dynamic_data = dyn;
        return STATUS_SUCCESS;
    }
#endif
    FIXME( "Not implemented\n" );
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS udp_endpoint_enumerate_all( void *key_data, UINT key_size, void *rw_data, UINT rw_size,
                                            void *dynamic_data, UINT dynamic_size,
                                            void *static_data, UINT static_size, UINT_PTR *count )
{
    UINT num = 0;
    NTSTATUS status = STATUS_SUCCESS;
    BOOL want_data = key_size || rw_size || dynamic_size || static_size;
    struct nsi_udp_endpoint_key key, *key_out = key_data;
    struct nsi_udp_endpoint_static stat, *stat_out = static_data;
    struct ipv6_addr_scope *addr_scopes = NULL;
    unsigned int addr_scopes_size = 0, pid_map_size = 0;
    struct pid_map *pid_map = NULL;

    TRACE( "%p %d %p %d %p %d %p %d %p\n", key_data, key_size, rw_data, rw_size,
           dynamic_data, dynamic_size, static_data, static_size, count );

#ifdef __linux__
    {
        FILE *fp;
        char buf[512], *ptr;
        int inode;
        UINT addr;

        if (!(fp = fopen( "/proc/net/udp", "r" ))) return ERROR_NOT_SUPPORTED;

        memset( &key, 0, sizeof(key) );
        memset( &stat, 0, sizeof(stat) );
        if (stat_out) pid_map = get_pid_map( &pid_map_size );

        /* skip header line */
        ptr = fgets( buf, sizeof(buf), fp );
        while ((ptr = fgets( buf, sizeof(buf), fp )))
        {
            if (sscanf( ptr, "%*u: %x:%hx %*s %*s %*s %*s %*s %*s %*s %d",
                        &addr, &key.local.Ipv4.sin_port, &inode ) != 3)
                continue;

            key.local.Ipv4.sin_family = WS_AF_INET;
            key.local.Ipv4.sin_addr.WS_s_addr = addr;
            key.local.Ipv4.sin_port = htons( key.local.Ipv4.sin_port );

            if (stat_out)
            {
                stat.pid = find_owning_pid( pid_map, pid_map_size, inode );
                stat.create_time = 0; /* FIXME */
                stat.flags = 0; /* FIXME */
                stat.mod_info = 0; /* FIXME */
            }

            if (num < *count)
            {
                if (key_out) *key_out++ = key;
                if (stat_out) *stat_out++ = stat;
            }
            num++;
        }
        fclose( fp );

        if ((fp = fopen( "/proc/net/udp6", "r" )))
        {
            memset( &key, 0, sizeof(key) );
            memset( &stat, 0, sizeof(stat) );

            addr_scopes = get_ipv6_addr_scope_table( &addr_scopes_size );

            /* skip header line */
            ptr = fgets( buf, sizeof(buf), fp );
            while ((ptr = fgets( buf, sizeof(buf), fp )))
            {
                UINT *local_addr = (UINT *)&key.local.Ipv6.sin6_addr;

                if (sscanf( ptr, "%*u: %8x%8x%8x%8x:%hx %*s %*s %*s %*s %*s %*s %*s %d",
                            local_addr, local_addr + 1, local_addr + 2, local_addr + 3,
                            &key.local.Ipv6.sin6_port, &inode ) != 6)
                    continue;
                key.local.Ipv6.sin6_family = WS_AF_INET6;
                key.local.Ipv6.sin6_port = htons( key.local.Ipv6.sin6_port );
                key.local.Ipv6.sin6_scope_id = find_ipv6_addr_scope( &key.local.Ipv6.sin6_addr, addr_scopes,
                                                                     addr_scopes_size );

                if (stat_out)
                {
                    stat.pid = find_owning_pid( pid_map, pid_map_size, inode );
                    stat.create_time = 0; /* FIXME */
                    stat.flags = 0; /* FIXME */
                    stat.mod_info = 0; /* FIXME */
                }

                if (num < *count)
                {
                    if (key_out) *key_out++ = key;
                    if (stat_out) *stat_out++ = stat;
                }
                num++;
            }
            fclose( fp );
        }
    }
#elif defined(HAVE_SYS_SYSCTL_H) && defined(UDPCTL_PCBLIST) && defined(HAVE_STRUCT_XINPGEN)
    {
        int mib[] = { CTL_NET, PF_INET, IPPROTO_UDP, UDPCTL_PCBLIST };
        size_t len = 0;
        char *buf = NULL;
        struct xinpgen *xig, *orig_xig;

        if (sysctl( mib, ARRAY_SIZE(mib), NULL, &len, NULL, 0 ) < 0)
        {
            ERR( "Failure to read net.inet.udp.pcblist via sysctlbyname!\n" );
            status = STATUS_NOT_SUPPORTED;
            goto err;
        }

        buf = malloc( len );
        if (!buf)
        {
            status = STATUS_NO_MEMORY;
            goto err;
        }

        if (sysctl( mib, ARRAY_SIZE(mib), buf, &len, NULL, 0 ) < 0)
        {
            ERR( "Failure to read net.inet.udp.pcblist via sysctlbyname!\n" );
            status = STATUS_NOT_SUPPORTED;
            goto err;
        }

        /* Might be nothing here; first entry is just a header it seems */
        if (len <= sizeof(struct xinpgen)) goto err;

        addr_scopes = get_ipv6_addr_scope_table( &addr_scopes_size );
        if (stat_out) pid_map = get_pid_map( &pid_map_size );

        orig_xig = (struct xinpgen *)buf;
        xig = orig_xig;

        for (xig = (struct xinpgen *)((char *)xig + xig->xig_len);
             xig->xig_len > sizeof (struct xinpgen);
             xig = (struct xinpgen *)((char *)xig + xig->xig_len))
        {
#if __FreeBSD_version >= 1200026
            struct xinpcb *in = (struct xinpcb *)xig;
            struct xsocket *sock = &in->xi_socket;
#else
            struct inpcb *in = &((struct xinpcb *)xig)->xi_inp;
            struct xsocket *sock = &((struct xinpcb *)xig)->xi_socket;
#endif
            static const struct in6_addr zero;

            /* Ignore sockets for other protocols */
            if (sock->xso_protocol != IPPROTO_UDP) continue;

            /* Ignore PCBs that were freed while generating the data */
            if (in->inp_gencnt > orig_xig->xig_gen) continue;

            /* we're only interested in IPv4 and IPv6 addresses */
            if (!(in->inp_vflag & (INP_IPV4 | INP_IPV6))) continue;

            /* If all 0's, skip it */
            if (in->inp_vflag & INP_IPV4 && !in->inp_laddr.s_addr) continue;
            if (in->inp_vflag & INP_IPV6 && !memcmp( &in->in6p_laddr, &zero, sizeof(zero) ) && !in->inp_lport) continue;

            if (in->inp_vflag & INP_IPV4)
            {
                key.local.Ipv4.sin_family = WS_AF_INET;
                key.local.Ipv4.sin_addr.WS_s_addr = in->inp_laddr.s_addr;
                key.local.Ipv4.sin_port = in->inp_lport;
            }
            else
            {
                key.local.Ipv6.sin6_family = WS_AF_INET6;
                memcpy( &key.local.Ipv6.sin6_addr, &in->in6p_laddr, sizeof(in->in6p_laddr) );
                key.local.Ipv6.sin6_port = in->inp_lport;
                key.local.Ipv6.sin6_scope_id = find_ipv6_addr_scope( &key.local.Ipv6.sin6_addr, addr_scopes,
                                                                     addr_scopes_size );
            }

            if (stat_out)
            {
                stat.pid = find_owning_pid( pid_map, pid_map_size, (UINT_PTR)sock->so_pcb );
                stat.create_time = 0; /* FIXME */
                stat.flags = !(in->inp_flags & INP_ANONPORT);
                stat.mod_info = 0; /* FIXME */
            }

            if (num < *count)
            {
                if (key_out) *key_out++ = key;
                if (stat_out) *stat_out++ = stat;
            }
            num++;
        }
    err:
        free( buf );
    }
#else
    FIXME( "not implemented\n" );
    return STATUS_NOT_IMPLEMENTED;
#endif

    if (!want_data || num <= *count) *count = num;
    else status = STATUS_BUFFER_OVERFLOW;

    free( pid_map );
    free( addr_scopes );
    return status;
}

static struct module_table udp_tables[] =
{
    {
        NSI_UDP_STATS_TABLE,
        {
            sizeof(USHORT), 0,
            sizeof(struct nsi_udp_stats_dynamic), 0
        },
        NULL,
        udp_stats_get_all_parameters,
    },
    {
        NSI_UDP_ENDPOINT_TABLE,
        {
            sizeof(struct nsi_udp_endpoint_key), 0,
            0, sizeof(struct nsi_udp_endpoint_static)
        },
        udp_endpoint_enumerate_all,
    },
    {
        ~0u
    }
};

const struct module udp_module =
{
    &NPI_MS_UDP_MODULEID,
    udp_tables
};
