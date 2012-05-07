/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// net_udp.c

#include "quakedef.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <unistd.h>

#include <arpa/inet.h>

#ifdef __sun__
#include <sys/filio.h>
#endif

#ifdef NeXT
#include <libc.h>
#endif

extern cvar_t hostname;

static int net_acceptsocket = -1;		// socket for fielding new connections
static int net_controlsocket;
static int net_broadcastsocket = 0;
static struct qsockaddr broadcastaddr;

static unsigned long myAddr;

#include "net_udp.h"


// **** Start of Android-specific code ****
// copied from ifc_utils.c
//
// This isn't a good long-term solution:
// 1) It is hard-wired to a particular wireless driver
// 2) It doesn't handle the IP address changing over time

#include <sys/ioctl.h>
#include <net/if.h>

//
// system dependent 
//

#define SOCK_DGRAM_PORTABLE    2
#define SOCK_STREAM_PORTABLE   1
#define FIONBIO_PORTABLE       0x5421
#define FIONREAD_PORTABLE      0x541B
#define ECONNREFUSED_PORTABLE  111
#define SOL_SOCKET_PORTABLE     1 
#define SO_BROADCAST_PORTABLE   6 

#define SOCK_DGRAM_MIPS    1
#define SOCK_STREAM_MIPS   2
#define FIONBIO_MIPS       0x667e
#define FIONREAD_MIPS      0x467f
#define ECONNREFUSED_MIPS  146
#define SOL_SOCKET_MIPS     0xffff 
#define SO_BROADCAST_MIPS   0x0020


int socket_portable(int domain, int type, int protocol);
int ioctl_portable(int fd, int cmd, void *);  // ToDo: ioctl_portable(int fd, int cmd, ...)
int setsockopt_portable(int s, int level, int optname, const void *optval, socklen_t optlen);
int errno_portable();


#if !defined(__GDK__)
int socket_portable(int domain, int type, int protocol)
{
#if defined(__mips__) 
   switch(type) {
   case SOCK_DGRAM_PORTABLE: 
      type = SOCK_DGRAM_MIPS; 
      break;
   case SOCK_STREAM_PORTABLE:
      type = SOCK_STREAM_MIPS; 
      break;
   }
#endif // __mips__
   return socket(domain, type, protocol);
}

int ioctl_portable(int fd, int cmd, void *arg)
{
#if defined(__mips__)
   switch(cmd) {
    case FIONBIO_PORTABLE:
      cmd = FIONBIO_MIPS; 
      break;
    case FIONREAD_PORTABLE:
      cmd = FIONREAD_MIPS;
    //ToDo: there are a lot more diff!  
   }
#endif // __mips__
   return ioctl(fd, cmd, arg);
}
   
int errno_portable()
{
   int ret = errno;
#if defined(__mips__)
   switch(ret) {
   case ECONNREFUSED_PORTABLE: 
      ret = ECONNREFUSED_MIPS;
      break;
     //ToDo: there are a lot more diff!
   }
#endif // __mips__   
   return ret;
}
   
int setsockopt_portable(int s, int level, int optname, const void *optval, socklen_t optlen)
{
#if defined(__mips__)
   switch(level) {
   case SOL_SOCKET_PORTABLE: 
      level = SOL_SOCKET_MIPS;
      break;
     //ToDo: there are a lot more diff!
   }
   
   switch(optname) {
   case SO_BROADCAST_PORTABLE: 
      optname = SO_BROADCAST_MIPS;
      break;
     //ToDo: there are a lot more diff!
   }
#endif   
   return setsockopt(s, level, optname, optval, optlen);
}

#endif // !__GDK__


static int ifc_ctl_sock = -1;

int ifc_init(void)
{
    if (ifc_ctl_sock == -1) {
        ifc_ctl_sock = socket_portable(AF_INET, SOCK_DGRAM_PORTABLE, 0);
        if (ifc_ctl_sock < 0) {
        	Con_Printf("socket() failed: %s\n", strerror(errno_portable()));
        }
    }
    return ifc_ctl_sock < 0 ? -1 : 0;
}

