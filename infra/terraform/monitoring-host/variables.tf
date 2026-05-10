variable "aws_region" {
  description = "Region for the monitoring host. Must match tape-host (Tokyo) so VPC-internal scraping works without inter-region traffic."
  type        = string
  default     = "ap-northeast-1"
}

variable "availability_zone" {
  description = "AZ within aws_region. Match the tape-host AZ so EBS placement is tidy and inter-AZ network charges don't apply to scrape traffic."
  type        = string
  default     = "ap-northeast-1a"
}

variable "instance_type" {
  description = "EC2 instance type. Prometheus + Grafana + Alertmanager fit comfortably in 2 GB; t3.small is the sweet spot. Bump to t3.medium if retention grows past ~30d or you add many more services to scrape."
  type        = string
  default     = "t3.small"
}

variable "data_disk_gb" {
  description = "Size of the EBS volume mounted at /opt/bpt-monitoring/data. Holds Prometheus tsdb. 50 GB easily fits 30 days of metrics for the current scrape set."
  type        = number
  default     = 50
}

variable "operator_ssh_cidr" {
  description = "CIDR allowed to SSH (port 22). Bootstrap-time only — once Tailscale is enrolled and verified, drop this rule and access via tailnet only."
  type        = string
  // No default — operator must supply.
}

variable "ssh_public_key" {
  description = "Operator's SSH public key for the EC2 key pair. Same key as tape-host is fine, or a separate one if you want stricter blast-radius control."
  type        = string
  // No default — operator must supply.
}

variable "tape_host_vpc_name_tag" {
  description = "Name tag of the existing tape-host VPC. Looked up via data source so monitoring-host joins the same VPC and scrapes tape-host over private IPs without tunnels."
  type        = string
  default     = "bpt-tape-vpc"
}

variable "tape_host_sg_name" {
  description = "Name of the existing tape-host security group. Looked up via data source so the monitoring-host module can add its own ingress rules to it (for ports 9111 + 9100)."
  type        = string
  default     = "bpt-tape-sg"
}

variable "ami_owner" {
  description = "AMI owner. 099720109477 = Canonical (Ubuntu)."
  type        = string
  default     = "099720109477"
}

variable "ami_name_pattern" {
  description = "AMI name pattern. Defaults to latest Ubuntu 24.04 LTS amd64 (HVM, gp3)."
  type        = string
  default     = "ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*"
}
