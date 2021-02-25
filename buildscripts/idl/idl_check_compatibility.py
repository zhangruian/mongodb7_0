# Copyright (C) 2021-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
"""Checks compatibility of old and new IDL files.

In order to support user-selectable API versions for the server, server commands are now
defined using IDL files. This script checks that old and new commands are compatible with each
other, which allows commands to be updated without breaking the API specifications within a
specific API version.

This script accepts two directories as arguments, the "old" and the "new" IDL directory.
Before running this script, run checkout_idl_files_from_past_releases.py to find and create
directories containing the old IDL files from previous releases.
"""

import argparse
import logging
import os
import sys
from typing import Dict, List, Optional, Tuple, Union

from idl import parser, syntax, errors, common
from idl.compiler import CompilerImportResolver
from idl_compatibility_errors import IDLCompatibilityContext, IDLCompatibilityErrorCollection


def get_new_commands(
        ctxt: IDLCompatibilityContext, new_idl_dir: str, import_directories: List[str]
) -> Tuple[Dict[str, syntax.Command], Dict[str, syntax.IDLParsedSpec], Dict[str, str]]:
    """Get new IDL commands and check validity."""
    new_commands: Dict[str, syntax.Command] = dict()
    new_command_file: Dict[str, syntax.IDLParsedSpec] = dict()
    new_command_file_path: Dict[str, str] = dict()

    for dirpath, _, filenames in os.walk(new_idl_dir):
        for new_filename in filenames:
            if not new_filename.endswith('.idl'):
                continue

            new_idl_file_path = os.path.join(dirpath, new_filename)
            with open(new_idl_file_path) as new_file:
                new_idl_file = parser.parse(
                    new_file, new_idl_file_path,
                    CompilerImportResolver(import_directories + [new_idl_dir]))
                if new_idl_file.errors:
                    new_idl_file.errors.dump_errors()
                    raise ValueError(f"Cannot parse {new_idl_file_path}")

                for new_cmd in new_idl_file.spec.symbols.commands:
                    if new_cmd.api_version == "":
                        continue

                    if new_cmd.api_version != "1":
                        # We're not ready to handle future API versions yet.
                        ctxt.add_command_invalid_api_version_error(
                            new_cmd.command_name, new_cmd.api_version, new_idl_file_path)
                        continue

                    if new_cmd.command_name in new_commands:
                        ctxt.add_duplicate_command_name_error(new_cmd.command_name, new_idl_dir,
                                                              new_idl_file_path)
                        continue
                    new_commands[new_cmd.command_name] = new_cmd

                    new_command_file[new_cmd.command_name] = new_idl_file
                    new_command_file_path[new_cmd.command_name] = new_idl_file_path

    return new_commands, new_command_file, new_command_file_path


def get_field_type(field: Union[syntax.Field, syntax.Command], idl_file: syntax.IDLParsedSpec,
                   idl_file_path: str) -> Optional[Union[syntax.Enum, syntax.Struct, syntax.Type]]:
    """Resolve and get field type of a field from the IDL file."""
    parser_ctxt = errors.ParserContext(idl_file_path, errors.ParserErrorCollection())
    field_type = idl_file.spec.symbols.resolve_field_type(parser_ctxt, field, field.name,
                                                          field.type)
    if parser_ctxt.errors.has_errors():
        parser_ctxt.errors.dump_errors()
    return field_type


def check_subset(ctxt: IDLCompatibilityContext, cmd_name: str, field_name: str, type_name: str,
                 sub_list: List[Union[str, syntax.EnumValue]],
                 super_list: List[Union[str, syntax.EnumValue]], file_path: str):
    # pylint: disable=too-many-arguments
    """Check if sub_list is a subset of the super_list and log an error if not."""
    if not set(sub_list).issubset(super_list):
        ctxt.add_command_not_subset_error(cmd_name, field_name, type_name, file_path)


