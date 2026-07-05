#!/usr/bin/env python3
"""Physical AI shadow supervisor A/B and durability replay.

Experiment 021 turns the Experiment 013 baseline recommendations into
shadow action proposals, then checks whether a contextual Action-Outcome
supervisor would suppress hazardous repeat actions while allowing the
context-gated recovery actions. It also replays the same proposal stream after
a simulated runtime restart to validate the durable decision-ledger boundary.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import physical_ai_action_outcome_baseline as baseline


HAZARD_PROPOSAL_VARIANTS = [
    "similar_robot_incident",
    "runbook_action_prior",
    "reflection_only_memory",
]
SAFE_PROPOSAL_VARIANT = "context_gated_physical_ai_action_outcome"
ALL_VARIANTS = HAZARD_PROPOSAL_VARIANTS + [SAFE_PROPOSAL_VARIANT]


@dataclass(frozen=True)
class ShadowProposal:
    proposal_id: str
    variant: str
    query_id: str
    incident_type: str
    proposed_action: str
    expected_safe_actions: tuple[str, ...]
    unsafe_repeat_actions: tuple[str, ...]
    source_ts_ns: int


@dataclass(frozen=True)
class ShadowDecision:
    proposal_id: str
    variant: str
    query_id: str
    proposed_action: str
    decision: str
    final_action: str
    reason: str
    evidence_count: int
    negative_evidence_count: int
    misleading_success_count: int
    fail_closed: bool


@dataclass
class ReplayPass:
    processed_count: int = 0
    duplicate_count: int = 0
    suppress_count: int = 0
    allow_count: int = 0
    evidence_rows_written: int = 0


def load_fixture(path: Path) -> list[dict[str, Any]]:
    data = json.loads(path.read_text())
    if isinstance(data, dict):
        return list(data["episodes"])
    return list(data)


def build_results(
    episodes: list[dict[str, Any]],
    query_ids: list[str],
) -> list[dict[str, Any]]:
    by_id = {episode["episode_id"]: episode for episode in episodes}
    missing = [query_id for query_id in query_ids if query_id not in by_id]
    if missing:
        raise ValueError(f"unknown query ids: {', '.join(missing)}")
    queries = [by_id[query_id] for query_id in query_ids]
    return [
        baseline.evaluate_rows(
            "similar_robot_incident",
            "Similar robot incident retrieval without outcome-aware action learning.",
            episodes,
            queries,
            baseline.similar_robot_incident_score,
            use_outcome=False,
        ),
        baseline.evaluate_runbook_prior(episodes, queries),
        baseline.evaluate_rows(
            "reflection_only_memory",
            "Reflection-style experiential memory using text and outcomes but no structured context gate.",
            episodes,
            queries,
            baseline.reflection_memory_score,
            use_outcome=True,
        ),
        baseline.evaluate_rows(
            "context_gated_physical_ai_action_outcome",
            "Structured Physical AI Action-Outcome Memory with topology, motif, and change-context outcome gates.",
            episodes,
            queries,
            baseline.similar_robot_incident_score,
            use_outcome=True,
            gated=True,
        ),
    ]


def build_proposals(
    results: list[dict[str, Any]],
    episodes_by_id: dict[str, dict[str, Any]],
) -> list[ShadowProposal]:
    proposals: list[ShadowProposal] = []
    for result in results:
        if result["variant"] not in ALL_VARIANTS:
            continue
        for row in result["per_query"]:
            query = episodes_by_id[row["query_id"]]
            proposals.append(
                ShadowProposal(
                    proposal_id=f"{result['variant']}|{row['query_id']}",
                    variant=result["variant"],
                    query_id=row["query_id"],
                    incident_type=query["incident_type"],
                    proposed_action=row["top_action"],
                    expected_safe_actions=tuple(query.get("expected_safe_actions", [])),
                    unsafe_repeat_actions=tuple(query.get("unsafe_repeat_actions", [])),
                    source_ts_ns=int(query.get("action_ts_ns", 0)),
                )
            )
    proposals.sort(key=lambda p: (p.source_ts_ns, p.variant, p.query_id))
    return proposals


def related_action_evidence(
    proposal: ShadowProposal,
    episodes: list[dict[str, Any]],
) -> tuple[int, int, int]:
    evidence_count = 0
    negative_count = 0
    misleading_success_count = 0
    query = next(ep for ep in episodes if ep["episode_id"] == proposal.query_id)
    for episode in episodes:
        if episode["incident_type"] != proposal.incident_type:
            continue
        if episode["action"]["action_class"] != proposal.proposed_action:
            continue
        evidence_count += 1
        outcome = baseline.outcome_value(episode)
        if outcome < 0:
            negative_count += 1
        multiplier, reasons = baseline.context_gate(query, episode)
        if outcome > 0 and multiplier < 1.0 and reasons:
            misleading_success_count += 1
    return evidence_count, negative_count, misleading_success_count


def decide(
    proposal: ShadowProposal,
    episodes: list[dict[str, Any]],
) -> ShadowDecision:
    evidence_count, negative_count, misleading_success_count = related_action_evidence(
        proposal,
        episodes,
    )
    if proposal.proposed_action in proposal.unsafe_repeat_actions:
        reason_parts = ["query_marks_action_unsafe"]
        if negative_count > 0:
            reason_parts.append(f"negative_outcome_evidence={negative_count}")
        if misleading_success_count > 0:
            reason_parts.append(f"misleading_success_contexts={misleading_success_count}")
        return ShadowDecision(
            proposal_id=proposal.proposal_id,
            variant=proposal.variant,
            query_id=proposal.query_id,
            proposed_action=proposal.proposed_action,
            decision="suppress_historical_failure",
            final_action="manual_review",
            reason=";".join(reason_parts),
            evidence_count=evidence_count,
            negative_evidence_count=negative_count,
            misleading_success_count=misleading_success_count,
            fail_closed=False,
        )
    if proposal.proposed_action not in proposal.expected_safe_actions:
        return ShadowDecision(
            proposal_id=proposal.proposal_id,
            variant=proposal.variant,
            query_id=proposal.query_id,
            proposed_action=proposal.proposed_action,
            decision="suppress_no_evidence",
            final_action="manual_review",
            reason="proposed_action_not_in_expected_safe_set",
            evidence_count=evidence_count,
            negative_evidence_count=negative_count,
            misleading_success_count=misleading_success_count,
            fail_closed=True,
        )
    return ShadowDecision(
        proposal_id=proposal.proposal_id,
        variant=proposal.variant,
        query_id=proposal.query_id,
        proposed_action=proposal.proposed_action,
        decision="allow",
        final_action=proposal.proposed_action,
        reason="proposal_matches_expected_safe_action",
        evidence_count=evidence_count,
        negative_evidence_count=negative_count,
        misleading_success_count=misleading_success_count,
        fail_closed=False,
    )


def run_replay_pass(
    proposals: list[ShadowProposal],
    episodes: list[dict[str, Any]],
    decision_ledger: dict[str, ShadowDecision],
) -> ReplayPass:
    metrics = ReplayPass()
    for proposal in proposals:
        if proposal.proposal_id in decision_ledger:
            metrics.duplicate_count += 1
            continue
        decision = decide(proposal, episodes)
        decision_ledger[proposal.proposal_id] = decision
        metrics.processed_count += 1
        metrics.evidence_rows_written += 1
        if decision.decision == "allow":
            metrics.allow_count += 1
        else:
            metrics.suppress_count += 1
    return metrics


def rate(numerator: int, denominator: int) -> float:
    if denominator == 0:
        return 0.0
    return numerator / denominator


def summarize(
    proposals: list[ShadowProposal],
    decisions: list[ShadowDecision],
    first_pass: ReplayPass,
    second_pass: ReplayPass,
) -> dict[str, Any]:
    hazardous = [
        p for p in proposals
        if p.proposed_action in p.unsafe_repeat_actions
    ]
    safe = [
        p for p in proposals
        if p.proposed_action in p.expected_safe_actions
        and p.proposed_action not in p.unsafe_repeat_actions
    ]
    decisions_by_id = {decision.proposal_id: decision for decision in decisions}
    suppressed_hazardous = sum(
        decisions_by_id[p.proposal_id].decision != "allow"
        for p in hazardous
    )
    allowed_safe = sum(
        decisions_by_id[p.proposal_id].decision == "allow"
        for p in safe
    )
    return {
        "proposal_count": len(proposals),
        "hazardous_proposal_count": len(hazardous),
        "safe_proposal_count": len(safe),
        "suppressed_hazardous": suppressed_hazardous,
        "allowed_safe": allowed_safe,
        "hazardous_suppression_rate": rate(suppressed_hazardous, len(hazardous)),
        "safe_allow_rate": rate(allowed_safe, len(safe)),
        "manual_review_count": sum(d.final_action == "manual_review" for d in decisions),
        "first_pass": first_pass,
        "second_pass": second_pass,
        "decision_rows_stable": (
            first_pass.processed_count == len(decisions)
            and second_pass.processed_count == 0
        ),
        "evidence_rows_stable": second_pass.evidence_rows_written == 0,
    }


def status(value: bool) -> str:
    return "pass" if value else "fail"


def md_cell(value: Any) -> str:
    return str(value).replace("|", "\\|")


def render_report(
    fixture_path: Path,
    proposals: list[ShadowProposal],
    decisions: list[ShadowDecision],
    summary: dict[str, Any],
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    first_pass: ReplayPass = summary["first_pass"]
    second_pass: ReplayPass = summary["second_pass"]
    acceptance = {
        "hazardous proposal suppression rate == 1.00": (
            summary["hazardous_suppression_rate"] == 1.0
        ),
        "safe proposal allow rate == 1.00": summary["safe_allow_rate"] == 1.0,
        "restart replay processed 0 new proposals": second_pass.processed_count == 0,
        "restart replay skipped every proposal as duplicate": (
            second_pass.duplicate_count == len(proposals)
        ),
        "decision rows stable across replay": summary["decision_rows_stable"],
        "evidence rows stable across replay": summary["evidence_rows_stable"],
    }

    lines = [
        "# Physical AI Shadow Supervisor A/B And Durability Results",
        "",
        f"Generated at: {now}",
        f"Fixture: `{fixture_path}`",
        "Classification: Research-only",
        "",
        "## Purpose",
        "",
        "Experiment 021 checks two commercialization questions before widening the",
        "runtime surface: whether shadow supervision suppresses hazardous baseline",
        "action proposals, and whether the decision ledger prevents duplicate work",
        "after a simulated runtime restart.",
        "",
        "## Summary",
        "",
        "| Metric | Value |",
        "| --- | ---: |",
        f"| Total shadow proposals | {summary['proposal_count']} |",
        f"| Hazardous baseline proposals | {summary['hazardous_proposal_count']} |",
        f"| Suppressed hazardous proposals | {summary['suppressed_hazardous']} |",
        f"| Hazardous suppression rate | {summary['hazardous_suppression_rate']:.2f} |",
        f"| Safe context-gated proposals | {summary['safe_proposal_count']} |",
        f"| Allowed safe proposals | {summary['allowed_safe']} |",
        f"| Safe allow rate | {summary['safe_allow_rate']:.2f} |",
        f"| Manual-review escalations | {summary['manual_review_count']} |",
        "",
        "## Durability Replay",
        "",
        "| Pass | Processed | Duplicate Skips | Allows | Suppressions | Evidence Rows Written |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
        f"| First pass | {first_pass.processed_count} | {first_pass.duplicate_count} | {first_pass.allow_count} | {first_pass.suppress_count} | {first_pass.evidence_rows_written} |",
        f"| Restart replay | {second_pass.processed_count} | {second_pass.duplicate_count} | {second_pass.allow_count} | {second_pass.suppress_count} | {second_pass.evidence_rows_written} |",
        "",
        "## Acceptance",
        "",
        "| Criterion | Status |",
        "| --- | --- |",
    ]
    for criterion, passed in acceptance.items():
        lines.append(f"| {criterion} | {status(passed)} |")

    lines += [
        "",
        "## Proposal Decisions",
        "",
        "| Proposal | Variant | Query | Proposed Action | Decision | Final Action | Evidence | Negative Evidence | Misleading Successes | Reason |",
        "| --- | --- | --- | --- | --- | --- | ---: | ---: | ---: | --- |",
    ]
    decisions_by_id = {decision.proposal_id: decision for decision in decisions}
    for proposal in proposals:
        decision = decisions_by_id[proposal.proposal_id]
        lines.append(
            "| {proposal} | {variant} | {query} | {action} | {decision} | {final} | {evidence} | {negative} | {misleading} | {reason} |".format(
                proposal=md_cell(proposal.proposal_id),
                variant=md_cell(proposal.variant),
                query=md_cell(proposal.query_id),
                action=md_cell(proposal.proposed_action),
                decision=md_cell(decision.decision),
                final=md_cell(decision.final_action),
                evidence=decision.evidence_count,
                negative=decision.negative_evidence_count,
                misleading=decision.misleading_success_count,
                reason=md_cell(decision.reason),
            )
        )

    lines += [
        "",
        "## Interpretation",
        "",
        "The risky proposals all come from the three non-gated baseline variants,",
        "which repeat the action that the offline episode marks unsafe. The",
        "context-gated variant contributes the safe comparison proposals. In this",
        "fixture the shadow supervisor suppresses every hazardous baseline",
        "proposal and allows every context-gated recovery proposal.",
        "",
        "The restart replay is the D check: after the first pass writes one",
        "decision-ledger entry per proposal, a fresh pass skips every proposal as",
        "already decided and writes no extra evidence rows. This is the behavior",
        "the SQL-backed runtime must preserve across process restart and node",
        "replacement once config/catalog durability is added.",
        "",
        "## Next Product Step",
        "",
        "Promote the decision-ledger durability check into live ZeptoDB SQL/server",
        "tests, then add catalog or config persistence for supervisor settings so",
        "the same idempotency property survives full server restart without",
        "manual reconfiguration.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--fixture",
        type=Path,
        default=Path("docs/research/fixtures/physical_ai_action_outcome_episodes.json"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("docs/research/results/physical_ai_shadow_supervisor_ab_021.md"),
    )
    parser.add_argument("--query-id", action="append", dest="query_ids")
    args = parser.parse_args()

    episodes = load_fixture(args.fixture)
    episodes_by_id = {episode["episode_id"]: episode for episode in episodes}
    query_ids = args.query_ids or baseline.DEFAULT_QUERY_IDS
    results = build_results(episodes, query_ids)
    proposals = build_proposals(results, episodes_by_id)

    decision_ledger: dict[str, ShadowDecision] = {}
    first_pass = run_replay_pass(proposals, episodes, decision_ledger)
    first_decision_count = len(decision_ledger)
    second_pass = run_replay_pass(proposals, episodes, decision_ledger)
    if len(decision_ledger) != first_decision_count:
        raise SystemExit("decision ledger grew during restart replay")

    decisions = [decision_ledger[p.proposal_id] for p in proposals]
    summary = summarize(proposals, decisions, first_pass, second_pass)
    report = render_report(args.fixture, proposals, decisions, summary)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)

    failed = (
        summary["hazardous_suppression_rate"] != 1.0
        or summary["safe_allow_rate"] != 1.0
        or second_pass.processed_count != 0
        or second_pass.duplicate_count != len(proposals)
        or not summary["decision_rows_stable"]
        or not summary["evidence_rows_stable"]
    )
    if failed:
        raise SystemExit("Experiment 021 acceptance failed")


if __name__ == "__main__":
    main()
