#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


ROOT_VCPKG_DEPENDENCIES = [
    "directxtk12",
    "winpixevent",
]

OPTIONAL_PROJECTS = {
    "assimp": {
        "AssimpDuck",
        "MeshDeformation",
        "VolumetricClouds",
    },
    "fsr": {
        "Upscaler",
    },
    "openusd": {
        "UsdStaticScene",
    },
}

PROJECT_RE = re.compile(
    r'^Project\("\{[0-9A-Fa-f\-]+\}"\) = "([^"]+)", "([^"]+)", "(\{[0-9A-Fa-f\-]+\})"$'
)
CONFIG_ACTIVE_RE = re.compile(
    r"^(\s*)(\{[0-9A-Fa-f\-]+\})\.(Debug|Release)\|x64\.ActiveCfg = (Debug|Release)\|x64$"
)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a LightD3D12 solution and install the selected optional dependencies."
    )
    parser.add_argument("--assimp", choices=["on", "off"], help="Enable or disable Assimp projects/support.")
    parser.add_argument("--fsr", choices=["on", "off"], help="Enable or disable AMD FSR SDK projects/support.")
    parser.add_argument("--openusd", choices=["on", "off"], help="Enable or disable OpenUSD projects/support.")
    parser.add_argument("--skip-install", action="store_true", help="Only generate files; do not run vcpkg/bootstrap.")
    parser.add_argument("--non-interactive", action="store_true", help="Use defaults for unspecified options.")
    return parser.parse_args()


def ask_bool(question: str, default: bool) -> bool:
    suffix = "Y/n" if default else "y/N"
    while True:
        value = input(f"{question} [{suffix}]: ").strip().lower()
        if not value:
            return default
        if value in {"y", "yes"}:
            return True
        if value in {"n", "no"}:
            return False
        print("Please answer yes or no.")


def option_value(raw: str | None, default: bool, question: str, non_interactive: bool) -> bool:
    if raw is not None:
        return raw == "on"
    if non_interactive:
        return default
    return ask_bool(question, default)


def collect_project_guids(solution_text: str) -> dict[str, str]:
    guids: dict[str, str] = {}
    for line in solution_text.splitlines():
        match = PROJECT_RE.match(line)
        if match:
            name = match.group(1)
            guid = match.group(3).upper()
            guids[name] = guid
    return guids


def filter_solution(solution_text: str, enabled: dict[str, bool]) -> str:
    disabled_project_names: set[str] = set()
    enabled_optional_names: set[str] = set()
    for feature, project_names in OPTIONAL_PROJECTS.items():
        if enabled[feature]:
            enabled_optional_names.update(project_names)
        else:
            disabled_project_names.update(project_names)

    project_guids = collect_project_guids(solution_text)
    disabled_guids = {
        project_guids[name].upper()
        for name in disabled_project_names
        if name in project_guids
    }
    enabled_optional_guids = {
        project_guids[name].upper()
        for name in enabled_optional_names
        if name in project_guids
    }

    source_lines = solution_text.splitlines()
    output_lines: list[str] = []
    index = 0
    in_project_config_section = False

    while index < len(source_lines):
        line = source_lines[index]

        if line.startswith("Project("):
            block = [line]
            index += 1
            while index < len(source_lines):
                block.append(source_lines[index])
                if source_lines[index] == "EndProject":
                    break
                index += 1

            match = PROJECT_RE.match(block[0])
            if match:
                guid = match.group(3).upper()
                if guid not in disabled_guids:
                    output_lines.extend(block)
            else:
                output_lines.extend(block)

            index += 1
            continue

        if line.strip() == "GlobalSection(ProjectConfigurationPlatforms) = postSolution":
            in_project_config_section = True
            output_lines.append(line)
            index += 1
            continue

        if in_project_config_section and line.strip() == "EndGlobalSection":
            in_project_config_section = False
            output_lines.append(line)
            index += 1
            continue

        if in_project_config_section:
            upper_line = line.upper()
            if any(guid in upper_line for guid in disabled_guids):
                index += 1
                continue

            is_optional_build_line = ".BUILD.0 =" in upper_line and any(
                guid in upper_line for guid in enabled_optional_guids
            )
            if is_optional_build_line:
                index += 1
                continue

            output_lines.append(line)

            match = CONFIG_ACTIVE_RE.match(line)
            if match:
                indent, guid, solution_config, project_config = match.groups()
                normalized_guid = guid.upper()
                if normalized_guid in enabled_optional_guids:
                    output_lines.append(
                        f"{indent}{guid}.{solution_config}|x64.Build.0 = {project_config}|x64"
                    )

            index += 1
            continue

        output_lines.append(line)
        index += 1

    return "\r\n".join(output_lines) + "\r\n"


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    path.write_text(normalized, encoding="utf-8", newline="\r\n")


