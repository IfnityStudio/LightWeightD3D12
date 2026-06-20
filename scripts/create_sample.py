#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import shutil
import sys
import tempfile
import urllib.request
import uuid
import zipfile
from pathlib import Path


CPP_PROJECT_TYPE_GUID = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}"
LIGHTD3D12_PROJECT_GUID = "{A96F3AFD-6AB4-4DF6-BF2B-D3D31D34F9C9}"
ENKITS_URL = "https://github.com/dougbinks/enkiTS/archive/03e6a2c0c97208ade44478d617d2002b0f95faf4.zip"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def ask_yes_no(question: str, default: bool = False) -> bool:
    suffix = "Y/n" if default else "y/N"
    while True:
        answer = input(f"{question} [{suffix}]: ").strip().lower()
        if not answer:
            return default
        if answer in {"y", "yes", "s", "si"}:
            return True
        if answer in {"n", "no"}:
            return False
        print("Please answer y/n.")


def sanitize_project_name(raw: str) -> str:
    name = re.sub(r"[^A-Za-z0-9_]", "", raw.strip())
    if not name:
        raise ValueError("The sample name cannot be empty.")
    if name[0].isdigit():
        name = f"Sample{name}"
    return name


def read_solution(path: Path) -> tuple[str, str]:
    text = path.read_text(encoding="utf-8-sig")
    newline = "\r\n" if "\r\n" in text else "\n"
    return text, newline


def ensure_enkits(root: Path) -> None:
    target = root / "third_party" / "enkiTS"
    scheduler_cpp = target / "src" / "TaskScheduler.cpp"
    if scheduler_cpp.exists():
        print("enkiTS already exists in third_party/enkiTS.")
        return

    if target.exists() and any(target.iterdir()):
        raise RuntimeError(
            "third_party/enkiTS exists but does not look like a valid enkiTS copy. "
            "Please inspect it or delete it before continuing."
        )

    print("Downloading official enkiTS into third_party/enkiTS...")
    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        archive_path = temp_path / "enkits.zip"
        urllib.request.urlretrieve(ENKITS_URL, archive_path)

        extract_dir = temp_path / "extract"
        with zipfile.ZipFile(archive_path, "r") as archive:
            archive.extractall(extract_dir)

        extracted_roots = [entry for entry in extract_dir.iterdir() if entry.is_dir()]
        if len(extracted_roots) != 1:
            raise RuntimeError("Could not detect the extracted enkiTS root folder.")

        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            shutil.rmtree(target)
        shutil.move(str(extracted_roots[0]), str(target))

    if not scheduler_cpp.exists():
        raise RuntimeError("La descarga de enkiTS termino, pero falta src/TaskScheduler.cpp.")


def main_cpp(project_name: str, use_lightd3d12: bool, use_enkits: bool) -> str:
    includes: list[str] = []
    body: list[str] = []

    if use_lightd3d12:
        includes.append('#include <LightD3D12/LightD3D12.hpp>')
    if use_enkits:
        includes.append('#include "TaskScheduler.h"')
        includes.extend(
            [
                "",
                "#include <cstdint>",
                "#include <iostream>",
                "#include <mutex>",
            ]
        )
    else:
        includes.append("#include <iostream>")

    if use_enkits:
        body.append(
            """
namespace
{
    std::mutex gOutputMutex;

    class ExampleTask final : public enki::ITaskSet
    {
    public:
        ExampleTask(): enki::ITaskSet( 1024, 128 ) {}

        void ExecuteRange( enki::TaskSetPartition range, uint32_t threadnum ) override
        {
            uint64_t checksum = 0;
            for( uint32_t index = range.start; index < range.end; ++index )
            {
                checksum += static_cast<uint64_t>( index + 1u ) * 17u;
            }

            std::lock_guard<std::mutex> lock( gOutputMutex );
            std::cout << "Task range [" << range.start << ", " << range.end
                      << ") on thread " << threadnum
                      << ", checksum=" << checksum << '\\n';
        }
    };
}
""".strip()
        )

    body.append("int main()")
    body.append("{")
    body.append(f'    std::cout << "{project_name} running.\\n";')

    if use_lightd3d12:
        body.append("    lightd3d12::SubmitHandle emptySubmitHandle{};")
        body.append("    (void)emptySubmitHandle;")

    if use_enkits:
        body.extend(
            [
                "",
                "    enki::TaskScheduler scheduler;",
                "    scheduler.Initialize();",
                "",
                "    ExampleTask task;",
                "    scheduler.AddTaskSetToPipe( &task );",
                "    scheduler.WaitforAll();",
                "    scheduler.WaitforAllAndShutdown();",
            ]
        )

    body.append("    return 0;")
    body.append("}")

    return "\n".join(includes) + "\n\n" + "\n".join(body) + "\n"


