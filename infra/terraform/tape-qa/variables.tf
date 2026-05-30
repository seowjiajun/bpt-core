variable "aws_region" {
  description = "Region for the QA harness. Tokyo — same as the prod recorder, so capture latency/topology matches."
  type        = string
  default     = "ap-northeast-1"
}

variable "availability_zone" {
  description = "AZ for the ephemeral QA instance."
  type        = string
  default     = "ap-northeast-1a"
}

variable "qa_bucket_name" {
  description = "Persistent QA bucket. Per-run prefixes (raw/<run-id>/, verdict/<run-id>.json, progress/<run-id>.json) expire via lifecycle — the data 'tears down' even though the bucket persists."
  type        = string
  default     = "bpt-tape-qa"
}

variable "qa_retention_days" {
  description = "Lifecycle expiry for everything in the QA bucket. QA data is disposable."
  type        = number
  default     = 7
}

variable "releases_bucket_name" {
  description = "Release-tarball bucket the QA box pulls from (same artifact prod deploys). From the releases-storage module."
  type        = string
  // No default — must match your releases bucket (e.g. bpt-releases).
}

variable "notify_email" {
  description = "Email subscribed to the SNS topic — the orchestration-level safety-net channel (dead box / timeout / launch error). Confirm the subscription once after first apply."
  type        = string
  // No default — operator must supply.
}

variable "loki_url" {
  description = "Push endpoint for promtail on the QA box (your central Loki, e.g. http://<monitor-host>:3100/loki/api/v1/push). Empty disables Loki shipping."
  type        = string
  default     = ""
}

variable "ntfy_url" {
  description = "ntfy.sh topic URL the box curls on normal pass/fail. Empty disables the phone ping."
  type        = string
  default     = ""
  sensitive   = true
}

variable "default_instance_type" {
  description = "Default for the SSM form. t3.micro DELIBERATELY — matches prod; the 2026-05-30 OOM was instance-size-specific and a bigger box would hide it."
  type        = string
  default     = "t3.micro"
}

variable "ami_owner" {
  description = "AMI owner. 099720109477 = Canonical (Ubuntu)."
  type        = string
  default     = "099720109477"
}

variable "ami_name_pattern" {
  description = "AMI name pattern — latest Ubuntu 24.04 LTS amd64, matching the prod recorder."
  type        = string
  default     = "ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*"
}
