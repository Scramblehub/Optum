"""Decoys: names built at runtime, generated code, overrides of an outside base."""
import argparse


class Shell:
    def run_command(self, name):
        return getattr(self, "_cmd_" + name)()  # a prefix: every _cmd_* is reachable

    def _cmd_export(self):  # decoy: reached through the "_cmd_" prefix
        return "export"

    def section(self, part):
        return getattr(self, f"_{part}_section")()  # a suffix after an f-string hole

    def _footer_section(self):  # decoy: reached through the "_section" suffix
        return "footer"


TEMPLATE = "cell = _render_cell(row)"  # generated code names it


def _render_cell(row):  # decoy: called by generated code
    return str(row)


class Parser(argparse.ArgumentParser):
    def _check_value(self, action, value):  # decoy: argparse (outside the project) calls it
        return super()._check_value(action, value)

    def _helper(self):  # decoy: on a class with an outside base, any method may be called by it
        return None