def vcxproj(project_name: str, project_guid: str, use_lightd3d12: bool, use_enkits: bool) -> str:
    include_dirs: list[str] = []
    dependencies: list[str] = []
    extra_clcompile = ""
    extra_clinclude = ""
    project_reference = ""

    if use_lightd3d12:
        include_dirs.extend(
            [
                "$(SolutionDir)LightD3D12\\include",
                "$(SolutionDir)third_party\\imgui",
            ]
        )
        dependencies.extend(["d3d12.lib", "dxgi.lib", "dxguid.lib", "d3dcompiler.lib"])
        project_reference = f"""
  <ItemGroup>
    <ProjectReference Include="..\\..\\LightD3D12\\LightD3D12.vcxproj">
      <Project>{LIGHTD3D12_PROJECT_GUID}</Project>
    </ProjectReference>
  </ItemGroup>"""

    if use_enkits:
        include_dirs.append("$(SolutionDir)third_party\\enkiTS\\src")
        extra_clcompile = """
    <ClCompile Include="..\\..\\third_party\\enkiTS\\src\\TaskScheduler.cpp" />"""
        extra_clinclude = """
  <ItemGroup>
    <ClInclude Include="..\\..\\third_party\\enkiTS\\src\\Atomics.h" />
    <ClInclude Include="..\\..\\third_party\\enkiTS\\src\\LockLessMultiReadPipe.h" />
    <ClInclude Include="..\\..\\third_party\\enkiTS\\src\\TaskScheduler.h" />
    <ClInclude Include="..\\..\\third_party\\enkiTS\\src\\Threads.h" />
  </ItemGroup>"""

    include_dirs.append("%(AdditionalIncludeDirectories)")
    dependencies.append("%(AdditionalDependencies)")
    include_dirs_text = ";".join(include_dirs)
    dependencies_text = ";".join(dependencies)
    target_name = f"LightD3D12{project_name}" if use_lightd3d12 else project_name

    return f"""<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>17.0</VCProjectVersion>
    <ProjectGuid>{project_guid}</ProjectGuid>
    <Keyword>Win32Proj</Keyword>
    <RootNamespace>{project_name}</RootNamespace>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <UseDebugLibraries>true</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <UseDebugLibraries>false</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <WholeProgramOptimization>true</WholeProgramOptimization>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
  <ImportGroup Label="ExtensionSettings" />
  <ImportGroup Label="Shared" />
  <ImportGroup Label="PropertySheets" Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <Import Project="$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props" Condition="exists('$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props')" Label="LocalAppDataPlatform" />
  </ImportGroup>
  <ImportGroup Label="PropertySheets" Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <Import Project="$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props" Condition="exists('$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props')" Label="LocalAppDataPlatform" />
  </ImportGroup>
  <Import Project="$(SolutionDir)LightD3D12.PIX.props" Condition="exists('$(SolutionDir)LightD3D12.PIX.props')" />
  <PropertyGroup Label="UserMacros" />
  <PropertyGroup>
    <OutDir>$(SolutionDir)build\\$(Configuration)\\</OutDir>
    <IntDir>$(SolutionDir)build\\obj\\$(ProjectName)\\$(Configuration)\\</IntDir>
    <TargetName>{target_name}</TargetName>
  </PropertyGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <ClCompile>
      <WarningLevel>Level4</WarningLevel>
      <SDLCheck>true</SDLCheck>
      <PreprocessorDefinitions>WIN32_LEAN_AND_MEAN;NOMINMAX;UNICODE;_UNICODE;_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <ConformanceMode>true</ConformanceMode>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <AdditionalIncludeDirectories>{include_dirs_text}</AdditionalIncludeDirectories>
      <ExceptionHandling>Sync</ExceptionHandling>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
    </ClCompile>
    <Link>
      <SubSystem>Console</SubSystem>
      <AdditionalDependencies>{dependencies_text}</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <ClCompile>
      <WarningLevel>Level4</WarningLevel>
      <FunctionLevelLinking>true</FunctionLevelLinking>
      <IntrinsicFunctions>true</IntrinsicFunctions>
      <SDLCheck>true</SDLCheck>
      <PreprocessorDefinitions>WIN32_LEAN_AND_MEAN;NOMINMAX;UNICODE;_UNICODE;NDEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <ConformanceMode>true</ConformanceMode>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <AdditionalIncludeDirectories>{include_dirs_text}</AdditionalIncludeDirectories>
      <ExceptionHandling>Sync</ExceptionHandling>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
    </ClCompile>
    <Link>
      <SubSystem>Console</SubSystem>
      <AdditionalDependencies>{dependencies_text}</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="main.cpp" />{extra_clcompile}
  </ItemGroup>{extra_clinclude}{project_reference}
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
  <ImportGroup Label="ExtensionTargets" />
</Project>
"""


