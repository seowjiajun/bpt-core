// ── AMI lookup ─────────────────────────────────────────────────────────────
data "aws_ami" "ubuntu" {
  most_recent = true
  owners      = [var.ami_owner]

  filter {
    name   = "name"
    values = [var.ami_name_pattern]
  }
  filter {
    name   = "virtualization-type"
    values = ["hvm"]
  }
  filter {
    name   = "architecture"
    values = ["x86_64"]
  }
}

// ── Discover tape-host VPC + subnet + SG ───────────────────────────────────
// Looks up resources created by the sibling tape-host module. We don't
// import them — we just reference them by tag (VPC, SG) and AZ (subnet).
// This keeps the two modules independently appliable: tape-host can be
// destroyed and recreated and monitoring-host re-plans without churn.
data "aws_vpc" "tape" {
  filter {
    name   = "tag:Name"
    values = [var.tape_host_vpc_name_tag]
  }
}

data "aws_subnet" "tape" {
  vpc_id            = data.aws_vpc.tape.id
  availability_zone = var.availability_zone
}

data "aws_security_group" "tape" {
  filter {
    name   = "vpc-id"
    values = [data.aws_vpc.tape.id]
  }
  filter {
    name   = "group-name"
    values = [var.tape_host_sg_name]
  }
}

// Look up the tape instance itself so we can output its private IP for
// prometheus.yml templating.
data "aws_instance" "tape" {
  filter {
    name   = "tag:Name"
    values = ["bpt-tape"]
  }
  filter {
    name   = "instance-state-name"
    values = ["running"]
  }
}

// ── Security group for monitor host ────────────────────────────────────────
// Inbound: SSH from operator IP (bootstrap only — drop once Tailscale is
//          confirmed working). Grafana port 3000 is NOT exposed publicly;
//          the operator reaches it via Tailscale tailnet (100.x.y.z).
// Egress: all (Tailscale coordination, package updates, Prometheus
//          scrape over the VPC, Grafana plugin downloads).
resource "aws_security_group" "monitor" {
  name        = "bpt-monitor-sg"
  description = "Monitoring host: bootstrap SSH from operator, Tailscale-only access for Grafana."
  vpc_id      = data.aws_vpc.tape.id

  ingress {
    description = "Bootstrap SSH from operator IP. Drop after Tailscale enrollment is verified."
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.operator_ssh_cidr]
  }

  egress {
    description = "All egress (Tailscale, scrape targets, apt, Docker Hub)."
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = {
    Name      = "bpt-monitor-sg"
    Role      = "monitor"
    Env       = "prod"
    Owner     = "jseow"
    ManagedBy = "terraform-monitoring-host"
  }
}

// ── Cross-SG ingress: tape-host accepts scrape from monitor host ──────────
// Added here (not in tape-host module) so tape-host stays unchanged. The
// rule references SGs by ID, not IP — robust against either SG getting
// recreated.
resource "aws_security_group_rule" "tape_ingress_bpt_tape_metrics" {
  type                     = "ingress"
  from_port                = 9111
  to_port                  = 9111
  protocol                 = "tcp"
  security_group_id        = data.aws_security_group.tape.id
  source_security_group_id = aws_security_group.monitor.id
  description              = "bpt-tape /metrics scraped by monitor host"
}

resource "aws_security_group_rule" "tape_ingress_node_exporter" {
  type                     = "ingress"
  from_port                = 9100
  to_port                  = 9100
  protocol                 = "tcp"
  security_group_id        = data.aws_security_group.tape.id
  source_security_group_id = aws_security_group.monitor.id
  description              = "tape host node-exporter scraped by monitor host"
}

// ── SSH key pair ───────────────────────────────────────────────────────────
resource "aws_key_pair" "operator" {
  key_name   = "bpt-monitor-operator"
  public_key = var.ssh_public_key
}

