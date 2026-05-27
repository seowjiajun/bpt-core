output "monthly_budget_name" {
  description = "AWS Budgets resource name for the monthly spend ceiling."
  value       = aws_budgets_budget.monthly_total.name
}

output "credit_runway_budget_name" {
  description = "AWS Budgets resource name for the credit-runway alarm. Empty if disabled."
  value       = try(aws_budgets_budget.credit_runway[0].name, "")
}

output "notification_email" {
  description = "Email subscribed to notifications. First email arrives ~6h after apply (Budgets evaluates on a multi-hour cadence)."
  value       = var.operator_email
}
