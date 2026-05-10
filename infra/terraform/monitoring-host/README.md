# `monitoring-host` — Tokyo EC2 instance for the prod monitoring stack

Runs Prometheus + Grafana + Alertmanager on its own host inside the
same VPC as `tape-host`. Operator access via Tailscale tailnet — no
public Grafana port, no SG rule for the operator IP after bootstrap.

## Architecture

```
   operator (laptop, on tailnet)
   ─────────────────────────────                       ap-northeast-1
                                                        ──────────────
                                              ┌────────────────────┐
       browser via tailnet ───────────────▶   │  monitor-tokyo-01  │
       http://bpt-monitor-tokyo-01:3000      │  (this module)      │
                                             │  Grafana   :3000   │
                                             │  Prometheus :9090  │
                                             │  Alertmgr   :9093  │
                                             │  EBS 50G (tsdb)    │
                                             └─────────┬──────────┘
                                                       │
                                          same-VPC scrape via private IP
                                                       │
                                             ┌─────────▼──────────┐
                                             │  tape-hl-tokyo-01  │
                                             │  bpt-tape    :9111 │
                                             │  node-exporter:9100│
                                             └────────────────────┘
```

## Provisions

- EC2 t3.small, Ubuntu 24.04, 30 GB root EBS
- 50 GB gp3 EBS attached at `/opt/bpt-monitoring/data` (Prometheus tsdb + Grafana state)
- EIP for stable bootstrap-time SSH
- Security group: SSH from `operator_ssh_cidr` (drop after Tailscale enrollment)
- Cross-SG ingress on `tape-host`'s SG: ports 9111 + 9100 from this module's SG (so Prometheus can scrape the recorder)
- Cloud-init: docker-engine, tailscale, basic packages, data-disk format/mount

Joins the **existing** `tape-host` VPC (looked up by tag) — does not
create its own VPC. Both modules can be applied independently.

## Apply

Prerequisite: `tape-host` module is already applied (this module looks
up its VPC and SG).

```bash
cd infra/terraform/monitoring-host

# Init state (uses the same backend bucket as tape-host).
terraform init

# Look up your operator IP for SSH:
MY_IP=$(curl -s https://api.ipify.org)

terraform apply \
    -var "operator_ssh_cidr=${MY_IP}/32" \
    -var "ssh_public_key=$(cat ~/.ssh/id_ed25519.pub)"
```

Plan should show ~7 resources to create. Apply takes 2–3 minutes.

After apply, the relevant outputs:

```bash
terraform output public_ip                # SSH here for bootstrap
terraform output tape_host_private_ip     # pass this to bootstrap.sh
```

## Post-apply: bootstrap

Cloud-init handles docker + tailscale install + data-disk format. The
actual stack startup is the operator's job, via `bootstrap.sh`:

```bash
TAPE_IP=$(terraform output -raw tape_host_private_ip)
MONITOR_EIP=$(terraform output -raw public_ip)

# 1. Copy monitoring stack files + bootstrap script to the host
scp -i ~/.ssh/id_ed25519 \
    -r ../../../monitoring \
    bootstrap.sh prometheus.yml.tmpl \
    ubuntu@${MONITOR_EIP}:/tmp/

# 2. SSH and run bootstrap
ssh -i ~/.ssh/id_ed25519 ubuntu@${MONITOR_EIP} \
    "sudo bash /tmp/bootstrap.sh ${TAPE_IP}"
```

`bootstrap.sh` walks through:

1. `tailscale up` — produces a browser link, click it on your laptop
   (you're already signed into your tailnet) → device auto-approved
2. Lay out monitoring stack files at `/opt/bpt-monitoring/stack`
3. Render `prometheus.yml` with the tape host's private IP
4. Patch the docker-compose to bind tsdb directories on the data EBS
5. `docker compose up -d`
6. Smoke test all targets

After it returns successfully, the stack is live. Verify from your
laptop's browser (already on the tailnet):

- `http://bpt-monitor-tokyo-01:3000` → Grafana (admin / admin — change immediately)
- BPT folder → BPT Tape dashboard → all panels populated with real
  recorder + tape-host node-exporter data

## Lock down (after Tailscale is verified)

Once you're confident Tailscale access works:

1. **Drop public SSH access** — change `operator_ssh_cidr` to a closed
   value (e.g. `127.0.0.1/32`) and re-apply. You'll still SSH via
   Tailscale: `ssh ubuntu@bpt-monitor-tokyo-01`.

   ```bash
   terraform apply -var "operator_ssh_cidr=127.0.0.1/32" -var "ssh_public_key=..."
   ```

2. **Change Grafana admin password.** First login at
   `http://bpt-monitor-tokyo-01:3000` prompts for a new password —
   set it via your password manager, store with the `infra@` account
   credentials.

3. **Tear down the laptop's redundant local stack** (now monitoring
   prod 24/7 on its own host):

   ```bash
   cd ~/code/bpt-core/monitoring && docker compose down
   pkill -f "ssh.*-L 0.0.0.0:911[12]"
   pkill -f "ssh.*-L 0.0.0.0:9120"
   ```

## Sizing

| Workload | Default | Why |
|---|---|---|
| CPU | 2 vCPU (`t3.small`) | Prometheus + Grafana + Alertmanager fit comfortably; ~5–15% steady CPU for current scrape set |
| RAM | 2 GB | Prometheus ~300 MB, Grafana ~150 MB, Alertmanager ~50 MB; plenty of headroom for filesystem cache |
| Root disk | 30 GB gp3 | OS + docker images |
| Data disk | 50 GB gp3 | tsdb at 15s scrape × 5 services × 30d retention ≈ 5–10 GB; 50 GB gives 6+ months runway |
| Bandwidth | n/a | Scrape traffic is tiny (~50 KB/s); browser sessions even smaller |

Bump to `t3.medium` if you start scraping many more services, or push
retention past 60d.

## Cost (on-demand, ap-northeast-1)

- t3.small 24/7: ~$15/month
- 80 GB gp3 (root + data): ~$8/month
- Elastic IP (attached): $0
- Same-region scrape traffic: $0
- Tailscale (free tier): $0
- **~$23/month before any RI/Savings Plan optimization**

## Security caveats

- **Bootstrap-time SSH is on the public internet** (port 22 open to
  `operator_ssh_cidr`). Drop after Tailscale is confirmed working —
  the lock-down step above.
- **Default Grafana admin password is admin/admin** until you change
  it on first login. Do this in the same session as the bootstrap.
- **Tailscale auth** uses your tailnet identity (`infra@bishanparktrading.com`).
  Hardware-key MFA on that Google account is a hard requirement.
- **No backup of the tsdb EBS** — by design, since metrics are
  observational, not authoritative. If the volume dies, you lose
  history but not state. Snapshot before any major upgrade if you
  want to be cautious.
- **No public Grafana exposure** — the architectural difference vs the
  laptop dev stack. Operator access is solely via tailnet.

## Future hardening

- Move SG ingress on tape-host's 9111/9100 to **only** this module's SG
  (already done in main.tf). Drop tape-host's public SSH too once both
  hosts are tailscale-enrolled.
- Provision an IAM role for the monitor instance with read-only access
  to CloudWatch + EC2 metadata; ship Prometheus alertmanager to PagerDuty
  via the existing integration key.
- Consider an ALB + ACM cert if you want HTTPS Grafana via tailnet
  hostname (currently HTTP). Tailscale's `serve` feature also handles
  this if you prefer that route.
