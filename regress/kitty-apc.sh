#!/bin/sh

# Kitty graphics APCs must be handled before generic APC title handling.

set -e

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX="$(cd .. && pwd -P)/tmux"
[ -x "$TEST_TMUX" ] || exit 1
TMUX="$TEST_TMUX -Ltest"
CONF=$(mktemp)
TMP=$(mktemp)
APC=$(mktemp)
OUT=$(mktemp)

kill_server() {
	$TMUX kill-server 2>/dev/null || true
}

script_client() {
	cmd="$TMUX -f$CONF new -x 20 -y 5 \"$1\""

	: >$OUT
	TERM=xterm-256color script -q "$OUT" sh -c "$cmd" \
	    >/dev/null 2>&1 && return 0
	: >$OUT
	TERM=xterm-256color script -q -c "$cmd" "$OUT" \
	    >/dev/null 2>&1 && return 0
	return 1
}

script_client_input() {
	input=$1
	cmd="$TMUX -f$CONF new -x 20 -y 5 \"$2\""

	: >$OUT
	{ sleep 0.2; printf '%b' "$input"; sleep 0.1; } | \
	    TERM=xterm-256color script -q "$OUT" sh -c "$cmd" \
	    >/dev/null 2>&1 && return 0
	: >$OUT
	{ sleep 0.2; printf '%b' "$input"; sleep 0.1; } | \
	    TERM=xterm-256color script -q -c "$cmd" "$OUT" \
	    >/dev/null 2>&1 && return 0
	return 1
}

trap 'kill_server; rm -f "$CONF" "$TMP" "$APC" "$OUT"' 0 1 15

kill_server
printf 'set -g allow-set-title on\n' >$CONF

$TMUX -f$CONF new -d 'sleep 1'
case "$($TMUX display -p '#{kitty_support}')" in
1) ;;
*) [ -n "$REQUIRE_KITTY_IMAGES" ] && exit 1; exit 0 ;;
esac
kill_server

if command -v script >/dev/null 2>&1; then
	printf 'set -g allow-set-title on\nset -as terminal-features "*:kitty"\nset -g status off\n' >$CONF
	script_client "printf '\033_Ga=T,q=1,i=94,t=d,f=24,s=1,v=1,c=1,r=1;AAAA\033\\\\'; sleep 0.5" || exit 1
	kill_server
	grep -aq 'a=t,t=d,f=24,s=1,v=1,q=1,i=' $OUT || exit 1
	grep -aq 'a=p,c=1,r=1,q=1,i=' $OUT || exit 1
	grep -aq 'KITTY IMAGE' $OUT && exit 1

	script_client "printf '\033_Ga=T,q=1,i=96,t=d,f=24,s=1,v=1,c=1,r=1;AAAA\033\\\\'; sleep 0.2; $TMUX refresh-client; sleep 0.5" || exit 1
	kill_server
	[ "$(grep -ao 'a=t,t=d,f=24,s=1,v=1,q=1,i=' $OUT | wc -l | tr -d ' ')" -ge 2 ] || exit 1
	grep -aq 'a=d,d=i,i=.*q=1' $OUT || exit 1
	grep -aq 'a=d,d=i,i=.*p=.*q=1' $OUT || exit 1
	grep -aq 'KITTY IMAGE' $OUT && exit 1

	printf 'set -g allow-set-title on\nset -g status off\n' >$CONF
	script_client "printf '\033_Ga=T,q=1,i=95,t=d,f=24,s=1,v=1,c=1,r=1;AAAA\033\\\\'; sleep 0.5" || exit 1
	kill_server
	grep -aq 'KITTY IMAGE (1x1)' $OUT || exit 1
	grep -aq 'a=t,t=d,f=24,s=1,v=1,q=1,i=' $OUT && exit 1

	script_client_input '\033_Gi=31;OK\033\\' \
	    "sleep 0.4; printf '\033_Ga=T,q=1,i=97,t=d,f=24,s=1,v=1,c=1,r=1;AAAA\033\\\\'; sleep 0.5" || exit 1
	kill_server
	grep -aq 'a=t,t=d,f=24,s=1,v=1,q=1,i=' $OUT || exit 1
	grep -aq 'a=p,c=1,r=1,q=1,i=' $OUT || exit 1
	grep -aq 'KITTY IMAGE' $OUT && exit 1

	script_client_input '\033_Gi=31;ENOSYS\033\\' \
	    "sleep 0.4; printf '\033_Ga=T,q=1,i=98,t=d,f=24,s=1,v=1,c=1,r=1;AAAA\033\\\\'; sleep 0.5" || exit 1
	kill_server
	grep -aq 'KITTY IMAGE (1x1)' $OUT || exit 1
	grep -aq 'a=t,t=d,f=24,s=1,v=1,q=1,i=' $OUT && exit 1
	printf 'set -g allow-set-title on\n' >$CONF
