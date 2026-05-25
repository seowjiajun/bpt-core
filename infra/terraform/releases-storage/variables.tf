variable "aws_region" {
  description = "Region for the releases bucket. Singapore matches the trading host's region for free same-region transfers."
  type        = string
  default     = "ap-southeast-1"
}

variable "releases_bucket_name" {
  description = "S3 bucket holding the deploy tarballs. Globally unique — append a suffix if collision."
  type        = string
  default     = "bpt-releases"
}

variable "retain_days" {
  description = "Days to retain release tarballs before expiry. Tags can always be rebuilt from git; old tarballs are cache, not record."
  type        = number
  default     = 90
}

variable "ci_role_principal" {
  description = "Optional IAM role ARN granted PutObject on the bucket (typically the GitHub Actions OIDC role). Empty means no policy is attached."
  type        = string
  default     = ""
}

variable "owner_tag" {
  description = "Owner tag stamped on all resources."
  type        = string
  default     = "operator"
}

variable "env_tag" {
  description = "Env tag stamped on all resources."
  type        = string
  default     = "shared"
}
