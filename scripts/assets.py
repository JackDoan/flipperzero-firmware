#!/usr/bin/env python3

from flipper.app import App

import logging
import argparse
import subprocess
import io
import os
import sys

ICONS_SUPPORTED_FORMATS = ["png"]

ICONS_TEMPLATE_H_HEADER = """#pragma once
#include <gui/icon.h>

"""
ICONS_TEMPLATE_H_ICON_NAME = "extern const Icon {name};\n"

ICONS_TEMPLATE_C_HEADER = """#include \"assets_icons.h\"

#include <gui/icon_i.h>

"""
ICONS_TEMPLATE_C_FRAME = "const uint8_t {name}[] = {data};\n"
ICONS_TEMPLATE_C_DATA = "const uint8_t* const {name}[] = {data};\n"
ICONS_TEMPLATE_C_ICONS = "const Icon {name} = {{.width={width},.height={height},.frame_count={frame_count},.frame_rate={frame_rate},.frames=_{name}}};\n"


class Main(App):
    def init(self):
        # command args
        self.subparsers = self.parser.add_subparsers(help="sub-command help")
        self.parser_icons = self.subparsers.add_parser(
            "icons", help="Process icons and build icon registry"
        )
        self.parser_icons.add_argument("input_directory", help="Source directory")
        self.parser_icons.add_argument("output_directory", help="Output directory")
        self.parser_icons.set_defaults(func=self.icons)

        self.parser_manifest = self.subparsers.add_parser(
            "manifest", help="Create directory Manifest"
        )
        self.parser_manifest.add_argument("local_path", help="local_path")
        self.parser_manifest.set_defaults(func=self.manifest)

        self.parser_copro = self.subparsers.add_parser(
            "copro", help="Gather copro binaries for packaging"
        )
        self.parser_copro.add_argument("cube_dir", help="Path to Cube folder")
        self.parser_copro.add_argument("output_dir", help="Path to output folder")
        self.parser_copro.add_argument("mcu", help="MCU series as in copro folder")
        self.parser_copro.set_defaults(func=self.copro)

        self.parser_dolphin = self.subparsers.add_parser(
            "dolphin", help="Assemble dolphin resources"
        )
        self.parser_dolphin.add_argument(
            "-s",
            "--symbol-name",
            help="Symbol and file name in dolphin output directory",
            default=None,
        )
        self.parser_dolphin.add_argument(
            "input_directory", help="Dolphin source directory"
        )
        self.parser_dolphin.add_argument(
            "output_directory", help="Dolphin output directory"
        )
        self.parser_dolphin.set_defaults(func=self.dolphin)

    def _icon2header(self, file):
        output = subprocess.check_output(["convert", file, "xbm:-"])
        assert output
        f = io.StringIO(output.decode().strip())
        width = int(f.readline().strip().split(" ")[2])
        height = int(f.readline().strip().split(" ")[2])
        data = f.read().strip().replace("\n", "").replace(" ", "").split("=")[1][:-1]
        data_bin_str = data[1:-1].replace(",", " ").replace("0x", "")
        data_bin = bytearray.fromhex(data_bin_str)
        # Encode icon data with LZSS
        data_encoded_str = subprocess.check_output(
            ["heatshrink", "-e", "-w8", "-l4"], input=data_bin
        )
        assert data_encoded_str
        data_enc = bytearray(data_encoded_str)
        data_enc = bytearray([len(data_enc) & 0xFF, len(data_enc) >> 8]) + data_enc
        # Use encoded data only if its lenght less than original, including header
        if len(data_enc) < len(data_bin) + 1:
            data = (
                "{0x01,0x00,"
                + "".join("0x{:02x},".format(byte) for byte in data_enc)
                + "}"
            )
        else:
            data = "{0x00," + data[1:]
        return width, height, data

    def _iconIsSupported(self, filename):
        extension = filename.lower().split(".")[-1]
        return extension in ICONS_SUPPORTED_FORMATS

    def icons(self):
        self.logger.debug(f"Converting icons")
        icons_c = open(os.path.join(self.args.output_directory, "assets_icons.c"), "w")
        icons_c.write(ICONS_TEMPLATE_C_HEADER)
        icons = []
        # Traverse icons tree, append image data to source file
        for dirpath, dirnames, filenames in os.walk(self.args.input_directory):
            self.logger.debug(f"Processing directory {dirpath}")
            dirnames.sort()
            filenames.sort()
            if not filenames:
                continue
            if "frame_rate" in filenames:
                self.logger.debug(f"Folder contatins animation")
                icon_name = "A_" + os.path.split(dirpath)[1].replace("-", "_")
                width = height = None
                frame_count = 0
                frame_rate = 0
                frame_names = []
                for filename in sorted(filenames):
                    fullfilename = os.path.join(dirpath, filename)
                    if filename == "frame_rate":
                        frame_rate = int(open(fullfilename, "r").read().strip())
                        continue
                    elif not self._iconIsSupported(filename):
                        continue
                    self.logger.debug(f"Processing animation frame {filename}")
                    temp_width, temp_height, data = self._icon2header(fullfilename)
                    if width is None:
                        width = temp_width
                    if height is None:
                        height = temp_height
                    assert width == temp_width
                    assert height == temp_height
                    frame_name = f"_{icon_name}_{frame_count}"
                    frame_names.append(frame_name)
                    icons_c.write(
                        ICONS_TEMPLATE_C_FRAME.format(name=frame_name, data=data)
                    )
                    frame_count += 1
                assert frame_rate > 0
                assert frame_count > 0
                icons_c.write(
                    ICONS_TEMPLATE_C_DATA.format(
                        name=f"_{icon_name}", data=f'{{{",".join(frame_names)}}}'
                    )
                )
                icons_c.write("\n")
                icons.append((icon_name, width, height, frame_rate, frame_count))
            else:
                # process icons
                for filename in filenames:
                    if not self._iconIsSupported(filename):
                        continue
                    self.logger.debug(f"Processing icon {filename}")
                    icon_name = "I_" + "_".join(filename.split(".")[:-1]).replace(
                        "-", "_"
                    )
                    fullfilename = os.path.join(dirpath, filename)
                    width, height, data = self._icon2header(fullfilename)
                    frame_name = f"_{icon_name}_0"
                    icons_c.write(
                        ICONS_TEMPLATE_C_FRAME.format(name=frame_name, data=data)
                    )
                    icons_c.write(
                        ICONS_TEMPLATE_C_DATA.format(
                            name=f"_{icon_name}", data=f"{{{frame_name}}}"
                        )
                    )
                    icons_c.write("\n")
                    icons.append((icon_name, width, height, 0, 1))
        # Create array of images:
        self.logger.debug(f"Finalizing source file")
        for name, width, height, frame_rate, frame_count in icons:
            icons_c.write(
                ICONS_TEMPLATE_C_ICONS.format(
                    name=name,
                    width=width,
                    height=height,
                    frame_rate=frame_rate,
                    frame_count=frame_count,
                )
            )
        icons_c.write("\n")
        # Create Public Header
        self.logger.debug(f"Creating header")
        icons_h = open(os.path.join(self.args.output_directory, "assets_icons.h"), "w")
        icons_h.write(ICONS_TEMPLATE_H_HEADER)
        for name, width, height, frame_rate, frame_count in icons:
            icons_h.write(ICONS_TEMPLATE_H_ICON_NAME.format(name=name))
        self.logger.debug(f"Done")
        return 0

    def manifest(self):
        from flipper.assets.manifest import Manifest

        directory_path = os.path.normpath(self.args.local_path)
        if not os.path.isdir(directory_path):
            self.logger.error(f'"{directory_path}" is not a directory')
            exit(255)
        manifest_file = os.path.join(directory_path, "Manifest")
        old_manifest = Manifest()
        if os.path.exists(manifest_file):
            self.logger.info("old manifest is present, loading for compare")
            old_manifest.load(manifest_file)
        self.logger.info(f'Creating new Manifest for directory "{directory_path}"')
        new_manifest = Manifest()
        new_manifest.create(directory_path)

        self.logger.info(f"Comparing new manifest with old")
        only_in_old, changed, only_in_new = Manifest.compare(old_manifest, new_manifest)
        for record in only_in_old:
            self.logger.info(f"Only in old: {record}")
        for record in changed:
            self.logger.info(f"Changed: {record}")
        for record in only_in_new:
            self.logger.info(f"Only in new: {record}")
        if any((only_in_old, changed, only_in_new)):
            self.logger.warning("Manifests are different, updating")
            new_manifest.save(manifest_file)
        else:
            self.logger.info("Manifest is up-to-date!")

        self.logger.info(f"Complete")

        return 0

    def copro(self):
        from flipper.assets.copro import Copro

        self.logger.info(f"Bundling coprocessor binaries")
        copro = Copro(self.args.mcu)
        self.logger.info(f"Loading CUBE info")
        copro.loadCubeInfo(self.args.cube_dir)
        self.logger.info(f"Bundling")
        copro.bundle(self.args.output_dir)
        self.logger.info(f"Complete")

        return 0

    def dolphin(self):
        from flipper.assets.dolphin import Dolphin

        self.logger.info(f"Processing Dolphin sources")
        dolphin = Dolphin()
        self.logger.info(f"Loading data")
        dolphin.load(self.args.input_directory)
        self.logger.info(f"Packing")
        dolphin.pack(self.args.output_directory, self.args.symbol_name)
        self.logger.info(f"Complete")

        return 0


if __name__ == "__main__":
    Main()()