fi

test_apc() {
	kill_server
	$TMUX -f$CONF new -d "printf '$1'; sleep 1"
	sleep 0.5

	$TMUX capturep -pS0 >$TMP || exit 1
	grep -q "$2" $TMP || exit 1
	[ -n "$4" ] && grep -q "$4" $TMP && exit 1

	title=$($TMUX display -p '#{pane_title}')
	case "$title" in
	*"$3"*) exit 1 ;;
	esac
}

test_apc '\033_Ga=q,t=d,f=24,s=1,v=1;AAAA\033\\after-query\n' \
    'after-query' 'Ga=q'
test_apc '\033_Gbad\033\\after-malformed\n' 'after-malformed' 'Gbad'

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,f=32,s=1,v=1,c=1,r=1;!!!!\033\\\\after-invalid\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-invalid/ { print NR; exit }' $TMP)" = 1 ] || exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=q,t=d,f=24,s=1,v=1;AAAA\033\\\\'; dd bs=1 count=64 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 4e 4f 53 59 53' && exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '4f 4b' || exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=q,q=3,t=d,f=24,s=1,v=1;AAAA\033\\\\'; dd bs=1 count=64 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 4e 4f 53 59 53' && exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 49 4e 56 41 4c' && exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '4f 4b' && exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=q,q=1,t=d,f=24,s=1,v=1;AAAA\033\\\\'; dd bs=1 count=64 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 4e 4f 53 59 53' && exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '4f 4b' && exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=q,q=2,t=d,f=24,s=1,v=1;AAAA\033\\\\'; dd bs=1 count=64 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 4e 4f 53 59 53' && exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '4f 4b' || exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=q,t=d,f=1,s=1,v=1;AAAA\033\\\\'; dd bs=1 count=128 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 49 4e 56 41 4c' || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '4f 4b' && exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=q,q=3,t=d,f=1,s=1,v=1;AAAA\033\\\\'; dd bs=1 count=128 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 49 4e 56 41 4c' && exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '4f 4b' && exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=q,t=d,o=x,f=24,s=1,v=1;AAAA\033\\\\'; dd bs=1 count=128 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 49 4e 56 41 4c' || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '4f 4b' && exit 1

PNG_1X1='iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC'
BAD_PNG_HEADER='iVBORw0KGgoAAAANSUhEUgAAAAEAAAAB'
INTERLACED_PNG_1X1='iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAAHncGNIAAAADElEQVR4nGNgYGAAAAAEAAH2FzhVAAAAAElFTkSuQmCC'
ZLIB_RGB_1X1='eJxjYGAAAAADAAE='
kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,f=100;$PNG_1X1\033\\\\after-png\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-png/ { print NR; exit }' $TMP)" = 2 ] || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,f=100;$BAD_PNG_HEADER\033\\\\after-bad-png\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-bad-png/ { print NR; exit }' $TMP)" = 1 ] || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,f=100;$INTERLACED_PNG_1X1\033\\\\after-interlaced-png\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-interlaced-png/ { print NR; exit }' $TMP)" = 2 ] || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,o=z,f=24,s=1,v=1,c=1,r=1;$ZLIB_RGB_1X1\033\\\\after-zlib\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-zlib/ { print NR; exit }' $TMP)" = 2 ] || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=t,i=1,t=d,f=24,s=1,v=1;AAAA\033\\\\after-transmit\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-transmit/ { print NR; exit }' $TMP)" = 1 ] || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=t,q=1,i=84,t=d,f=24,s=1,v=1;AAAA\033\\\\\033_Ga=p,q=1,i=84,c=1,r=1\033\\\\first-place\n\033_Ga=p,q=1,i=84,c=1,r=1\033\\\\second-place\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL' $TMP && exit 1
grep -q 'second-place' $TMP || exit 1

