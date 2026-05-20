variable "aws_region" {
  description = "AWS region. ap-northeast-1 matches the existing tape-archive bucket region."
  type        = string
  default     = "ap-northeast-1"
}

variable "name_prefix" {
  description = "Prefix applied to every resource name. Lets you stand up a parallel stack (e.g. 'bpt-secmaster-test') without collisions."
  type        = string
  default     = "bpt-secmaster"
}

variable "trading_host_cidr" {
  description = "CIDR of the trading host that runs bpt-refdata. Allowed to reach RDS on 5432. e.g. '203.0.113.42/32'."
  type        = string
}

variable "db_instance_class" {
  description = "RDS instance class. db.t4g.micro is Free Tier eligible for 12 months."
  type        = string
  default     = "db.t4g.micro"
}

variable "db_allocated_storage_gb" {
  description = "RDS allocated storage in GB. Free Tier covers 20 GB. Storage auto-grows past this."
  type        = number
  default     = 20
}

variable "db_name" {
  description = "Postgres database name created inside the RDS instance."
  type        = string
  default     = "secmaster"
}

variable "db_master_username" {
  description = "Master DB user. Has full admin privileges. Application uses less-privileged roles created via the schema bootstrap."
  type        = string
  default     = "bptadmin"
}

variable "db_backup_retention_days" {
  description = "Days of automated PITR snapshots RDS retains. AWS Free Plan caps this at 1; legacy Free Tier / paid accounts can set up to 35. Combine with manual snapshots + the weekly pg_dump → S3 backup if you need longer retention on a Free Plan account."
  type        = number
  default     = 1
}

variable "lambda_schedule_cron" {
  description = "EventBridge cron expression. Default: daily at 03:00 UTC."
  type        = string
  default     = "cron(0 3 * * ? *)"
}

variable "lambda_image_tag" {
  description = "ECR image tag the Lambda runs. Updated by deploy.sh after each push."
  type        = string
  default     = "latest"
}

variable "lambda_memory_mb" {
  description = "Lambda memory allocation. Drives CPU allocation proportionally."
  type        = number
  default     = 512
}

variable "lambda_timeout_seconds" {
  description = "Lambda max runtime. Daily refresh historically takes ~3-5 min; cap at 10."
  type        = number
  default     = 600
}

variable "discord_webhook_url" {
  description = "Optional Discord webhook URL for the daily ingest summary. Leave empty to disable."
  type        = string
  default     = ""
  sensitive   = true
}

variable "log_retention_days" {
  description = "CloudWatch log retention for the Lambda. AWS defaults to 'never expire' which silently accumulates cost."
  type        = number
  default     = 30
}

variable "snapshot_s3_bucket" {
  description = "S3 bucket name to upload the rendered instrument_mapping.json snapshot to. Reuses the existing bpt-tape-archive bucket from [[project_tape_storage]]."
  type        = string
  default     = "bpt-tape-archive"
}

variable "snapshot_s3_key" {
  description = "Object key in the snapshot bucket. Single overwriting key; S3 versioning on the bucket provides history."
  type        = string
  default     = "secmaster/instrument_mapping.json"
}
