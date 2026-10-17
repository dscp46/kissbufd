# kissbufd - portable Makefile (GNU make and BSD make)
# SPDX-License-Identifier: MIT

PROG	= kissbufd
VERSION	= 0.1.0

CC	?= cc
CFLAGS	?= -O2 -g
PREFIX	?= /usr/local
SBINDIR	?= $(PREFIX)/sbin
MANDIR	?= $(PREFIX)/share/man
DOCDIR	?= $(PREFIX)/share/doc/kissbufd
SYSCONFDIR ?= $(PREFIX)/etc
RUNDIR	?= /var/run
SYSTEMDUNITDIR ?= $(PREFIX)/lib/systemd/system
RCDIR	?=
DESTDIR	?=
INSTALL	?= install
CLANG_FORMAT ?= clang-format

KB_CFLAGS = -std=c99 -Wall -Wextra -Wshadow -Wstrict-prototypes \
	    -Wmissing-prototypes -DKB_VERSION=\"$(VERSION)\"

# Substitutes install paths into the config sample and service files.
SUBST	= sed -e 's|@SBINDIR@|$(SBINDIR)|g' \
	      -e 's|@SYSCONFDIR@|$(SYSCONFDIR)|g' \
	      -e 's|@RUNDIR@|$(RUNDIR)|g' \
	      -e 's|@DOCDIR@|$(DOCDIR)|g' \
	      -e 's|@VERSION@|$(VERSION)|g'

OBJS	= src/kissbufd.o src/config.o src/kiss.o src/tracker.o src/txq.o \
	  src/serial.o
TOBJS	= src/config.o src/kiss.o src/tracker.o src/txq.o

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KB_CFLAGS) -c -o $@ $<

src/kissbufd.o: src/kissbufd.c src/compat.h src/config.h src/kiss.h \
	src/serial.h src/tracker.h src/txq.h
src/config.o: src/config.c src/compat.h src/config.h
src/kiss.o: src/kiss.c src/compat.h src/kiss.h
src/tracker.o: src/tracker.c src/compat.h src/kiss.h src/tracker.h
src/txq.o: src/txq.c src/compat.h src/txq.h
src/serial.o: src/serial.c src/compat.h src/serial.h

tests/test_units: tests/test_units.o $(TOBJS)
	$(CC) $(LDFLAGS) -o $@ tests/test_units.o $(TOBJS)

tests/test_units.o: tests/test_units.c src/compat.h src/config.h \
	src/kiss.h src/tracker.h src/txq.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KB_CFLAGS) -Isrc -c -o $@ tests/test_units.c

# Unit tests (no external dependencies; run by dh_auto_test).
check: tests/test_units
	./tests/test_units

tests/sim_tnc: tests/sim_tnc.c src/compat.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KB_CFLAGS) -Isrc $(LDFLAGS) -o $@ tests/sim_tnc.c

# End-to-end test against a simulated ACKMODE TNC on a pty.
integration: $(PROG) tests/sim_tnc
	./tests/sim_tnc ./$(PROG)

tools/cstyle: tools/cstyle.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KB_CFLAGS) $(LDFLAGS) -o $@ tools/cstyle.c

