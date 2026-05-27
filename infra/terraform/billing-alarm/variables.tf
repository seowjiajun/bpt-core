variable "operator_email" {
  description = "Email address to notify when budget thresholds are crossed."
  type        = string
  default     = "seowjiajun@gmail.com"
}

variable "monthly_budget_usd" {
  description = "Hard ceiling on monthly AWS spend. Three notifications fire against this number: 50% forecasted, 80% actual, 100% actual."
  type        = number
  default     = 50
}

variable "credit_runway_alert_usd" {
  description = "Separate budget — fires when YTD spend gets close to the $60 credit grant. Set lower than monthly_budget_usd so we get warned about the credit-window even if monthly spend is fine. 0 disables."
  type        = number
  default     = 50
}
