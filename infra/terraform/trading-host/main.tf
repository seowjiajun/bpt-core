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

// ── Network: standalone VPC in Singapore ───────────────────────────────────
// Trading host is independently appliable from tape-host (which lives in
// Tokyo). Single public subnet, single AZ — matches the tape-host shape.
// Cross-region peering to the Tokyo monitor host happens over Tailscale,
// not VPC peering, so we don't need anything fancier here.
resource "aws_vpc" "trading" {
  cidr_block           = "10.43.0.0/16"
  enable_dns_hostnames = true
  enable_dns_support   = true
  tags = {
    Name      = "bpt-trading-vpc"
    Role      = "trading"
    Env       = var.env_tag
    Owner     = var.owner_tag
    ManagedBy = "terraform-trading-host"
  }
}

resource "aws_internet_gateway" "trading" {
  vpc_id = aws_vpc.trading.id
  tags = {
    Name      = "bpt-trading-igw"
    Owner     = var.owner_tag
    ManagedBy = "terraform-trading-host"
  }
}

resource "aws_subnet" "trading" {
  vpc_id                  = aws_vpc.trading.id
  cidr_block              = "10.43.1.0/24"
  availability_zone       = var.availability_zone
  map_public_ip_on_launch = true
  tags = {
    Name      = "bpt-trading-subnet"
    Owner     = var.owner_tag
    ManagedBy = "terraform-trading-host"
  }
}

resource "aws_route_table" "trading" {
  vpc_id = aws_vpc.trading.id

  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.trading.id
  }
  tags = {
    Name      = "bpt-trading-rt"
    Owner     = var.owner_tag
    ManagedBy = "terraform-trading-host"
  }
}

resource "aws_route_table_association" "trading" {
  subnet_id      = aws_subnet.trading.id
  route_table_id = aws_route_table.trading.id
}

// ── Security group ─────────────────────────────────────────────────────────
// Inbound: SSH from operator IP (bootstrap; drop once Tailscale verified).
// Egress: all (venue WS/REST, apt mirrors, github clone, Tailscale).
// Notably NOT S3: the trading stack loads instrument_mapping from the
// cloned repo on local disk, not from S3 — a runtime S3 fetch would make
// startup brittle against credential / region / network glitches.
resource "aws_security_group" "trading" {
  name        = "bpt-trading-sg"
  description = "Trading host: SSH inbound from operator, full egress."
  vpc_id      = aws_vpc.trading.id

  ingress {
    description = "Bootstrap SSH from operator IP. Drop after Tailscale enrollment is verified."
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.operator_ssh_cidr]
  }

  egress {
    description = "All egress (venue API + S3 + apt + GitHub + Tailscale)."
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = {
    Name      = "bpt-trading-sg"
    Role      = "trading"
    Env       = var.env_tag
    Owner     = var.owner_tag
    ManagedBy = "terraform-trading-host"
  }
}

// ── SSH key pair ───────────────────────────────────────────────────────────
resource "aws_key_pair" "operator" {
  key_name   = "bpt-trading-operator"
  public_key = var.ssh_public_key
}

