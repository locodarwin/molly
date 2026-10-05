CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11
INSTALL ?= install

PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
LOCALBIN = $(HOME)/.local/bin

molly: molly.c
	$(CC) $(CFLAGS) molly.c -o molly

install: molly
	@if [ -n "$(DESTDIR)" ]; then \
		$(INSTALL) -d $(DESTDIR)$(BINDIR); \
		$(INSTALL) -m 0755 molly $(DESTDIR)$(BINDIR)/molly; \
		echo "Installed $(DESTDIR)$(BINDIR)/molly"; \
	elif mkdir -p "$(BINDIR)" 2>/dev/null && [ -w "$(BINDIR)" ]; then \
		$(INSTALL) -m 0755 molly "$(BINDIR)/molly"; \
		echo "Installed $(BINDIR)/molly"; \
	else \
		echo "*** $(BINDIR) is not writable; falling back to"; \
		echo "***   $(LOCALBIN)"; \
		mkdir -p "$(LOCALBIN)"; \
		$(INSTALL) -m 0755 molly "$(LOCALBIN)/molly"; \
		echo "Installed $(LOCALBIN)/molly"; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/molly $(LOCALBIN)/molly

test: molly
	python3 tests/test_molly.py ./molly
	python3 tests/fuzz_replace.py ./molly

test-oom: molly
	python3 tests/oom_sweep.py ./molly

clean:
	rm -f molly editor

.PHONY: clean install uninstall test test-oom