def check_superset(ctxt: IDLCompatibilityContext, cmd_name: str, param_name: str, type_name: str,
                   super_list: List[Union[str, syntax.EnumValue]],
                   sub_list: List[Union[str, syntax.EnumValue]], file_path: str):
    # pylint: disable=too-many-arguments
    """Check if super_list is a superset of the sub_list and log an error if not."""
    if not set(super_list).issuperset(sub_list):
        ctxt.add_command_parameter_type_not_superset_error(cmd_name, param_name, type_name,
                                                           file_path)


def check_type_superset(ctxt: IDLCompatibilityContext, cmd_name: str, type_name: str,
                        sub_list: List[Union[str, syntax.EnumValue]],
                        super_list: List[Union[str, syntax.EnumValue]], file_path: str):
    # pylint: disable=too-many-arguments
    """Check if sub_list is a subset of the super_list and log an error if not."""
    if not set(sub_list).issubset(super_list):
        ctxt.add_command_type_not_superset_error(cmd_name, type_name, file_path)


def check_reply_field_type_recursive(
        ctxt: IDLCompatibilityContext, old_field_type: syntax.Type,
        new_field_type: Optional[Union[syntax.Enum, syntax.Struct, syntax.Type]], cmd_name: str,
        field_name: str, old_idl_file: syntax.IDLParsedSpec, new_idl_file: syntax.IDLParsedSpec,
        old_idl_file_path: str, new_idl_file_path: str) -> None:
    # pylint: disable=too-many-arguments,too-many-branches
    """Check compatibility between old and new reply field type if old field type is a syntax.Type instance."""
    if not isinstance(new_field_type, syntax.Type):
        ctxt.add_new_reply_field_type_enum_or_struct_error(
            cmd_name, field_name, new_field_type.name, old_field_type.name, new_idl_file_path)
        return

    if "any" in old_field_type.bson_serialization_type:
        ctxt.add_old_reply_field_bson_any_error(cmd_name, field_name, old_field_type.name,
                                                old_idl_file_path)
        return
    if "any" in new_field_type.bson_serialization_type:
        ctxt.add_new_reply_field_bson_any_error(cmd_name, field_name, new_field_type.name,
                                                new_idl_file_path)
        return

    if isinstance(old_field_type, syntax.VariantType):
        # If the new type is not variant just check the single type.
        new_variant_types = new_field_type.variant_types if isinstance(
            new_field_type, syntax.VariantType) else [new_field_type]
        old_variant_types = old_field_type.variant_types

        # Check that new variant types are a subset of old variant types.
        for new_variant_type in new_variant_types:
            old_variant_type_exists = False
            for old_variant_type in old_variant_types:
                if old_variant_type.name == new_variant_type.name:
                    old_variant_type_exists = True
                    # Check that the old and new version of each variant type is also compatible.
                    check_reply_field_type_recursive(
                        ctxt, old_variant_type, new_variant_type, cmd_name, field_name,
                        old_idl_file, new_idl_file, old_idl_file_path, new_idl_file_path)

            if not old_variant_type_exists:
                ctxt.add_new_reply_field_variant_type_not_subset_error(
                    cmd_name, field_name, new_field_type.name, new_idl_file_path)

        # If new type is variant and has a struct as a variant type, compare old and new variant_struct_type.
        # Since enums can't be part of variant types, we don't explicitly check for enums.
        if isinstance(new_field_type,
                      syntax.VariantType) and new_field_type.variant_struct_type is not None:
            if old_field_type.variant_struct_type is None:
                ctxt.add_new_reply_field_variant_type_not_subset_error(
                    cmd_name, field_name, new_field_type.variant_struct_type.name,
                    new_idl_file_path)
            else:
                check_reply_fields(ctxt, old_field_type.variant_struct_type,
                                   new_field_type.variant_struct_type, cmd_name, old_idl_file,
                                   new_idl_file, old_idl_file_path, new_idl_file_path)

    else:
        if isinstance(new_field_type, syntax.VariantType):
            ctxt.add_new_reply_field_variant_type_error(cmd_name, field_name, old_field_type.name,
                                                        new_field_type.name, new_idl_file_path)
        else:
            check_subset(ctxt, cmd_name, field_name, new_field_type.name,
                         new_field_type.bson_serialization_type,
                         old_field_type.bson_serialization_type, new_idl_file_path)


