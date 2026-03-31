#!/bin/bash
set -e

echo "---> Starting the MUNGE Authentication service (munged) ..."
gosu munge /usr/sbin/munged

echo "---> Waiting for slurmctld to become active before starting Open OnDemand ..."
until 2>/dev/null >/dev/tcp/slurmctld/6817
do
    echo "-- slurmctld is not available.  Sleeping ..."
    sleep 2
done
echo "-- slurmctld is now active ..."

# Ensure the ood user's home directory exists with SSH keys intact
mkdir -p /home/ood/.ssh
cp -n /etc/skel/.ssh/* /home/ood/.ssh/ 2>/dev/null || true
chown -R ood:ood /home/ood

echo "---> Starting the SSH service (sshd) ..."
/usr/sbin/sshd

# OOD runs on HTTP only in this demo setup
rm -f /etc/httpd/conf.d/ssl.conf

echo "---> Generating Open OnDemand portal configuration ..."
/opt/ood/ood-portal-generator/sbin/update_ood_portal --insecure

# Clear stale Dex database so the config takes effect on restart
rm -f /etc/ood/dex/dex.db

echo "---> Starting Dex OIDC provider ..."
/usr/sbin/ondemand-dex serve /etc/ood/dex/config.yaml &

echo "---> Starting Apache httpd (Open OnDemand) ..."
exec /usr/sbin/httpd -DFOREGROUND
