#!/usr/bin/env bash
# Élő próba helyben: wrangler dev --local (Miniflare, helyi D1), valódi HTTP-kérések curl-lel. A Cloudflare felé nincs
# hívás (--local, metrikaküldés kikapcsolva); a helyi állapot egy átmeneti mappában van, utána törlődik.
# Futtatás a mappából: bash tools/live_probe.sh
set -euo pipefail
cd "$(dirname "$0")/.."
export WRANGLER_SEND_METRICS=false
PORT=${PORT:-8799}
STATE=$(mktemp -d)
VARS=$(mktemp)
trap 'kill $PID 2>/dev/null || true; rm -rf "$STATE" "$VARS"' EXIT
echo "RATE_SALT=elo-proba-$(date +%s%N)" >"$VARS"

npx wrangler d1 migrations apply aqulify-hibajelentes --local --persist-to "$STATE" >/dev/null
npx wrangler dev --local --port "$PORT" --persist-to "$STATE" --env-file "$VARS" --var RATE_PER_IP_PER_HOUR:3 >"$STATE/dev.log" 2>&1 &
PID=$!
for _ in $(seq 1 60); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.5; done

URL="http://127.0.0.1:$PORT/report"
ok='{"format":1,"kind":"stall","symptom":"frozen","created_unix":1795100000,"state":{"version":"2.6.1","platform":"linux","package":"deb"},"crash":"","log":["[Hálózat] kliens 192.168.1.23","[Lejátszó] Szól: /home/anna/Zene/a.dsf","DAC: FiiO BTR7"]}'
req() { curl -s -o /dev/stderr -w "%{http_code}" -X POST "$URL" -H "Content-Type: application/json" -H "CF-Connecting-IP: 198.51.100.5" "$@"; }

echo "1) helyes jelentés:";        c=$(req --data-binary "$ok" 2>&1); echo "   $c"
echo "2) rossz séma (format 2):";  c=$(req --data-binary "${ok/\"format\":1/\"format\":2}" 2>&1); echo "   $c"
echo "3) túl nagy (70 KB):";       c=$(head -c 70000 /dev/zero | tr '\0' 'a' | req --data-binary @- 2>&1); echo "   $c"
# A 3) nem számít a korlátba: a bevallott Content-Length alapján még a korlát (és a D1) előtt elutasítva.
echo "4) harmadik számított kérés ugyanarról az IP-ről (korlát: 3/óra):"; c=$(req --data-binary "$ok" 2>&1); echo "   $c"
echo "4b) negyedik számított kérés:"; c=$(req -D - --data-binary "$ok" 2>&1 | tr -d '\r' | grep -iE '^(HTTP|retry-after)|ok'); echo "$c" | sed 's/^/   /'

echo "5) a D1 tartalma (helyi):"
npx wrangler d1 execute aqulify-hibajelentes --local --persist-to "$STATE" --json \
  --command "SELECT id, kind, version, platform, server_scrub_changes, body FROM reports" | tr -d '\n' | sed 's/  */ /g'
echo
echo "6) a korlát-tábla kulcsai:"
npx wrangler d1 execute aqulify-hibajelentes --local --persist-to "$STATE" --json --command "SELECT k, n FROM rate_limit" | tr -d '\n' | sed 's/  */ /g'
echo
echo "7) a wrangler dev naplójában van-e IP vagy jelentéstartalom:"
if grep -E "198\.51\.100\.5|anna|192\.168" "$STATE/dev.log"; then echo "   IGEN (hiba)"; exit 1; else echo "   nincs"; fi