def generate_vcpkg_manifest(root: Path, enabled: dict[str, bool]) -> Path:
    dependencies = list(ROOT_VCPKG_DEPENDENCIES)
    if enabled["assimp"]:
        dependencies.append("assimp")
    if enabled["openusd"]:
        dependencies.append("usd")

    manifest = {
        "$schema": "https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg.schema.json",
        "name": "lightweightd3d12-generated",
        "version-string": "0.1.0",
        "dependencies": dependencies,
    }

    manifest_path = root / "build" / "generated" / "vcpkg" / "vcpkg.json"
    write_text(manifest_path, json.dumps(manifest, indent=2) + "\n")
    return manifest_path


def generate_feature_props(root: Path, enabled: dict[str, bool]) -> Path:
    def msbuild_bool(value: bool) -> str:
        return "true" if value else "false"

    assimp_root = ""
    if enabled["assimp"]:
        assimp_root = (
            "    <LightD3D12AssimpRoot>$(MSBuildThisFileDirectory)..\\..\\vcpkg_installed\\$(VcpkgTriplet)\\</LightD3D12AssimpRoot>\n"
        )

    props = f"""<?xml version="1.0" encoding="utf-8"?>
<Project>
  <PropertyGroup>
    <LightD3D12GeneratedConfiguration>true</LightD3D12GeneratedConfiguration>
    <LightD3D12EnableAssimp>{msbuild_bool(enabled["assimp"])}</LightD3D12EnableAssimp>
    <LightD3D12EnableAmdFsrSdk>{msbuild_bool(enabled["fsr"])}</LightD3D12EnableAmdFsrSdk>
    <LightD3D12EnableOpenUsd>{msbuild_bool(enabled["openusd"])}</LightD3D12EnableOpenUsd>
    <VcpkgManifestRoot>$(MSBuildThisFileDirectory)vcpkg\\</VcpkgManifestRoot>
{assimp_root.rstrip()}
  </PropertyGroup>
</Project>
"""
    props_path = root / "build" / "generated" / "LightD3D12.Features.props"
    write_text(props_path, props)
    return props_path


def generate_solution(root: Path, enabled: dict[str, bool]) -> Path:
    source_solution = root / "LightD3D12.sln"
    generated_solution = root / "LightD3D12.generated.sln"
    solution_text = source_solution.read_text(encoding="utf-8")
    write_text(generated_solution, filter_solution(solution_text, enabled))
    return generated_solution


def run_bootstrap(root: Path, enabled: dict[str, bool]) -> None:
    command = [
        "powershell",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(root / "scripts" / "bootstrap-vcpkg.ps1"),
        "-ManifestRoot",
        "build\\generated\\vcpkg",
    ]
    if not enabled["fsr"]:
        command.append("-SkipAmdFsrSdk")

    subprocess.run(command, cwd=root, check=True)


def main() -> int:
    root = repo_root()
    args = parse_args()

    try:
        enabled = {
            "assimp": option_value(args.assimp, True, "Enable Assimp?", args.non_interactive),
            "fsr": option_value(args.fsr, False, "Enable FSR?", args.non_interactive),
            "openusd": option_value(args.openusd, False, "Enable OpenUSD?", args.non_interactive),
        }

        manifest_path = generate_vcpkg_manifest(root, enabled)
        props_path = generate_feature_props(root, enabled)
        solution_path = generate_solution(root, enabled)

        print("")
        print("Generated configuration:")
        print(f"  Solution: {solution_path.relative_to(root)}")
        print(f"  Feature props: {props_path.relative_to(root)}")
        print(f"  vcpkg manifest: {manifest_path.relative_to(root)}")
        print(f"  Assimp: {'enabled' if enabled['assimp'] else 'disabled'}")
        print(f"  FSR: {'enabled' if enabled['fsr'] else 'disabled'}")
        print(f"  OpenUSD: {'enabled' if enabled['openusd'] else 'disabled'}")

        if args.skip_install:
            print("")
            print("Skipped dependency install.")
        else:
            print("")
            print("Installing selected dependencies...")
            sys.stdout.flush()
            run_bootstrap(root, enabled)

        print("")
        print("OK")
        print("Open LightD3D12.generated.sln to build this configuration.")
        return 0
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