void ifc_close(void)
{
    if (ifc_ctl_sock != -1) {
        (void)close(ifc_ctl_sock);
        ifc_ctl_sock = -1;
    }
}

static void ifc_init_ifr(const char *name, struct ifreq *ifr)
{
    memset(ifr, 0, sizeof(struct ifreq));
    strncpy(ifr->ifr_name, name, IFNAMSIZ);
    ifr->ifr_name[IFNAMSIZ - 1] = 0;
}

int ifc_get_info(const char *name, in_addr_t *addr, in_addr_t *mask, unsigned *flags)
{
    struct ifreq ifr;
    ifc_init_ifr(name, &ifr);

    if (addr != NULL) {
        if(ioctl_portable(ifc_ctl_sock, SIOCGIFADDR, &ifr) < 0) {
            *addr = 0;
        } else {
            *addr = ((struct sockaddr_in*) (void*) &ifr.ifr_addr)->sin_addr.s_addr;
        }
    }

    if (mask != NULL) {
        if(ioctl_portable(ifc_ctl_sock, SIOCGIFNETMASK, &ifr) < 0) {
            *mask = 0;
        } else {
            *mask = ((struct sockaddr_in*) (void*) &ifr.ifr_addr)->sin_addr.s_addr;
        }
    }

    if (flags != NULL) {
        if(ioctl_portable(ifc_ctl_sock, SIOCGIFFLAGS, &ifr) < 0) {
            *flags = 0;
        } else {
            *flags = ifr.ifr_flags;
        }
    }

    return 0;
}

void AndroidGetAddr() {
	if (ifc_init()) {
		return;
	}
	in_addr_t addr;
	ifc_get_info("tiwlan0", &addr, 0, 0);
	myAddr = addr;
	ifc_close();
}


// **** End of Android-specific code ****

//=============================================================================

int UDP_Init (void)
{
	struct hostent *local;
	char	buff[MAXHOSTNAMELEN];
	struct qsockaddr addr;
	char *colon;

	if (COM_CheckParm ("-noudp"))
		return -1;

#if 1 // Android
	AndroidGetAddr();
#else
	// determine my name & address
	gethostname(buff, MAXHOSTNAMELEN);
	local = gethostbyname(buff);

	if(!local)
	{
		Con_Printf("Could not gethostbyname(\"%s\")\n", buff);
		return -1;
	}

	myAddr = *(int *)local->h_addr_list[0];

	// if the quake hostname isn't set, set it to the machine name
	if (Q_strcmp(hostname.string, "UNNAMED") == 0)
	{
		buff[15] = 0;
		Cvar_Set ("hostname", buff);
	}
#endif
	if ((net_controlsocket = UDP_OpenSocket (0)) == -1)
		Sys_Error("UDP_Init: Unable to open control socket\n");

	sockaddr_in temp;

	memcpy(&temp, &broadcastaddr, sizeof(temp));

	temp.sin_family = AF_INET;
	temp.sin_addr.s_addr = INADDR_BROADCAST;
	temp.sin_port = htons(net_hostport);

	memcpy(&broadcastaddr, &temp, sizeof(temp));

	UDP_GetSocketAddr (net_controlsocket, &addr);
	Q_strcpy(my_tcpip_address,  UDP_AddrToString (&addr));
	colon = Q_strrchr (my_tcpip_address, ':');
	if (colon)
		*colon = 0;

	Con_Printf("UDP Initialized\n");
	tcpipAvailable = true;

	return net_controlsocket;
}

//=============================================================================

void UDP_Shutdown (void)
{
	UDP_Listen (false);
	UDP_CloseSocket (net_controlsocket);
}

//=============================================================================

void UDP_Listen (qboolean state)
{
	// enable listening
	if (state)
	{
		if (net_acceptsocket != -1)
			return;
		if ((net_acceptsocket = UDP_OpenSocket (net_hostport)) == -1)
			Sys_Error ("UDP_Listen: Unable to open accept socket\n");
		return;
	}

	// disable listening
	if (net_acceptsocket == -1)
		return;
	UDP_CloseSocket (net_acceptsocket);
	net_acceptsocket = -1;
}

//=============================================================================

