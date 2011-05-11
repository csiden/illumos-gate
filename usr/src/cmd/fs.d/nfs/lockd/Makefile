#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy is of the CDDL is also available via the Internet
# at http://www.illumos.org/license/CDDL.
#

#
# Copyright 2010 Nexenta Systems, Inc.  All rights reserved.
#

# cmd/fs.d/nfs/lockd/Makefile

FSTYPE=		nfs
TYPEPROG=	lockd
ATTMK=		$(TYPEPROG)

include		../../Makefile.fstype

LOCAL=		lockd.o
LIBOBJS=	nfs_tbind.o thrpool.o daemon.o smfcfg.o
OBJS=		$(LOCAL) $(LIBOBJS)

SRCS=		$(LOCAL:%.o=%.c) $(LIBOBJS:%.o=../lib/%.c)
CPPFLAGS +=	-I../lib
C99MODE= $(C99_ENABLE)
LDLIBS +=	-lnsl -lscf

#
# Message catalog
#
POFILE= lockd.po

$(TYPEPROG):	$(OBJS)
		$(LINK.c) -o $@ $(OBJS) $(LDLIBS)
		$(POST_PROCESS)

%.o:	../lib/%.c
	$(COMPILE.c) -o $@ $<

#
# message catalog
#
catalog: $(POFILE)

$(POFILE): $(SRCS)
	$(RM) $@
	$(COMPILE.cpp) $(SRCS) > $(POFILE).i
	$(XGETTEXT) $(XGETFLAGS) $(POFILE).i
	sed "/^domain/d" messages.po > $@
	$(RM) $(POFILE).i messages.po

lint:		$(SRCS) lint_SRCS

clean:
		$(RM) $(OBJS)
