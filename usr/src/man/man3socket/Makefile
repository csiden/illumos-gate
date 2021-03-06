#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet
# at http://www.illumos.org/license/CDDL.
#

# Copyright 2011, Richard Lowe

include ../../Makefile.master

MANSECT = 	3socket

MANFILES = 	accept.3socket		\
		bind.3socket		\
		byteorder.3socket	\
		connect.3socket		\
		ethers.3socket		\
		getaddrinfo.3socket	\
		getipnodebyname.3socket \
		getnetbyname.3socket	\
		getpeername.3socket	\
		getprotobyname.3socket	\
		getservbyname.3socket	\
		getsockname.3socket	\
		getsockopt.3socket	\
		getsourcefilter.3socket \
		icmp6_filter.3socket	\
		if_nametoindex.3socket	\
		inet.3socket		\
		inet6_opt.3socket	\
		inet6_rth.3socket	\
		listen.3socket		\
		rcmd.3socket		\
		recv.3socket		\
		rexec.3socket		\
		sctp_bindx.3socket	\
		sctp_getladdrs.3socket	\
		sctp_getpaddrs.3socket	\
		sctp_opt_info.3socket	\
		sctp_peeloff.3socket	\
		sctp_recvmsg.3socket	\
		sctp_send.3socket	\
		sctp_sendmsg.3socket	\
		send.3socket		\
		shutdown.3socket	\
		socket.3socket		\
		socketpair.3socket	\
		spray.3socket

MANSOFILES =	endnetent.3socket		\
		endprotoent.3socket		\
		endservent.3socket		\
		ether_aton.3socket		\
		ether_hostton.3socket		\
		ether_line.3socket		\
		ether_ntoa.3socket		\
		ether_ntohost.3socket		\
		freeaddrinfo.3socket		\
		freehostent.3socket		\
		gai_strerror.3socket		\
		getipnodebyaddr.3socket		\
		getipv4sourcefilter.3socket	\
		getnameinfo.3socket		\
		getnetbyaddr.3socket		\
		getnetbyaddr_r.3socket		\
		getnetbyname_r.3socket		\
		getnetent.3socket		\
		getnetent_r.3socket		\
		getprotobyname_r.3socket	\
		getprotobynumber.3socket	\
		getprotobynumber_r.3socket	\
		getprotoent.3socket		\
		getprotoent_r.3socket		\
		getservbyname_r.3socket		\
		getservbyport.3socket		\
		getservbyport_r.3socket		\
		getservent.3socket		\
		getservent_r.3socket		\
		htonl.3socket			\
		htonll.3socket			\
		htons.3socket			\
		if_freenameindex.3socket	\
		if_indextoname.3socket		\
		if_nameindex.3socket		\
		inet6.3socket			\
		inet6_opt_append.3socket	\
		inet6_opt_find.3socket		\
		inet6_opt_finish.3socket	\
		inet6_opt_get_val.3socket	\
		inet6_opt_init.3socket		\
		inet6_opt_next.3socket		\
		inet6_opt_set_val.3socket	\
		inet6_rth_add.3socket		\
		inet6_rth_getaddr.3socket	\
		inet6_rth_init.3socket		\
		inet6_rth_reverse.3socket	\
		inet6_rth_segments.3socket	\
		inet6_rth_space.3socket		\
		inet_addr.3socket		\
		inet_aton.3socket		\
		inet_lnaof.3socket		\
		inet_makeaddr.3socket		\
		inet_netof.3socket		\
		inet_network.3socket		\
		inet_ntoa.3socket		\
		inet_ntop.3socket		\
		inet_pton.3socket		\
		ntohl.3socket			\
		ntohll.3socket			\
		ntohs.3socket			\
		rcmd_af.3socket			\
		recvfrom.3socket		\
		recvmsg.3socket			\
		rexec_af.3socket		\
		rresvport.3socket		\
		rresvport_af.3socket		\
		ruserok.3socket			\
		sctp_freeladdrs.3socket		\
		sctp_freepaddrs.3socket		\
		sendmsg.3socket			\
		sendto.3socket			\
		setipv4sourcefilter.3socket	\
		setnetent.3socket		\
		setprotoent.3socket		\
		setservent.3socket		\
		setsockopt.3socket		\
		setsourcefilter.3socket

MANFILES +=	$(MANSOFILES)

htonl.3socket			:= SOSRC = man3socket/byteorder.3socket
htonll.3socket			:= SOSRC = man3socket/byteorder.3socket
htons.3socket			:= SOSRC = man3socket/byteorder.3socket
ntohl.3socket			:= SOSRC = man3socket/byteorder.3socket
ntohll.3socket			:= SOSRC = man3socket/byteorder.3socket
ntohs.3socket			:= SOSRC = man3socket/byteorder.3socket

ether_aton.3socket		:= SOSRC = man3socket/ethers.3socket
ether_hostton.3socket		:= SOSRC = man3socket/ethers.3socket
ether_line.3socket		:= SOSRC = man3socket/ethers.3socket
ether_ntoa.3socket		:= SOSRC = man3socket/ethers.3socket
ether_ntohost.3socket		:= SOSRC = man3socket/ethers.3socket

freeaddrinfo.3socket		:= SOSRC = man3socket/getaddrinfo.3socket
gai_strerror.3socket		:= SOSRC = man3socket/getaddrinfo.3socket
getnameinfo.3socket		:= SOSRC = man3socket/getaddrinfo.3socket

freehostent.3socket		:= SOSRC = man3socket/getipnodebyname.3socket
getipnodebyaddr.3socket		:= SOSRC = man3socket/getipnodebyname.3socket

