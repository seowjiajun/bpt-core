# Trading host (AWS Singapore)

Terraform module for an on-demand QA trading host in `ap-southeast-1`. Mirrors the `tape-host/` pattern; lives independently so you can spin up / tear down without touching the recorder.

## Why Singapore (not Tokyo)

HL's public API is fronted by AWS CloudFront — anycast, ~5 ms from any well-peered region. Singapore vs Tokyo: identical venue RTT, ~5× lower SSH latency from a Singapore operator. The tape host stays in Tokyo because its workload is unattended after deploy.

## First-time setup

```bash
cd infra/terraform/trading-host

# tfvars are gitignored — derive yours from the template
cp terraform.tfvars.example terraform.tfvars
# edit terraform.tfvars: update operator_ssh_cidr if your IP differs

terraform init                # downloads providers, configures remote state
terraform plan                # review what will be created
terraform apply               # ~3 min: VPC + EC2 + EBS + EIP
```

After apply, terraform prints:

```
ssh_command   = "ssh -i ~/.ssh/id_ed25519 ubuntu@<eip>"
stop_command  = "aws ec2 stop-instances --instance-ids i-... --region ap-southeast-1"
start_command = "aws ec2 start-instances --instance-ids i-... --region ap-southeast-1"
```

Run the bootstrap (first time only):

```bash
EIP=$(terraform output -raw public_ip)
scp bootstrap.sh ubuntu@$EIP:/tmp/
ssh ubuntu@$EIP 'bash /tmp/bootstrap.sh'   # ~15-20 min: apt deps + bazel build
```

Bootstrap drops you back at a shell once binaries are built and systemd units are in place. Then SSH in interactively:

```bash
ssh -i ~/.ssh/id_ed25519 ubuntu@$EIP

# 1. Enroll in Tailscale (browser auth flow)
sudo tailscale up

# 2. Drop in HL credentials
mkdir -p ~/.bpt-secrets
cat > ~/.bpt-secrets/bpt-testnet-HYPERLIQUID <<'EOF'
HYPERLIQUID_PRIVATE_KEY=0x<your-agent-key>
HYPERLIQUID_WALLET_ADDRESS=0x<your-wallet>
EOF
chmod 0600 ~/.bpt-secrets/bpt-testnet-HYPERLIQUID

# 3. Start the stack
systemctl --user start bpt-stack.target

# 4. Watch logs
journalctl --user -fu bpt-strategy
```

## On-demand QA pattern

The whole point of this module: provision when testing, deprovision when not.

```bash
# Pause (keeps EBS + EIP, ~$3-5/mo idle):
aws ec2 stop-instances --instance-ids $(terraform output -raw instance_id) \
    --region ap-southeast-1

# Resume (~30 s boot — data EBS already populated):
aws ec2 start-instances --instance-ids $(terraform output -raw instance_id) \
    --region ap-southeast-1

# Nuke entirely (releases EIP, deletes EBS — data lost):
terraform destroy
```

Cost ballpark at QA cadence (4 hr/day):
- c7i.2xlarge compute: ~$54/mo
- 50 GB gp3 + 30 GB root: ~$6.40/mo
- EIP held while stopped: ~$3.60/mo (skippable — release between sessions)
- **Total: ~$65/mo** (well under $100 of promo credits per month)

## Switching strategies / envs

```bash
cd /opt/bpt/code/bpt-core
./deploy/switch-env.sh dev-hyperliquid             # AvellanedaStoikov on XMR
./deploy/switch-env.sh dev-hyperliquid-funding-arb # FundingArb on PURR
./deploy/switch-env.sh prod-hyperliquid            # MAINNET — real money
systemctl --user daemon-reload
systemctl --user restart bpt-stack.target
```

## What's NOT in this module (intentional)

- **IAM role for any runtime AWS fetch** — instrument mapping is loaded from
  `config/instruments/` in the cloned repo. Pulling it from S3 at startup
  would introduce a hard dependency on S3 reachability + correct IAM, and
  break service startup on any cross-region / credential glitch. The repo
  is the source of truth; S3 archives are for backup/distribution of
  derived artifacts (recordings, snapshots), not for the live load path.
- **CloudWatch agent** — Prometheus on the monitor host already scrapes; CW would duplicate cost without value.
- **VPC peering to the Tokyo tape VPC** — cross-region metrics scrape goes over Tailscale, no peering needed.
- **AMI baking** — for faster repeated provisioning, snapshot the instance after bootstrap (`aws ec2 create-image`) and parameterize `ami_name_pattern` to your snapshot. Worth doing once you're tearing down/recreating regularly.

## Pre-flight checklist

- [ ] AWS account has remaining promo credits (check via `aws billing` or the console)
- [ ] `_bootstrap/` module already applied (state bucket exists)
- [ ] `~/.ssh/id_ed25519.pub` matches `terraform.tfvars` → `ssh_public_key`
- [ ] Operator residential IP in `terraform.tfvars` → `operator_ssh_cidr` is current
- [ ] HL testnet wallet funded with USDC + (PURR for FA / XMR for AS / etc.) before starting the stack