def check_reply_field_type(ctxt: IDLCompatibilityContext,
                           old_field_type: Optional[Union[syntax.Enum, syntax.Struct, syntax.Type]],
                           new_field_type: Optional[Union[syntax.Enum, syntax.Struct, syntax.Type]],
                           cmd_name: str, field_name: str, old_idl_file: syntax.IDLParsedSpec,
                           new_idl_file: syntax.IDLParsedSpec, old_idl_file_path: str,
                           new_idl_file_path: str):
    """Check compatibility between old and new reply field type."""
    # pylint: disable=too-many-arguments,too-many-branches
    if old_field_type is None:
        ctxt.add_reply_field_type_invalid_error(cmd_name, field_name, old_idl_file_path)
        ctxt.errors.dump_errors()
        sys.exit(1)
    if new_field_type is None:
        ctxt.add_reply_field_type_invalid_error(cmd_name, field_name, new_idl_file_path)
        ctxt.errors.dump_errors()
        sys.exit(1)

    if isinstance(old_field_type, syntax.Type):
        check_reply_field_type_recursive(ctxt, old_field_type, new_field_type, cmd_name, field_name,
                                         old_idl_file, new_idl_file, old_idl_file_path,
                                         new_idl_file_path)

    elif isinstance(old_field_type, syntax.Enum):
        if isinstance(new_field_type, syntax.Enum):
            check_subset(ctxt, cmd_name, field_name, new_field_type.name, new_field_type.values,
                         old_field_type.values, new_idl_file_path)
        else:
            ctxt.add_new_reply_field_type_not_enum_error(cmd_name, field_name, new_field_type.name,
                                                         old_field_type.name, new_idl_file_path)
    elif isinstance(old_field_type, syntax.Struct):
        if isinstance(new_field_type, syntax.Struct):
            check_reply_fields(ctxt, old_field_type, new_field_type, cmd_name, old_idl_file,
                               new_idl_file, old_idl_file_path, new_idl_file_path)
        else:
            ctxt.add_new_reply_field_type_not_struct_error(
                cmd_name, field_name, new_field_type.name, old_field_type.name, new_idl_file_path)


def check_command_parameter_type(
        ctxt: IDLCompatibilityContext,
        old_parameter_type: Optional[Union[syntax.Enum, syntax.Struct, syntax.Type]],
        new_parameter_type: Optional[Union[syntax.Enum, syntax.Struct, syntax.Type]], cmd_name: str,
        param_name: str, old_idl_file_path: str, new_idl_file_path: str):
    """Check compatibility between old and new command parameter type."""
    # pylint: disable=too-many-arguments,too-many-branches
    if old_parameter_type is None:
        ctxt.add_command_parameter_type_invalid_error(cmd_name, param_name, old_idl_file_path)
        ctxt.errors.dump_errors()
        sys.exit(1)
    if new_parameter_type is None:
        ctxt.add_command_parameter_type_invalid_error(cmd_name, param_name, new_idl_file_path)
        ctxt.errors.dump_errors()
        sys.exit(1)

    if isinstance(old_parameter_type, syntax.Type):
        if isinstance(new_parameter_type, syntax.Type):
            if "any" in old_parameter_type.bson_serialization_type:
                ctxt.add_old_parameter_type_bson_any_error(
                    cmd_name, param_name, old_parameter_type.name, old_idl_file_path)
            elif "any" in new_parameter_type.bson_serialization_type:
                ctxt.add_new_parameter_type_bson_any_error(
                    cmd_name, param_name, new_parameter_type.name, new_idl_file_path)

            else:
                check_superset(ctxt, cmd_name, param_name, new_parameter_type.name,
                               new_parameter_type.bson_serialization_type,
                               old_parameter_type.bson_serialization_type, new_idl_file_path)
        else:
            ctxt.add_new_command_parameter_type_enum_or_struct_error(
                cmd_name, param_name, new_parameter_type.name, old_parameter_type.name,
                new_idl_file_path)

    elif isinstance(old_parameter_type, syntax.Enum):
        if isinstance(new_parameter_type, syntax.Enum):
            check_superset(ctxt, cmd_name, param_name, new_parameter_type.name,
                           new_parameter_type.values, old_parameter_type.values, new_idl_file_path)
        else:
            ctxt.add_new_command_parameter_type_not_enum_error(
                cmd_name, param_name, new_parameter_type.name, old_parameter_type.name,
                new_idl_file_path)

    elif isinstance(old_parameter_type, syntax.Struct):
        if not isinstance(new_parameter_type, syntax.Struct):
            ctxt.add_new_command_parameter_type_not_struct_error(
                cmd_name, param_name, new_parameter_type.name, old_parameter_type.name,
                new_idl_file_path)


