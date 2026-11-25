// SPDX-License-Identifier: AGPL-3.0-only
//
// LAN-only stand-ins for chiaki-ng's internet "holepunch" module.
//
// DuoPlay's bridge always sits on the same network as the console, so the PSN
// holepunch path (and its curl / json-c / libevent / miniupnpc dependencies) is
// never used. chiaki only calls into it when connect_info.holepunch_session is
// non-NULL, which the bridge never sets; these stubs just satisfy the linker and
// fail loudly if that assumption is ever broken.

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <chiaki/remote/holepunch.h>
#include <string.h>

// Certificate-chain recovery for PSN HTTPS (aia.c) is internet-only as well.
ChiakiErrorCode chiaki_aia_init(void) { return CHIAKI_ERR_SUCCESS; }
void chiaki_aia_fini(void) {}

CHIAKI_EXPORT ChiakiHolepunchRegistInfo chiaki_get_regist_info(ChiakiHolepunchSession session)
{
	(void)session;
	ChiakiHolepunchRegistInfo info;
	memset(&info, 0, sizeof(info));
	return info;
}

CHIAKI_EXPORT chiaki_socket_t *chiaki_get_holepunch_sock(ChiakiHolepunchSession session, ChiakiHolepunchPortType type)
{
	(void)session; (void)type;
	return NULL;
}

CHIAKI_EXPORT ChiakiErrorCode holepunch_session_create_offer(ChiakiHolepunchSession session)
{
	(void)session;
	return CHIAKI_ERR_INVALID_DATA;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_session_punch_hole(ChiakiHolepunchSession session, ChiakiHolepunchPortType port_type)
{
	(void)session; (void)port_type;
	return CHIAKI_ERR_INVALID_DATA;
}

CHIAKI_EXPORT void chiaki_holepunch_session_fini(ChiakiHolepunchSession session)
{
	(void)session;
}

CHIAKI_EXPORT void chiaki_get_ps_selected_addr(ChiakiHolepunchSession session, char *ps_ip)
{
	(void)session;
	if(ps_ip)
		ps_ip[0] = '\0';
}

CHIAKI_EXPORT uint16_t chiaki_get_ps_ctrl_port(ChiakiHolepunchSession session)
{
	(void)session;
	return 0;
}