{
	i=1
	while [ $i -le 25 ]; do
		printf '\033_Ga=t,q=1,i=%s,t=d,f=24,s=1,v=1;AAAA\033\\' "$i"
		i=$((i + 1))
	done
	printf '\033_Ga=p,q=1,i=1,c=1,r=1\033\\after-source-quota\n'
} >$APC
kill_server
$TMUX -f$CONF new -d "cat '$APC'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL' $TMP && exit 1
grep -q 'after-source-quota' $TMP || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,q=1,i=85,t=d,f=24,s=1,v=1,c=1,r=1;AAAA\033\\\\\033[Hcover\033_Ga=p,q=1,i=85,c=1,r=1\033\\\\after-overwrite\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL' $TMP && exit 1
grep -q 'after-overwrite' $TMP || exit 1

kill_server
$TMUX -f$CONF new -d -x 20 -y 4 \
    "printf '\033_Ga=T,q=1,i=86,t=d,f=24,s=1,v=1,c=1,r=1;AAAA\033\\\\one\r\ntwo\r\nthree\r\nfour\r\n\033_Ga=p,q=1,i=86,c=1,r=1\033\\\\after-reuse\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL' $TMP && exit 1
grep -q 'after-reuse' $TMP || exit 1

{
	printf 'one\r\ntwo\r\nthree\r\nfour'
	printf '\033_Ga=t,q=1,i=93,t=d,f=24,s=1,v=64;'
	head -c 256 /dev/zero | tr '\000' A
	printf '\033\\\033[4;1H\033_Ga=p,q=1,i=93\033\\after-place'
} >$APC
kill_server
$TMUX -f$CONF new -d -x 20 -y 4 "cat '$APC'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL' $TMP && exit 1
[ "$(sed -n '1p' $TMP)" = "three" ] || exit 1
[ "$(sed -n '2p' $TMP)" = "four" ] || exit 1
[ "$(sed -n '4p' $TMP)" = "after-place" ] || exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=t,I=7,t=d,f=24,s=1,v=1;AAAA\033\\\\'; dd bs=1 count=128 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
REPLY=$(tr -s '[:space:]' ' ' <$TMP)
echo "$REPLY" | grep -q '1b 5f 47 69 3d' || exit 1
echo "$REPLY" | grep -q '2c 49 3d 37' || exit 1
echo "$REPLY" | grep -q '3b 4f 4b' || exit 1
echo "$REPLY" | grep -q '1b 5f 47 49 3d 37 3b 4f 4b' && exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=t,t=d,f=24,s=1,v=1;AAAA\033\\\\'; dd bs=1 count=128 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 49 4e 56 41 4c' || exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=t,q=2,t=d,f=24,s=1,v=1;AAAA\033\\\\'; dd bs=1 count=128 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 49 4e 56 41 4c' && exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=T,i=11,p=22,t=d,f=24,s=1,v=1;AAAA\033\\\\'; dd bs=1 count=128 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q \
    '1b 5f 47 69 3d 31 31 2c 70 3d 32 32 3b 4f 4b' || exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=T,t=f,f=100,s=1,v=1,r=1;L3RtcC9pbWc=\033\\\\'; dd bs=1 count=128 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 49 4e 56 41 4c' || exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=T,q=2,t=f,f=100,s=1,v=1,r=1;L3RtcC9pbWc=\033\\\\'; dd bs=1 count=128 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 49 4e 56 41 4c' && exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,f=24,s=1,v=1,r=1,C=1;AAAA\033\\\\after-stay\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-stay/ { print NR; exit }' $TMP)" = 1 ] || exit 1

kill_server
$TMUX -f$CONF new -d -x 20 -y 4 \
    "printf 'one\r\ntwo\r\n\033_Ga=T,q=1,t=d,f=24,s=1,v=1,c=1,r=2;AAAA\033\\\\after-scroll'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q '^one' $TMP && exit 1
grep -q '^two$' $TMP || exit 1
[ "$(awk '/after-scroll/ { print NR; exit }' $TMP)" = 4 ] || exit 1

