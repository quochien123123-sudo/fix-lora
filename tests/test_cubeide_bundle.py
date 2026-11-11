"""Validate real generated CubeIDE project metadata without claiming IDE execution.

Run with ``python -m unittest discover -s tests -p test_cubeide_bundle.py -v``.
The original ZIPs and reviewed source overlays are used in a temporary directory;
ARM tools, CubeIDE and a connected board are not required for these checks.
"""
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "build_cubeide_bundle", ROOT / "tools/build_cubeide_bundle.py")
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)
PREFIX = BUILDER.PREFIX


class CubeIDEProjectsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="cubeide-metadata-tests-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.destination = Path(cls.temporary.name) / "Projects"
        cls.projects = BUILDER.prepare_projects(cls.destination)

    def configurations(self, project):
        return ET.parse(project / ".cproject").findall(".//configuration")

    def launch(self, project):
        files = list(project.glob("*.launch"))
        self.assertEqual(len(files), 1, project.name)
        root = ET.parse(files[0]).getroot()
        return root, {e.get("key"): e.get("value") for e in root if e.get("key")}

    def test_all_four_projects_can_coexist_in_one_workspace(self):
        expected = {"LoRa_Master", "LoRa_Node1", "LoRa_Node2", "LoRa_Node3"}
        self.assertEqual({p.name for p in self.projects}, expected)
        names = [ET.parse(p / ".project").findtext("name") for p in self.projects]
        self.assertEqual(set(names), expected)
        self.assertEqual(len(names), len(set(names)))
        config_ids = [c.get("id") for p in self.projects for c in self.configurations(p)]
        self.assertEqual(len(config_ids), len(set(config_ids)))
        for project in self.projects:
            for entry in ET.parse(project / ".cproject").findall(".//project"):
                self.assertEqual(entry.get("name"), project.name)

    def test_both_profiles_have_valid_build_and_linker_paths(self):
        for project in self.projects:
            configs = self.configurations(project)
            self.assertEqual({c.get("name") for c in configs}, {"Debug", "Release"})
            for config in configs:
                with self.subTest(project=project.name, profile=config.get("name")):
                    expected = "${workspace_loc:/" + project.name + "}/" + config.get("name")
                    self.assertEqual(config.find("folderInfo/toolChain/builder").get("buildPath"), expected)
                    self.assertEqual(config.get("artifactName"), "${ProjName}")
                    self.assertEqual(config.get("artifactExtension"), "elf")
                    for language in ["c", "cpp"]:
                        superclass = PREFIX + f"gnu.managedbuild.tool.{language}.linker.option.script"
                        scripts = [o for o in config.findall(".//option")
                                   if o.get("superClass") == superclass]
                        self.assertEqual(len(scripts), 1)
                        self.assertEqual(scripts[0].get("value"),
                                         "${workspace_loc:/${ProjName}/STM32F103C8TX_FLASH.ld}")
                    chain = config.find("folderInfo/toolChain")
                    for output in ["converthex", "convertbinary"]:
                        options = [o for o in chain.findall("option") if o.get("superClass") ==
                                   PREFIX + "gnu.managedbuild.option." + output]
                        self.assertEqual(len(options), 1)
                        self.assertEqual(options[0].get("value"), "true")

    def test_gnu11_toolchain_and_node_ids_are_consistent_in_both_profiles(self):
        for project in self.projects:
            metadata = (project / ".cproject").read_text()
            self.assertIn("<?fileVersion 4.0.0?>", metadata)
            self.assertNotIn("7-2018-q2-update", metadata)
            self.assertNotIn("option.internal.toolchain.version", metadata)
            node = int(project.name[-1]) if project.name.startswith("LoRa_Node") else None
            if node is not None:
                config = (project / "Core/Inc/app_config.h").read_text()
                self.assertRegex(config, rf"(?m)^#define NODE_ID {node}U$")
            for profile in ["Debug", "Release"]:
                flags = BUILDER.compiler_flags(project, profile)
                with self.subTest(project=project.name, profile=profile):
                    self.assertEqual(flags.count("-std=gnu11"), 1)
                    self.assertIn("-DUSE_HAL_DRIVER", flags)
                    self.assertIn("-DSTM32F103xB", flags)
                    self.assertEqual([f for f in flags if f.startswith("-DNODE_ID=")],
                                     [f"-DNODE_ID={node}"] if node else [])
                    self.assertIn("-g3" if profile == "Debug" else "-g0", flags)
                    self.assertIn("-O0" if profile == "Debug" else "-Os", flags)
                    if profile == "Debug":
                        self.assertIn("-DDEBUG", flags)
                    include_paths = [Path(f[2:]) for f in flags if f.startswith("-I")]
                    self.assertGreaterEqual(len(include_paths), 5)
                    self.assertTrue(all(p.is_dir() and p.is_relative_to(project.resolve())
                                        for p in include_paths))

    def test_settings_and_launch_references_resolve_to_real_ids(self):
        for project in self.projects:
            tree = ET.parse(project / ".cproject")
            ids = {e.get("id") for e in tree.iter() if e.get("id")}
            config_ids = {c.get("id") for c in self.configurations(project)}
            # Scanner discovery also lives directly in .cproject; stale tool/input
            # references can leave a valid build with a broken IDE indexer.
            for entry in tree.iter():
                if entry.get("instanceId"):
                    for component in entry.get("instanceId").split(";"):
                        if component.startswith(PREFIX):
                            self.assertIn(component, ids)
            for settings in (project / ".settings").glob("*.xml"):
                settings_tree = ET.parse(settings)
                for config in settings_tree.findall("configuration"):
                    self.assertIn(config.get("id"), config_ids)
                for entry in settings_tree.iter():
                    if entry.get("instanceId"):
                        for component in entry.get("instanceId").split(";"):
                            if component.startswith(PREFIX):
                                self.assertIn(component, ids)
            launch, attrs = self.launch(project)
            debug_id = next(c.get("id") for c in self.configurations(project)
                            if c.get("name") == "Debug")
            self.assertEqual(attrs["org.eclipse.cdt.launch.PROJECT_BUILD_CONFIG_ID_ATTR"], debug_id)
            self.assertEqual(attrs["org.eclipse.cdt.launch.PROJECT_ATTR"], project.name)
            self.assertEqual(attrs["org.eclipse.cdt.launch.PROGRAM_NAME"],
                             f"Debug/{project.name}.elf")
            items = json.loads(attrs[PREFIX + "debug.launch.loadList"])["fItems"]
            self.assertEqual(len(items), 1)
            self.assertEqual(items[0]["fProjectName"], project.name)
            self.assertEqual(items[0]["fPath"], f"Debug/{project.name}.elf")
            self.assertTrue(items[0]["fPerformBuild"])
            self.assertTrue(items[0]["fDownload"])
            self.assertTrue(items[0]["fLoadSymbols"])
            mapped = launch.find("listAttribute[@key='org.eclipse.debug.core.MAPPED_RESOURCE_PATHS']")
            self.assertEqual([e.get("value") for e in mapped], ["/" + project.name])

    def test_metadata_contains_no_machine_specific_windows_paths(self):
        for project in self.projects:
            files = [project / ".project", project / ".cproject"]
            files += list((project / ".settings").glob("*.xml"))
            files += list(project.glob("*.launch"))
            for file in files:
                with self.subTest(file=str(file.relative_to(self.destination))):
                    text = file.read_text()
                    self.assertIsNone(re.search(r"(?<![A-Za-z_])[A-Za-z]:[\\/]", text))
                    self.assertNotIn("C:\\Users\\Asus", text)
                    self.assertNotIn("/workspace/", text)

    def test_sources_and_reserved_flash_layout_are_complete(self):
        required = ["Core/Src/main.c", "Core/Src/lora.c", "Core/Src/boot_counter.c",
                    "Core/Src/sysmem.c", "Core/Inc/main.h", "Core/Inc/lora.h",
                    "Core/Inc/protocol.h", "Core/Startup/startup_stm32f103c8tx.s",
                    "Drivers/CMSIS/Include/core_cm3.h",
                    "Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f103xb.h",
                    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal.c",
                    "Drivers/STM32F1xx_HAL_Driver/Inc/stm32f1xx_hal.h",
                    "STM32F103C8TX_FLASH.ld"]
        for project in self.projects:
            for relative in required:
                self.assertTrue((project / relative).is_file(), f"{project.name}/{relative}")
            self.assertRegex((project / "Core/Src/sysmem.c").read_text(),
                             r"(?m)^\s*#\s*include\s*<sys/types\.h>")
            script = (project / "STM32F103C8TX_FLASH.ld").read_text()
            self.assertRegex(script, r"FLASH\s*\(rx\)\s*:\s*ORIGIN\s*=\s*0x8000000,\s*LENGTH\s*=\s*62K")
            self.assertFalse((project / "Debug").exists())
            self.assertFalse((project / "Release").exists())

    def test_reference_ioc_cannot_be_mistaken_for_regeneratable_project(self):
        for project in self.projects:
            self.assertEqual(list(project.glob("*.ioc")), [])
            self.assertEqual(len(list((project / "Reference").glob("*.ioc"))), 1)
            natures = [e.text for e in ET.parse(project / ".project").findall("natures/nature")]
            self.assertNotIn(PREFIX + "MCUCubeProjectNature", natures)
            self.assertIn(PREFIX + "MCUProjectNature", natures)
            self.assertIn("org.eclipse.cdt.core.cnature", natures)

    def test_launch_uses_swd_and_documents_reset_wiring(self):
        readme = (ROOT / "CUBEIDE_README.md").read_text()
        for project in self.projects:
            launch, attrs = self.launch(project)
            self.assertEqual(attrs[PREFIX + "debug.launch.swd_mode"], "true")
            self.assertEqual(attrs["org.eclipse.cdt.debug.gdbjtag.core.jtagDevice"],
                             "ST-LINK (ST-LINK GDB server)")
            self.assertEqual(attrs[PREFIX + "debug.launch.verify_flash_download"], "true")
            if attrs[PREFIX + "debug.stlink.reset_strategy"] == "connect_under_reset":
                self.assertIn("NRST", readme)
            self.assertNotIn(PREFIX + "debug.stlink.log_file", attrs)

    def test_preparation_never_overwrites_an_existing_directory(self):
        sentinel = self.destination / "user-file.txt"
        sentinel.write_text("preserve my local work")
        with self.assertRaises(FileExistsError):
            BUILDER.prepare_projects(self.destination)
        self.assertEqual(sentinel.read_text(), "preserve my local work")


if __name__ == "__main__":
    unittest.main()
