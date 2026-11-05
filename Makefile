CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11
INSTALL ?= install

# Exploit mitigations. For a compiler without these flags: make HARDEN=
HARDEN  ?= -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fPIE
ifneq ($(strip $(HARDEN)),)
HARDEN_LD ?= -pie -Wl,-z,relro,-z,now
endif

PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
LOCALBIN = $(HOME)/.local/bin

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)

molly: $(OBJ)
	$(CC) $(CFLAGS) $(HARDEN) $(OBJ) -o molly $(HARDEN_LD)

src/%.o: src/%.c src/molly.h
	$(CC) $(CFLAGS) $(HARDEN) -c $< -o $@

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

# Objects without main(), for linking a test that provides its own.
LIBOBJ = $(filter-out src/main.o,$(OBJ))

test: molly test-units tests/regex_cli
	python3 tests/test_molly.py ./molly
	python3 tests/test_golden.py ./molly
	python3 tests/shared_files.py ./molly
	python3 tests/fuzz_replace.py ./molly
	python3 tests/fuzz_regex.py ./tests/regex_cli

# Unit tests for the parts that need no terminal: the event loop (the editor
# registers one source, so only a direct test covers waiting on several) and the
# theme's color handling.
test-units: $(LIBOBJ) tests/test_units.c src/molly.h
	$(CC) $(CFLAGS) $(HARDEN) -Isrc tests/test_units.c $(LIBOBJ) -o tests/units_test $(HARDEN_LD)
	./tests/units_test

# Rewrite the golden screens from current behavior. Only when an output change
# is intended -- read the diff it produces before committing it.
golden: molly
	MOLLY_UPDATE_GOLDEN=1 python3 tests/test_golden.py ./molly

# The driver tests/fuzz_regex.py drives: molly's regex engine on one side of a
# differential test, with Python's re on the other.
tests/regex_cli: $(LIBOBJ) tests/regex_cli.c src/molly.h
	$(CC) $(CFLAGS) $(HARDEN) -Isrc tests/regex_cli.c $(LIBOBJ) -o tests/regex_cli $(HARDEN_LD)

test-oom: molly
	python3 tests/oom_sweep.py ./molly

# Linux only: confirms the binary really has the mitigations in HARDEN.
check-hardening: molly
	readelf -hW molly | grep -q 'Type:.*DYN'
	readelf -lW molly | grep -q GNU_RELRO
	readelf -dW molly | grep -q BIND_NOW
	readelf -sW --dyn-syms molly | grep -q __stack_chk_fail
	readelf -sW --dyn-syms molly | grep -q '_chk'

# The unit checks and the pty checks of what a file, a paste or another user can
# reach, against an AddressSanitizer and UBSan build, failing on any report. Built
# in one go so its objects stay apart from the ordinary ones.
# Needs a compiler with sanitizer runtimes: make test-sanitize CC=clang
SAN = -g -O1 -std=c11 -fno-omit-frame-pointer -fsanitize=address,undefined \
      -fno-sanitize-recover=undefined
test-sanitize:
	$(CC) $(SAN) $(SRC) -o molly_asan
	$(CC) $(SAN) -Isrc tests/test_units.c $(filter-out src/main.c,$(SRC)) -o tests/units_asan
	sh tests/sanitize.sh ./molly_asan ./tests/units_asan

clean:
	rm -f molly molly_asan editor $(OBJ) tests/units_test tests/units_asan tests/regex_cli

.PHONY: clean install uninstall test test-oom test-units test-sanitize check-hardening golden