// ── EC2 instance ───────────────────────────────────────────────────────────
resource "aws_instance" "monitor" {
  ami                         = data.aws_ami.ubuntu.id
  instance_type               = var.instance_type
  subnet_id                   = data.aws_subnet.tape.id
  vpc_security_group_ids      = [aws_security_group.monitor.id]
  key_name                    = aws_key_pair.operator.key_name
  associate_public_ip_address = true

  // Root volume small — system + docker images. Prometheus tsdb lives on
  // the separate data EBS so OS rebuilds don't lose history.
  root_block_device {
    volume_size           = 30
    volume_type           = "gp3"
    delete_on_termination = true
    encrypted             = true
  }

  // Cloud-init keeps to bare essentials: docker (for the monitoring
  // stack), tailscale (for operator access), basic tooling. The actual
  // monitoring stack startup is in bootstrap.sh which the operator runs
  // post-apply (after they've SCP'd monitoring/ + bootstrap.sh).
  //
  // We deliberately do NOT put the Tailscale auth key in user-data —
  // it would be base64-readable by anyone with EC2 metadata access. The
  // operator runs `sudo tailscale up` interactively during bootstrap
  // and authenticates via browser link.
  user_data = <<-EOF
    #!/bin/bash
    set -euxo pipefail
    apt-get update
    apt-get install -y curl jq htop ca-certificates gnupg

    # Docker Engine (official Docker apt repo, not Ubuntu's older snap)
    install -m 0755 -d /etc/apt/keyrings
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
         | gpg --dearmor -o /etc/apt/keyrings/docker.gpg
    chmod a+r /etc/apt/keyrings/docker.gpg
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
          https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo "$${VERSION_CODENAME}") stable" \
         > /etc/apt/sources.list.d/docker.list
    apt-get update
    apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
    usermod -aG docker ubuntu

    # Tailscale (apt repo). Daemon installs but doesn't auto-up — operator
    # runs `sudo tailscale up` from bootstrap.sh, which produces a browser
    # link they click to authenticate as their tailnet identity.
    curl -fsSL https://pkgs.tailscale.com/stable/ubuntu/noble.noarmor.gpg \
         | tee /usr/share/keyrings/tailscale-archive-keyring.gpg >/dev/null
    curl -fsSL https://pkgs.tailscale.com/stable/ubuntu/noble.tailscale-keyring.list \
         | tee /etc/apt/sources.list.d/tailscale.list
    apt-get update
    apt-get install -y tailscale

    # Find + format + mount the data EBS for Prometheus tsdb. Same
    # discovery pattern as tape-host: probe for the first non-root
    # "disk" device. The earlier awk-on-MOUNTPOINT approach silently
    # missed every disk on Ubuntu 24.04 — see commit history.
    ROOT_PART=$(findmnt -no SOURCE /)
    ROOT_DISK="/dev/$(lsblk -no PKNAME "$ROOT_PART" | head -1)"
    DATA_DEV=""
    for _ in $(seq 1 60); do
      for dev in $(lsblk -dnp -o NAME,TYPE | awk '$2 == "disk" {print $1}'); do
        if [ "$dev" != "$ROOT_DISK" ]; then
          DATA_DEV=$dev
          break 2
        fi
      done
      sleep 2
    done
    if [ -n "$DATA_DEV" ]; then
      mkfs.ext4 -F -L bpt-monitor-data "$DATA_DEV"
      mkdir -p /opt/bpt-monitoring/data
      echo 'LABEL=bpt-monitor-data /opt/bpt-monitoring/data ext4 defaults,noatime 0 2' >> /etc/fstab
      mount /opt/bpt-monitoring/data
      mkdir -p /opt/bpt-monitoring/data/prometheus /opt/bpt-monitoring/data/grafana
      chown -R ubuntu:ubuntu /opt/bpt-monitoring
    else
      echo "WARNING: no unmounted data disk found — manual mount required" >&2
    fi
  EOF

  tags = {
    Name      = "bpt-monitor-tokyo-01"
    Role      = "monitor"
    Env       = "prod"
    Owner     = "jseow"
    ManagedBy = "terraform-monitoring-host"
  }
}

// ── Data EBS for Prometheus tsdb ──────────────────────────────────────────
resource "aws_ebs_volume" "data" {
  availability_zone = var.availability_zone
  size              = var.data_disk_gb
  type              = "gp3"
  encrypted         = true
  tags = {
    Name      = "bpt-monitor-data"
    Role      = "monitor-data"
    Env       = "prod"
    Owner     = "jseow"
    ManagedBy = "terraform-monitoring-host"
  }
}

resource "aws_volume_attachment" "data" {
  device_name = "/dev/sdf"
  volume_id   = aws_ebs_volume.data.id
  instance_id = aws_instance.monitor.id
}

// ── Elastic IP ─────────────────────────────────────────────────────────────
// Stable address for bootstrap-time SSH. Once Tailscale is verified
// working, the EIP is mostly cosmetic — operator hits the host via its
// 100.x.y.z tailnet IP. Keep it for ops debugging convenience.
resource "aws_eip" "monitor" {
  instance = aws_instance.monitor.id
  domain   = "vpc"
  tags = {
    Name      = "bpt-monitor-eip"
    Role      = "monitor"
    Env       = "prod"
    Owner     = "jseow"
    ManagedBy = "terraform-monitoring-host"
  }
}