int UDP_OpenSocket (int port)
{
	int newsocket;
	union {
	    struct sockaddr_in in;
	    struct sockaddr sockaddr;
	} address;
	qboolean _true = true;

	if ((newsocket = socket_portable(PF_INET, SOCK_DGRAM_PORTABLE, IPPROTO_UDP)) == -1)
		return -1;

	if (ioctl_portable(newsocket, FIONBIO_PORTABLE, (char *)&_true) == -1)
		goto ErrorReturn;

	address.in.sin_family = AF_INET;
	address.in.sin_addr.s_addr = INADDR_ANY;
	address.in.sin_port = htons(port);
	if( bind (newsocket, &address.sockaddr, sizeof(address.in)) == -1)
		goto ErrorReturn;

	return newsocket;

ErrorReturn:
	close (newsocket);
	return -1;
}

//=============================================================================

int UDP_CloseSocket (int socket)
{
	if (socket == net_broadcastsocket)
		net_broadcastsocket = 0;
	return close (socket);
}


//=============================================================================
/*
============
PartialIPAddress

this lets you type only as much of the net address as required, using
the local network components to fill in the rest
============
*/
static int PartialIPAddress (const char *in, struct qsockaddr *hostaddr)
{
	char buff[256];
	char *b;
	int addr;
	int num;
	int mask;
	int run;
	int port;

	buff[0] = '.';
	b = buff;
	strcpy(buff+1, in);
	if (buff[1] == '.')
		b++;

	addr = 0;
	mask=-1;
	while (*b == '.')
	{
		b++;
		num = 0;
		run = 0;
		while (!( *b < '0' || *b > '9'))
		{
		  num = num*10 + *b++ - '0';
		  if (++run > 3)
		  	return -1;
		}
		if ((*b < '0' || *b > '9') && *b != '.' && *b != ':' && *b != 0)
			return -1;
		if (num < 0 || num > 255)
			return -1;
		mask<<=8;
		addr = (addr<<8) + num;
	}

	if (*b++ == ':')
		port = Q_atoi(b);
	else
		port = net_hostport;

	hostaddr->sa_family = AF_INET;
	((struct sockaddr_in *)hostaddr)->sin_port = htons((short)port);
	((struct sockaddr_in *)hostaddr)->sin_addr.s_addr = (myAddr & htonl(mask)) | htonl(addr);

	return 0;
}
//=============================================================================

int UDP_Connect (int socket, struct qsockaddr *addr)
{
	return 0;
}

//=============================================================================

int UDP_CheckNewConnections (void)
{
	unsigned long	available;

	if (net_acceptsocket == -1)
		return -1;

	if (ioctl_portable(net_acceptsocket, FIONREAD_PORTABLE, &available) == -1)
		Sys_Error ("UDP: ioctlsocket (FIONREAD) failed\n");
	if (available)
		return net_acceptsocket;
	return -1;
}

//=============================================================================

int UDP_Read (int socket, byte *buf, int len, struct qsockaddr *addr)
{
	int addrlen = sizeof (struct qsockaddr);
	int ret;

	ret = recvfrom (socket, buf, len, 0, (struct sockaddr *)addr, (socklen_t*) &addrlen);
	if (ret == -1 && (errno_portable() == EWOULDBLOCK || errno_portable() == ECONNREFUSED_PORTABLE))
		return 0;
	return ret;
}

//=============================================================================

int UDP_MakeSocketBroadcastCapable (int socket)
{
	int				i = 1;

	// make this socket broadcast capable
	if (setsockopt_portable(socket, SOL_SOCKET_PORTABLE, SO_BROADCAST_PORTABLE, (char *)&i, sizeof(i)) < 0)
		return -1;
	net_broadcastsocket = socket;

	return 0;
}

//=============================================================================

int UDP_Broadcast (int socket, byte *buf, int len)
{
	int ret;

	if (socket != net_broadcastsocket)
	{
		if (net_broadcastsocket != 0)
			Sys_Error("Attempted to use multiple broadcasts sockets\n");
		ret = UDP_MakeSocketBroadcastCapable (socket);
		if (ret == -1)
		{
			Con_Printf("Unable to make socket broadcast capable\n");
			return ret;
		}
	}

	return UDP_Write (socket, buf, len, &broadcastaddr);
}