def check_command_type(ctxt: IDLCompatibilityContext,
                       old_type: Optional[Union[syntax.Enum, syntax.Struct, syntax.Type]],
                       new_type: Optional[Union[syntax.Enum, syntax.Struct, syntax.Type]],
                       cmd_name: str, old_idl_file: syntax.IDLParsedSpec,
                       new_idl_file: syntax.IDLParsedSpec, old_idl_file_path: str,
                       new_idl_file_path: str):
    """Check compatibility between old and new command type."""
    # pylint: disable=too-many-arguments,too-many-branches
    if old_type is None:
        ctxt.add_command_type_invalid_error(cmd_name, old_idl_file_path)
        ctxt.errors.dump_errors()
        sys.exit(1)
    if new_type is None:
        ctxt.add_command_type_invalid_error(cmd_name, new_idl_file_path)
        ctxt.errors.dump_errors()
        sys.exit(1)

    if isinstance(old_type, syntax.Type):
        if isinstance(new_type, syntax.Type):
            if "any" in old_type.bson_serialization_type:
                ctxt.add_old_command_type_bson_any_error(cmd_name, old_type.name, old_idl_file_path)
            elif "any" in new_type.bson_serialization_type:
                ctxt.add_new_command_type_bson_any_error(cmd_name, new_type.name, new_idl_file_path)
            else:
                check_type_superset(ctxt, cmd_name, new_type.name, old_type.bson_serialization_type,
                                    new_type.bson_serialization_type, new_idl_file_path)
        else:
            ctxt.add_new_command_type_enum_or_struct_error(cmd_name, new_type.name, old_type.name,
                                                           new_idl_file_path)
    elif isinstance(old_type, syntax.Enum):
        if isinstance(new_type, syntax.Enum):
            check_type_superset(ctxt, cmd_name, new_type.name, old_type.values, new_type.values,
                                new_idl_file_path)
        else:
            ctxt.add_new_command_type_not_enum_error(cmd_name, new_type.name, old_type.name,
                                                     new_idl_file_path)
    elif isinstance(old_type, syntax.Struct):
        if isinstance(new_type, syntax.Struct):
            check_command_type_struct_fields(ctxt, old_type, new_type, cmd_name, old_idl_file,
                                             new_idl_file, old_idl_file_path, new_idl_file_path)
        else:
            ctxt.add_new_command_type_not_struct_error(cmd_name, new_type.name, old_type.name,
                                                       new_idl_file_path)


def check_param_or_type_validator(ctxt: IDLCompatibilityContext, old_field: syntax.Field,
                                  new_field: syntax.Field, cmd_name: str, new_idl_file_path: str,
                                  type_name: Optional[str], is_command_parameter: bool):
    """
    Check compatibility between old and new validators.

    Check compatibility between old and new validators in command parameters and command type
    struct fields.
    """
    # pylint: disable=too-many-arguments
    if new_field.validator:
        if old_field.validator:
            if new_field.validator != old_field.validator:
                if is_command_parameter:
                    ctxt.add_command_parameter_validators_not_equal_error(
                        cmd_name, new_field.name, new_idl_file_path)
                else:
                    ctxt.add_command_type_validators_not_equal_error(
                        cmd_name, type_name, new_field.name, new_idl_file_path)
        else:
            if is_command_parameter:
                ctxt.add_command_parameter_contains_validator_error(cmd_name, new_field.name,
                                                                    new_idl_file_path)
            else:
                ctxt.add_command_type_contains_validator_error(cmd_name, type_name, new_field.name,
                                                               new_idl_file_path)


