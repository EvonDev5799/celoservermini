#!/usr/bin/env bash
# Scenario de demonstration automatique :
#   1. client tue brutalement (kill -9) puis relance  -> reprise sans trou
#   2. serveur tue brutalement (kill -9) puis relance -> historique recharge, client reconnecte
# Usage : ./demo.sh [repertoire_de_build]   (defaut : build)
set -u
cd "$(dirname "$0")"
BIN="${1:-build}"
PORT=$((20000 + RANDOM % 20000))
WORK="$(mktemp -d)"
SPID=""; CPID=""

cleanup() { kill -9 $SPID $CPID 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

start_server() { "$BIN/news_server" --port "$PORT" --users "$WORK/users.txt" --history "$WORK/history.log" --interval 200 2>>"$WORK/server.log" & SPID=$!; }
start_client() { "$BIN/news_client" --port "$PORT" --user alice --pass secret --state "$WORK/state" >>"$WORK/client.out" 2>>"$WORK/client.log" & CPID=$!; }

"$BIN/news_server" --add-user alice secret --users "$WORK/users.txt" >/dev/null
echo "== 1. Demarrage serveur + client"
start_server; sleep 0.5; start_client; sleep 2
echo "== 2. kill -9 du client (le serveur continue de produire des news)"
kill -9 $CPID; wait $CPID 2>/dev/null; sleep 1.5
echo "== 3. Relance du client : il doit rattraper les news manquees"
start_client; sleep 2
echo "== 4. kill -9 du SERVEUR, puis relance"
kill -9 $SPID; wait $SPID 2>/dev/null; sleep 1
start_server; sleep 5     # laisse le temps au backoff du client

kill $CPID $SPID 2>/dev/null; wait 2>/dev/null

echo "== Verification : identifiants recus sans trou (doublons tolerables : at-least-once)"
total=$(wc -l < "$WORK/client.out")
awk -F'\t' 'NR==1{ if($1!=1) bad=1 } NR>1{ if($1==prev) dup++; else if($1!=prev+1) bad=1 } {prev=$1}
            END{ printf "%d lignes, dernier id=%d, doublons=%d\n", NR, prev, dup; exit bad }' "$WORK/client.out"
rc=$?
if [ "$total" -lt 10 ]; then echo "ECHEC : trop peu de messages recus"; rc=1; fi
if [ $rc -eq 0 ]; then echo "OK : flux continu malgre les deux crashs"; else echo "ECHEC : trou dans le flux"; tail -n 20 "$WORK/client.log"; fi
exit $rc
