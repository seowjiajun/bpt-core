// ── Monthly spend ceiling ──────────────────────────────────────────────────
// Three notifications:
//   1. 50% of budget FORECASTED  → "looks like you're trending high this month"
//   2. 80% of budget ACTUAL      → "spend approaching limit, decide if you want to act"
//   3. 100% of budget ACTUAL     → "limit hit; investigate"
//
// AWS Budgets emails directly — no SNS topic, no Lambda, no SES setup needed.
// Email subscriber must confirm via emailed link the first time.

resource "aws_budgets_budget" "monthly_total" {
  name              = "bpt-monthly-${var.monthly_budget_usd}usd"
  budget_type       = "COST"
  limit_amount      = tostring(var.monthly_budget_usd)
  limit_unit        = "USD"
  time_unit         = "MONTHLY"

  cost_types {
    include_credit             = false  // credits absorb spend silently; we want the gross number
    include_discount           = true
    include_other_subscription = true
    include_recurring          = true
    include_refund             = false
    include_subscription       = true
    include_support            = true
    include_tax                = true
    include_upfront            = true
    use_amortized              = false
    use_blended                = false
  }

  // ── 50% forecasted — early warning ─────────────────────────────────────
  notification {
    comparison_operator        = "GREATER_THAN"
    threshold                  = 50
    threshold_type             = "PERCENTAGE"
    notification_type          = "FORECASTED"
    subscriber_email_addresses = [var.operator_email]
  }

  // ── 80% actual — escalate ──────────────────────────────────────────────
  notification {
    comparison_operator        = "GREATER_THAN"
    threshold                  = 80
    threshold_type             = "PERCENTAGE"
    notification_type          = "ACTUAL"
    subscriber_email_addresses = [var.operator_email]
  }

  // ── 100% actual — budget breached ──────────────────────────────────────
  notification {
    comparison_operator        = "GREATER_THAN"
    threshold                  = 100
    threshold_type             = "PERCENTAGE"
    notification_type          = "ACTUAL"
    subscriber_email_addresses = [var.operator_email]
  }
}

// ── Credit-runway alarm ────────────────────────────────────────────────────
// Distinct from the monthly budget: this one tracks YTD gross spend INCLUDING
// what credits paid for. Fires when total burn approaches the credit grant
// ($60). Tells you when the free runway is ending, separate from monthly
// rate-of-spend.
resource "aws_budgets_budget" "credit_runway" {
  count = var.credit_runway_alert_usd > 0 ? 1 : 0

  name         = "bpt-credit-runway-${var.credit_runway_alert_usd}usd"
  budget_type  = "COST"
  limit_amount = tostring(var.credit_runway_alert_usd)
  limit_unit   = "USD"
  time_unit    = "ANNUALLY"

  cost_types {
    include_credit             = true  // include credits — we're tracking total runway burn
    include_discount           = true
    include_other_subscription = true
    include_recurring          = true
    include_refund             = false
    include_subscription       = true
    include_support            = true
    include_tax                = true
    include_upfront            = true
    use_amortized              = false
    use_blended                = false
  }

  notification {
    comparison_operator        = "GREATER_THAN"
    threshold                  = 80
    threshold_type             = "PERCENTAGE"
    notification_type          = "ACTUAL"
    subscriber_email_addresses = [var.operator_email]
  }
}