def check_command_type_struct_field(
        ctxt: IDLCompatibilityContext, type_name: str, old_field: syntax.Field,
        new_field: syntax.Field, cmd_name: str, old_idl_file: syntax.IDLParsedSpec,
        new_idl_file: syntax.IDLParsedSpec, old_idl_file_path: str, new_idl_file_path: str):
    """Check compatibility between old and new type struct field."""
    # pylint: disable=too-many-arguments
    if new_field.unstable:
        ctxt.add_new_command_type_field_unstable_error(cmd_name, type_name, new_field.name,
                                                       new_idl_file_path)
    if old_field.optional and not new_field.optional:
        ctxt.add_new_command_type_field_required_error(cmd_name, type_name, new_field.name,
                                                       new_idl_file_path)

    check_param_or_type_validator(ctxt, old_field, new_field, cmd_name, new_idl_file_path,
                                  type_name, is_command_parameter=False)

    old_field_type = get_field_type(old_field, old_idl_file, old_idl_file_path)
    new_field_type = get_field_type(new_field, new_idl_file, new_idl_file_path)

    check_command_type(ctxt, old_field_type, new_field_type, cmd_name, old_idl_file, new_idl_file,
                       old_idl_file_path, new_idl_file_path)


def check_command_type_struct_fields(
        ctxt: IDLCompatibilityContext, old_type: syntax.Struct, new_type: syntax.Struct,
        cmd_name: str, old_idl_file: syntax.IDLParsedSpec, new_idl_file: syntax.IDLParsedSpec,
        old_idl_file_path: str, new_idl_file_path: str):
    """Check compatibility between old and new type fields."""
    # pylint: disable=too-many-arguments
    for old_field in old_type.fields or []:
        if old_field.unstable:
            continue

        new_field_exists = False
        for new_field in new_type.fields or []:
            if new_field.name == old_field.name:
                new_field_exists = True
                check_command_type_struct_field(ctxt, old_type.name, old_field, new_field, cmd_name,
                                                old_idl_file, new_idl_file, old_idl_file_path,
                                                new_idl_file_path)

                break

        if not new_field_exists:
            ctxt.add_new_command_type_field_missing_error(cmd_name, old_type.name, old_field.name,
                                                          old_idl_file_path)


def check_reply_field(ctxt: IDLCompatibilityContext, old_field: syntax.Field,
                      new_field: syntax.Field, cmd_name: str, old_idl_file: syntax.IDLParsedSpec,
                      new_idl_file: syntax.IDLParsedSpec, old_idl_file_path: str,
                      new_idl_file_path: str):
    """Check compatibility between old and new reply field."""
    # pylint: disable=too-many-arguments
    if new_field.unstable:
        ctxt.add_new_reply_field_unstable_error(cmd_name, new_field.name, new_idl_file_path)
    if new_field.optional and not old_field.optional:
        ctxt.add_new_reply_field_optional_error(cmd_name, new_field.name, new_idl_file_path)

    if old_field.validator:
        # Not implemented.
        ctxt.add_reply_field_contains_validator_error(cmd_name, old_field.name, old_idl_file_path)
        ctxt.errors.dump_errors()
        sys.exit(1)
    if new_field.validator:
        # Not implemented.
        ctxt.add_reply_field_contains_validator_error(cmd_name, new_field.name, new_idl_file_path)
        ctxt.errors.dump_errors()
        sys.exit(1)

    old_field_type = get_field_type(old_field, old_idl_file, old_idl_file_path)
    new_field_type = get_field_type(new_field, new_idl_file, new_idl_file_path)

    check_reply_field_type(ctxt, old_field_type, new_field_type, cmd_name, old_field.name,
                           old_idl_file, new_idl_file, old_idl_file_path, new_idl_file_path)


