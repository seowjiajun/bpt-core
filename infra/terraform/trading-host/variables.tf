variable "aws_region" {
  description = "Region for the trading host. ap-southeast-1 (Singapore) by default — same ~5 ms CloudFront RTT to HL as Tokyo, ~5× lower SSH latency from a Singapore operator."
  type        = string
  default     = "ap-southeast-1"
}

variable "availability_zone" {
  description = "Specific AZ within aws_region. Lock to one so EBS + EC2 stay co-located."
  type        = string
  default     = "ap-southeast-1a"
}

variable "instance_type" {
  description = "EC2 instance type. c7i.2xlarge (8 vCPU / 16 GB) is the QA default: non-burstable, fits the hot-thread set with headroom. NEVER use t3/t4g — credit exhaustion will throttle hot threads mid-trade. Bump to c7i.4xlarge for prod-shape multi-strategy."
  type        = string
  default     = "c7i.2xlarge"
}

variable "data_disk_gb" {
  description = "EBS volume at /opt/bpt — holds the source repo + bazel cache + binaries + logs. 50 GB is plenty (bazel cache for this repo is ~10-15 GB, logs grow ~50 MB/day)."
  type        = number
  default     = 50
}

variable "operator_ssh_cidr" {
  description = "CIDR allowed to SSH (port 22). Set to your operator IP /32. NEVER 0.0.0.0/0 in prod — switch to Tailscale-only once enrollment is verified."
  type        = string
  // No default — operator MUST supply.
}

variable "ssh_public_key" {
  description = "Operator's SSH public key for the EC2 key pair. Paste contents of ~/.ssh/id_ed25519.pub or similar."
  type        = string
  // No default — operator MUST supply.
}

variable "ami_owner" {
  description = "Owner ID for the AMI lookup. 099720109477 = Canonical (Ubuntu)."
  type        = string
  default     = "099720109477"
}

variable "ami_name_pattern" {
  description = "AMI name pattern. Defaults to the latest Ubuntu 24.04 LTS amd64 (HVM, gp3)."
  type        = string
  default     = "ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*"
}

variable "owner_tag" {
  description = "Owner tag stamped on all resources for cost-attribution."
  type        = string
  default     = "jseow"
}

variable "env_tag" {
  description = "Env tag — dev / qa / test / prod. Drives downstream alerting policy AND appears in the hostname."
  type        = string
  default     = "test"
}

variable "loc_code" {
  description = "Location code for the hostname. sg1 = AWS ap-southeast-1, ty1 = AWS ap-northeast-1. Equinix-style 3-char codes — see feedback-host-naming memory."
  type        = string
  default     = "sg1"
}

variable "host_role" {
  description = "Role segment of the hostname. trd = trading. Other roles (mon, rec, rsh, bst) live in their own modules; this var exists so future module reuse stays explicit."
  type        = string
  default     = "trd"
}

variable "host_seq" {
  description = "Per-role sequence number, zero-padded to 2 digits. First host of a role = 1."
  type        = number
  default     = 1
}

variable "auto_stop_cron" {
  description = "EventBridge cron expression for the daily auto-stop. Default is 23:00 SGT (15:00 UTC). Set to empty string to disable the schedule entirely."
  type        = string
  default     = "cron(0 15 * * ? *)"
}

variable "auto_stop_enabled" {
  description = "Whether the daily auto-stop schedule is active. Set to false during long-running tests where you actually want to keep the instance up overnight."
  type        = bool
  default     = true
}
