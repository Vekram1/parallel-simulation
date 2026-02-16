#!/usr/bin/env bash
set -euo pipefail

mkdir -p /var/run/sshd /root/.ssh
chmod 700 /root/.ssh

if [[ -f /docker-mpi/authorized_keys ]]; then
  cp /docker-mpi/authorized_keys /root/.ssh/authorized_keys
  chmod 600 /root/.ssh/authorized_keys
fi

# Ensure SSH host keys exist for this container instance.
ssh-keygen -A >/dev/null 2>&1 || true

exec /usr/sbin/sshd -D -e