def check_reply_fields(ctxt: IDLCompatibilityContext, old_reply: syntax.Struct,
                       new_reply: syntax.Struct, cmd_name: str, old_idl_file: syntax.IDLParsedSpec,
                       new_idl_file: syntax.IDLParsedSpec, old_idl_file_path: str,
                       new_idl_file_path: str):
    """Check compatibility between old and new reply fields."""
    # pylint: disable=too-many-arguments
    for old_field in old_reply.fields or []:
        if old_field.unstable:
            continue

        new_field_exists = False
        for new_field in new_reply.fields or []:
            if new_field.name == old_field.name:
                new_field_exists = True
                check_reply_field(ctxt, old_field, new_field, cmd_name, old_idl_file, new_idl_file,
                                  old_idl_file_path, new_idl_file_path)

                break

        if not new_field_exists:
            ctxt.add_new_reply_field_missing_error(cmd_name, old_field.name, old_idl_file_path)


def check_command_parameters(ctxt: IDLCompatibilityContext, old_cmd: syntax.Command,
                             new_cmd: syntax.Command, cmd_name: str,
                             old_idl_file: syntax.IDLParsedSpec, new_idl_file: syntax.IDLParsedSpec,
                             old_idl_file_path: str, new_idl_file_path: str):
    """Check compatibility between old and new command parameters."""
    # pylint: disable=too-many-arguments
    for old_param in old_cmd.fields:
        new_param_exists = False
        for new_param in new_cmd.fields:
            if new_param.name == old_param.name:
                new_param_exists = True
                check_command_parameter(ctxt, old_param, new_param, cmd_name, old_idl_file,
                                        new_idl_file, old_idl_file_path, new_idl_file_path)
                break

        if not new_param_exists and not old_param.unstable:
            ctxt.add_command_parameter_removed_error(old_cmd.command_name, old_param.name,
                                                     old_idl_file_path)

    # Check if a new parameter has been added to the command.
    # If so, it must be optional.
    for new_param in new_cmd.fields:
        newly_added = True
        for old_param in old_cmd.fields:
            if new_param.name == old_param.name:
                newly_added = False

        if newly_added and not new_param.optional and not new_param.unstable:
            ctxt.add_new_command_parameter_required_error(new_cmd.name, new_param.name,
                                                          new_idl_file_path)


def check_command_parameter(ctxt: IDLCompatibilityContext, old_param: syntax.Field,
                            new_param: syntax.Field, cmd_name: str,
                            old_idl_file: syntax.IDLParsedSpec, new_idl_file: syntax.IDLParsedSpec,
                            old_idl_file_path: str, new_idl_file_path: str):
    """Check compatibility between the old and new command parameter."""
    # pylint: disable=too-many-arguments
    if not old_param.unstable and new_param.unstable:
        ctxt.add_command_parameter_unstable_error(cmd_name, old_param.name, old_idl_file_path)
    if old_param.unstable and not new_param.optional and not new_param.unstable:
        ctxt.add_command_parameter_stable_required_error(cmd_name, old_param.name,
                                                         old_idl_file_path)
    if old_param.optional and not new_param.optional:
        ctxt.add_command_parameter_required_error(cmd_name, old_param.name, old_idl_file_path)

    check_param_or_type_validator(ctxt, old_param, new_param, cmd_name, new_idl_file_path,
                                  type_name=None, is_command_parameter=True)

    old_parameter_type = get_field_type(old_param, old_idl_file, old_idl_file_path)
    new_parameter_type = get_field_type(new_param, new_idl_file, new_idl_file_path)

    check_command_parameter_type(ctxt, old_parameter_type, new_parameter_type, cmd_name,
                                 old_param.name, old_idl_file_path, new_idl_file_path)


