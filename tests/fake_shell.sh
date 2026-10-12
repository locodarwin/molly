#!/bin/sh
# fake_shell.sh -- a "shell" whose screen is the same everywhere.
#
# The integrated terminal's golden screens cannot run a real shell: dash and
# bash draw their prompts and echo their input differently, and a snapshot of
# either would be a snapshot of the machine. This prints a fixed screen of what
# the emulator has to get right -- bold, reverse, the sixteen and the 256
# colours, a wide character, DEC line drawing -- and then waits, reading and
# discarding, so the terminal stays open while it is photographed.
printf '\033[1;32mgreen bold\033[m plain \033[7mreverse\033[m\n'
printf '\033[38;5;208morange\033[m \033[44;37mwhite on blue\033[m\n'
printf 'wide \344\270\255\346\226\207 box \033(0lqqk\033(B\n'
printf '$ '
exec cat >/dev/null
