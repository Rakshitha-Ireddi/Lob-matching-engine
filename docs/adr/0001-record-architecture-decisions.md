# 1. Record architecture decisions

- Status: accepted
- Date: 2026-09-08

## Context

This is a latency-sensitive system where a lot of the design rationale ("why a
flat array and not a tree", "why one writer thread", "why fixed point") is not
obvious from the code and tends to get re-litigated. Reviewers and future-me
need the reasoning, the alternatives that were rejected, and the trade-offs
that were accepted.

## Decision

Keep short architecture decision records in `docs/adr/`, one file per
decision, numbered in order. Format follows Michael Nygard's template:
context, decision, consequences. Code comments link to the relevant ADR
instead of repeating the argument.

## Consequences

- Each non-trivial structural choice has a single place that explains it.
- ADRs are immutable once accepted; a later reversal gets a new ADR that
  supersedes the old one rather than an edit.