def check_namespace(ctxt: IDLCompatibilityContext, old_cmd: syntax.Command, new_cmd: syntax.Command,
                    old_idl_file: syntax.IDLParsedSpec, new_idl_file: syntax.IDLParsedSpec,
                    old_idl_file_path: str, new_idl_file_path: str):
    """Check compatibility between old and new namespace."""
    # pylint: disable=too-many-arguments
    old_namespace = old_cmd.namespace
    new_namespace = new_cmd.namespace

    # IDL parser already checks that namespace must be one of these 4 types.
    if old_namespace == common.COMMAND_NAMESPACE_IGNORED:
        if new_namespace != common.COMMAND_NAMESPACE_IGNORED:
            ctxt.add_new_namespace_incompatible_error(old_cmd.command_name, old_namespace,
                                                      new_namespace, new_idl_file_path)
    elif old_namespace == common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB_OR_UUID:
        if new_namespace not in (common.COMMAND_NAMESPACE_IGNORED,
                                 common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB_OR_UUID):
            ctxt.add_new_namespace_incompatible_error(old_cmd.command_name, old_namespace,
                                                      new_namespace, new_idl_file_path)
    elif old_namespace == common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB:
        if new_namespace == common.COMMAND_NAMESPACE_TYPE:
            ctxt.add_new_namespace_incompatible_error(old_cmd.command_name, old_namespace,
                                                      new_namespace, new_idl_file_path)
    elif old_namespace == common.COMMAND_NAMESPACE_TYPE:
        old_type = get_field_type(old_cmd, old_idl_file, old_idl_file_path)
        if new_namespace == common.COMMAND_NAMESPACE_TYPE:
            new_type = get_field_type(new_cmd, new_idl_file, new_idl_file_path)
            check_command_type(ctxt, old_type, new_type, old_cmd.command_name, old_idl_file,
                               new_idl_file, old_idl_file_path, new_idl_file_path)

        # If old type is "namespacestring", the new namespace can be changed to any
        # of the other namespace types.
        elif old_type.name != "namespacestring":
            # Otherwise, the new namespace can only be changed to "ignored".
            if new_namespace != common.COMMAND_NAMESPACE_IGNORED:
                ctxt.add_new_namespace_incompatible_error(old_cmd.command_name, old_namespace,
                                                          new_namespace, new_idl_file_path)
    else:
        assert False, 'unrecognized namespace option'


def check_error_reply(old_basic_types_path: str, new_basic_types_path: str,
                      import_directories: List[str]) -> IDLCompatibilityErrorCollection:
    """Check IDL compatibility between old and new ErrorReply."""
    old_idl_dir = os.path.dirname(old_basic_types_path)
    new_idl_dir = os.path.dirname(new_basic_types_path)
    ctxt = IDLCompatibilityContext(old_idl_dir, new_idl_dir, IDLCompatibilityErrorCollection())
    with open(old_basic_types_path) as old_file:
        old_idl_file = parser.parse(old_file, old_basic_types_path,
                                    CompilerImportResolver(import_directories))
        if old_idl_file.errors:
            old_idl_file.errors.dump_errors()
            raise ValueError(f"Cannot parse {old_basic_types_path}")

        old_error_reply_struct = old_idl_file.spec.symbols.get_struct("ErrorReply")

        if old_error_reply_struct is None:
            ctxt.add_missing_error_reply_error(old_basic_types_path)
        else:
            with open(new_basic_types_path) as new_file:
                new_idl_file = parser.parse(new_file, new_basic_types_path,
                                            CompilerImportResolver(import_directories))
                if new_idl_file.errors:
                    new_idl_file.errors.dump_errors()
                    raise ValueError(f"Cannot parse {new_basic_types_path}")

                new_error_reply_struct = new_idl_file.spec.symbols.get_struct("ErrorReply")
                if new_error_reply_struct is None:
                    ctxt.add_missing_error_reply_error(new_basic_types_path)
                else:
                    check_reply_fields(ctxt, old_error_reply_struct, new_error_reply_struct, "n/a",
                                       old_idl_file, new_idl_file, old_basic_types_path,
                                       new_basic_types_path)
    ctxt.errors.dump_errors()
    return ctxt.errors


