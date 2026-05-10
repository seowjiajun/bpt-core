// State for monitoring-host lives in the same S3 bucket as tape-host
// (same _bootstrap-provisioned bucket + DynamoDB table), distinct key
// so the two modules can be planned/applied independently.
terraform {
  backend "s3" {
    bucket         = "bpt-tfstate-761f96"
    key            = "monitoring-host/terraform.tfstate"
    region         = "ap-northeast-1"
    dynamodb_table = "bpt-tfstate-lock"
    encrypt        = true
  }
}
