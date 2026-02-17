#!/bin/sh

dbglevel=0

names=$(cd /usr/share/doc ; ls | shuf)

while true; do
        guess=$RANDOM
        test $guess -lt 64000 || continue
        test $guess -gt 1024 || continue
        if netstat -ant | grep -q $guess ; then continue; fi
        if ! test "$portA" ; then portA=$guess; continue; fi
        if ! test "$portB" ; then portB=$guess; continue; fi
        break;
done

tmpdir=$(mktemp -d)
urllist=$tmpdir/liste
cleanup() {
  rm -r "$tmpdir"
}

if ! test "$NOCLEAN" ; then
        trap cleanup INT EXIT TERM
fi

if test -e "$1" ; then
        (cd "$1" && ls foo/*.deb | sed -e s,^,http://localhost:$portB/, > $urllist)
        echo "Will use $1 as cache dir"
        cachedir="$1"
else
        cachedir="$tmpdir"
        mkdir -p "$tmpdir/foo"
        for nam in $names ; do
                sz=$(du -shm $tmpdir | cut -f1)
                if test $sz -gt 100 ; then break; fi
                fulnam="$tmpdir/foo/$nam.deb"
                dd if=/dev/random "of=$fulnam" bs=200 count=$RANDOM 2>/dev/null
                dd if=/dev/random bs=1 count=$RANDOM >> "$fulnam" 2>/dev/null
                echo http://localhost:$portB/foo/$nam.deb >> $urllist
                echo -n .
                fsz=$(du --apparent-size -b $fulnam | cut -f1)
                (
                cat <<EOM
HTTP/1.1 200 OK
Last-Modified: Thu, 04 Feb 2021 20:00:12 GMT
Content-Length: $fsz
Connection: keep-alive
X-Original-Source: http://foo/$nam.deb
Date: Sat Feb  6 15:30:06 2021

EOM
) | perl -pe 's/\n/\r\n/' > "$fulnam.head"
        done

        echo " constructed $tmpdir ($sz)"
fi

mkdir "$tmpdir/logA"
mkdir "$tmpdir/logB"

echo Will use ports: $portA, $portB
echo "List: $urllist"

dld=$tmpdir/dld
mkdir -p $dld

(
cd $cachedir/foo
md5sum *.deb > MD5SUMS
cp MD5SUMS $dld
)

set -x

../builddir/apt-cacher-ng port=$portA "cachedir=$cachedir" "logdir=$tmpdir/logA" debug=$dbglevel socketpath= "pidfile=$tmpdir/pidA" foreground=1 allowuserports=0 &
../builddir/apt-cacher-ng port=$portB "cachedir=$cachedir/tempcache" "logdir=$tmpdir/logB" debug=$dbglevel socketpath= "pidfile=$tmpdir/pidB" foreground=1 allowuserports=0 "proxy=http://127.0.0.1:$portA" &
sleep 5
cd $dld

limit=10000
while true; do
        wget -i $urllist || exit 1
        if ! md5sum -c MD5SUMS ; then echo ERROR ; exit 1; fi
        rm *.deb
        rm -rf "$cachedir/tempcache/foo"
        limit=$(( $limit - 1 ))
        if test $limit -lt 1 ; then exit 0; fi
done

kill $(cat $tmpdir/pidA)
kill $(cat $tmpdir/pidB)