def filters(use_enkits: bool) -> str:
    enki_filter = ""
    enki_compile = ""
    enki_headers = ""
    if use_enkits:
        enki_filter = """
    <Filter Include="enkiTS">
      <UniqueIdentifier>{76F6C4D5-6140-41D4-8BC7-0D9BAA6B673D}</UniqueIdentifier>
    </Filter>"""
        enki_compile = """
    <ClCompile Include="..\\..\\third_party\\enkiTS\\src\\TaskScheduler.cpp">
      <Filter>enkiTS</Filter>
    </ClCompile>"""
        enki_headers = """
  <ItemGroup>
    <ClInclude Include="..\\..\\third_party\\enkiTS\\src\\Atomics.h">
      <Filter>enkiTS</Filter>
    </ClInclude>
    <ClInclude Include="..\\..\\third_party\\enkiTS\\src\\LockLessMultiReadPipe.h">
      <Filter>enkiTS</Filter>
    </ClInclude>
    <ClInclude Include="..\\..\\third_party\\enkiTS\\src\\TaskScheduler.h">
      <Filter>enkiTS</Filter>
    </ClInclude>
    <ClInclude Include="..\\..\\third_party\\enkiTS\\src\\Threads.h">
      <Filter>enkiTS</Filter>
    </ClInclude>
  </ItemGroup>"""

    return f"""<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup>
    <Filter Include="Source Files">
      <UniqueIdentifier>{{A4890C87-880F-42C6-AF6B-2EC412124CEC}}</UniqueIdentifier>
    </Filter>{enki_filter}
  </ItemGroup>
  <ItemGroup>
    <ClCompile Include="main.cpp">
      <Filter>Source Files</Filter>
    </ClCompile>{enki_compile}
  </ItemGroup>{enki_headers}
</Project>
"""


def readme(project_name: str, use_lightd3d12: bool, use_enkits: bool) -> str:
    deps = []
    if use_lightd3d12:
        deps.append("LightD3D12")
    if use_enkits:
        deps.append("enkiTS")
    deps_text = ", ".join(deps) if deps else "no extra dependencies"
    return f"""# {project_name}

Sample generated with `scripts/create_sample.py`.

Dependencies: {deps_text}.

Build it from `LightD3D12.sln` using `Debug|x64` or `Release|x64`.
"""


