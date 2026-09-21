#!/usr/bin/env bash
# Deploys the bot on an existing k3s (Debian) WITHOUT touching what already runs on it.
#
#   deploy/deploy.sh check      read-only: what is on the cluster, will there be room and no conflict
#   deploy/deploy.sh install    build the image, create the namespace, the secret and the workloads
#   deploy/deploy.sh upgrade    rebuild the image and restart the console only (SQL Server keeps running)
#   deploy/deploy.sh status     pods, volumes and the address
#   deploy/deploy.sh backup [f]  saves the database to a .bak file on this machine (default lordsbot-DATE.bak)
#   deploy/deploy.sh restore f  replaces the database by a .bak file (the console is stopped meanwhile)
#   deploy/deploy.sh uninstall  removes the lmbot namespace (asks first; the database volume goes with it)
#
# Everything created lives in the "lmbot" namespace. Nothing outside it is created, changed, restarted or
# deleted: no cluster-wide object, no change to k3s, Traefik or the other namespaces.
#
# Settings (environment):
#   LMBOT_HOST       host name of the console on the ingress, e.g. bot.example.com (empty: no ingress, use port-forward)
#   LMBOT_IMAGE      image to use (default lmbot:latest, built here and imported into k3s)
#   NAMESPACE        default lmbot
#   YES=1            do not ask questions
set -euo pipefail

NAMESPACE="${NAMESPACE:-lmbot}"
IMAGE="${LMBOT_IMAGE:-lmbot:latest}"
HOST_NAME="${LMBOT_HOST:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
NEED_MB=2560      # SQL Server (2 GB minimum) + the console

say()  { printf '\033[1m%s\033[0m\n' "$*"; }
warn() { printf '\033[33mATTENTION: %s\033[0m\n' "$*"; WARNINGS=$((WARNINGS + 1)); }
die()  { printf '\033[31mERREUR: %s\033[0m\n' "$*" >&2; exit 1; }
WARNINGS=0

if command -v kubectl >/dev/null 2>&1; then K=(kubectl)
elif command -v k3s >/dev/null 2>&1; then K=(k3s kubectl)
else die "ni kubectl ni k3s trouvés : lancez ce script sur le serveur."; fi
# k3s writes its kubeconfig readable by root only
if ! "${K[@]}" get nodes >/dev/null 2>&1; then
  if [ -r /etc/rancher/k3s/k3s.yaml ]; then export KUBECONFIG=/etc/rancher/k3s/k3s.yaml
  elif command -v sudo >/dev/null 2>&1; then K=(sudo "${K[@]}")
  fi
fi
"${K[@]}" get nodes >/dev/null 2>&1 || die "le cluster ne répond pas (essayez avec sudo, ou KUBECONFIG=/etc/rancher/k3s/k3s.yaml)."
KN=("${K[@]}" -n "$NAMESPACE")

confirm() {
  [ "${YES:-0}" = 1 ] && return 0
  read -r -p "$1 [o/N] " answer
  [[ "$answer" =~ ^[oOyY] ]]
}

container_tool() {
  if command -v docker >/dev/null 2>&1; then echo docker
  elif command -v podman >/dev/null 2>&1; then echo podman
  else echo ""; fi
}

