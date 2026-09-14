import unittest
from pathlib import Path
import re


class OpenUSimStageSkillDocsTest(unittest.TestCase):
    def repo_root(self):
        return Path(__file__).resolve().parents[2]

    def read_text(self, relative_path):
        return (self.repo_root() / relative_path).read_text(encoding="utf-8")

    def reference_files(self):
        return sorted((self.repo_root() / ".codex/skills/openusim-references").glob("*.md"))

    def skill_files(self):
        return [
            self.repo_root() / relative_path
            for relative_path in (
                ".codex/skills/openusim-welcome/SKILL.md",
                ".codex/skills/openusim-plan-experiment/SKILL.md",
                ".codex/skills/openusim-run-experiment/SKILL.md",
                ".codex/skills/openusim-analyze-results/SKILL.md",
                ".codex/skills/openusim-capture-insights/SKILL.md",
            )
        ]

    def frontmatter_description(self, text):
        frontmatter = re.match(r"---\n(.*?)\n---", text, re.S)
        self.assertIsNotNone(frontmatter)
        match = re.search(
            r"^description:\s*(?:>\s*)?(.*?)(?=\n[a-z-]+:|\Z)",
            frontmatter.group(1),
            re.S | re.M,
        )
        self.assertIsNotNone(match)
        return " ".join(line.strip() for line in match.group(1).splitlines()).strip()

    def test_stage_skill_bundle_exists(self):
        repo_root = self.repo_root()
        for relative_path in (
            ".codex/skills/openusim-welcome/SKILL.md",
            ".codex/skills/openusim-plan-experiment/SKILL.md",
            ".codex/skills/openusim-run-experiment/SKILL.md",
            ".codex/skills/openusim-analyze-results/SKILL.md",
            ".codex/skills/openusim-capture-insights/SKILL.md",
        ):
            self.assertTrue((repo_root / relative_path).is_file(), msg=relative_path)

    def test_timing_offset_demo_uses_runtime_parameter_catalog(self):
        generator = self.read_text("scratch/ub_parallel_timing_offset_demo/generate_case.py")
        self.assertIn("network_attribute_writer.write_network_attributes", generator)
        self.assertNotIn("NETWORK_ATTRIBUTES =", generator)

    def test_stage_skill_descriptions_are_trigger_only(self):
        for path in self.skill_files():
            text = path.read_text(encoding="utf-8")
            description = self.frontmatter_description(text)
            self.assertTrue(description.startswith("Use when "), msg=path.name)
            self.assertLessEqual(len(description), 240, msg=path.name)
            for process_leak in (
                "Phase 1",
                "Phase 2",
                "The Process",
                "hand off",
                "handoff",
                "Called by",
            ):
                self.assertNotIn(process_leak, description, msg=path.name)

    def test_stage_processes_have_checkable_completion_criteria(self):
        for path in self.skill_files():
            text = path.read_text(encoding="utf-8")
            self.assertIn("Completion criterion:", text, msg=path.name)

    def test_stage_skills_keep_reference_lists_single_sourced(self):
        for path in self.skill_files():
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("\n## References\n", text, msg=path.name)

    def test_claude_skill_symlinks_cover_repo_local_stage_skills(self):
        repo_root = self.repo_root()
        for skill_dir in (
            "openusim-welcome",
            "openusim-plan-experiment",
            "openusim-run-experiment",
            "openusim-analyze-results",
            "openusim-capture-insights",
        ):
            path = repo_root / ".claude/skills" / skill_dir
            self.assertTrue(path.is_symlink(), msg=skill_dir)
            self.assertEqual(
                path.readlink(),
                Path("../../.codex/skills") / skill_dir,
                msg=skill_dir,
            )

    def test_stage_skill_docs_define_handoff_surface(self):
        welcome_text = self.read_text(".codex/skills/openusim-welcome/SKILL.md")
        plan_text = self.read_text(".codex/skills/openusim-plan-experiment/SKILL.md")
        run_text = self.read_text(".codex/skills/openusim-run-experiment/SKILL.md")
        analyze_text = self.read_text(".codex/skills/openusim-analyze-results/SKILL.md")
        capture_text = self.read_text(".codex/skills/openusim-capture-insights/SKILL.md")
        topology_text = self.read_text(".codex/skills/openusim-references/topology-options.md")
        spec_rules_text = self.read_text(".codex/skills/openusim-references/spec-rules.md")

        for text in (welcome_text, plan_text, run_text, analyze_text, capture_text):
            self.assertIn("## Overview", text)
            self.assertIn("## When to Use", text)
            self.assertIn("## Handoff", text)
            self.assertIn("## Integration", text)
            self.assertIn("Stay in this skill when:", text)

        self.assertIn("Hand off to `openusim-plan-experiment` when:", welcome_text)
        self.assertIn("Hand off to `openusim-run-experiment` when:", plan_text)
        self.assertIn(
            "Before handoff for `single-case`, ensure `{case_dir}/experiment-spec.md` exists",
            plan_text,
        )
        self.assertIn("Return to `openusim-welcome` when:", plan_text)
        self.assertIn("scratch/ns-3-ub-tools/net_sim_builder.py", run_text)
        self.assertIn("scratch/ns-3-ub-tools/traffic_maker/build_traffic.py", run_text)
        self.assertIn("Hand off to `openusim-analyze-results` when:", run_text)
        self.assertIn("Return to `openusim-plan-experiment` when:", run_text)
        self.assertIn("routing_intent", plan_text)
        self.assertIn("transport_channel_mode", plan_text)
        self.assertIn("default `on-demand`", plan_text)
        self.assertIn("planning_mode", plan_text)
        self.assertIn("single-case", plan_text)
        self.assertIn("experiment-group", plan_text)
        self.assertIn("control", plan_text)
        self.assertIn("treatments", plan_text)
        self.assertIn("changed_variable", plan_text)
        self.assertIn("fixed_controls", plan_text)
        self.assertIn("prediction", plan_text)
        self.assertIn("falsification_signal", plan_text)
        self.assertIn("evidence_plan", plan_text)
        self.assertIn("checkpoint_policy", plan_text)
        self.assertIn("matrix.yaml", plan_text)
        self.assertIn("command-manifest.yaml", plan_text)
        self.assertIn("run-ledger.md", plan_text)
        self.assertIn("custom-graph", plan_text)
        self.assertIn("graph.output_dir", run_text)
        self.assertIn("validate", run_text)
        self.assertIn("transport_channel_mode", run_text)
        self.assertIn("matrix.yaml", run_text)
        self.assertIn("command-manifest.yaml", run_text)
        self.assertIn("run-ledger.md", run_text)
        self.assertIn("artifact contract", run_text)
        self.assertIn("checkpoint policy", run_text)
        self.assertIn("Lightweight", run_text)
        self.assertIn("do not add or remove matrix rows", run_text)
        self.assertIn("Changing predictions after seeing outputs", run_text)
        self.assertIn("../openusim-references/", analyze_text)
        self.assertIn("<HARD-GATE>", analyze_text)
        self.assertIn("do not read full cards first", analyze_text)
        self.assertIn("<reference-hint>...</reference-hint>", analyze_text)
        self.assertIn("prefer `rg -U -o`", analyze_text)
        self.assertIn("fallback to `perl -0ne`", analyze_text)
        self.assertIn("fallback to `python3`", analyze_text)
        self.assertIn("Python regex is the fallback", analyze_text)
        self.assertIn("re.search", analyze_text)
        self.assertIn("perl", analyze_text)
        self.assertIn("rg -U -o", analyze_text)
        self.assertIn("do not `cat` every card in the directory just to choose", analyze_text)
        self.assertIn("do not hardcode a fixed card list as the only valid source", analyze_text)
        self.assertIn("within about 160 characters", analyze_text)
        self.assertIn("## Failure Interpretation Checklist", analyze_text)
        self.assertIn("prediction-vs-actual", analyze_text)
        self.assertIn("matched", analyze_text)
        self.assertIn("partially_matched", analyze_text)
        self.assertIn("mismatched", analyze_text)
        self.assertIn("inconclusive", analyze_text)
        self.assertIn("## Experiment Group Checklist", analyze_text)
        self.assertIn("evidence source labels", analyze_text)
        self.assertIn("Hand off to `openusim-plan-experiment` when:", analyze_text)
        self.assertIn("Hand off to `openusim-capture-insights` when:", analyze_text)
        self.assertIn("ask the user whether they want to preserve it as a knowledge card", analyze_text)
        self.assertIn("conclusion summary", analyze_text)
        self.assertIn("candidate existing card", analyze_text)
        self.assertIn("<HARD-GATE>", capture_text)
        self.assertIn("Do not create or modify a knowledge card unless the user has clearly agreed", capture_text)
        self.assertIn("write the judgment or insight, not the chat transcript", capture_text)
        self.assertIn("examples or evidence", capture_text)
        self.assertIn("future reader who does not know this conversation", capture_text)
        self.assertIn("future reader who does not know this case or chat", capture_text)
        self.assertIn("main-repo PR", capture_text)
        self.assertIn("Called by: `openusim-analyze-results`", capture_text)
        self.assertIn("### `custom-graph`", topology_text)
        self.assertIn("## Routing Intent", spec_rules_text)
        self.assertIn("## Transport Channel Mode", spec_rules_text)
        self.assertIn("## Planning modes", spec_rules_text)
        self.assertIn("## Single-case minimal template", spec_rules_text)
        self.assertIn("## Experiment-group minimal template", spec_rules_text)

    def test_controlled_experiment_method_is_integrated(self):
        method_text = self.read_text(
            ".codex/skills/openusim-references/controlled-experiment-method.md"
        )
        plan_text = self.read_text(".codex/skills/openusim-plan-experiment/SKILL.md")
        run_text = self.read_text(".codex/skills/openusim-run-experiment/SKILL.md")
        analyze_text = self.read_text(".codex/skills/openusim-analyze-results/SKILL.md")
        spec_rules_text = self.read_text(".codex/skills/openusim-references/spec-rules.md")

        for marker in (
            "<reference-hint>",
            "## Contents",
            "## Mode selection",
            "## Controlled-variable design",
            "## Case artifact generation contract",
            "## Checkpoint policy",
            "## Artifact contract",
            "## Run ledger",
            "## Prediction-vs-actual analysis",
            "## Mismatch investigation",
        ):
            self.assertIn(marker, method_text)

        for marker in (
            "claim",
            "control",
            "treatments",
            "changed_variable",
            "fixed_controls",
            "prediction",
            "falsification_signal",
            "evidence_plan",
            "checkpoint_policy",
        ):
            self.assertIn(marker, method_text)
            self.assertIn(marker, plan_text)
            self.assertIn(marker, spec_rules_text)

        for text in (plan_text, run_text, analyze_text):
            self.assertIn("../openusim-references/controlled-experiment-method.md", text)

        for marker in (
            "matrix.yaml",
            "command-manifest.yaml",
            "run-ledger.md",
            "experiment-plan.md",
        ):
            self.assertIn(marker, method_text)
            self.assertIn(marker, spec_rules_text)

        self.assertIn("Plan owns the matrix", method_text)
        self.assertIn("Run must not invent cases", method_text)
        self.assertIn("Plan owns the matrix", run_text)

    def test_reference_cards_expose_reference_hint_block(self):
        for path in self.reference_files():
            text = path.read_text(encoding="utf-8")
            lines = text.splitlines()
            self.assertTrue(lines, msg=path.name)
            self.assertTrue(lines[0].startswith("# "), msg=path.name)
            hint = re.search(r"<reference-hint>(.*?)</reference-hint>", text, re.S)
            self.assertIsNotNone(hint, msg=f"{path.name}: missing <reference-hint> block")

            hint_text = hint.group(1)
            use_when = re.search(r"<use-when>(.*?)</use-when>", hint_text, re.S)
            focus = re.search(r"<focus>(.*?)</focus>", hint_text, re.S)
            keywords = re.search(r"<keywords>(.*?)</keywords>", hint_text, re.S)

            self.assertIsNotNone(use_when, msg=f"{path.name}: missing <use-when>")
            self.assertIsNotNone(focus, msg=f"{path.name}: missing <focus>")
            self.assertIsNotNone(keywords, msg=f"{path.name}: missing <keywords>")

            use_when_text = use_when.group(1).strip()
            self.assertTrue(
                use_when_text.startswith("Use this reference when "),
                msg=f"{path.name}: <use-when> must start with 'Use this reference when '",
            )
            self.assertLessEqual(
                len(use_when_text),
                160,
                msg=f"{path.name}: <use-when> should stay within 160 characters",
            )

    def test_long_reference_cards_include_contents_section(self):
        for path in self.reference_files():
            lines = path.read_text(encoding="utf-8").splitlines()
            if len(lines) <= 100:
                continue
            self.assertIn("## Contents", lines[:30], msg=path.name)

    def test_welcome_skill_spells_out_startup_gate(self):
        welcome_text = self.read_text(".codex/skills/openusim-welcome/SKILL.md")
        for marker in (
            "`./ns3` exists",
            "`scratch/ns-3-ub-tools/` exists",
            "`scratch/ns-3-ub-tools/requirements.txt` exists",
            "`scratch/ns-3-ub-tools/net_sim_builder.py` exists",
            "`scratch/ns-3-ub-tools/traffic_maker/build_traffic.py` exists",
            "`scratch/ns-3-ub-tools/trace_analysis/parse_trace.py` exists",
            "`build/` exists",
            "`cmake-cache/` exists",
            "`scratch/2nodes_single-tp` exists",
            "`git submodule update --init --recursive`",
            "`python3 -m pip install --user -r scratch/ns-3-ub-tools/requirements.txt`",
            "`python3.12 ./ns3 configure --enable-modules=unified-bus --disable-examples --disable-tests --disable-mpi --disable-mtp --disable-werror -d release -G Ninja`",
            "`BUILD_JOBS=${BUILD_JOBS:-$(python3.12 -c 'import os; print(os.cpu_count() or 1)')}`",
            '`python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example`',
            "`python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp'`",
        ):
            self.assertIn(marker, welcome_text)

    def test_spec_rules_define_minimal_experiment_spec_shape(self):
        spec_rules_text = self.read_text(
            ".codex/skills/openusim-references/spec-rules.md"
        )
        for marker in (
            "## Planning modes",
            "## Single-case minimal template",
            "## Experiment-group minimal template",
            "# Experiment Spec",
            "## Planning Mode",
            "## Goal",
            "## Topology",
            "## Topology Realization",
            "## Workload",
            "## Routing Intent",
            "## Network Overrides",
            "## Transport Channel Mode",
            "## Observability",
            "## Startup Readiness",
            "## Execution Record",
            "## Validation Notes",
            "## Analysis Notes",
            "experiment-plan.md",
            "matrix.yaml",
            "command-manifest.yaml",
            "run-ledger.md",
            "checkpoint policy",
            "prediction-vs-actual",
        ):
            self.assertIn(marker, spec_rules_text)
        self.assertIn("default: `on-demand`", spec_rules_text)

    def test_repo_agents_route_by_stage_not_monolith(self):
        agents_text = self.read_text("AGENTS.md")
        self.assertIn("openusim-welcome", agents_text)
        self.assertIn("openusim-plan-experiment", agents_text)
        self.assertIn("openusim-run-experiment", agents_text)
        self.assertIn("openusim-analyze-results", agents_text)
        self.assertNotIn("openusim-helper", agents_text)

    def test_repo_entry_docs_match_stage_skill_surface(self):
        readme_text = self.read_text("README.md")
        readme_en_text = self.read_text("README_en.md")
        quick_start_text = self.read_text("QUICK_START.md")
        quick_start_en_text = self.read_text("QUICK_START_en.md")

        for text in (
            readme_text,
            readme_en_text,
        ):
            self.assertIn(".codex/skills/", text)
            self.assertIn("openusim-welcome", text)
            self.assertIn("openusim-plan-experiment", text)
            self.assertIn("openusim-run-experiment", text)
            self.assertIn("openusim-analyze-results", text)
            self.assertIn("openusim-capture-insights", text)
            self.assertIn("A/B", text)
            self.assertIn("sweep", text)
            self.assertNotIn("openusim-helper", text)
            self.assertNotIn("single-case", text)
            self.assertNotIn("experiment-group", text)
            self.assertNotIn("matrix.yaml", text)
            self.assertNotIn("command-manifest.yaml", text)
            self.assertNotIn("run-ledger.md", text)

        for text in (quick_start_text, quick_start_en_text):
            self.assertIn(".codex/skills/", text)
            self.assertIn("smoke", text)
            self.assertIn("A/B", text)
            self.assertIn("sweep", text)
            self.assertIn("baseline", text)
            self.assertNotIn("openusim-helper", text)
            self.assertNotIn("single-case", text)
            self.assertNotIn("experiment-group", text)
            self.assertNotIn("matrix.yaml", text)
            self.assertNotIn("command-manifest.yaml", text)
            self.assertNotIn("run-ledger.md", text)

        self.assertIn("baseline", readme_text)
        self.assertIn("预期结果", readme_text)
        self.assertIn("baseline", readme_en_text)
        self.assertIn("prediction", readme_en_text)

        self.assertIn("阶段包括：", readme_text)
        self.assertIn("Included skills:", readme_en_text)

    def test_skills_readme_and_repo_agents_document_capture_insights(self):
        skills_readme_text = self.read_text(".codex/skills/README.md")
        agents_text = self.read_text("AGENTS.md")

        self.assertIn("openusim-capture-insights", skills_readme_text)
        self.assertIn("Optional after analyze", skills_readme_text)
        self.assertIn("<reference-hint>", skills_readme_text)

        self.assertIn("openusim-capture-insights", agents_text)
        self.assertIn("optional post-analysis companion skill", agents_text)
        self.assertIn("not a fifth stage", agents_text)

    def test_entry_docs_document_experiment_group_contract(self):
        skills_readme_text = self.read_text(".codex/skills/README.md")
        agents_text = self.read_text("AGENTS.md")

        for text in (skills_readme_text, agents_text):
            self.assertIn("single-case", text)
            self.assertIn("experiment-group", text)
            self.assertIn("experiment-plan.md", text)
            self.assertIn("matrix.yaml", text)
            self.assertIn("command-manifest.yaml", text)
            self.assertIn("run-ledger.md", text)
            self.assertIn("controlled-experiment-method.md", text)
            self.assertIn("prediction-vs-actual", text)

        self.assertIn("Plan owns the matrix", skills_readme_text)
        self.assertIn("run must execute only the planned matrix", agents_text)
        self.assertIn("changed variable", agents_text)
        self.assertIn("falsification signal", agents_text)

    def test_pfc_dynamic_paper_reference_names_source_paper(self):
        lessons_text = self.read_text(
            ".codex/skills/openusim-references/congestion-control-and-pfc-lessons.md"
        )
        toolchain_text = self.read_text(
            ".codex/skills/openusim-references/spec-to-toolchain.md"
        )
        scratch_readme_text = self.read_text("scratch/README.md")
        paper_title = "Congestion Control for Large-Scale RDMA Deployments"

        for text in (lessons_text, toolchain_text, scratch_readme_text):
            self.assertIn("PFC_DYNAMIC_PAPER", text)
            self.assertIn(paper_title, text)

    def test_old_openusim_helper_surface_is_gone(self):
        repo_root = self.repo_root()
        self.assertFalse((repo_root / ".codex/skills/openusim-helper/SKILL.md").exists())
        self.assertFalse((repo_root / ".codex/skills/openusim-helper" / "scripts").exists())