endnetent.3socket		:= SOSRC = man3socket/getnetbyname.3socket
getnetbyaddr.3socket		:= SOSRC = man3socket/getnetbyname.3socket
getnetbyaddr_r.3socket		:= SOSRC = man3socket/getnetbyname.3socket
getnetbyname_r.3socket		:= SOSRC = man3socket/getnetbyname.3socket
getnetent.3socket		:= SOSRC = man3socket/getnetbyname.3socket
getnetent_r.3socket		:= SOSRC = man3socket/getnetbyname.3socket
setnetent.3socket		:= SOSRC = man3socket/getnetbyname.3socket

endprotoent.3socket		:= SOSRC = man3socket/getprotobyname.3socket
getprotobyname_r.3socket	:= SOSRC = man3socket/getprotobyname.3socket
getprotobynumber.3socket	:= SOSRC = man3socket/getprotobyname.3socket
getprotobynumber_r.3socket	:= SOSRC = man3socket/getprotobyname.3socket
getprotoent.3socket		:= SOSRC = man3socket/getprotobyname.3socket
getprotoent_r.3socket		:= SOSRC = man3socket/getprotobyname.3socket
setprotoent.3socket		:= SOSRC = man3socket/getprotobyname.3socket

endservent.3socket		:= SOSRC = man3socket/getservbyname.3socket
getservbyname_r.3socket		:= SOSRC = man3socket/getservbyname.3socket
getservbyport.3socket		:= SOSRC = man3socket/getservbyname.3socket
getservbyport_r.3socket		:= SOSRC = man3socket/getservbyname.3socket
getservent.3socket		:= SOSRC = man3socket/getservbyname.3socket
getservent_r.3socket		:= SOSRC = man3socket/getservbyname.3socket
setservent.3socket		:= SOSRC = man3socket/getservbyname.3socket

setsockopt.3socket		:= SOSRC = man3socket/getsockopt.3socket

getipv4sourcefilter.3socket	:= SOSRC = man3socket/getsourcefilter.3socket
setipv4sourcefilter.3socket	:= SOSRC = man3socket/getsourcefilter.3socket
setsourcefilter.3socket		:= SOSRC = man3socket/getsourcefilter.3socket

if_freenameindex.3socket	:= SOSRC = man3socket/if_nametoindex.3socket
if_indextoname.3socket		:= SOSRC = man3socket/if_nametoindex.3socket
if_nameindex.3socket		:= SOSRC = man3socket/if_nametoindex.3socket

inet6.3socket			:= SOSRC = man3socket/inet.3socket
inet_addr.3socket		:= SOSRC = man3socket/inet.3socket
inet_aton.3socket		:= SOSRC = man3socket/inet.3socket
inet_lnaof.3socket		:= SOSRC = man3socket/inet.3socket
inet_makeaddr.3socket		:= SOSRC = man3socket/inet.3socket
inet_netof.3socket		:= SOSRC = man3socket/inet.3socket
inet_network.3socket		:= SOSRC = man3socket/inet.3socket
inet_ntoa.3socket		:= SOSRC = man3socket/inet.3socket
inet_ntop.3socket		:= SOSRC = man3socket/inet.3socket
inet_pton.3socket		:= SOSRC = man3socket/inet.3socket

inet6_opt_append.3socket	:= SOSRC = man3socket/inet6_opt.3socket
inet6_opt_find.3socket		:= SOSRC = man3socket/inet6_opt.3socket
inet6_opt_finish.3socket	:= SOSRC = man3socket/inet6_opt.3socket
inet6_opt_get_val.3socket	:= SOSRC = man3socket/inet6_opt.3socket
inet6_opt_init.3socket		:= SOSRC = man3socket/inet6_opt.3socket
inet6_opt_next.3socket		:= SOSRC = man3socket/inet6_opt.3socket
inet6_opt_set_val.3socket	:= SOSRC = man3socket/inet6_opt.3socket

inet6_rth_add.3socket		:= SOSRC = man3socket/inet6_rth.3socket
inet6_rth_getaddr.3socket	:= SOSRC = man3socket/inet6_rth.3socket
inet6_rth_init.3socket		:= SOSRC = man3socket/inet6_rth.3socket
inet6_rth_reverse.3socket	:= SOSRC = man3socket/inet6_rth.3socket
inet6_rth_segments.3socket	:= SOSRC = man3socket/inet6_rth.3socket
inet6_rth_space.3socket		:= SOSRC = man3socket/inet6_rth.3socket

rcmd_af.3socket			:= SOSRC = man3socket/rcmd.3socket
rresvport.3socket		:= SOSRC = man3socket/rcmd.3socket
rresvport_af.3socket		:= SOSRC = man3socket/rcmd.3socket
ruserok.3socket			:= SOSRC = man3socket/rcmd.3socket

recvfrom.3socket		:= SOSRC = man3socket/recv.3socket
recvmsg.3socket			:= SOSRC = man3socket/recv.3socket

rexec_af.3socket		:= SOSRC = man3socket/rexec.3socket

sctp_freeladdrs.3socket		:= SOSRC = man3socket/sctp_getladdrs.3socket

sctp_freepaddrs.3socket		:= SOSRC = man3socket/sctp_getpaddrs.3socket

sendmsg.3socket			:= SOSRC = man3socket/send.3socket
sendto.3socket			:= SOSRC = man3socket/send.3socket

.KEEP_STATE:

include ../Makefile.man

install: $(ROOTMANFILES)
