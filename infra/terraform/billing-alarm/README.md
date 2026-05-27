# Billing alarm

Two AWS Budgets, both notifying via direct email (no SNS / Lambda):

1. **Monthly spend ceiling** (`bpt-monthly-50usd` by default) — fires at
   50% forecasted, 80% actual, 100% actual. Credits are NOT counted —
   tracks the gross dollar burn. Tells you "rate of spend is rising
   toward $50/mo."

2. **Credit runway** (`bpt-credit-runway-50usd` by default) — fires at
   80% of the credit grant ($50 of $60 free credits). Credits ARE
   counted — tracks total YTD burn including what credits absorbed.
   Tells you "runway is ending; future spend is real dollars."

## First apply

```bash
cd infra/terraform/billing-alarm
terraform init
terraform apply
```

After apply, AWS sends a confirmation email to the subscriber address.
Click the link to confirm. Future alerts arrive on threshold crossings
(Budgets evaluates roughly every 6h — not real-time).

## Tuning

Default budget is $50/mo, calibrated for:
- ~$25 baseline (tape + monitor + S3)
- +$30 light trading-host use (4hr/day stopped between sessions)
- Headroom buffer

If you scale up trading-host usage:

```bash
# In terraform.tfvars (gitignored):
monthly_budget_usd      = 100
credit_runway_alert_usd = 50
```

## Why direct email and not SNS

At one-operator scale, the SNS → email path adds a topic + subscription
+ confirmation step for zero benefit. AWS Budgets natively supports up
to 10 email subscribers per notification, hands the email off to AWS-
managed SES under the hood. Simpler stack.

If you ever add Slack / ntfy / PagerDuty for ops, swap to SNS topic
(the existing alertmanager already integrates with ntfy — see
monitoring/alertmanager/).

## What this WON'T catch

- Sudden $100 spike in <6h — Budgets is a low-frequency check
- Specific service overruns — this is total-account, not per-service
- Credits expiring on a specific date — track via Billing Console

For sharper alerting, layer on CloudWatch metric alarms against
`AWS/Billing` namespace (us-east-1 only; lower latency than Budgets).
