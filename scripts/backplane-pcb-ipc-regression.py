#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the headless PCB operations added to the stable fork."""

import argparse
from pathlib import Path
import uuid
import re

from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()
    with IpcSession(args.kicad_cli) as api:
        project = api.module("common.commands.project_commands")
        editor = api.module("common.commands.editor_commands")
        commands = api.module("board.board_commands")
        board = api.module("board.board_types")
        rules = api.module("board.board")
        types = api.module("common.types.base_types")
        jobs = api.module("common.types.jobs")
        enums = api.module("common.types.enums")
        sch_jobs = api.module("schematic.schematic_jobs")
        design = api.copy_project("demos/ecc83", "pcb-regression")
        board_path = design / "ecc83-pp.kicad_pcb"
        schematic_path = design / "ecc83-pp.kicad_sch"
        document = api.request(project.OpenDocument(type=types.DOCTYPE_PCB, path=str(board_path)),
                               project.OpenDocumentResponse).document
        header = types.ItemHeader(document=document)

        def request(command, response=Empty, **fields):
            return api.request(command(board=document, **fields), response)

        def items(kind):
            result = api.request(editor.GetItems(header=header, types=[kind]), editor.GetItemsResponse)
            assert result.status == types.IRS_OK, result
            return result.items

        layers = request(commands.GetBoardEnabledLayers, commands.BoardEnabledLayersResponse)
        changed_layers = request(commands.SetBoardEnabledLayers, commands.BoardEnabledLayersResponse,
                                 copper_layer_count=layers.copper_layer_count, layers=layers.layers)
        assert changed_layers == layers
        for invalid_count in (0, 1, 3, 1024):
            rejected = api.request_raw(commands.SetBoardEnabledLayers(
                board=document, copper_layer_count=invalid_count, layers=layers.layers))
            assert rejected.status.status == api.envelope.AS_BAD_REQUEST, rejected
            assert request(commands.GetBoardEnabledLayers, commands.BoardEnabledLayersResponse) == layers
        for origin_type in (commands.BOT_GRID, commands.BOT_DRILL):
            original = request(commands.GetBoardOrigin, types.Vector2, type=origin_type)
            origin = types.Vector2(x_nm=11000000, y_nm=12000000)
            request(commands.SetBoardOrigin, type=origin_type, origin=origin)
            assert request(commands.GetBoardOrigin, types.Vector2, type=origin_type) == origin
            request(commands.SetBoardOrigin, type=origin_type, origin=original)
        zones = []
        for packed in items(enums.KOT_PCB_ZONE):
            zone = board.Zone()
            assert packed.Unpack(zone)
            if zone.type != board.ZT_RULE_AREA:
                zones.append(zone.id)
        assert zones, "Fixture requires a copper zone"
        request(commands.RefillZones, zones=zones[:1])
        request(commands.RefillZones)
        print("PASS PCB enabled layers, both origins, selected/all zone refill", flush=True)

        sample = board.Track()
        assert items(enums.KOT_PCB_TRACE)[0].Unpack(sample)
        tracks = []
        for offset in (0, 1000000):
            track = board.Track(id=types.KIID(value=str(uuid.uuid4())),
                                start=types.Vector2(x_nm=500000000 + offset, y_nm=500000000),
                                end=types.Vector2(x_nm=501000000 + offset, y_nm=500000000),
                                width=types.Distance(value_nm=250000), layer=board.BL_F_Cu,
                                net=sample.net)
            tracks.append(track)
        transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
        create = editor.CreateItems(header=header)
        for track in tracks:
            create.items.add().Pack(track)
        created = api.request(create, editor.CreateItemsResponse)
        assert all(item.status.code == editor.ISC_OK for item in created.created_items), created
        api.request(editor.EndCommit(header=header, id=transaction.id, action=editor.CMA_COMMIT,
                                     message="Connectivity regression"), editor.EndCommitResponse)

        def track_ids(command):
            result = api.request(command, editor.GetItemsResponse)
            assert result.status == types.IRS_OK, result
            result_ids = set()
            for packed in result.items:
                track = board.Track()
                if packed.Unpack(track):
                    result_ids.add(track.id.value)
            return result_ids

        connected = track_ids(commands.GetConnectedItems(header=header, items=[tracks[0].id],
                                                        types=[enums.KOT_PCB_TRACE]))
        assert tracks[1].id.value in connected, connected
        by_net = track_ids(commands.GetItemsByNet(header=header, nets=[sample.net],
                                                 types=[enums.KOT_PCB_TRACE]))
        assert {track.id.value for track in tracks} <= by_net
        by_class = track_ids(commands.GetItemsByNetClass(header=header, net_classes=["Default"],
                                                       types=[enums.KOT_PCB_TRACE]))
        assert by_class, "Default netclass query returned no tracks"
        api.request(editor.AddToSelection(header=header, items=[track.id for track in tracks]),
                    editor.SelectionResponse)
        assert len(api.request(editor.GetSelection(header=header), editor.SelectionResponse).items) == 2
        api.request(editor.ClearSelection(header=header), Empty)
        assert not api.request(editor.GetSelection(header=header), editor.SelectionResponse).items
        invalid_types = api.request_raw(commands.GetConnectedItems(
            header=header, items=[tracks[0].id], types=[enums.KOT_SCH_SYMBOL]))
        assert invalid_types.status.status == api.envelope.AS_BAD_REQUEST, invalid_types
        print("PASS PCB connectivity, net/netclass queries, headless selection", flush=True)

        design_rules = request(commands.GetBoardDesignRules, commands.BoardDesignRulesResponse).rules
        design_rules.constraints.min_clearance.value_nm = 230000
        assert len(design_rules.teardrops.target_params) == 3
        design_rules.teardrops.target_vias = not design_rules.teardrops.target_vias
        design_rules.teardrops.target_params[0].params.max_length.value_nm = 1700000
        request(commands.SetBoardDesignRules, commands.BoardDesignRulesResponse, rules=design_rules)
        assert request(commands.GetBoardDesignRules, commands.BoardDesignRulesResponse).rules.constraints.min_clearance.value_nm == 230000
        assert request(commands.GetBoardDesignRules, commands.BoardDesignRulesResponse).rules.teardrops == design_rules.teardrops
        before_invalid = request(commands.GetBoardDesignRules, commands.BoardDesignRulesResponse).rules
        invalid = rules.BoardDesignRules()
        invalid.CopyFrom(before_invalid)
        invalid.constraints.min_clearance.value_nm = 999999
        invalid.severities.add(error_key="backplane-invalid-key", severity=types.RS_ERROR)
        rejected = api.request_raw(commands.SetBoardDesignRules(board=document, rules=invalid))
        assert rejected.status.status == api.envelope.AS_BAD_REQUEST, rejected
        assert request(commands.GetBoardDesignRules, commands.BoardDesignRulesResponse).rules == before_invalid
        custom = rules.CustomRule(name="Backplane regression clearance", condition="A.Type == 'Track'",
                                  severity=types.RS_ERROR)
        custom.constraints.add(type=rules.CRCT_CLEARANCE, numeric=types.MinOptMax(min=240000))
        request(commands.SetCustomDesignRules, commands.CustomRulesResponse, rules=[custom])
        restored = request(commands.GetCustomDesignRules, commands.CustomRulesResponse)
        assert restored.status == commands.CRS_VALID and restored.rules[0].name == custom.name, restored
        assert restored.rules[0].constraints[0].numeric.min == 240000, restored
        plot = request(commands.GetBoardPlotSettings, commands.BoardPlotSettingsResponse).plot_settings
        plot.mirror = not plot.mirror
        request(commands.SetBoardPlotSettings, plot_settings=plot)
        assert request(commands.GetBoardPlotSettings, commands.BoardPlotSettingsResponse).plot_settings.mirror == plot.mirror
        print("PASS board/custom rules and plot-settings read/write", flush=True)

        schematic = api.request(project.OpenDocument(type=types.DOCTYPE_SCHEMATIC,
                                                      path=str(schematic_path)),
                                project.OpenDocumentResponse).document
        netlist_path = design / "updated-netlist.net"
        result = api.request(sch_jobs.RunSchematicJobExportNetlist(
            job_settings=jobs.RunJobSettings(document=schematic, output_path=str(netlist_path)),
            format=sch_jobs.SNF_KICAD_SEXPR), jobs.RunJobResponse)
        assert result.status == jobs.JS_SUCCESS, result
        native_netlist = netlist_path.read_text()
        component = re.search(r'(\(comp\s+\(ref "(R[^"\n]+)"\)\s+\(value ")([^"\n]*)("\))',
                              native_netlist)
        assert component, "Fixture has no resistor in its native netlist"
        reference = component.group(2)
        original_value = component.group(3)
        native_netlist = (native_netlist[:component.start(3)] + "2.2k IPC" +
                          native_netlist[component.end(3):])
        netlist_path.write_text(native_netlist)

        def value():
            for packed in items(enums.KOT_PCB_FOOTPRINT):
                footprint = board.FootprintInstance()
                assert packed.Unpack(footprint)
                if footprint.reference_field.text.text.text == reference:
                    return footprint.value_field.text.text.text
            raise AssertionError(f"Footprint {reference} is missing")

        before = value()
        result = request(commands.ImportNetlist, commands.ImportNetlistResponse,
                         netlist_path=str(netlist_path), dry_run=True, match_mode=commands.NMM_REFERENCE)
        assert result.error_count == 0, result.report
        assert value() == before, "Dry-run netlist import mutated the board"
        result = request(commands.ImportNetlist, commands.ImportNetlistResponse,
                         netlist_path=str(netlist_path), match_mode=commands.NMM_REFERENCE)
        assert result.error_count == 0, result.report
        assert value() == "2.2k IPC", (original_value, value())
        api.request(project.SaveDocument(document=document), Empty)
        api.request(project.CloseDocument(document=document), Empty)
        document = api.request(project.OpenDocument(type=types.DOCTYPE_PCB, path=str(board_path)),
                               project.OpenDocumentResponse).document
        header = types.ItemHeader(document=document)
        assert value() == "2.2k IPC"
        assert request(commands.GetBoardDesignRules, commands.BoardDesignRulesResponse).rules.constraints.min_clearance.value_nm == 230000
        assert request(commands.GetBoardPlotSettings, commands.BoardPlotSettingsResponse).plot_settings.mirror == plot.mirror
        assert request(commands.GetBoardDesignRules, commands.BoardDesignRulesResponse).rules.teardrops == design_rules.teardrops
        print("PASS netlist dry-run/update and save/reopen persistence", flush=True)
        api.request(project.CloseAllDocuments(force=True), Empty)


if __name__ == "__main__":
    main()