//=============================================================================

int UDP_Write (int socket, byte *buf, int len, struct qsockaddr *addr)
{
	int ret;

	ret = sendto (socket, buf, len, 0, (struct sockaddr *)addr, sizeof(struct qsockaddr));
	if (ret == -1 && errno_portable() == EWOULDBLOCK)
		return 0;
	return ret;
}

//=============================================================================

char *UDP_AddrToString (struct qsockaddr *addr)
{
	static char buffer[22];
	int haddr;

	haddr = ntohl(((struct sockaddr_in *)addr)->sin_addr.s_addr);
	sprintf(buffer, "%d.%d.%d.%d:%d", (haddr >> 24) & 0xff, (haddr >> 16) & 0xff, (haddr >> 8) & 0xff, haddr & 0xff, ntohs(((struct sockaddr_in *)addr)->sin_port));
	return buffer;
}

//=============================================================================

int UDP_StringToAddr (const char *string, struct qsockaddr *addr)
{
	int ha1, ha2, ha3, ha4, hp;
	int ipaddr;

	sscanf(string, "%d.%d.%d.%d:%d", &ha1, &ha2, &ha3, &ha4, &hp);
	ipaddr = (ha1 << 24) | (ha2 << 16) | (ha3 << 8) | ha4;

	addr->sa_family = AF_INET;
	((struct sockaddr_in *)addr)->sin_addr.s_addr = htonl(ipaddr);
	((struct sockaddr_in *)addr)->sin_port = htons(hp);
	return 0;
}

//=============================================================================

int UDP_GetSocketAddr (int socket, struct qsockaddr *addr)
{
	int addrlen = sizeof(struct qsockaddr);
	unsigned int a;

	Q_memset(addr, 0, sizeof(struct qsockaddr));
	getsockname(socket, (struct sockaddr *)addr, (socklen_t*) &addrlen);
	a = ((struct sockaddr_in *)addr)->sin_addr.s_addr;
	if (a == 0 || (in_addr_t) a == inet_addr("127.0.0.1"))
		((struct sockaddr_in *)addr)->sin_addr.s_addr = myAddr;

	return 0;
}

//=============================================================================

int UDP_GetNameFromAddr (struct qsockaddr *addr, char *name)
{
	struct hostent *hostentry;

	hostentry = gethostbyaddr ((char *)&((struct sockaddr_in *)addr)->sin_addr, sizeof(struct in_addr), AF_INET);
	if (hostentry)
	{
		Q_strncpy (name, (char *)hostentry->h_name, NET_NAMELEN - 1);
		return 0;
	}

	Q_strcpy (name, UDP_AddrToString (addr));
	return 0;
}

//=============================================================================

int UDP_GetAddrFromName(const char *name, struct qsockaddr *addr)
{
	struct hostent *hostentry;

	if (name[0] >= '0' && name[0] <= '9')
		return PartialIPAddress (name, addr);

	hostentry = gethostbyname (name);
	if (!hostentry)
		return -1;

	addr->sa_family = AF_INET;
	((struct sockaddr_in *)addr)->sin_port = htons(net_hostport);
	((struct sockaddr_in *)addr)->sin_addr.s_addr = *(int *)hostentry->h_addr_list[0];

	return 0;
}

//=============================================================================

int UDP_AddrCompare (struct qsockaddr *addr1, struct qsockaddr *addr2)
{
	if (addr1->sa_family != addr2->sa_family)
		return -1;

	if (((struct sockaddr_in *)addr1)->sin_addr.s_addr != ((struct sockaddr_in *)addr2)->sin_addr.s_addr)
		return -1;

	if (((struct sockaddr_in *)addr1)->sin_port != ((struct sockaddr_in *)addr2)->sin_port)
		return 1;

	return 0;
}

//=============================================================================

int UDP_GetSocketPort (struct qsockaddr *addr)
{
	return ntohs(((struct sockaddr_in *)addr)->sin_port);
}


int UDP_SetSocketPort (struct qsockaddr *addr, int port)
{
	((struct sockaddr_in *)addr)->sin_port = htons(port);
	return 0;
}

//=============================================================================
