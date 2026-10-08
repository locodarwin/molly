CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11
INSTALL ?= install

PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
LOCALBIN = $(HOME)/.local/bin

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)

molly: $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o molly

src/%.o: src/%.c src/molly.h
	$(CC) $(CFLAGS) -c $< -o $@

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
	python3 tests/fuzz_replace.py ./molly
	python3 tests/fuzz_regex.py ./tests/regex_cli

# Unit tests for the parts that need no terminal: the event loop (the editor
# registers one source, so only a direct test covers waiting on several) and the
# theme's color handling.
test-units: $(LIBOBJ) tests/test_units.c src/molly.h
	$(CC) $(CFLAGS) -Isrc tests/test_units.c $(LIBOBJ) -o tests/units_test
	./tests/units_test

# Rewrite the golden screens from current behavior. Only when an output change
# is intended -- read the diff it produces before committing it.
golden: molly
	MOLLY_UPDATE_GOLDEN=1 python3 tests/test_golden.py ./molly

# The driver tests/fuzz_regex.py drives: molly's regex engine on one side of a
# differential test, with Python's re on the other.
tests/regex_cli: $(LIBOBJ) tests/regex_cli.c src/molly.h
	$(CC) $(CFLAGS) -Isrc tests/regex_cli.c $(LIBOBJ) -o tests/regex_cli

test-oom: molly
	python3 tests/oom_sweep.py ./molly

clean:
	rm -f molly editor $(OBJ) tests/units_test tests/regex_cli

.PHONY: clean install uninstall test test-oom test-units golden