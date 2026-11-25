#!/usr/bin/env bash
# Repository policy checks. See CONTRIBUTING.md.
#
#   policy-check.sh text  [label]     check text on stdin for restricted title names
#   policy-check.sh msg   <file>      check a commit message file
#   policy-check.sh files <path>...   check paths for restricted names / game content
#
# The restricted pattern is assembled from fragments so that this file does not
# itself contain the names it rejects.
set -u

a='g'; b='t'; c='a'
sep='[^[:alnum:]]*'
abbr="${a}${sep}${b}${sep}${c}"
word1="${a}rand"; word2="${b}hef${b}"; word3="au${b}o"
num="(6|vi|six)"

# 1. abbreviation as its own token, optionally followed by a number/numeral
# 2. abbreviation glued to a number/numeral or common suffix
# 3. the spelled-out name, in any spacing
NAME_RE="(^|[^[:alnum:]])${abbr}(${sep}${num})?([^[:alnum:]]|\$)"
NAME_RE="${NAME_RE}|${a}${b}${c}${sep}(${num}|[0-9]|online|dump)"
NAME_RE="${NAME_RE}|${word1}${sep}${word2}|${word2}${sep}${word3}"

# Game / console content that must never be committed.
CONTENT_RE='\.(rpf|pkg|pfs|self|sprx|prx|elf|sfo|trp|ucp|pup|sdat|edat|rif|rap)$'
CONTENT_DIR_RE='(^|/)(sce_sys|sce_module|[^/]*dump[^/]*)(/|$)'
MAX_BYTES=$((5 * 1024 * 1024))

fail=0

check_text() { # $1 = label, text on stdin
	if grep -qiE -- "$NAME_RE"; then
		echo "policy: $1 names a restricted game title. Refer to it as \"the target title\"." >&2
		fail=1
	fi
}

mode="${1:-}"
[ $# -gt 0 ] && shift

case "$mode" in
text)
	check_text "${1:-text}"
	;;
msg)
	grep -v '^#' "$1" | check_text "commit message"
	;;
files)
	for f in "$@"; do
		p="${f//\\//}"
		printf '%s\n' "$p" | check_text "path '$p'"
		lower="$(printf '%s' "$p" | tr '[:upper:]' '[:lower:]')"
		if printf '%s\n' "$lower" | grep -qE -- "$CONTENT_RE|$CONTENT_DIR_RE"; then
			echo "policy: '$p' looks like game or console content; it must not be committed." >&2
			fail=1
		fi
		[ -f "$f" ] || continue
		size=$(wc -c <"$f" | tr -d ' ')
		if [ "$size" -gt "$MAX_BYTES" ]; then
			echo "policy: '$p' is larger than 5 MiB; large binaries are not accepted." >&2
			fail=1
		fi
		if grep -Iq . "$f" 2>/dev/null; then
			check_text "contents of '$p'" <"$f"
		fi
	done
	;;
*)
	echo "usage: $0 text [label] | msg <file> | files <path>..." >&2
	exit 2
	;;
esac

exit $fail