check() {
  say "== Cluster"
  "${K[@]}" version 2>/dev/null | head -2 || true
  "${K[@]}" get nodes -o wide
  echo
  say "== Ce qui tourne déjà (rien de tout cela ne sera modifié)"
  "${K[@]}" get pods -A --no-headers 2>/dev/null | awk '{c[$1]++} END {for (n in c) printf "  %-20s %d pod(s)\n", n, c[n]}' | sort

  echo
  say "== Espace de noms « $NAMESPACE »"
  if "${K[@]}" get ns "$NAMESPACE" >/dev/null 2>&1; then
    if [ "$("${K[@]}" get ns "$NAMESPACE" -o jsonpath='{.metadata.labels.app\.kubernetes\.io/part-of}')" = "lmbot" ]; then
      echo "  existe déjà et vient de ce déploiement : install le mettra à jour."
    else
      warn "l'espace de noms « $NAMESPACE » existe et n'a pas été créé par ce script : c'est peut-être votre bot. Choisissez un autre nom (NAMESPACE=lmbot-bank ...) ou vérifiez avant."
    fi
  else
    echo "  n'existe pas : il sera créé."
  fi

  echo
  say "== Mémoire"
  local alloc_ki req
  alloc_ki=$("${K[@]}" get nodes -o jsonpath='{.items[0].status.allocatable.memory}' | sed 's/Ki//')
  echo "  allouable sur le nœud : $((alloc_ki / 1024)) Mo"
  if [ -r /proc/meminfo ] && "${K[@]}" get nodes -o name | grep -q "$(hostname)"; then
    local avail; avail=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
    echo "  disponible maintenant  : ${avail} Mo (il en faut environ ${NEED_MB})"
    [ "$avail" -lt "$NEED_MB" ] && warn "moins de ${NEED_MB} Mo disponibles : SQL Server (2 Go minimum) risque de ne pas démarrer ou de gêner votre bot déjà en place. Ajoutez de la mémoire ou du swap avant d'installer."
  fi
  "${K[@]}" describe node 2>/dev/null | sed -n '/Allocated resources/,/Events/p' | head -8 | sed 's/^/  /'

  echo
  say "== Stockage"
  "${K[@]}" get storageclass 2>/dev/null | sed 's/^/  /'
  "${K[@]}" get storageclass -o jsonpath='{range .items[*]}{.metadata.annotations.storageclass\.kubernetes\.io/is-default-class}{"\n"}{end}' | grep -q true \
    || warn "aucune StorageClass par défaut : le volume de SQL Server restera en attente. k3s fournit normalement « local-path »."
  df -h / 2>/dev/null | tail -1 | awk '{print "  disque / : " $4 " libres"}'

  echo
  say "== Ingress existants (pour éviter un conflit de nom d'hôte)"
  "${K[@]}" get ingress -A --no-headers 2>/dev/null | sed 's/^/  /' || true
  if [ -n "$HOST_NAME" ] && "${K[@]}" get ingress -A -o jsonpath='{range .items[*]}{.spec.rules[*].host}{"\n"}{end}' 2>/dev/null | tr ' ' '\n' | grep -qx "$HOST_NAME"; then
    warn "le nom d'hôte $HOST_NAME est déjà utilisé par un autre Ingress : choisissez-en un autre."
  fi
  [ -z "$HOST_NAME" ] && echo "  LMBOT_HOST non défini : pas d'Ingress créé, accès par « port-forward » (voir status)."

  echo
  say "== Outil de construction d'image"
  local tool; tool=$(container_tool)
  if [ -n "$tool" ]; then echo "  $tool trouvé."
  else warn "ni docker ni podman : installez-en un (sudo apt install podman) ou construisez l'image ailleurs (voir docs/deployment.md) puis LMBOT_IMAGE=... ./deploy/deploy.sh install"; fi
  echo
  if [ "$WARNINGS" -gt 0 ]; then say "$WARNINGS point(s) d'attention ci-dessus."; else say "Rien ne s'oppose à l'installation."; fi
}

build_image() {
  local tool; tool=$(container_tool)
  [ -n "$tool" ] || die "aucun outil de construction : sudo apt install podman, ou fournissez LMBOT_IMAGE."
  say "== Construction de l'image $IMAGE (avec $tool)"
  "$tool" build -t "$IMAGE" "$REPO"
  say "== Import dans le containerd de k3s (ne redémarre rien)"
  "$tool" save "$IMAGE" | sudo k3s ctr images import -
}

