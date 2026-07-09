#!/bin/bash
for b in bufferevent_readcb bufferevent_writecb evbuffer_add evbuffer_drain evbuffer_readln; do
    printf "%s" "$b"
    for i in 1 2 3 4 5; do
        printf "\t%s" "$(build/bin/bench_kvrocks_$b -d 3 | awk -F'ns_per_op=' '{print $2}')"
    done
    echo
done
