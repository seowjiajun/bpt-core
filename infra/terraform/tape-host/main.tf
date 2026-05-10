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

// ── Network: minimal VPC just for this host ────────────────────────────────
// Single public subnet. Real prod-grade networking (private subnets +
// NAT + bastion / SSM-only) is a future refactor — irrelevant for a
// passive WS recorder that only needs egress to venue endpoints + S3.
resource "aws_vpc" "tape" {
  cidr_block           = "10.42.0.0/16"
  enable_dns_hostnames = true
  enable_dns_support   = true
  tags                 = { Name = "bpt-tape-vpc" }
}

resource "aws_internet_gateway" "tape" {
  vpc_id = aws_vpc.tape.id
  tags   = { Name = "bpt-tape-igw" }
}

resource "aws_subnet" "tape" {
  vpc_id                  = aws_vpc.tape.id
  cidr_block              = "10.42.1.0/24"
  availability_zone       = var.availability_zone
  map_public_ip_on_launch = true
  tags                    = { Name = "bpt-tape-subnet" }
}

resource "aws_route_table" "tape" {
  vpc_id = aws_vpc.tape.id

  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.tape.id
  }
  tags = { Name = "bpt-tape-rt" }
}

resource "aws_route_table_association" "tape" {
  subnet_id      = aws_subnet.tape.id
  route_table_id = aws_route_table.tape.id
}

// ── Security group: SSH from operator, egress all ──────────────────────────
resource "aws_security_group" "tape" {
  name        = "bpt-tape-sg"
  description = "Tape recording host: SSH inbound from operator, full egress."
  vpc_id      = aws_vpc.tape.id

  ingress {
    description = "SSH from operator IP"
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.operator_ssh_cidr]
  }

  egress {
    description = "All egress (venue WS + S3 + apt updates)"
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "bpt-tape-sg" }
}

// ── SSH key pair ───────────────────────────────────────────────────────────
resource "aws_key_pair" "operator" {
  key_name   = "bpt-tape-operator"
  public_key = var.ssh_public_key
}

// ── EC2 instance ───────────────────────────────────────────────────────────
resource "aws_instance" "tape" {
  ami                         = data.aws_ami.ubuntu.id
  instance_type               = var.instance_type
  subnet_id                   = aws_subnet.tape.id
  vpc_security_group_ids      = [aws_security_group.tape.id]
  key_name                    = aws_key_pair.operator.key_name
  associate_public_ip_address = true

  // Root volume small — system only. Data goes on a separate EBS that
  // can be detached/snapshotted independently of the OS.
  root_block_device {
    volume_size           = 30
    volume_type           = "gp3"
    delete_on_termination = true
    encrypted             = true
  }

  // Bare cloud-init: just enough to make the box mount /opt/bpt/data
  // and have rclone pre-installed. App deploy is a separate concern
  // (deploy.sh over SSH or future Ansible playbook).
  //
  // Device-name caveat: on Nitro instances (t3, m5, c5+ generations)
  // the kernel exposes EBS as /dev/nvmeXn1, NOT the /dev/sdf alias we
  // pass to AWS in volume_attachment.device_name. We discover the data
  // disk by finding the first unmounted block device, which is robust
  // across both Nitro and the older Xen-based instance types.
  user_data = <<-EOF
    #!/bin/bash
    set -euxo pipefail
    apt-get update
    apt-get install -y rclone zstd jq curl htop

    # Find the data EBS. The previous awk-on-MOUNTPOINT pattern was
    # fragile: lsblk collapses adjacent whitespace, so a row with empty
    # MOUNTPOINT had its TYPE column shifted into $2 and the filter
    # missed every disk. Discover the root device first, then pick the
    # first non-root "disk" device.
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
      mkfs.ext4 -F -L bpt-data "$DATA_DEV"
      mkdir -p /opt/bpt/data
      echo 'LABEL=bpt-data /opt/bpt/data ext4 defaults,noatime 0 2' >> /etc/fstab
      mount /opt/bpt/data
      mkdir -p /opt/bpt/data/raw /opt/bpt/data/backtest-cache
      chown -R ubuntu:ubuntu /opt/bpt
    else
      echo "WARNING: no unmounted data disk found — manual mount required" >&2
    fi
  EOF

  tags = { Name = "bpt-tape" }
}

// ── Data EBS: separate volume so OS rebuilds don't touch tape data ─────────
resource "aws_ebs_volume" "data" {
  availability_zone = var.availability_zone
  size              = var.data_disk_gb
  type              = "gp3"
  encrypted         = true
  tags              = { Name = "bpt-tape-data" }
}

resource "aws_volume_attachment" "data" {
  device_name = "/dev/sdf"
  volume_id   = aws_ebs_volume.data.id
  instance_id = aws_instance.tape.id
}

// ── Elastic IP: stable address across instance restarts ────────────────────
resource "aws_eip" "tape" {
  instance = aws_instance.tape.id
  domain   = "vpc"
  tags     = { Name = "bpt-tape-eip" }
}