def check_compatibility(old_idl_dir: str, new_idl_dir: str,
                        import_directories: List[str]) -> IDLCompatibilityErrorCollection:
    """Check IDL compatibility between old and new IDL commands."""
    # pylint: disable=too-many-locals
    ctxt = IDLCompatibilityContext(old_idl_dir, new_idl_dir, IDLCompatibilityErrorCollection())

    new_commands, new_command_file, new_command_file_path = get_new_commands(
        ctxt, new_idl_dir, import_directories)

    # Check new commands' compatibility with old ones.
    # Note, a command can be added to V1 at any time, it's ok if a
    # new command has no corresponding old command.
    old_commands: Dict[str, syntax.Command] = dict()
    for dirpath, _, filenames in os.walk(old_idl_dir):
        for old_filename in filenames:
            if not old_filename.endswith('.idl'):
                continue

            old_idl_file_path = os.path.join(dirpath, old_filename)
            with open(old_idl_file_path) as old_file:
                old_idl_file = parser.parse(
                    old_file, old_idl_file_path,
                    CompilerImportResolver(import_directories + [old_idl_dir]))
                if old_idl_file.errors:
                    old_idl_file.errors.dump_errors()
                    raise ValueError(f"Cannot parse {old_idl_file_path}")

                for old_cmd in old_idl_file.spec.symbols.commands:
                    if old_cmd.api_version == "":
                        continue

                    if old_cmd.api_version != "1":
                        # We're not ready to handle future API versions yet.
                        ctxt.add_command_invalid_api_version_error(
                            old_cmd.command_name, old_cmd.api_version, old_idl_file_path)
                        continue

                    if old_cmd.command_name in old_commands:
                        ctxt.add_duplicate_command_name_error(old_cmd.command_name, old_idl_dir,
                                                              old_idl_file_path)
                        continue

                    old_commands[old_cmd.command_name] = old_cmd

                    if old_cmd.command_name not in new_commands:
                        # Can't remove a command from V1
                        ctxt.add_command_removed_error(old_cmd.command_name, old_idl_file_path)
                        continue

                    new_cmd = new_commands[old_cmd.command_name]
                    new_idl_file = new_command_file[old_cmd.command_name]
                    new_idl_file_path = new_command_file_path[old_cmd.command_name]

                    # Check compatibility of command's parameters.
                    check_command_parameters(ctxt, old_cmd, new_cmd, old_cmd.command_name,
                                             old_idl_file, new_idl_file, old_idl_file_path,
                                             new_idl_file_path)

                    check_namespace(ctxt, old_cmd, new_cmd, old_idl_file, new_idl_file,
                                    old_idl_file_path, new_idl_file_path)

                    old_reply = old_idl_file.spec.symbols.get_struct(old_cmd.reply_type)
                    new_reply = new_idl_file.spec.symbols.get_struct(new_cmd.reply_type)
                    check_reply_fields(ctxt, old_reply, new_reply, old_cmd.command_name,
                                       old_idl_file, new_idl_file, old_idl_file_path,
                                       new_idl_file_path)

    ctxt.errors.dump_errors()
    return ctxt.errors


def main():
    """Run the script."""
    arg_parser = argparse.ArgumentParser(description=__doc__)
    arg_parser.add_argument("-v", "--verbose", action="count", help="Enable verbose logging")
    arg_parser.add_argument("old_idl_dir", metavar="OLD_IDL_DIR",
                            help="Directory where old IDL files are located")
    arg_parser.add_argument("new_idl_dir", metavar="NEW_IDL_DIR",
                            help="Directory where new IDL files are located")
    args = arg_parser.parse_args()

    error_coll = check_compatibility(args.old_idl_dir, args.new_idl_dir, [])
    if error_coll.errors.has_errors():
        sys.exit(1)

    old_basic_types_path = os.path.join(args.old_idl_dir, "mongo/idl/basic_types.idl")
    new_basic_types_path = os.path.join(args.new_idl_dir, "mongo/idl/basic_types.idl")
    error_reply_coll = check_error_reply(old_basic_types_path, new_basic_types_path, [])
    if error_reply_coll.has_errors():
        sys.exit(1)


if __name__ == "__main__":
    main()