kill_server
$TMUX -f$CONF new -d -x 20 -y 5 \
    "printf '\033[1;1Htop\033[2;1Hr2\033[3;1Hr3\033[4;1Hr4\033[5;1Hbottom\033[2;4r\033[4;1H\033_Ga=T,q=1,i=88,t=d,f=24,s=1,v=1,c=1,r=1;AAAA\033\\\\after-region'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(sed -n '1p' $TMP)" = "top" ] || exit 1
[ "$(sed -n '2p' $TMP)" = "r3" ] || exit 1
[ "$(sed -n '3p' $TMP)" = "r4" ] || exit 1
[ "$(sed -n '4p' $TMP)" = "after-region" ] || exit 1
[ "$(sed -n '5p' $TMP)" = "bottom" ] || exit 1

kill_server
$TMUX -f$CONF new -d -x 20 -y 5 \
    "printf '\033[1;1Htop\033[2;1Hr2\033[3;1Hr3\033[4;1Hr4\033[5;1Hbottom\033[3;1H\033_Ga=T,q=1,i=89,t=d,f=24,s=1,v=1,c=1,r=1,C=1;AAAA\033\\\\\033[2;4r\033[2;1H\033[L\033[5;1H\033_Ga=p,q=1,i=89,c=1,r=1,C=1\033\\\\after-insert-line'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL' $TMP && exit 1
[ "$(sed -n '1p' $TMP)" = "top" ] || exit 1
[ "$(sed -n '2p' $TMP)" = "" ] || exit 1
[ "$(sed -n '3p' $TMP)" = "r2" ] || exit 1
[ "$(sed -n '4p' $TMP)" = "r3" ] || exit 1
[ "$(sed -n '5p' $TMP)" = "after-insert-line" ] || exit 1

kill_server
$TMUX -f$CONF new -d -x 20 -y 5 \
    "printf '\033[1;1Htop\033[2;1Hr2\033[3;1Hr3\033[4;1Hr4\033[5;1Hbottom\033[2;1H\033_Ga=T,q=1,i=90,t=d,f=24,s=1,v=3,c=1,r=3,C=1;AAAAAAAAAAAA\033\\\\\033[2;4r\033[2;1H\033[M\033[5;1H\033_Ga=p,q=1,i=90,c=1,r=1,C=1\033\\\\after-delete-line'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL' $TMP && exit 1
[ "$(sed -n '1p' $TMP)" = "top" ] || exit 1
[ "$(sed -n '2p' $TMP)" = "r3" ] || exit 1
[ "$(sed -n '3p' $TMP)" = "r4" ] || exit 1
[ "$(sed -n '4p' $TMP)" = "" ] || exit 1
[ "$(sed -n '5p' $TMP)" = "after-delete-line" ] || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,f=24,s=2,v=1,c=2,r=1,m=1;AAAA\033\\\\\033_Gm=0;AAAA\033\\\\after-chunk\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-chunk/ { print NR; exit }' $TMP)" = 2 ] || exit 1

{
	printf '\033_Ga=T,q=1,t=d,f=24,s=262500,v=1,c=1,r=1,m=1;'
	head -c 4000 /dev/zero | tr '\000' A
	printf '\033\\'
	i=1
	while [ $i -lt 262 ]; do
		printf '\033_Gm=1;'
		head -c 4000 /dev/zero | tr '\000' A
		printf '\033\\'
		i=$((i + 1))
	done
	printf '\033_Gm=0;'
	head -c 2000 /dev/zero | tr '\000' A
	printf '\033\\after-large-chunk\n'
} >$APC
kill_server
$TMUX -f$CONF new -d "cat '$APC'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL' $TMP && exit 1
[ "$(awk '/after-large-chunk/ { print NR; exit }' $TMP)" = 2 ] || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,f=24,s=2,v=1,c=2,r=1;AAAA'; sleep 1; printf 'AAAA\033\\\\after-split\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'AAAA' $TMP && exit 1
sleep 1.2
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-split/ { print NR; exit }' $TMP)" = 2 ] || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,f=24,s=1,v=1,m=1;AA\033\\\\\033_Gm=0;AA\033\\\\after-bad-chunk\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-bad-chunk/ { print NR; exit }' $TMP)" = 1 ] || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,f=24,s=2,v=1,m=1;AAAA\033\\\\\033_Gbad\033\\\\\033_Gm=0;AAAA\033\\\\after-abort\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-abort/ { print NR; exit }' $TMP)" = 1 ] || exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=T,i=73,t=d,f=24,s=2,v=1,m=1;AAAA\033\\\\\033_Gbad\033\\\\'; dd bs=1 count=128 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 49 4e 56 41 4c' || exit 1