# Reformat the C sources (needs clang-format).
STYLE_SRCS = src/*.c src/*.h tests/*.c tools/*.c
style: tools/cstyle
	@for f in $(STYLE_SRCS); do \
		./tools/cstyle -1 < $$f | $(CLANG_FORMAT) --style=file --assume-filename=$$f | ./tools/cstyle -2 > $$f.style || exit 1; \
		if cmp -s $$f $$f.style; then rm -f $$f.style; else mv $$f.style $$f; echo "restyled $$f"; fi; \
	done

# Binary, man pages, documentation and the sample configuration.
install: $(PROG)
	$(INSTALL) -d $(DESTDIR)$(SBINDIR) $(DESTDIR)$(MANDIR)/man8 \
	    $(DESTDIR)$(MANDIR)/man5 $(DESTDIR)$(DOCDIR)
	$(INSTALL) -m 0755 $(PROG) $(DESTDIR)$(SBINDIR)/$(PROG)
	$(SUBST) man/kissbufd.8 > $(DESTDIR)$(MANDIR)/man8/kissbufd.8
	$(SUBST) man/kissbufd.conf.5 > $(DESTDIR)$(MANDIR)/man5/kissbufd.conf.5
	$(SUBST) etc/kissbufd.conf.sample.in > \
	    $(DESTDIR)$(DOCDIR)/kissbufd.conf.sample
	$(INSTALL) -m 0644 README.md LICENSE $(DESTDIR)$(DOCDIR)/
	chmod 0644 $(DESTDIR)$(MANDIR)/man8/kissbufd.8 \
	    $(DESTDIR)$(MANDIR)/man5/kissbufd.conf.5 \
	    $(DESTDIR)$(DOCDIR)/kissbufd.conf.sample

# systemd template unit (Linux).
install-systemd:
	$(INSTALL) -d $(DESTDIR)$(SYSTEMDUNITDIR)
	$(SUBST) services/kissbufd@.service.in > \
	    $(DESTDIR)$(SYSTEMDUNITDIR)/kissbufd@.service
	chmod 0644 $(DESTDIR)$(SYSTEMDUNITDIR)/kissbufd@.service

# The service file that fits the running system. RCDIR overrides the
# destination of BSD rc.d scripts.
install-service:
	@os=`uname -s`; rcdir='$(RCDIR)'; \
	case "$$os" in \
	Linux) \
		$(MAKE) install-systemd ;; \
	FreeBSD|DragonFly) \
		: $${rcdir:=$(PREFIX)/etc/rc.d}; \
		$(INSTALL) -d $(DESTDIR)$$rcdir; \
		$(SUBST) services/freebsd-rc.in > $(DESTDIR)$$rcdir/kissbufd; \
		chmod 0555 $(DESTDIR)$$rcdir/kissbufd; \
		echo "installed $(DESTDIR)$$rcdir/kissbufd" ;; \
	NetBSD) \
		: $${rcdir:=/etc/rc.d}; \
		$(INSTALL) -d $(DESTDIR)$$rcdir; \
		$(SUBST) services/netbsd-rc.in > $(DESTDIR)$$rcdir/kissbufd; \
		chmod 0555 $(DESTDIR)$$rcdir/kissbufd; \
		echo "installed $(DESTDIR)$$rcdir/kissbufd" ;; \
	OpenBSD) \
		: $${rcdir:=/etc/rc.d}; \
		$(INSTALL) -d $(DESTDIR)$$rcdir; \
		$(SUBST) services/openbsd-rc.in > $(DESTDIR)$$rcdir/kissbufd; \
		chmod 0555 $(DESTDIR)$$rcdir/kissbufd; \
		echo "installed $(DESTDIR)$$rcdir/kissbufd" ;; \
	Darwin) \
		$(INSTALL) -d $(DESTDIR)/Library/LaunchDaemons; \
		$(SUBST) services/kissbufd.plist.in > \
		    $(DESTDIR)/Library/LaunchDaemons/kissbufd.plist; \
		chmod 0644 $(DESTDIR)/Library/LaunchDaemons/kissbufd.plist; \
		echo "installed $(DESTDIR)/Library/LaunchDaemons/kissbufd.plist" ;; \
	*) \
		echo "no service file for $$os; see services/"; exit 1 ;; \
	esac

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/$(PROG) \
	    $(DESTDIR)$(MANDIR)/man8/kissbufd.8 \
	    $(DESTDIR)$(MANDIR)/man5/kissbufd.conf.5 \
	    $(DESTDIR)$(DOCDIR)/kissbufd.conf.sample \
	    $(DESTDIR)$(DOCDIR)/README.md $(DESTDIR)$(DOCDIR)/LICENSE \
	    $(DESTDIR)$(SYSTEMDUNITDIR)/kissbufd@.service

clean:
	rm -f $(PROG) $(OBJS) tests/test_units tests/test_units.o tests/sim_tnc \
	    tools/cstyle

.PHONY: all check integration style install install-systemd install-service \
	uninstall clean
