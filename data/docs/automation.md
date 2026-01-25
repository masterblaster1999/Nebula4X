# Automation & Profiles

Automation is designed to reduce repetitive UI work while keeping player intent explicit.

## Colony Profiles

Colony Profiles define how a colony should behave over time:

- what installations to prioritize
- what resources to export/import
- how to react to shortages

Open (default): **Ctrl+Shift+B**

## Ship Profiles

Ship Profiles define:

- default mission behaviors
- survey/explore loops
- refuel and resupply preferences

Open (default): **Ctrl+Shift+M**

## Automation Center

The Automation Center is a bulk triage tool:

- filter ships by profile / mission state
- enable/disable automation flags at scale
- jump to offenders quickly

If something looks "stuck": inspect it here and then jump to Details for the ship/colony.

## Practical workflow

1. Make a couple of Profiles you like.
2. Apply them early to reduce micromanagement.
3. Use the Event Log / toasts + Advisor window to catch failures.

## Debugging automation

Automation is usually state-driven.

If you want to understand *why* a ship did something:

1. Open **OmniSearch** (default: **Ctrl+F**).
2. Search for the ship id.
3. Look for `orders`, `automation`, `mission`, and `fuel` fields.