// AWS Budgets is a global service but the provider requires a region.
// us-east-1 is the canonical home for billing/CloudFront/global services.
provider "aws" {
  region = "us-east-1"
}