// ── EC2 instance ───────────────────────────────────────────────────────────
// SMT disabled at launch (threads_per_core=1) so hot-thread pinning lands
// on physical cores. core_count derived from the instance type — c7i.2xlarge
// has 4 physical cores (8 vCPU at 2 threads/core, we keep 4 at 1).
resource "aws_instance" "trading" {
  ami                         = data.aws_ami.ubuntu.id
  instance_type               = var.instance_type
  subnet_id                   = aws_subnet.trading.id
  vpc_security_group_ids      = [aws_security_group.trading.id]
  key_name                    = aws_key_pair.operator.key_name
  associate_public_ip_address = true

  cpu_options {
    core_count       = 4 // c7i.2xlarge = 4 physical cores
    threads_per_core = 1 // disable SMT at the hypervisor — cleaner than fighting it in-guest
  }

  // Root volume holds OS + apt cache + bazel cache. Bazel's incremental
  // cache fits comfortably alongside the OS on 30 GB.
  root_block_device {
    volume_size           = 30
    volume_type           = "gp3"
    delete_on_termination = true
    encrypted             = true
  }

  // Cloud-init keeps to bare essentials: apt deps, mount data EBS, install
  // Tailscale (daemon only — operator runs `tailscale up` interactively),
  // clone the repo. The heavy lifting (bazel build, systemd units) lives
  // in bootstrap.sh which the operator runs over SSH post-apply.
  //
  // We deliberately do NOT put credentials, Tailscale auth keys, or env
  // files in user-data — anyone with EC2 metadata access could read them.
  user_data = <<-EOF
    #!/bin/bash
    set -euxo pipefail
    apt-get update
    apt-get install -y curl jq htop git ca-certificates gnupg unzip build-essential

    # Tailscale apt repo (daemon only — operator runs `tailscale up` post-apply).
    curl -fsSL https://pkgs.tailscale.com/stable/ubuntu/noble.noarmor.gpg \
         | tee /usr/share/keyrings/tailscale-archive-keyring.gpg >/dev/null
    curl -fsSL https://pkgs.tailscale.com/stable/ubuntu/noble.tailscale-keyring.list \
         | tee /etc/apt/sources.list.d/tailscale.list
    apt-get update
    apt-get install -y tailscale

    # Mount data EBS at /opt/bpt. Same Nitro-aware discovery the tape-host
    # uses — probe for the first non-root "disk" block device.
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
      # Only format if there's no filesystem already — preserves data
      # across instance recreation when the EBS is reattached.
      if ! blkid "$DATA_DEV" >/dev/null 2>&1; then
        mkfs.ext4 -F -L bpt-trading-data "$DATA_DEV"
      fi
      mkdir -p /opt/bpt
      echo 'LABEL=bpt-trading-data /opt/bpt ext4 defaults,noatime 0 2' >> /etc/fstab
      mount /opt/bpt
      mkdir -p /opt/bpt/code /opt/bpt/data /opt/bpt/logs
      chown -R ubuntu:ubuntu /opt/bpt
    else
      echo "WARNING: no unmounted data disk found — manual mount required" >&2
    fi

    # Drop a marker the operator's bootstrap.sh can poll on.
    echo "cloud-init complete at $(date -Iseconds)" > /var/log/bpt-cloud-init.done
  EOF

  tags = {
    Name      = "bpt-trading-sg-01"
    Role      = "trading"
    Env       = var.env_tag
    Owner     = var.owner_tag
    ManagedBy = "terraform-trading-host"
  }
}

// ── Data EBS ───────────────────────────────────────────────────────────────
// Persistent across instance stop/start cycles. Holds the source repo,
// bazel cache, binaries, logs — so a stopped+restarted instance returns
// fully provisioned without re-running bootstrap.sh.
resource "aws_ebs_volume" "data" {
  availability_zone = var.availability_zone
  size              = var.data_disk_gb
  type              = "gp3"
  encrypted         = true
  tags = {
    Name      = "bpt-trading-data"
    Role      = "trading-data"
    Env       = var.env_tag
    Owner     = var.owner_tag
    ManagedBy = "terraform-trading-host"
  }
}

resource "aws_volume_attachment" "data" {
  device_name = "/dev/sdf"
  volume_id   = aws_ebs_volume.data.id
  instance_id = aws_instance.trading.id
}

// ── Elastic IP ─────────────────────────────────────────────────────────────
// Stable address across instance stop/start. Trading host doesn't run a
// public service, so the EIP is purely for operator SSH convenience.
// Tailscale gives a private 100.x.y.z address once enrolled; the EIP is
// the fallback if Tailscale ever breaks.
resource "aws_eip" "trading" {
  instance = aws_instance.trading.id
  domain   = "vpc"
  tags = {
    Name      = "bpt-trading-eip"
    Role      = "trading"
    Env       = var.env_tag
    Owner     = var.owner_tag
    ManagedBy = "terraform-trading-host"
  }
}
