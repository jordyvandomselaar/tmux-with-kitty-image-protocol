#!/bin/sh

# Kitty graphics APCs must be handled before generic APC title handling.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
CONF=$(mktemp)
TMP=$(mktemp)
trap "$TMUX kill-server 2>/dev/null; rm -f $CONF $TMP" 0 1 15

$TMUX kill-server 2>/dev/null
printf 'set -g allow-set-title on\n' >$CONF

$TMUX -f$CONF new -d 'sleep 1'
case "$($TMUX display -p '#{image_support}')" in
*kitty*) ;;
*) [ -n "$REQUIRE_KITTY_IMAGES" ] && exit 1; exit 0 ;;
esac
$TMUX kill-server 2>/dev/null

test_apc() {
	$TMUX kill-server 2>/dev/null
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
    'after-query' 'Ga=q' 'OK'
test_apc '\033_Gbad\033\\after-malformed\n' 'after-malformed' 'Gbad'

$TMUX kill-server 2>/dev/null
$TMUX -f$CONF new -d \
    "printf '\033_Ga=t,i=1,t=d,f=24,s=1,v=1;AAAA\033\\\\after-transmit\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-transmit/ { print NR; exit }' $TMP)" = 1 ] || exit 1

$TMUX kill-server 2>/dev/null
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,f=24,s=1,v=1,r=1,C=1;AAAA\033\\\\after-stay\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-stay/ { print NR; exit }' $TMP)" = 1 ] || exit 1

$TMUX kill-server 2>/dev/null
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,f=24,s=1,v=1,m=1;AA\033\\\\\033_Gm=0;AA\033\\\\after-chunk\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-chunk/ { print NR; exit }' $TMP)" = 2 ] || exit 1

$TMUX kill-server 2>/dev/null
$TMUX -f$CONF new -d \
    "printf '\033_Ga=T,t=d,f=24,s=1,v=1,m=1;AA\033\\\\\033_Gbad\033\\\\\033_Gm=0;AA\033\\\\after-abort\\n'; sleep 1"
sleep 0.5
$TMUX capturep -pS0 >$TMP || exit 1
[ "$(awk '/after-abort/ { print NR; exit }' $TMP)" = 1 ] || exit 1

$TMUX kill-server 2>/dev/null

exit 0