def add_project_to_solution(root: Path, project_name: str, project_guid: str) -> None:
    sln_path = root / "LightD3D12.sln"
    text, newline = read_solution(sln_path)
    rel_project_path = f"samples\\{project_name}\\{project_name}.vcxproj"

    if rel_project_path in text or f'= "{project_name}",' in text:
        raise RuntimeError(f"{project_name} ya parece estar registrado en LightD3D12.sln.")

    project_block = (
        f'Project("{CPP_PROJECT_TYPE_GUID}") = "{project_name}", "{rel_project_path}", "{project_guid}"{newline}'
        f"EndProject{newline}"
    )
    global_index = text.find(f"{newline}Global")
    if global_index == -1:
        raise RuntimeError("No se encontro la seccion Global en LightD3D12.sln.")
    text = text[: global_index + len(newline)] + project_block + text[global_index + len(newline):]

    config_lines = (
        f"\t\t{project_guid}.Debug|x64.ActiveCfg = Debug|x64{newline}"
        f"\t\t{project_guid}.Debug|x64.Build.0 = Debug|x64{newline}"
        f"\t\t{project_guid}.Release|x64.ActiveCfg = Release|x64{newline}"
        f"\t\t{project_guid}.Release|x64.Build.0 = Release|x64{newline}"
    )
    section_match = re.search(
        r"(\tGlobalSection\(ProjectConfigurationPlatforms\) = postSolution\r?\n)(.*?)(\tEndGlobalSection)",
        text,
        flags=re.DOTALL,
    )
    if not section_match:
        raise RuntimeError("No se encontro ProjectConfigurationPlatforms en LightD3D12.sln.")

    insert_at = section_match.start(3)
    text = text[:insert_at] + config_lines + text[insert_at:]
    sln_path.write_text(text, encoding="utf-8")


def create_sample(project_name: str, use_lightd3d12: bool, use_enkits: bool, force: bool) -> None:
    root = repo_root()
    samples_root = root / "samples"
    sample_dir = samples_root / project_name

    if sample_dir.exists():
        if not force:
            raise RuntimeError(f"samples/{project_name} already exists. Use --force if you want to overwrite sample files.")

    if use_enkits:
        ensure_enkits(root)

    sample_dir.mkdir(parents=True, exist_ok=True)

    project_guid = "{" + str(uuid.uuid4()).upper() + "}"
    files = {
        sample_dir / "main.cpp": main_cpp(project_name, use_lightd3d12, use_enkits),
        sample_dir / f"{project_name}.vcxproj": vcxproj(project_name, project_guid, use_lightd3d12, use_enkits),
        sample_dir / f"{project_name}.vcxproj.filters": filters(use_enkits),
        sample_dir / "README.md": readme(project_name, use_lightd3d12, use_enkits),
    }

    for path, content in files.items():
        if path.exists() and not force:
            raise RuntimeError(f"{path.relative_to(root)} already exists. Use --force to overwrite it.")
        path.write_text(content, encoding="utf-8", newline="\r\n")

    add_project_to_solution(root, project_name, project_guid)

    print(f"Sample created: samples/{project_name}")
    print(f"Project added to LightD3D12.sln with GUID {project_guid}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Create a sample and add it to LightD3D12.sln.")
    parser.add_argument("--name", help="Sample/project name.")
    parser.add_argument("--lightd3d12", action="store_true", help="Include LightD3D12.")
    parser.add_argument("--enkits", action="store_true", help="Include enkiTS.")
    parser.add_argument("--force", action="store_true", help="Overwrite sample files if they already exist.")
    parser.add_argument("--non-interactive", action="store_true", help="Do not ask questions; requires --name.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.non_interactive:
            if not args.name:
                raise ValueError("--non-interactive requires --name.")
            project_name = sanitize_project_name(args.name)
            use_lightd3d12 = args.lightd3d12
            use_enkits = args.enkits
        else:
            raw_name = args.name or input("Sample name: ")
            project_name = sanitize_project_name(raw_name)
            use_lightd3d12 = args.lightd3d12 or ask_yes_no("Use LightD3D12?", True)
            use_enkits = args.enkits or ask_yes_no("Use enkiTS?", False)

        create_sample(project_name, use_lightd3d12, use_enkits, args.force)
        return 0
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