render() {
  # the host name given at install time is remembered on the namespace, so upgrades (CI) do not need it
  [ -n "$HOST_NAME" ] || HOST_NAME=$("${K[@]}" get ns "$NAMESPACE" -o jsonpath='{.metadata.annotations.lmbot-host}' 2>/dev/null || true)
  # the only edits: namespace, image and, when there is one, the ingress with its host name
  local out; out=$("${K[@]}" kustomize "$HERE/k8s")
  if [ -n "$HOST_NAME" ]; then
    out="$out"$'\n---\n'"$(cat "$HERE/k8s/ingress.yaml")"
    out=${out//lmbot.example.com/$HOST_NAME}
  fi
  out=${out//namespace: lmbot/namespace: $NAMESPACE}
  printf '%s\n' "${out//image: lmbot:latest/image: $IMAGE}"
}

install() {
  check
  echo
  confirm "Installer dans l'espace de noms « $NAMESPACE » ?" || die "annulé."
  [ -n "${LMBOT_IMAGE:-}" ] || build_image

  say "== Espace de noms et secret"
  "${K[@]}" get ns "$NAMESPACE" >/dev/null 2>&1 || "${K[@]}" create ns "$NAMESPACE"
  "${K[@]}" label ns "$NAMESPACE" app.kubernetes.io/part-of=lmbot --overwrite >/dev/null
  [ -z "$HOST_NAME" ] || "${K[@]}" annotate ns "$NAMESPACE" lmbot-host="$HOST_NAME" --overwrite >/dev/null
  if "${KN[@]}" get secret lmbot-secrets >/dev/null 2>&1; then
    echo "  le secret existe déjà : conservé (les mots de passe ne sont jamais réécrits)."
  else
    local sa token
    sa="Lm$(openssl rand -hex 8)-Aa1!"
    token="$(openssl rand -hex 24)"
    "${KN[@]}" create secret generic lmbot-secrets --from-literal=sa-password="$sa" --from-literal=token="$token" >/dev/null
    echo "  secret créé (mot de passe SQL et jeton générés, lisibles avec « $0 status »)."
  fi

  say "== Contrôle serveur (rien n'est encore appliqué)"
  render | "${KN[@]}" apply --dry-run=server -f - >/dev/null
  say "== Application"
  render | "${KN[@]}" apply -f -
  say "== Attente du démarrage (SQL Server met 30 à 90 s)"
  "${KN[@]}" rollout status statefulset/sqlserver --timeout=300s
  "${KN[@]}" rollout status deployment/lmbot --timeout=300s
  status
}

upgrade() {
  [ -n "${LMBOT_IMAGE:-}" ] || build_image
  say "== Mise à jour de la console seule (SQL Server et son volume ne sont pas touchés)"
  render | "${KN[@]}" apply -f -
  # a new image tag (CI uses the commit) already rolls the pod out; the local "latest" tag needs a restart
  case "$IMAGE" in *:latest|lmbot) "${KN[@]}" rollout restart deployment/lmbot ;; esac
  "${KN[@]}" rollout status deployment/lmbot --timeout=300s
  echo "Les bots qui tournaient sont relancés automatiquement par la console."
}

status() {
  "${KN[@]}" get pods,pvc,svc,ingress 2>/dev/null
  local token; token=$("${KN[@]}" get secret lmbot-secrets -o jsonpath='{.data.token}' 2>/dev/null | base64 -d || true)
  echo
  if [ -n "$HOST_NAME" ]; then
    say "Console : https://$HOST_NAME/#t=$token"
  else
    say "Console : lancez « ${K[*]} -n $NAMESPACE port-forward svc/lmbot 8765:80 » puis ouvrez http://localhost:8765/#t=$token"
  fi
}

sql_pod() {   # runs a T-SQL text (stdin) in the SQL Server pod, password taken from the pod's own environment
  "${KN[@]}" exec -i sqlserver-0 -- sh -c 'cat > /tmp/job.sql && /opt/mssql-tools18/bin/sqlcmd -C -S localhost -U sa -P "$MSSQL_SA_PASSWORD" -b -i /tmp/job.sql; rc=$?; rm -f /tmp/job.sql; exit $rc'
}

backup() {
  local out="${1:-lordsbot-$(date +%Y%m%d-%H%M).bak}"
  say "== Sauvegarde de la base (le service continue de tourner)"
  "${KN[@]}" exec sqlserver-0 -- mkdir -p /var/opt/mssql/backup
  sql_pod <<'SQL'
BACKUP DATABASE [lordsbot] TO DISK = N'/var/opt/mssql/backup/lordsbot.bak' WITH INIT, CHECKSUM, FORMAT;
RESTORE VERIFYONLY FROM DISK = N'/var/opt/mssql/backup/lordsbot.bak' WITH CHECKSUM;
SQL
  "${KN[@]}" exec sqlserver-0 -- cat /var/opt/mssql/backup/lordsbot.bak > "$out"
  "${KN[@]}" exec sqlserver-0 -- rm -f /var/opt/mssql/backup/lordsbot.bak
  chmod 600 "$out"
  say "Sauvegarde : $out ($(du -h "$out" | cut -f1)). Elle contient les clés d'accès de vos comptes : gardez-la comme un secret."
}

restore() {
  local file="${1:-}"
  [ -n "$file" ] && [ -r "$file" ] || die "usage : $0 restore fichier.bak"
  "${KN[@]}" get statefulset/sqlserver >/dev/null 2>&1 || die "SQL Server n'est pas installé : lancez d'abord « $0 install »."
  warn "la base actuelle (comptes, clés, journaux) sera REMPLACÉE par le contenu de $file."
  confirm "Continuer ?" || die "annulé."
  say "== Arrêt de la console (plus aucune écriture pendant la restauration)"
  "${KN[@]}" scale deployment/lmbot --replicas=0 >/dev/null 2>&1 || true
  "${KN[@]}" wait --for=delete pod -l app=lmbot --timeout=120s >/dev/null 2>&1 || true
  say "== Envoi et restauration"
  "${KN[@]}" exec sqlserver-0 -- mkdir -p /var/opt/mssql/backup
  "${KN[@]}" exec -i sqlserver-0 -- sh -c 'cat > /var/opt/mssql/backup/restore.bak' < "$file"
  sql_pod <<'SQL'
IF DB_ID(N'lordsbot') IS NOT NULL ALTER DATABASE [lordsbot] SET SINGLE_USER WITH ROLLBACK IMMEDIATE;
RESTORE DATABASE [lordsbot] FROM DISK = N'/var/opt/mssql/backup/restore.bak' WITH REPLACE, CHECKSUM, RECOVERY;
ALTER DATABASE [lordsbot] SET MULTI_USER;
SQL
  "${KN[@]}" exec sqlserver-0 -- rm -f /var/opt/mssql/backup/restore.bak
  say "== Redémarrage de la console"
  "${KN[@]}" scale deployment/lmbot --replicas=1 >/dev/null
  "${KN[@]}" rollout status deployment/lmbot --timeout=300s
  echo "Les bots qui tournaient au moment de la sauvegarde redémarrent seuls."
}

uninstall() {
  "${K[@]}" get ns "$NAMESPACE" >/dev/null 2>&1 || die "l'espace de noms $NAMESPACE n'existe pas."
  [ "$("${K[@]}" get ns "$NAMESPACE" -o jsonpath='{.metadata.labels.app\.kubernetes\.io/part-of}')" = "lmbot" ] \
    || die "« $NAMESPACE » n'a pas été créé par ce script : je refuse de le supprimer."
  warn "cela supprime la console ET la base (comptes, clés, journaux). Faites une sauvegarde d'abord (docs/deployment.md)."
  confirm "Supprimer l'espace de noms $NAMESPACE ?" || die "annulé."
  "${K[@]}" delete ns "$NAMESPACE"
}

case "${1:-}" in
  check) check ;;
  install) install ;;
  upgrade) upgrade ;;
  status) status ;;
  backup) backup "${2:-}" ;;
  restore) restore "${2:-}" ;;
  uninstall) uninstall ;;
  *) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