kill_server
$TMUX -f$CONF new -d \
    "stty raw -echo min 0 time 10; printf '\033_Ga=T,i=74,t=d,f=24,s=2,v=2;'; head -c 4097 /dev/zero | tr '\\000' A; printf '\033\\\\'; dd bs=1 count=128 2>/dev/null | od -An -tx1; sleep 1"
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
tr -s '[:space:]' ' ' <$TMP | grep -q '45 49 4e 56 41 4c' || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=t,q=1,i=81,t=d,f=24,s=1,v=1;AAAA\033\\\\\033_Ga=d,q=1,d=I,i=81\033\\\\\033_Ga=p,q=1,i=81,c=1,r=1\033\\\\after-delete\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL:unknown image id' $TMP || exit 1
grep -q 'after-delete' $TMP || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=t,q=1,i=87,t=d,f=24,s=1,v=1;AAAA\033\\\\\033_Ga=p,q=1,i=87,c=1,r=1\033\\\\\033_Ga=d,q=1,d=i,i=87\033\\\\\033_Ga=p,q=1,i=87,c=1,r=1\033\\\\after-delete-lower\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL:unknown image id' $TMP || exit 1
grep -q 'after-delete-lower' $TMP || exit 1

kill_server
$TMUX -f$CONF new -d -x 20 -y 5 \
    "printf '\033_Ga=t,q=1,i=91,t=d,f=24,s=1,v=1;AAAA\033\\\\main\n\033[?1049h\033_Ga=T,q=1,i=92,t=d,f=24,s=1,v=1,c=1,r=1;AAAA\033\\\\alt\n\033[?1049l\033_Ga=p,q=1,i=91,c=1,r=1\033\\\\after-alternate\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL' $TMP && exit 1
grep -q '^main$' $TMP || exit 1
grep -q 'after-alternate' $TMP || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=t,q=1,i=82,t=d,f=24,s=1,v=1;AAAA\033\\\\\033[2J\033_Ga=p,q=1,i=82,c=1,r=1\033\\\\after-clear\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL:unknown image id' $TMP || exit 1
grep -q 'after-clear' $TMP || exit 1

kill_server
$TMUX -f$CONF new -d \
    "printf '\033_Ga=t,q=1,i=83,t=d,f=24,s=1,v=1;AAAA\033\\\\\033_Ga=p,q=1,i=83,c=1,r=1\033\\\\\033[K\033_Ga=p,q=1,i=83,c=1,r=1\033\\\\after-erase\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL' $TMP && exit 1
grep -q 'after-erase' $TMP || exit 1

kill_server
$TMUX -f$CONF new -d -x 40 -y 8 \
    "printf '\033_Ga=t,q=1,i=71,t=d,f=24,s=1,v=1;AAAA\033\\\\after-resize-transmit\\n'; sleep 1; printf '\033_Ga=p,q=1,i=71,c=1,r=1\033\\\\after-resize-place\\n'; sleep 1"
sleep 0.3
$TMUX resize-window -x 30 -y 6 || exit 1
sleep 1.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL' $TMP && exit 1
[ "$(awk '/after-resize-place/ { print NR; exit }' $TMP)" = 3 ] || exit 1

kill_server
$TMUX -f$CONF new -d -x 40 -y 8 \
    "printf '\033_Ga=t,q=1,i=72,t=d,f=24,s=1,v=1;AAAA\033\\\\after-ri-transmit\\n'; printf '\033[H\033M'; printf '\033_Ga=p,q=1,i=72,c=1,r=1\033\\\\after-ri-place\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
grep -q 'EINVAL' $TMP && exit 1
[ "$(awk '/after-ri-place/ { print NR; exit }' $TMP)" = 2 ] || exit 1

kill_server

exit 0
